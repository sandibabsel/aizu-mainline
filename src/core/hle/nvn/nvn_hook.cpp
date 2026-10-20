// SPDX-FileCopyrightText: 2026 aizu-mainline contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/nvn/nvn_hle.h"
#include "core/hle/nvn/nvn_hook.h"
#include "core/hle/nvn/nvn_mia_procs.h"
#include "core/memory.h"

namespace Core::NvnHook {
namespace {

struct Symbol {
    u64 offset;
    u64 size;
};

enum class Mode { Off, Trace, Hle };

struct HookState {
    std::mutex mutex;
    u64 sdk_base{};
    u64 sdk_end{};
    u64 proc_wrapper{};
    u64 real_get_proc_address{};
    std::unordered_set<std::string> reported;

    // Trace / HLE mode
    Mode mode{Mode::Off};
    u64 hle_stubs{};
    std::vector<std::string> hle_slots;
    u64 trampolines{};
    std::array<u64, MaxTracedProcs> calls{};
    std::array<u64, MaxTracedProcs> calls_since_dump{};
    std::unordered_set<u64> logged_integers;
    u64 presents{};
};

HookState& GetState() {
    static HookState state;
    return state;
}

template <typename T>
std::optional<T> ReadAt(std::span<const u8> image, u64 offset) {
    if (offset > image.size() || image.size() - offset < sizeof(T)) {
        return std::nullopt;
    }
    T value;
    std::memcpy(&value, image.data() + offset, sizeof(T));
    return value;
}

// Looks a symbol up in the module's dynamic symbol table (MOD0 -> .dynamic -> .dynsym).
std::optional<Symbol> FindSymbol(std::span<const u8> image, std::string_view wanted) {
    const auto mod0_offset = ReadAt<u32>(image, 4);
    if (!mod0_offset || ReadAt<u32>(image, *mod0_offset) != 0x30444F4D) { // "MOD0"
        return std::nullopt;
    }
    const auto dynamic_rel = ReadAt<s32>(image, *mod0_offset + 4);
    if (!dynamic_rel) {
        return std::nullopt;
    }

    u64 strtab = 0, symtab = 0, strsz = 0;
    for (u64 entry = *mod0_offset + static_cast<s64>(*dynamic_rel);; entry += 16) {
        const auto tag = ReadAt<s64>(image, entry);
        const auto value = ReadAt<u64>(image, entry + 8);
        if (!tag || !value || *tag == 0) {
            break;
        }
        if (*tag == 5) {
            strtab = *value;
        } else if (*tag == 6) {
            symtab = *value;
        } else if (*tag == 10) {
            strsz = *value;
        }
    }
    if (strtab == 0 || symtab == 0 || strtab <= symtab || strtab + strsz > image.size()) {
        return std::nullopt;
    }

    // .dynsym directly precedes .dynstr in Nintendo SDK modules.
    for (u64 sym = symtab; sym + 24 <= strtab; sym += 24) {
        const auto name_offset = ReadAt<u32>(image, sym);
        const auto value = ReadAt<u64>(image, sym + 8);
        const auto size = ReadAt<u64>(image, sym + 16);
        if (!name_offset || !value || !size || *name_offset >= strsz || *value == 0) {
            continue;
        }
        const char* name = reinterpret_cast<const char*>(image.data() + strtab + *name_offset);
        const std::size_t max_len = strsz - *name_offset;
        if (strnlen(name, max_len) == wanted.size() &&
            std::memcmp(name, wanted.data(), wanted.size()) == 0) {
            return Symbol{*value, *size};
        }
    }
    return std::nullopt;
}

bool Matches(std::span<const u8> image, u64 offset, std::span<const u32> code) {
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (ReadAt<u32>(image, offset + i * 4) != code[i]) {
            return false;
        }
    }
    return true;
}

void Write(std::span<u8> image, u64 offset, std::span<const u32> code) {
    std::memcpy(image.data() + offset, code.data(), code.size_bytes());
}

// Tail of nvnBootstrapLoader as shipped in the SDK used by Made in Abyss (instructions 12-21):
//   cbz x0, fail; ldr x2, [x0]; mov x1, x19; mov x0, xzr; ldp x20, x19, [sp, #0x10];
//   ldp x29, x30, [sp], #0x20; br x2; fail: ldp x20, x19, [sp, #0x10]; ldp ...; ret
constexpr std::array<u32, 10> BootstrapTail{
    0xB40000E0, 0xF9400002, 0xAA1303E1, 0xAA1F03E0, 0xA9414FF4,
    0xA8C27BFD, 0xD61F0040, 0xA9414FF4, 0xA8C27BFD, 0xD65F03C0,
};

// Same tail, but the driver lookup is called instead of tail-called and its result is reported.
constexpr std::array<u32, 10> HookedBootstrapTail{
    0xB40000E0, // cbz  x0, fail
    0xF9400002, // ldr  x2, [x0]
    0xAA1303E1, // mov  x1, x19
    0xAA1F03E0, // mov  x0, xzr
    0xD63F0040, // blr  x2
    0xAA1303E1, // mov  x1, x19
    0xD4014001, // svc  #0xA00
    0xA9414FF4, // fail: ldp x20, x19, [sp, #0x10]
    0xA8C27BFD, // ldp  x29, x30, [sp], #0x20
    0xD65F03C0, // ret
};

// nvnDeviceGetProcAddress(device, name) wrapper placed over nvnBootstrapLoaderInternal.
constexpr std::array<u32, 11> ProcAddressWrapper{
    0xA9BE7BFD, // stp  x29, x30, [sp, #-0x20]!
    0xA9014FF4, // stp  x20, x19, [sp, #0x10]
    0x910003FD, // mov  x29, sp
    0xAA0103F3, // mov  x19, x1
    0xD4014021, // svc  #0xA01      (x2 = real GetProcAddress)
    0xD63F0040, // blr  x2
    0xAA1303E1, // mov  x1, x19
    0xD4014041, // svc  #0xA02
    0xA9414FF4, // ldp  x20, x19, [sp, #0x10]
    0xA8C27BFD, // ldp  x29, x30, [sp], #0x20
    0xD65F03C0, // ret
};

constexpr std::size_t TrampolineWords = 12;

// Trampoline for traced procedure `index` living at `address`, calling the real one at `target`:
//   stp x29, x30, [sp, #-0x30]!; mov x29, sp; stp x0, x1, [sp, #0x10]; stp x2, x3, [sp, #0x20]
//   svc #(0xC00 + index); bl target; ldp x1, x2, [sp, #0x10]; ldr x3, [sp, #0x20]
//   svc #(0xE00 + index); ldp x29, x30, [sp], #0x30; ret; nop
// NVN procedures take at most eight integer arguments, so the extra frame never hides stack
// arguments, and SVCs preserve every register except the ones the host explicitly changes.
std::array<u32, TrampolineWords> MakeTrampoline(u32 index, u64 address, u64 target) {
    const s64 bl_delta = (static_cast<s64>(target) - static_cast<s64>(address + 5 * 4)) / 4;
    return {
        0xA9BD7BFD,
        0x910003FD,
        0xA90107E0,
        0xA9020FE2,
        0xD4000001 | ((SvcTraceEnterBase + index) << 5),
        0x94000000 | (static_cast<u32>(bl_delta) & 0x03FFFFFF),
        0xA9410BE1,
        0xF94013E3,
        0xD4000001 | ((SvcTraceExitBase + index) << 5),
        0xA8C37BFD,
        0xD65F03C0,
        0xD503201F,
    };
}

Mode RequestedMode() {
    const auto& config = Common::FS::GetYuzuPath(Common::FS::YuzuPath::ConfigDir);
    if (Common::FS::Exists(config / "nvn_hle")) {
        return Mode::Hle;
    }
    if (Common::FS::Exists(config / "nvn_trace")) {
        return Mode::Trace;
    }
    return Mode::Off;
}

// HLE stubs follow the trampolines inside the code cave: `svc #(SvcHleBase + slot); ret`.
constexpr u64 HleStubOffset = MadeInAbyss::UsedProcs.size() * TrampolineWords * sizeof(u32);
constexpr u64 HleStubCount =
    std::min<u64>((MadeInAbyss::CodeCaveSize - HleStubOffset) / 8, MaxHleSlots);
static_assert(HleStubOffset % 16 == 0 && HleStubCount >= 0x100);

std::optional<std::size_t> FindTracedProc(std::string_view name) {
    const auto& procs = MadeInAbyss::UsedProcs;
    for (std::size_t i = 0; i < procs.size(); ++i) {
        if (procs[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

// Writes one trampoline per traced procedure into the unused code cave of the known sdk build.
bool InstallTrampolines(std::span<u8> image, std::span<const u8> build_id, u64 module_address,
                        bool with_hle_stubs, u64& out_trampolines) {
    using namespace MadeInAbyss;
    if (build_id.size() < SdkBuildId.size() ||
        !std::equal(SdkBuildId.begin(), SdkBuildId.end(), build_id.begin())) {
        return false;
    }
    const auto cave = FindSymbol(image, "regcomp");
    const std::size_t needed = UsedProcs.size() * TrampolineWords * sizeof(u32);
    if (!cave || cave->offset != CodeCaveOffset || cave->size != CodeCaveSize ||
        needed > CodeCaveSize || UsedProcs.size() > MaxTracedProcs) {
        return false;
    }
    for (std::size_t i = 0; i < UsedProcs.size(); ++i) {
        const u64 offset = CodeCaveOffset + i * TrampolineWords * sizeof(u32);
        const auto code = MakeTrampoline(static_cast<u32>(i), offset, UsedProcs[i].sdk_offset);
        Write(image, offset, code);
    }
    if (with_hle_stubs) {
        for (u32 slot = 0; slot < HleStubCount; ++slot) {
            const std::array<u32, 2> stub{0xD4000001 | ((SvcHleBase + slot) << 5), 0xD65F03C0};
            Write(image, CodeCaveOffset + HleStubOffset + slot * 8, stub);
        }
    }
    out_trampolines = module_address + CodeCaveOffset;
    return true;
}

void Report(HookState& state, std::string_view source, const std::string& name, u64 proc) {
    if (!state.reported.insert(name).second) {
        return;
    }
    if (proc >= state.sdk_base && proc < state.sdk_end) {
        LOG_INFO(Service_NVDRV, "NVN {} {} = sdk+0x{:X}", source, name, proc - state.sdk_base);
    } else {
        LOG_INFO(Service_NVDRV, "NVN {} {} = 0x{:X}", source, name, proc);
    }
}

} // Anonymous namespace

void PatchModule(std::span<u8> module_image, std::string_view module_name,
                 std::span<const u8> build_id, u64 module_address, bool is_final_load) {
    if (module_name != "sdk") {
        return;
    }
    const auto bootstrap = FindSymbol(module_image, "nvnBootstrapLoader");
    const auto internal = FindSymbol(module_image, "nvnBootstrapLoaderInternal");
    const auto devtools = FindSymbol(module_image, "nvnDevtoolsBootstrap");
    if (!bootstrap || !internal || !devtools) {
        return;
    }

    const u64 tail_offset = bootstrap->offset + 12 * 4;
    const u64 wrapper_space = devtools->offset + devtools->size - internal->offset;
    const bool already_hooked = Matches(module_image, tail_offset, HookedBootstrapTail) &&
                                Matches(module_image, internal->offset, ProcAddressWrapper);
    if (already_hooked) {
        // Nothing to rewrite, only (re)register the module below.
    } else if (bootstrap->size != 0x58 || !Matches(module_image, tail_offset, BootstrapTail) ||
        internal->offset < bootstrap->offset + bootstrap->size ||
        devtools->offset < internal->offset ||
        internal->offset + ProcAddressWrapper.size() * sizeof(u32) > module_image.size() ||
        wrapper_space < ProcAddressWrapper.size() * sizeof(u32)) {
        if (is_final_load) {
            LOG_WARNING(Service_NVDRV, "NVN hook: unknown nvnBootstrapLoader layout, not hooked");
        }
        return;
    } else {
        Write(module_image, tail_offset, HookedBootstrapTail);
        Write(module_image, internal->offset, ProcAddressWrapper);
    }

    // Trace mode changes code, so it must be decided identically for both loader passes.
    u64 trampolines = 0;
    const Mode requested = RequestedMode();
    const bool installed =
        requested != Mode::Off &&
        InstallTrampolines(module_image, build_id, module_address, requested == Mode::Hle,
                           trampolines);
    const Mode mode = installed ? requested : Mode::Off;

    if (is_final_load) {
        auto& state = GetState();
        std::scoped_lock lk{state.mutex};
        state.mode = mode;
        state.trampolines = trampolines;
        state.hle_stubs = trampolines + HleStubOffset;
        state.hle_slots.clear();
        Hle::Reset();
        state.calls = {};
        state.calls_since_dump = {};
        state.logged_integers.clear();
        state.presents = 0;
        if (mode == Mode::Hle) {
            LOG_INFO(Service_NVDRV, "NVN HLE enabled ({} stub slots)", HleStubCount);
        } else if (mode == Mode::Trace) {
            LOG_INFO(Service_NVDRV, "NVN trace enabled for {} procedures",
                     MadeInAbyss::UsedProcs.size());
        } else if (requested != Mode::Off) {
            LOG_WARNING(Service_NVDRV, "NVN trace/HLE requested but this sdk build is unknown");
        }
        state.sdk_base = module_address;
        state.sdk_end = module_address + module_image.size();
        state.proc_wrapper = module_address + internal->offset;
        state.real_get_proc_address = 0;
        state.reported.clear();
        LOG_INFO(Service_NVDRV, "NVN hook installed: nvnBootstrapLoader at sdk+0x{:X}",
                 bootstrap->offset);
    }
}

namespace {

u64 TrampolineFor(HookState& state, std::string_view name, u64 real) {
    if (state.mode == Mode::Off || real == 0) {
        return real;
    }
    if (state.mode == Mode::Hle && Hle::IsReplaced(name)) {
        auto slot = std::find(state.hle_slots.begin(), state.hle_slots.end(), name);
        if (slot == state.hle_slots.end()) {
            if (state.hle_slots.size() >= HleStubCount) {
                LOG_ERROR(Service_NVDRV, "NVN HLE: out of stub slots for {}", name);
                return real;
            }
            slot = state.hle_slots.emplace(state.hle_slots.end(), name);
        }
        return state.hle_stubs + static_cast<u64>(slot - state.hle_slots.begin()) * 8;
    }
    const auto index = FindTracedProc(name);
    if (!index || (state.mode == Mode::Hle && !Hle::IsObserved(name))) {
        return real;
    }
    const u64 expected = state.sdk_base + MadeInAbyss::UsedProcs[*index].sdk_offset;
    if (expected != real) {
        LOG_WARNING(Service_NVDRV, "NVN trace: {} resolved to 0x{:X}, expected 0x{:X}", name,
                    real, expected);
        return real;
    }
    return state.trampolines + *index * TrampolineWords * sizeof(u32);
}

void DumpCallCounts(HookState& state) {
    std::vector<std::pair<u64, std::size_t>> counts;
    for (std::size_t i = 0; i < MadeInAbyss::UsedProcs.size(); ++i) {
        if (state.calls_since_dump[i] != 0) {
            counts.emplace_back(state.calls_since_dump[i], i);
        }
    }
    std::sort(counts.begin(), counts.end(), std::greater<>());
    LOG_INFO(Service_NVDRV, "NVN trace: {} procedures called during the last 300 presents",
             counts.size());
    // All of them: this is what sizes the command-buffer translation (C2).
    for (std::size_t i = 0; i < counts.size(); ++i) {
        LOG_INFO(Service_NVDRV, "NVN trace:   {:>9} {}", counts[i].first,
                 MadeInAbyss::UsedProcs[counts[i].second].name);
    }
    state.calls_since_dump = {};
}

void HandleTrace(Core::System& system, HookState& state, u32 imm, std::span<u64, 8> args) {
    const bool is_enter = imm < SvcTraceExitBase;
    const std::size_t index = imm - (is_enter ? SvcTraceEnterBase : SvcTraceExitBase);
    if (index >= MadeInAbyss::UsedProcs.size()) {
        return;
    }
    const std::string_view name = MadeInAbyss::UsedProcs[index].name;

    if (state.mode == Mode::Hle) {
        if (is_enter) {
            Hle::OnEnter(system, name, args);
        }
        return;
    }

    if (is_enter) {
        const u64 count = ++state.calls[index];
        ++state.calls_since_dump[index];
        if (count <= 4) {
            LOG_INFO(Service_NVDRV, "NVN> {}(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}) #{}",
                     name, args[0], args[1], args[2], args[3], args[4], args[5], count);
        }
        if (name == "nvnQueuePresentTexture" && ++state.presents % 300 == 0) {
            DumpCallCounts(state);
        }
        return;
    }

    // Exit: x0 = result, x1/x2/x3 = x0/x1/x2 at entry.
    if (name == "nvnDeviceGetInteger") {
        if (state.logged_integers.insert(args[2]).second) {
            auto& memory = Kernel::GetCurrentProcess(system.Kernel()).GetMemory();
            LOG_INFO(Service_NVDRV, "NVN deviceInfo[0x{:X}] = 0x{:X}", args[2],
                     memory.Read32(args[3]));
        }
        return;
    }
    if (state.calls[index] <= 4) {
        LOG_INFO(Service_NVDRV, "NVN< {} = 0x{:X}", name, args[0]);
    }
}

} // Anonymous namespace

void HandleSvc(Core::System& system, u32 imm, std::span<u64, 8> args) {
    auto& state = GetState();

    if (imm >= SvcHleBase) {
        std::string name;
        {
            std::scoped_lock lk{state.mutex};
            const std::size_t slot = imm - SvcHleBase;
            if (slot >= state.hle_slots.size()) {
                return;
            }
            name = state.hle_slots[slot];
        }
        // Not under the hook lock: presenting may wait for the GPU thread.
        Hle::Call(system, name, args);
        return;
    }

    std::scoped_lock lk{state.mutex};

    if (imm >= SvcTraceEnterBase) {
        HandleTrace(system, state, imm, args);
        return;
    }

    if (imm == SvcGetRealProcAddress) {
        args[2] = state.real_get_proc_address;
        return;
    }

    auto& memory = Kernel::GetCurrentProcess(system.Kernel()).GetMemory();
    const std::string name = memory.ReadCString(args[1], 128);

    if (imm == SvcBootstrapResult) {
        Report(state, "bootstrap", name, args[0]);
        if (name == "nvnDeviceGetProcAddress" && args[0] != 0) {
            state.real_get_proc_address = args[0];
            args[0] = state.proc_wrapper;
        } else {
            args[0] = TrampolineFor(state, name, args[0]);
        }
        return;
    }

    if (imm == SvcProcAddressResult) {
        Report(state, "proc", name, args[0]);
        args[0] = TrampolineFor(state, name, args[0]);
    }
}

} // namespace Core::NvnHook
