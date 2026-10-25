// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <source_location>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/logging/log.h"

namespace Vulkan::StallProbe {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SUYU_STALL_PROBE");
#ifdef __ANDROID__
        // Dark Switch: on by default on Android so slow frames are always explained in the log.
        // Set SUYU_STALL_PROBE=0 to disable.
        return !(value && *value == '0');
#else
        return value && *value && *value != '0';
#endif
    }();
    return enabled;
}

inline u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline std::atomic<u64> build_wait_ns{0}, build_wait_count{0};
inline std::atomic<u64> acquire_ns{0}, present_ns{0}, frame_wait_ns{0};
inline std::atomic<u64> gpu_idle_ns{0}, draw_ns{0}, draw_count{0}, sched_wait_ns{0};
inline std::atomic<u64> shader_translate_ns{0}, shader_emit_ns{0}, shader_count{0};
inline std::atomic<u64> pipeline_build_ns{0}, pipeline_build_count{0};
inline std::atomic<u64> gpu_wait_ns{0}, gpu_wait_count{0};
// Breakdown of GraphicsPipeline::ConfigureImpl (all inside "configure").
inline std::atomic<u64> cfg_sync_ns{0}, cfg_stage_ns{0}, cfg_fill_ns{0}, cfg_bind_ns{0},
    cfg_buffers_ns{0}, cfg_push_ns{0}, cfg_rt_ns{0};

// Time spent blocked in GPU waits, attributed to the calling source line.
struct SiteStat {
    u64 ns = 0;
    u64 count = 0;
};
inline std::mutex site_mutex;
inline std::unordered_map<std::string, SiteStat>& Sites() {
    static std::unordered_map<std::string, SiteStat> sites;
    return sites;
}

class SiteScope {
public:
    explicit SiteScope(const std::source_location& loc)
        : loc_{loc}, enabled_{Enabled()}, start_{enabled_ ? Now() : 0} {}
    ~SiteScope() {
        if (!enabled_) return;
        const u64 elapsed = Now() - start_;
        const char* file = loc_.file_name();
        const char* slash = std::strrchr(file, '/');
        std::string key = std::string(slash ? slash + 1 : file) + ":" + std::to_string(loc_.line());
        std::scoped_lock lock{site_mutex};
        auto& stat = Sites()[key];
        stat.ns += elapsed;
        stat.count += 1;
    }
    SiteScope(const SiteScope&) = delete;
    SiteScope& operator=(const SiteScope&) = delete;
private:
    std::source_location loc_;
    bool enabled_;
    u64 start_;
};

// Adds the time since the previous Mark() (or construction) to the given sink.
class Lap {
public:
    Lap() : enabled_{Enabled()}, last_{enabled_ ? Now() : 0} {}
    void Mark(std::atomic<u64>& sink) {
        if (!enabled_) return;
        const u64 now = Now();
        sink.fetch_add(now - last_, std::memory_order_relaxed);
        last_ = now;
    }
private:
    bool enabled_;
    u64 last_;
};
inline std::atomic<u64> draw_flush_ns{0}, pipeline_lookup_ns{0}, pipeline_configure_ns{0},
    draw_record_ns{0};

// The disabled path performs neither clock reads nor atomic counter updates.
// Counts and durations are published together on scope completion. These are
// process-wide sums from multiple threads; they can overlap and a scope can
// straddle a reporting interval. They are NOT additive frame wall-time buckets.
class Accum {
public:
    explicit Accum(std::atomic<u64>& sink, std::atomic<u64>* count = nullptr)
        : sink_{sink}, count_{count}, enabled_{Enabled()}, start_{enabled_ ? Now() : 0} {}
    ~Accum() {
        if (!enabled_) return;
        sink_.fetch_add(Now() - start_, std::memory_order_relaxed);
        if (count_) count_->fetch_add(1, std::memory_order_relaxed);
    }
    Accum(const Accum&) = delete;
    Accum& operator=(const Accum&) = delete;
private:
    std::atomic<u64>& sink_;
    std::atomic<u64>* count_;
    bool enabled_;
    u64 start_;
};

inline void ReportFrame() {
    if (!Enabled()) return;
    // Present normally has one caller; TLS also avoids a plain-data race if
    // separate presentation threads report concurrently.
    static thread_local u64 last_present = 0;
    const u64 now = Now();
    const u64 build = build_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 builds = build_wait_count.exchange(0, std::memory_order_relaxed);
    const u64 acquire = acquire_ns.exchange(0, std::memory_order_relaxed);
    const u64 present = present_ns.exchange(0, std::memory_order_relaxed);
    const u64 framew = frame_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 idle = gpu_idle_ns.exchange(0, std::memory_order_relaxed);
    const u64 draw = draw_ns.exchange(0, std::memory_order_relaxed);
    const u64 draws = draw_count.exchange(0, std::memory_order_relaxed);
    const u64 schedw = sched_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 translate = shader_translate_ns.exchange(0, std::memory_order_relaxed);
    const u64 emit = shader_emit_ns.exchange(0, std::memory_order_relaxed);
    const u64 shaders = shader_count.exchange(0, std::memory_order_relaxed);
    const u64 pipeline_build = pipeline_build_ns.exchange(0, std::memory_order_relaxed);
    const u64 pipeline_builds = pipeline_build_count.exchange(0, std::memory_order_relaxed);
    const u64 flush = draw_flush_ns.exchange(0, std::memory_order_relaxed);
    const u64 lookup = pipeline_lookup_ns.exchange(0, std::memory_order_relaxed);
    const u64 configure = pipeline_configure_ns.exchange(0, std::memory_order_relaxed);
    const u64 record = draw_record_ns.exchange(0, std::memory_order_relaxed);
    const u64 c_sync = cfg_sync_ns.exchange(0, std::memory_order_relaxed);
    const u64 c_stage = cfg_stage_ns.exchange(0, std::memory_order_relaxed);
    const u64 c_fill = cfg_fill_ns.exchange(0, std::memory_order_relaxed);
    const u64 c_bind = cfg_bind_ns.exchange(0, std::memory_order_relaxed);
    const u64 c_buffers = cfg_buffers_ns.exchange(0, std::memory_order_relaxed);
    const u64 c_push = cfg_push_ns.exchange(0, std::memory_order_relaxed);
    const u64 c_rt = cfg_rt_ns.exchange(0, std::memory_order_relaxed);
    std::vector<std::pair<std::string, SiteStat>> sites;
    {
        std::scoped_lock lock{site_mutex};
        for (auto& [k, v] : Sites()) sites.emplace_back(k, v);
        Sites().clear();
    }
    std::sort(sites.begin(), sites.end(),
              [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    std::string site_text;
    for (size_t i = 0; i < sites.size() && i < 4; ++i) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s=%.1fms(n=%llu) ", sites[i].first.c_str(),
                      static_cast<double>(sites[i].second.ns) * 1.0e-6,
                      static_cast<unsigned long long>(sites[i].second.count));
        site_text += buf;
    }
    const u64 gpuwait = gpu_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 gpuwaits = gpu_wait_count.exchange(0, std::memory_order_relaxed);
    if (last_present && now - last_present > 45'000'000ULL) {
        const auto milliseconds = [](u64 ns) { return static_cast<double>(ns) * 1.0e-6; };
        LOG_INFO(Render_Vulkan,
                 "STALL present_interval={:.1f}ms completed_scope_totals(overlap_possible): "
                 "gpu_idle={:.1f}ms draw={:.1f}ms(n={}) sched_wait={:.1f}ms "
                 "build_wait={:.1f}ms(n={}) shader_translate={:.1f}ms shader_emit={:.1f}ms(n={}) "
                 "pipeline_build={:.1f}ms(n={}) draw_flush={:.1f}ms lookup={:.1f}ms "
                 "configure={:.1f}ms record={:.1f}ms acquire={:.1f}ms present={:.1f}ms "
                 "frame_wait={:.1f}ms cpu_wait_on_gpu={:.1f}ms(n={}) "
                 "cfg[sync={:.1f} stage={:.1f} fill={:.1f} bind={:.1f} buf={:.1f} push={:.1f} "
                 "rt={:.1f}]ms wait_sites[{}]",
                 milliseconds(now - last_present), milliseconds(idle), milliseconds(draw), draws,
                 milliseconds(schedw), milliseconds(build), builds, milliseconds(translate),
                 milliseconds(emit), shaders, milliseconds(pipeline_build), pipeline_builds,
                 milliseconds(flush), milliseconds(lookup), milliseconds(configure),
                 milliseconds(record), milliseconds(acquire), milliseconds(present),
                 milliseconds(framew), milliseconds(gpuwait), gpuwaits, milliseconds(c_sync),
                 milliseconds(c_stage), milliseconds(c_fill), milliseconds(c_bind),
                 milliseconds(c_buffers), milliseconds(c_push), milliseconds(c_rt), site_text);
    }
    last_present = now;
}

} // namespace Vulkan::StallProbe
