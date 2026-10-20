// SPDX-FileCopyrightText: 2026 aizu-mainline contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string_view>

#include "common/common_types.h"

namespace Core {
class System;
}

// Experimental NVN HLE (enabled by an empty "nvn_hle" file in the config directory).
//
// Object creation stays in the real NVN driver: devices, memory pools, textures, samplers,
// programs and windows are created exactly like on hardware, so every size, alignment and address
// the game sees is genuine. Only the parts that would make the emulated GPU do work are replaced:
// command buffer recording, queue submission, syncs and window presentation. The host watches the
// creation calls to learn where textures live, executes clears on the CPU and hands the window
// textures straight to the compositor. Draws are ignored for now.
namespace Core::NvnHook::Hle {

/// The procedure is fully implemented on the host.
bool IsReplaced(std::string_view name);

/// The procedure runs in the real driver, but the host needs to see its arguments.
bool IsObserved(std::string_view name);

/// Called when an observed procedure is entered, before the real driver runs it.
void OnEnter(Core::System& system, std::string_view name, std::span<u64, 8> args);

/// Runs a replaced procedure. args holds x0-x7 on entry; args[0] receives the result.
void Call(Core::System& system, std::string_view name, std::span<u64, 8> args);

/// Forgets every tracked object (a new process is being loaded).
void Reset();

} // namespace Core::NvnHook::Hle
