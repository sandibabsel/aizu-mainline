// SPDX-FileCopyrightText: 2026 aizu contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// RAM / CPU / GPU usage for the in-game resource overlay. Everything is read from procfs and
// sysfs on each call (about once a second), so nothing here runs while the overlay is off.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <jni.h>
#include <unistd.h>

#include <fmt/format.h>

#include "common/android/android_common.h"
#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace {

using Clock = std::chrono::steady_clock;

std::optional<std::string> ReadSmallFile(const char* path) {
    FILE* const file = std::fopen(path, "re");
    if (!file) {
        return std::nullopt;
    }
    std::array<char, 4096> buffer{};
    const size_t read = std::fread(buffer.data(), 1, buffer.size() - 1, file);
    std::fclose(file);
    if (read == 0) {
        return std::nullopt;
    }
    return std::string(buffer.data(), read);
}

/// Value in kB of a "Key:   1234 kB" line in /proc/meminfo or /proc/self/status.
u64 FindKb(const std::string& text, const char* key) {
    const size_t pos = text.find(key);
    if (pos == std::string::npos) {
        return 0;
    }
    return std::strtoull(text.c_str() + pos + std::strlen(key), nullptr, 10);
}

struct StatLine {
    std::string name;
    u64 ticks; ///< utime + stime
};

/// Parses /proc/.../stat: "pid (comm) state ppid ... utime(14) stime(15) ...".
std::optional<StatLine> ReadStat(const char* path) {
    const auto text = ReadSmallFile(path);
    if (!text) {
        return std::nullopt;
    }
    const size_t open = text->find('(');
    const size_t close = text->rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        return std::nullopt;
    }
    StatLine line{text->substr(open + 1, close - open - 1), 0};
    // After ')' come fields 3.. ; utime and stime are fields 14 and 15.
    const char* p = text->c_str() + close + 1;
    for (int field = 3; field <= 15 && *p; ++field) {
        while (*p == ' ') {
            ++p;
        }
        if (field == 14 || field == 15) {
            line.ticks += std::strtoull(p, nullptr, 10);
        }
        while (*p && *p != ' ') {
            ++p;
        }
    }
    return line;
}

std::string Mib(u64 bytes) {
    return fmt::format("{}", bytes >> 20);
}

class ResourceSampler {
public:
    std::string Sample() {
        const auto now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - last_time).count();
        const bool have_delta = has_sample && seconds > 0.2;

        std::string out = RamLine();
        out += '\n';
        out += CpuLines(have_delta ? seconds : 0.0);
        out += '\n';
        out += GpuLine();

        last_time = now;
        has_sample = true;
        return out;
    }

private:
    std::string RamLine() {
        const auto status = ReadSmallFile("/proc/self/status");
        const auto meminfo = ReadSmallFile("/proc/meminfo");
        const u64 rss = status ? FindKb(*status, "VmRSS:") : 0;
        const u64 swapped = status ? FindKb(*status, "VmSwap:") : 0;
        const u64 total = meminfo ? FindKb(*meminfo, "MemTotal:") : 0;
        const u64 avail = meminfo ? FindKb(*meminfo, "MemAvailable:") : 0;
        const u64 swap_total = meminfo ? FindKb(*meminfo, "SwapTotal:") : 0;
        const u64 swap_free = meminfo ? FindKb(*meminfo, "SwapFree:") : 0;
        std::string line = fmt::format("RAM {} MB", rss >> 10);
        if (swapped != 0) {
            line += fmt::format(" +swap {}", swapped >> 10);
        }
        line += fmt::format(" | free {}/{} MB", avail >> 10, total >> 10);
        if (swap_total != 0) {
            line += fmt::format(" swap {}/{}", swap_free >> 10, swap_total >> 10);
        }
        return line;
    }

    std::string CpuLines(double seconds) {
        const long hz = sysconf(_SC_CLK_TCK);
        const long cores = std::max(1L, sysconf(_SC_NPROCESSORS_CONF));

        std::string line = "CPU";
        if (const auto self = ReadStat("/proc/self/stat")) {
            if (seconds > 0.0 && self->ticks >= process_ticks) {
                const double used_cores =
                    static_cast<double>(self->ticks - process_ticks) / hz / seconds;
                line += fmt::format(" {:.0f}% ({:.1f}/{} core)", used_cores * 100.0 / cores,
                                    used_cores, cores);
            } else {
                line += " --";
            }
            process_ticks = self->ticks;
        }
        line += CpuFreqs();

        // Busiest threads (percent of one core): shows whether the guest CPU threads or the GPU
        // thread is the bottleneck.
        std::unordered_map<int, u64> ticks_now;
        std::vector<std::pair<double, std::string>> busy;
        if (DIR* const dir = opendir("/proc/self/task")) {
            while (const dirent* const entry = readdir(dir)) {
                const int tid = std::atoi(entry->d_name);
                if (tid <= 0) {
                    continue;
                }
                const auto path = fmt::format("/proc/self/task/{}/stat", tid);
                const auto stat = ReadStat(path.c_str());
                if (!stat) {
                    continue;
                }
                ticks_now[tid] = stat->ticks;
                const auto prev = thread_ticks.find(tid);
                if (seconds > 0.0 && prev != thread_ticks.end() && stat->ticks >= prev->second) {
                    const double percent =
                        static_cast<double>(stat->ticks - prev->second) * 100.0 / hz / seconds;
                    if (percent >= 1.0) {
                        busy.emplace_back(percent, stat->name);
                    }
                }
            }
            closedir(dir);
        }
        thread_ticks = std::move(ticks_now);

        std::sort(busy.begin(), busy.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string threads;
        for (size_t i = 0; i < std::min<size_t>(busy.size(), 4); ++i) {
            threads += fmt::format("{}{} {:.0f}%", i == 0 ? "  " : " · ", busy[i].second,
                                   busy[i].first);
        }
        if (!threads.empty()) {
            line += '\n';
            line += threads;
        }
        return line;
    }

    /// Current frequency of each cpufreq policy (cluster), e.g. " 1.80/2.00 GHz".
    static std::string CpuFreqs() {
        std::vector<double> ghz;
        for (int policy = 0; policy < 16; ++policy) {
            const auto path =
                fmt::format("/sys/devices/system/cpu/cpufreq/policy{}/scaling_cur_freq", policy);
            if (const auto text = ReadSmallFile(path.c_str())) {
                ghz.push_back(std::strtod(text->c_str(), nullptr) / 1e6); // kHz -> GHz
            }
        }
        if (ghz.empty()) {
            return {};
        }
        std::string out = " ";
        for (size_t i = 0; i < ghz.size(); ++i) {
            out += fmt::format("{}{:.2f}", i == 0 ? "" : "/", ghz[i]);
        }
        return out + " GHz";
    }

    std::string GpuLine() {
        std::string line = "GPU";
        if (const auto load = GpuLoad()) {
            line += fmt::format(" {}%", *load);
        } else {
            line += " load n/a";
        }
        if (const auto mhz = GpuFreqMhz()) {
            line += fmt::format(" {} MHz", *mhz);
        }
        u64 allocated = 0;
        u64 budget = 0;
        if (Vulkan::GetGpuMemoryStats(allocated, budget)) {
            line += fmt::format(" | VRAM {}/{} MB", Mib(allocated), Mib(budget));
        }
        return line;
    }

    /// GPU utilization in percent. Vendors expose it in different places and SELinux hides most
    /// of them from apps on retail firmware; the first readable source is remembered.
    std::optional<int> GpuLoad() {
        static constexpr std::array<const char*, 7> candidates{
            "/sys/kernel/ged/hal/gpu_utilization",               // MediaTek GED: "load block idle"
            "/sys/module/ged/parameters/gpu_loading",            // MediaTek
            "/sys/class/misc/mali0/device/utilization",          // Arm kbase (some vendors)
            "/sys/class/misc/mali0/device/gpu_utilization",      //
            "/sys/kernel/gpu/gpu_busy",                          // Samsung: "45 %"
            "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",      // Adreno: "45 %"
            "/sys/class/devfreq/gpufreq/load",                   // devfreq: "45@850000000Hz"
        };
        if (load_source < 0) {
            for (size_t i = 0; i < candidates.size(); ++i) {
                if (ParseLeadingInt(candidates[i])) {
                    load_source = static_cast<int>(i);
                    break;
                }
            }
            if (load_source < 0) {
                load_source = static_cast<int>(candidates.size()); // none readable
            }
        }
        if (load_source >= static_cast<int>(candidates.size())) {
            return std::nullopt;
        }
        const auto value = ParseLeadingInt(candidates[load_source]);
        if (!value || *value < 0 || *value > 100) {
            return std::nullopt;
        }
        return value;
    }

    static std::optional<int> GpuFreqMhz() {
        // MediaTek GED (the kernel spells it "freqency"): "<opp index> <kHz>".
        if (const auto text = ReadSmallFile("/sys/kernel/ged/hal/current_freqency")) {
            char* end = nullptr;
            std::strtoull(text->c_str(), &end, 10);
            const u64 khz = std::strtoull(end, nullptr, 10);
            if (khz != 0) {
                return static_cast<int>(khz / 1000);
            }
        }
        // Generic devfreq node of the Mali device, in Hz.
        if (const auto hz = ParseLeadingU64("/sys/class/misc/mali0/device/devfreq/cur_freq")) {
            return static_cast<int>(*hz / 1'000'000);
        }
        return std::nullopt;
    }

    static std::optional<int> ParseLeadingInt(const char* path) {
        const auto value = ParseLeadingU64(path);
        if (!value) {
            return std::nullopt;
        }
        return static_cast<int>(*value);
    }

    static std::optional<u64> ParseLeadingU64(const char* path) {
        const auto text = ReadSmallFile(path);
        if (!text) {
            return std::nullopt;
        }
        char* end = nullptr;
        const u64 value = std::strtoull(text->c_str(), &end, 10);
        if (end == text->c_str()) {
            return std::nullopt;
        }
        return value;
    }

    Clock::time_point last_time{};
    bool has_sample = false;
    u64 process_ticks = 0;
    std::unordered_map<int, u64> thread_ticks;
    int load_source = -1;
};

ResourceSampler g_sampler;

} // Anonymous namespace

extern "C" {

jstring Java_org_yuzu_yuzu_1emu_NativeLibrary_getResourceStats(JNIEnv* env, jclass clazz) {
    return Common::Android::ToJString(env, g_sampler.Sample());
}

} // extern "C"
