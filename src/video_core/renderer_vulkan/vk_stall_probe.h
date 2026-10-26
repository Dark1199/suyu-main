// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <algorithm>
#include <array>
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
        // Dark Switch: off by default so normal runs carry no probe cost. Set SUYU_STALL_PROBE=1
        // (on Android: add it as a custom variable in the Freedreno environment settings) to
        // get the STALL / bc / gpu_exec / gpu_thread telemetry.
        return value && *value && *value != '0';
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

// Dark Switch: buffer-cache cost per draw (host side, summed over draws since last report).
// upd_* = Update*Buffers (guest tracking, uploads, copies); bind_* = BindHost*Buffers.
inline std::atomic<u64> bc_total_ns{0}, bc_calls{0}, bc_retry{0};
inline std::atomic<u64> bc_upd_idx_ns{0}, bc_upd_vtx_ns{0}, bc_upd_xfb_ns{0}, bc_upd_ubo_ns{0},
    bc_upd_sbo_ns{0}, bc_upd_tex_ns{0}, bc_upd_ind_ns{0}, bc_upd_lock_ns{0};
inline std::atomic<u64> bc_bind_idx_ns{0}, bc_bind_vtx_ns{0}, bc_bind_xfb_ns{0},
    bc_bind_ubo_ns{0}, bc_bind_sbo_ns{0}, bc_bind_tex_ns{0}, bc_bind_ind_ns{0};
// Dark Switch: inside the slow (non-optimized) vertex bind path.
inline std::atomic<u64> bc_vtx_slots{0}, bc_vtx_skipped{0}, bc_vtx_synced{0}, bc_vtx_uploads{0};
inline std::atomic<u64> bc_vtx_mark_ns{0}, bc_vtx_bytes{0}, dp_batched_words{0};
inline std::atomic<u64> bc_vtx_touch_ns{0}, bc_vtx_sync_ns{0}, bc_vtx_build_ns{0}, bc_vtx_loop_ns{0};
// Dark Switch: uniform bind paths (streamed "fast" copy versus cached buffer).
inline std::atomic<u64> ubo_fast_ns{0}, ubo_fast_n{0}, ubo_fast_bytes{0}, ubo_cached_ns{0},
    ubo_cached_n{0};
// Dark Switch: emulated GPU thread accounting. Engine slots: 0=puller, 1+EngineTypes
// (1=nv01 2=kepler_compute 3=maxwell3d 4=fermi2d 5=maxwell_dma 6=kepler_memory), 7=other.
inline std::atomic<u64> gt_submit_ns{0}, gt_tick_ns{0}, gt_flush_ns{0}, gt_inval_ns{0}, gt_cmds{0};
inline std::array<std::atomic<u64>, 8> eng_ns{};
// Dark Switch: inside the GPU thread's Maxwell3D / command-list handling.
inline std::atomic<u64> m3d_macro_ns{0}, m3d_macro_n{0}, m3d_sink_ns{0}, m3d_sink_n{0},
    m3d_sink_entries{0};
inline std::atomic<u64> dp_read_ns{0}, dp_process_ns{0}, dp_words{0}, dp_lists{0};
// Dark Switch: GPU-side execution time measured with vkCmdWriteTimestamp (one begin/end pair
// per queue submission, begin = start of the upload cmdbuf, end = end of the main cmdbuf).
inline std::atomic<u64> gpu_exec_ns{0}, gpu_exec_count{0}, gpu_exec_max_ns{0};
inline std::atomic<u64> gpu_ts_dropped{0};

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

// Charges the scope's duration to whichever sink was chosen with Set() (none = not counted).
class PathTimer {
public:
    PathTimer() : enabled_{Enabled()}, start_{enabled_ ? Now() : 0} {}
    void Set(std::atomic<u64>& ns, std::atomic<u64>& count) {
        ns_ = &ns;
        count_ = &count;
    }
    ~PathTimer() {
        if (!enabled_ || !ns_) return;
        ns_->fetch_add(Now() - start_, std::memory_order_relaxed);
        count_->fetch_add(1, std::memory_order_relaxed);
    }
    PathTimer(const PathTimer&) = delete;
    PathTimer& operator=(const PathTimer&) = delete;
private:
    bool enabled_;
    u64 start_;
    std::atomic<u64>* ns_ = nullptr;
    std::atomic<u64>* count_ = nullptr;
};

// Times 1 in 16 calls and scales by 16. For very hot call sites (per register write) where a clock
// read on every call would cost more than the code being measured.
class SampledAccum {
public:
    explicit SampledAccum(std::atomic<u64>& sink)
        : sink_{sink}, sampled_{Enabled() && ((++Counter() & 15U) == 0)},
          start_{sampled_ ? Now() : 0} {}
    ~SampledAccum() {
        if (sampled_) {
            sink_.fetch_add((Now() - start_) * 16, std::memory_order_relaxed);
        }
    }
    SampledAccum(const SampledAccum&) = delete;
    SampledAccum& operator=(const SampledAccum&) = delete;
private:
    static u32& Counter() {
        static thread_local u32 counter = 0;
        return counter;
    }
    std::atomic<u64>& sink_;
    bool sampled_;
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
    const auto take = [](std::atomic<u64>& a) { return a.exchange(0, std::memory_order_relaxed); };
    const u64 b_total = take(bc_total_ns), b_calls = take(bc_calls), b_retry = take(bc_retry);
    const u64 u_idx = take(bc_upd_idx_ns), u_vtx = take(bc_upd_vtx_ns), u_xfb = take(bc_upd_xfb_ns),
              u_ubo = take(bc_upd_ubo_ns), u_sbo = take(bc_upd_sbo_ns), u_tex = take(bc_upd_tex_ns),
              u_ind = take(bc_upd_ind_ns), u_lock = take(bc_upd_lock_ns);
    const u64 k_idx = take(bc_bind_idx_ns), k_vtx = take(bc_bind_vtx_ns),
              k_xfb = take(bc_bind_xfb_ns), k_ubo = take(bc_bind_ubo_ns),
              k_sbo = take(bc_bind_sbo_ns), k_tex = take(bc_bind_tex_ns),
              k_ind = take(bc_bind_ind_ns);
    const u64 g_ns = take(gpu_exec_ns), g_n = take(gpu_exec_count), g_max = take(gpu_exec_max_ns),
              g_drop = take(gpu_ts_dropped);
    const u64 v_slots = take(bc_vtx_slots), v_skip = take(bc_vtx_skipped),
              v_sync = take(bc_vtx_synced), v_up = take(bc_vtx_uploads),
              v_touch = take(bc_vtx_touch_ns), v_syncns = take(bc_vtx_sync_ns),
              v_build = take(bc_vtx_build_ns), v_loop = take(bc_vtx_loop_ns);
    const u64 v_mark = take(bc_vtx_mark_ns), v_bytes = take(bc_vtx_bytes),
              dp_batched = take(dp_batched_words);
    const u64 uf_ns = take(ubo_fast_ns), uf_n = take(ubo_fast_n), uf_b = take(ubo_fast_bytes),
              uc_ns = take(ubo_cached_ns), uc_n = take(ubo_cached_n);
    const u64 t_submit = take(gt_submit_ns), t_tick = take(gt_tick_ns), t_flush = take(gt_flush_ns),
              t_inval = take(gt_inval_ns), t_cmds = take(gt_cmds);
    const u64 mc_ns = take(m3d_macro_ns), mc_n = take(m3d_macro_n), sk_ns = take(m3d_sink_ns),
              sk_n = take(m3d_sink_n), sk_e = take(m3d_sink_entries);
    const u64 dp_r = take(dp_read_ns), dp_p = take(dp_process_ns), dp_w = take(dp_words),
              dp_l = take(dp_lists);
    std::array<u64, 8> eng{};
    for (size_t i = 0; i < eng.size(); ++i) eng[i] = take(eng_ns[i]);
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
                 "rt={:.1f}]ms wait_sites[{}] "
                 "bc[calls={} retry={} total={:.1f} upd(idx={:.1f} vtx={:.1f} xfb={:.1f} "
                 "ubo={:.1f} sbo={:.1f} tex={:.1f} ind={:.1f} lock_other={:.1f}) "
                 "bind(idx={:.1f} vtx={:.1f} xfb={:.1f} ubo={:.1f} sbo={:.1f} tex={:.1f} "
                 "ind={:.1f})]ms "
                 "gpu_exec[submits={} sum={:.1f}ms max={:.1f}ms dropped={}] "
                 "vtx[slots={} skipped={} synced={} uploads={} touch={:.1f} sync={:.1f} "
                 "build={:.1f} loop={:.1f} mark={:.1f} bytes={}KB]ms "
                 "ubo[fast_n={} fast={:.1f}ms/{}KB cached_n={} cached={:.1f}ms] "
                 "gpu_thread[submit={:.1f} tick={:.1f} flush={:.1f} inval={:.1f} cmds={} | "
                 "puller={:.1f} m3d={:.1f} compute={:.1f} dma={:.1f} f2d={:.1f} kmem={:.1f} "
                 "nv01={:.1f}]ms "
                 "m3d[macro={:.1f}(n={}) sink={:.1f}(n={} entries={})]ms "
                 "dp[read={:.1f} process={:.1f}]ms(lists={} words={} batched={})",
                 milliseconds(now - last_present), milliseconds(idle), milliseconds(draw), draws,
                 milliseconds(schedw), milliseconds(build), builds, milliseconds(translate),
                 milliseconds(emit), shaders, milliseconds(pipeline_build), pipeline_builds,
                 milliseconds(flush), milliseconds(lookup), milliseconds(configure),
                 milliseconds(record), milliseconds(acquire), milliseconds(present),
                 milliseconds(framew), milliseconds(gpuwait), gpuwaits, milliseconds(c_sync),
                 milliseconds(c_stage), milliseconds(c_fill), milliseconds(c_bind),
                 milliseconds(c_buffers), milliseconds(c_push), milliseconds(c_rt), site_text,
                 b_calls, b_retry, milliseconds(b_total), milliseconds(u_idx), milliseconds(u_vtx),
                 milliseconds(u_xfb), milliseconds(u_ubo), milliseconds(u_sbo), milliseconds(u_tex),
                 milliseconds(u_ind), milliseconds(u_lock), milliseconds(k_idx), milliseconds(k_vtx),
                 milliseconds(k_xfb), milliseconds(k_ubo), milliseconds(k_sbo), milliseconds(k_tex),
                 milliseconds(k_ind), g_n, milliseconds(g_ns), milliseconds(g_max), g_drop, v_slots,
                 v_skip, v_sync, v_up, milliseconds(v_touch), milliseconds(v_syncns),
                 milliseconds(v_build), milliseconds(v_loop), milliseconds(v_mark), v_bytes / 1024,
                 uf_n, milliseconds(uf_ns),
                 uf_b / 1024, uc_n, milliseconds(uc_ns), milliseconds(t_submit),
                 milliseconds(t_tick), milliseconds(t_flush), milliseconds(t_inval), t_cmds,
                 milliseconds(eng[0]), milliseconds(eng[3]), milliseconds(eng[2]),
                 milliseconds(eng[5]), milliseconds(eng[4]), milliseconds(eng[6]),
                 milliseconds(eng[1]), milliseconds(mc_ns), mc_n, milliseconds(sk_ns), sk_n, sk_e,
                 milliseconds(dp_r), milliseconds(dp_p), dp_l, dp_w, dp_batched);
    }
    last_present = now;
}

} // namespace Vulkan::StallProbe
