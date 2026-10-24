// SPDX-FileCopyrightText: 2026 Dark Switch Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Lightweight instrumentation for tracing guest heap growth and host memset() of backing memory.
// All log lines start with "[DARKTRACE]" and use key=value pairs so they can be grepped from logcat.
namespace Common::DarkTrace {

// Only clears at least this large are logged individually (smaller ones are still counted).
constexpr std::uint64_t LargeClearThreshold = 1ULL << 20;

struct ProcMem {
    std::int64_t rss_kb = -1;
    std::int64_t anon_kb = -1;
    std::int64_t file_kb = -1;
    std::int64_t shmem_kb = -1;
    std::int64_t swap_kb = -1;
};

inline ProcMem ReadProcMem() {
    ProcMem m;
#if defined(__linux__) || defined(__ANDROID__)
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[160];
        while (std::fgets(line, sizeof(line), f)) {
            long long v = 0;
            if (std::sscanf(line, "VmRSS: %lld", &v) == 1) m.rss_kb = v;
            else if (std::sscanf(line, "RssAnon: %lld", &v) == 1) m.anon_kb = v;
            else if (std::sscanf(line, "RssFile: %lld", &v) == 1) m.file_kb = v;
            else if (std::sscanf(line, "RssShmem: %lld", &v) == 1) m.shmem_kb = v;
            else if (std::sscanf(line, "VmSwap: %lld", &v) == 1) m.swap_kb = v;
        }
        std::fclose(f);
    }
#endif
    return m;
}

// Global cumulative counters (all threads).
inline std::atomic<std::uint64_t> g_cleared_bytes{0};
inline std::atomic<std::uint64_t> g_cleared_zero_bytes{0};
inline std::atomic<std::uint64_t> g_clear_calls{0};

// Per-thread counters, so a SetHeapSize call can report exactly what it cleared itself.
inline thread_local std::uint64_t tl_cleared_bytes = 0;
inline thread_local std::uint64_t tl_cleared_calls = 0;
// Which kernel operation is currently issuing clears on this thread.
inline thread_local const char* tl_context = "other";

struct ScopedContext {
    explicit ScopedContext(const char* name) : prev(tl_context) {
        tl_context = name;
    }
    ~ScopedContext() {
        tl_context = prev;
    }
    const char* prev;
};

inline std::int64_t Delta(std::int64_t after, std::int64_t before) {
    return (after < 0 || before < 0) ? 0 : after - before;
}

} // namespace Common::DarkTrace
