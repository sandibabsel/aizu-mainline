// SPDX-FileCopyrightText: 2026 aizu-mainline contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/nvn/nvn_hle.h"
#include "core/hle/service/nvnflinger/buffer_transform_flags.h"
#include "core/hle/service/nvnflinger/pixel_format.h"
#include "core/memory.h"
#include "core/perf_stats.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"

namespace Core::NvnHook::Hle {
namespace {

// NVN texture format of RGBA8 color targets. Inferred from the trace: the window textures and the
// first fast-clear colour registered by Unreal use it, and 0x29 (most likely RGBA16F) is the HDR
// scene colour. Only RGBA8 is cleared until the format table is known.
constexpr u32 FormatRgba8 = 0x25;

struct PoolBuilder {
    u64 storage{};
    u64 size{};
};

struct TextureBuilder {
    u32 format{};
    u32 width{1};
    u32 height{1};
    u32 depth{1};
    u64 pool{};
    u64 offset{};
};

struct Texture {
    u32 format{};
    u32 width{};
    u32 height{};
    u64 cpu_address{};
};

struct CommandBuffer {
    std::array<u64, 8> render_targets{};
    u64 command_memory{};
};

struct Window {
    std::vector<u64> textures;
    u32 next{};
};

struct Mapping {
    DAddr address{};
    u64 size{};
};

struct HleState {
    std::mutex mutex;
    std::unordered_map<u64, PoolBuilder> pool_builders;
    std::unordered_map<u64, u64> pool_storage; // pool -> CPU address of its storage
    std::unordered_map<u64, TextureBuilder> texture_builders;
    std::unordered_map<u64, Texture> textures;
    std::unordered_map<u64, std::vector<u64>> window_builders;
    std::unordered_map<u64, Window> windows;
    std::unordered_map<u64, CommandBuffer> command_buffers;
    std::unordered_map<u64, Mapping> mappings; // page-aligned CPU address -> device mapping
    std::unordered_set<std::string> warned;
    std::optional<size_t> asid;
    u64 next_handle{1};
    u64 clears_logged{};
};

HleState& GetState() {
    static HleState state;
    return state;
}

Core::Memory::Memory& GuestMemory(Core::System& system) {
    return Kernel::GetCurrentProcess(system.Kernel()).GetMemory();
}

void WarnOnce(HleState& state, std::string_view what) {
    if (state.warned.emplace(what).second) {
        LOG_WARNING(Service_NVDRV, "NVN HLE: {}", what);
    }
}

u32 PackColor(Core::Memory::Memory& memory, u64 color_address) {
    u32 packed = 0;
    for (u32 i = 0; i < 4; ++i) {
        const float value = std::bit_cast<float>(memory.Read32(color_address + i * 4));
        const float clamped = std::isnan(value) ? 0.0f : std::clamp(value, 0.0f, 1.0f);
        packed |= static_cast<u32>(std::lround(clamped * 255.0f)) << (i * 8);
    }
    return packed;
}

void ClearTexture(Core::System& system, HleState& state, u64 texture, u64 color_address) {
    const auto it = state.textures.find(texture);
    if (it == state.textures.end() || color_address == 0) {
        WarnOnce(state, "clear of an unknown texture");
        return;
    }
    const Texture& tex = it->second;
    if (tex.format != FormatRgba8) {
        WarnOnce(state, fmt::format("clear of texture format 0x{:X} is not implemented", tex.format));
        return;
    }
    auto& memory = GuestMemory(system);
    const u32 color = PackColor(memory, color_address);
    if (state.clears_logged < 8) {
        ++state.clears_logged;
        LOG_INFO(Service_NVDRV, "NVN HLE: clear {}x{} texture at 0x{:X} to 0x{:08X}", tex.width,
                 tex.height, tex.cpu_address, color);
    }

    // A uniform colour looks the same in pitch and block-linear layouts, and the real driver
    // reserved at least width * height * 4 bytes for this texture.
    std::vector<u32> row(tex.width, color);
    const u64 row_bytes = row.size() * sizeof(u32);
    for (u32 y = 0; y < tex.height; ++y) {
        memory.WriteBlock(tex.cpu_address + y * row_bytes, row.data(), row_bytes);
    }
}

void Present(Core::System& system, HleState& state, u64 window, u32 index) {
    const auto win = state.windows.find(window);
    if (win == state.windows.end() || index >= win->second.textures.size()) {
        WarnOnce(state, "present of an unknown window texture");
        return;
    }
    const auto tex_it = state.textures.find(win->second.textures[index]);
    if (tex_it == state.textures.end()) {
        WarnOnce(state, "present of an untracked texture");
        return;
    }
    const Texture& tex = tex_it->second;

    // Map the texture memory into the device address space the compositor reads from.
    auto& smmu = system.Host1x().MemoryManager();
    if (!state.asid) {
        state.asid = smmu.RegisterProcess(&GuestMemory(system)).id;
    }
    const u64 base = Common::AlignDown(tex.cpu_address, Core::Memory::YUZU_PAGESIZE);
    const u64 size = Common::AlignUp(tex.cpu_address - base +
                                         Common::AlignUp(u64{tex.width} * 4, 64) *
                                             Common::AlignUp(u64{tex.height}, 128),
                                     Core::Memory::YUZU_PAGESIZE);
    auto& mapping = state.mappings[base];
    if (mapping.size < size) {
        if (mapping.size != 0) {
            smmu.Unmap(mapping.address, mapping.size);
            smmu.Free(mapping.address, mapping.size);
        }
        mapping.address = smmu.Allocate(size);
        mapping.size = size;
        smmu.Map(mapping.address, base, size, Core::Asid{*state.asid}, false);
    }

    std::vector<Tegra::FramebufferConfig> layers{Tegra::FramebufferConfig{
        .address = mapping.address,
        .offset = static_cast<u32>(tex.cpu_address - base),
        .width = tex.width,
        .height = tex.height,
        .stride = tex.width,
        .pixel_format = Service::android::PixelFormat::Rgba8888,
        .transform_flags = Service::android::BufferTransformFlags::Unset,
        .crop_rect = {0, 0, static_cast<int>(tex.width), static_cast<int>(tex.height)},
        .blending = Tegra::BlendMode::Opaque,
    }};
    system.GPU().RequestComposite(std::move(layers), {});
    system.SpeedLimiter().DoSpeedLimiting(system.CoreTiming().GetGlobalTimeUs());
    system.GetPerfStats().EndSystemFrame();
    system.GetPerfStats().BeginSystemFrame();
}

} // Anonymous namespace

bool IsReplaced(std::string_view name) {
    static constexpr std::array replaced{
        std::string_view{"nvnQueueSubmitCommands"}, std::string_view{"nvnQueueFlush"},
        std::string_view{"nvnQueueFinish"},         std::string_view{"nvnQueueFenceSync"},
        std::string_view{"nvnQueueWaitSync"},       std::string_view{"nvnQueuePresentTexture"},
        std::string_view{"nvnSyncWait"},            std::string_view{"nvnWindowAcquireTexture"},
    };
    return name.starts_with("nvnCommandBuffer") ||
           std::find(replaced.begin(), replaced.end(), name) != replaced.end();
}

bool IsObserved(std::string_view name) {
    return name.starts_with("nvnMemoryPoolBuilderSet") || name == "nvnMemoryPoolInitialize" ||
           name.starts_with("nvnTextureBuilderSet") || name == "nvnTextureInitialize" ||
           name == "nvnWindowBuilderSetTextures" || name == "nvnWindowInitialize";
}

void OnEnter(Core::System& system, std::string_view name, std::span<u64, 8> args) {
    auto& state = GetState();
    std::scoped_lock lk{state.mutex};

    if (name == "nvnMemoryPoolBuilderSetDefaults") {
        state.pool_builders[args[0]] = {};
    } else if (name == "nvnMemoryPoolBuilderSetStorage") {
        state.pool_builders[args[0]] = {.storage = args[1], .size = args[2]};
    } else if (name == "nvnMemoryPoolInitialize") {
        state.pool_storage[args[0]] = state.pool_builders[args[1]].storage;
    } else if (name == "nvnTextureBuilderSetDefaults") {
        state.texture_builders[args[0]] = {};
    } else if (name == "nvnTextureBuilderSetFormat") {
        state.texture_builders[args[0]].format = static_cast<u32>(args[1]);
    } else if (name == "nvnTextureBuilderSetSize1D") {
        auto& b = state.texture_builders[args[0]];
        b.width = static_cast<u32>(args[1]);
        b.height = b.depth = 1;
    } else if (name == "nvnTextureBuilderSetSize2D") {
        auto& b = state.texture_builders[args[0]];
        b.width = static_cast<u32>(args[1]);
        b.height = static_cast<u32>(args[2]);
        b.depth = 1;
    } else if (name == "nvnTextureBuilderSetSize3D") {
        auto& b = state.texture_builders[args[0]];
        b.width = static_cast<u32>(args[1]);
        b.height = static_cast<u32>(args[2]);
        b.depth = static_cast<u32>(args[3]);
    } else if (name == "nvnTextureBuilderSetStorage") {
        auto& b = state.texture_builders[args[0]];
        b.pool = args[1];
        b.offset = args[2];
    } else if (name == "nvnTextureInitialize") {
        const auto& b = state.texture_builders[args[1]];
        const auto pool = state.pool_storage.find(b.pool);
        if (pool == state.pool_storage.end()) {
            WarnOnce(state, "texture in an untracked memory pool");
            return;
        }
        state.textures[args[0]] = Texture{
            .format = b.format,
            .width = b.width,
            .height = b.height,
            .cpu_address = pool->second + b.offset,
        };
    } else if (name == "nvnWindowBuilderSetTextures") {
        auto& memory = GuestMemory(system);
        std::vector<u64> list(std::min<u64>(args[1], 8));
        for (std::size_t i = 0; i < list.size(); ++i) {
            list[i] = memory.Read64(args[2] + i * 8);
        }
        state.window_builders[args[0]] = std::move(list);
    } else if (name == "nvnWindowInitialize") {
        state.windows[args[0]] = Window{.textures = state.window_builders[args[1]]};
        LOG_INFO(Service_NVDRV, "NVN HLE: window with {} textures",
                 state.windows[args[0]].textures.size());
    }
}

void Call(Core::System& system, std::string_view name, std::span<u64, 8> args) {
    auto& state = GetState();
    std::scoped_lock lk{state.mutex};

    if (name.starts_with("nvnCommandBuffer")) {
        auto& cmd = state.command_buffers[args[0]];
        if (name == "nvnCommandBufferInitialize") {
            cmd = {};
            args[0] = 1;
        } else if (name == "nvnCommandBufferFinalize") {
            state.command_buffers.erase(args[0]);
        } else if (name == "nvnCommandBufferAddCommandMemory") {
            cmd.command_memory += args[3];
        } else if (name == "nvnCommandBufferGetCommandMemorySize") {
            args[0] = cmd.command_memory;
        } else if (name == "nvnCommandBufferEndRecording") {
            args[0] = state.next_handle++; // NVNcommandHandle passed to nvnQueueSubmitCommands
        } else if (name == "nvnCommandBufferSetRenderTargets") {
            auto& memory = GuestMemory(system);
            cmd.render_targets = {};
            const u64 count = std::min<u64>(args[1], cmd.render_targets.size());
            for (u64 i = 0; i < count && args[2] != 0; ++i) {
                cmd.render_targets[i] = memory.Read64(args[2] + i * 8);
            }
        } else if (name == "nvnCommandBufferClearColor") {
            if (args[1] < cmd.render_targets.size()) {
                ClearTexture(system, state, cmd.render_targets[args[1]], args[2]);
            }
        } else if (name == "nvnCommandBufferClearTexture") {
            ClearTexture(system, state, args[1], args[4]);
        }
        // Everything else (state binds, draws, copies, queries) is ignored for now.
        return;
    }

    if (name == "nvnWindowAcquireTexture") {
        auto& window = state.windows[args[0]];
        const u32 count = static_cast<u32>(std::max<std::size_t>(window.textures.size(), 1));
        GuestMemory(system).Write32(args[2], window.next);
        window.next = (window.next + 1) % count;
        args[0] = 0; // NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
    } else if (name == "nvnQueuePresentTexture") {
        Present(system, state, args[1], static_cast<u32>(args[2]));
    } else if (name == "nvnSyncWait") {
        args[0] = 0; // already signaled: the host executed everything immediately
    }
    // Queue submission, flushes, finishes and fences have nothing left to do.
}

void Reset() {
    auto& state = GetState();
    std::scoped_lock lk{state.mutex};
    state.pool_builders.clear();
    state.pool_storage.clear();
    state.texture_builders.clear();
    state.textures.clear();
    state.window_builders.clear();
    state.windows.clear();
    state.command_buffers.clear();
    state.mappings.clear();
    state.warned.clear();
    state.asid.reset();
    state.next_handle = 1;
    state.clears_logged = 0;
}

} // namespace Core::NvnHook::Hle
