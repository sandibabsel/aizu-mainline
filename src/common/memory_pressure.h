// SPDX-FileCopyrightText: 2026 aizu contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Common {

/// How close the host is to running out of RAM: 0 = fine, 1 = low, 2 = critical.
/// On phones the GPU caches live in the same RAM as everything else, so the texture and buffer
/// caches shrink themselves while this is raised instead of waiting for the low memory killer.
int GetMemoryPressure();

/// Raise the level for the next few seconds (Android onTrimMemory).
void ReportMemoryPressure(int level);

} // namespace Common
