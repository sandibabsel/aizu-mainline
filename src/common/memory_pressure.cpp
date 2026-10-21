// SPDX-FileCopyrightText: 2026 aizu contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/memory_pressure.h"

namespace Common {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto POLL_INTERVAL = std::chrono::seconds{1};
constexpr auto REPORT_HOLD = std::chrono::seconds{10};

std::atomic<int> polled_level{0};
std::atomic<Clock::rep> next_poll{0};
std::atomic<int> reported_level{0};
std::atomic<Clock::rep> reported_until{0};

/// Level from /proc/meminfo: MemAvailable as a share of the RAM, stricter once swap (which
/// Android leans on heavily) is nearly used up; lmkd then starts killing the foreground app.
int PollMeminfo() {
#if defined(__linux__) || defined(__ANDROID__)
    FILE* const file = std::fopen("/proc/meminfo", "re");
    if (!file) {
        return 0;
    }
    char line[128];
    unsigned long long total = 0, available = 0, swap_total = 0, swap_free = 0;
    while (std::fgets(line, sizeof(line), file)) {
        const auto read = [&](const char* key, unsigned long long& value) {
            const size_t length = std::strlen(key);
            if (std::strncmp(line, key, length) == 0) {
                value = std::strtoull(line + length, nullptr, 10);
            }
        };
        read("MemTotal:", total);
        read("MemAvailable:", available);
        read("SwapTotal:", swap_total);
        read("SwapFree:", swap_free);
    }
    std::fclose(file);
    if (total == 0) {
        return 0;
    }
    const double available_share = static_cast<double>(available) / static_cast<double>(total);
    const bool swap_low =
        swap_total == 0 || static_cast<double>(swap_free) < static_cast<double>(swap_total) * 0.15;
    if (available_share < 0.08 || (available_share < 0.12 && swap_low)) {
        return 2;
    }
    if (available_share < 0.14 || (available_share < 0.18 && swap_low)) {
        return 1;
    }
#endif
    return 0;
}

} // Anonymous namespace

int GetMemoryPressure() {
    const auto now = Clock::now().time_since_epoch().count();
    auto due = next_poll.load(std::memory_order_relaxed);
    if (now >= due &&
        next_poll.compare_exchange_strong(
            due, now + std::chrono::duration_cast<Clock::duration>(POLL_INTERVAL).count())) {
        polled_level.store(PollMeminfo(), std::memory_order_relaxed);
    }
    int level = polled_level.load(std::memory_order_relaxed);
    if (now < reported_until.load(std::memory_order_relaxed)) {
        level = std::max(level, reported_level.load(std::memory_order_relaxed));
    }
    return level;
}

void ReportMemoryPressure(int level) {
    const auto now = Clock::now();
    reported_level.store(std::clamp(level, 0, 2), std::memory_order_relaxed);
    reported_until.store((now + REPORT_HOLD).time_since_epoch().count(),
                         std::memory_order_relaxed);
}

} // namespace Common
