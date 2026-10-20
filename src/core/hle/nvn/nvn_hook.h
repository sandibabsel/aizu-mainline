// SPDX-FileCopyrightText: 2026 aizu-mainline contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string_view>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Core::NvnHook {

// Custom supervisor call numbers used by the guest code written into the "sdk" module.
// Horizon only uses numbers below 0x100, so these never collide with real SVCs.
constexpr u32 SvcBootstrapResult = 0xA00;    ///< x0 = proc returned by nvnBootstrapLoader, x1 = name
constexpr u32 SvcGetRealProcAddress = 0xA01; ///< Returns the real nvnDeviceGetProcAddress in x2
constexpr u32 SvcProcAddressResult = 0xA02;  ///< x0 = proc returned by GetProcAddress, x1 = name

// Trace mode (enabled by creating an empty file named "nvn_trace" in the config directory):
// every NVN procedure the game uses is routed through a trampoline that reports entry and exit.
constexpr u32 SvcTraceEnterBase = 0xC00; ///< + procedure index, x0-x7 = arguments
constexpr u32 SvcTraceExitBase = 0xE00;  ///< + procedure index, x0 = result, x1-x3 = x0-x2 at entry
constexpr u32 MaxTracedProcs = 0x200;

// HLE mode (empty "nvn_hle" file in the config directory, see nvn_hle.h): replaced procedures are
// routed to `svc #(SvcHleBase + slot); ret` stubs.
constexpr u32 SvcHleBase = 0x1000;
constexpr u32 MaxHleSlots = 0x400;

constexpr bool IsHookSvc(u32 imm) {
    return (imm >= SvcBootstrapResult && imm <= SvcProcAddressResult) ||
           (imm >= SvcTraceEnterBase && imm < SvcTraceEnterBase + MaxTracedProcs) ||
           (imm >= SvcTraceExitBase && imm < SvcTraceExitBase + MaxTracedProcs) ||
           (imm >= SvcHleBase && imm < SvcHleBase + MaxHleSlots);
}

/// Rewrites nvnBootstrapLoader (and the unused nvnBootstrapLoaderInternal/nvnDevtoolsBootstrap
/// that follow it) inside the "sdk" module so every NVN procedure lookup is reported to the host.
/// The game still receives the real NVN driver functions, so rendering is unchanged.
/// For known sdk builds trace mode can additionally be enabled, see SvcTraceEnterBase.
/// @param module_image Decompressed module image, starting at the module's .text.
/// @param module_address Guest address the module will be mapped at.
/// @param is_final_load False while only the process layout is being computed.
void PatchModule(std::span<u8> module_image, std::string_view module_name,
                 std::span<const u8> build_id, u64 module_address, bool is_final_load);

/// Handles one of the custom SVCs above. Arguments are x0-x7 of the calling thread.
void HandleSvc(Core::System& system, u32 imm, std::span<u64, 8> args);

} // namespace Core::NvnHook
