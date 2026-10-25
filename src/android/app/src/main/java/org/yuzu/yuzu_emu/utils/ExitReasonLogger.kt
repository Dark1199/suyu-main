// SPDX-FileCopyrightText: 2026 Dark Switch Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * On-device crash diagnosis without adb. Everything is written to files/log/:
 *  - exit_reasons.txt : why the previous processes ended
 *  - saved/prev_*.txt : copies of the previous run's emulator log (survives log rotation)
 *  - saved/logcat_*.txt : this app's own logcat as of the launch after an abnormal exit
 *  - mem_trace_<start>_<pid>.txt : once per second memory/priority/thermal samples (last 3 runs kept)
 */
object ExitReasonLogger {
    private const val MAX_SAVED = 8
    private const val MAX_TRACE_BYTES = 1_500_000L

    fun record(context: Context, filesDir: File) {
        val logDir = File(filesDir, "log").apply { mkdirs() }
        val saved = File(logDir, "saved").apply { mkdirs() }
        var abnormalStamp: String? = null
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
                val exits = am.getHistoricalProcessExitReasons(context.packageName, 0, 6)
                val fmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)
                val sb = StringBuilder()
                for (e in exits) {
                    sb.append(fmt.format(Date(e.timestamp))).append("  ")
                        .append(reasonName(e.reason)).append("  status=").append(e.status)
                        .append("  pss=").append(e.pss).append("KB  rss=").append(e.rss)
                        .append("KB  desc=").append(e.description ?: "").append('\n')
                    if (e.reason == ApplicationExitInfo.REASON_CRASH_NATIVE) {
                        try {
                            e.traceInputStream?.bufferedReader()?.use { r ->
                                sb.append(r.readText().take(6000)).append('\n')
                            }
                        } catch (_: Exception) {
                        }
                    }
                }
                File(logDir, "exit_reasons.txt").writeText(sb.toString())
                val last = exits.firstOrNull()
                if (last != null && last.reason != ApplicationExitInfo.REASON_USER_REQUESTED &&
                    last.reason != ApplicationExitInfo.REASON_EXIT_SELF
                ) {
                    abnormalStamp = last.timestamp.toString()
                }
            }
        } catch (_: Throwable) {
        }

        try {
            val old = File(logDir, "suyu_log.txt.old.txt")
            if (old.exists() && old.length() > 0) {
                val dest = File(saved, "prev_" + old.lastModified() + ".txt")
                if (!dest.exists()) old.copyTo(dest, overwrite = true)
            }
        } catch (_: Throwable) {
        }

        val stamp = abnormalStamp
        Thread {
            try {
                if (stamp != null) {
                    val dest = File(saved, "logcat_$stamp.txt")
                    if (!dest.exists()) {
                        val p = ProcessBuilder("logcat", "-d", "-v", "threadtime", "-t", "4000")
                            .redirectErrorStream(true).start()
                        val text = p.inputStream.bufferedReader().use { it.readText() }
                        dest.writeText(text.takeLast(600_000))
                    }
                }
                val files = saved.listFiles()?.sortedBy { it.lastModified() } ?: emptyList()
                if (files.size > MAX_SAVED) {
                    files.take(files.size - MAX_SAVED).forEach { it.delete() }
                }
            } catch (_: Throwable) {
            }
        }.apply { isDaemon = true; name = "dark-evidence" }.start()

        startMemoryTrace(context, logDir)
    }

    private fun startMemoryTrace(context: Context, logDir: File) {
        Thread {
            try {
                val pid = android.os.Process.myPid()
                val cur = File(logDir, "mem_trace_" + System.currentTimeMillis() + "_" + pid + ".txt")
                logDir.listFiles { f -> f.name.startsWith("mem_trace_") }
                    ?.sortedBy { it.lastModified() }?.dropLast(3)?.forEach { it.delete() }
                val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
                val pm = context.getSystemService(Context.POWER_SERVICE) as android.os.PowerManager
                val out = FileOutputStream(cur, true)
                val t0 = System.currentTimeMillis()
                val prevTicks = HashMap<String, Long>()
                while (true) {
                    val mem = readKv(
                        "/proc/meminfo",
                        setOf("MemAvailable", "MemFree", "AnonPages", "Shmem", "SwapFree")
                    )
                    val self = readKv("/proc/self/status", setOf("VmRSS", "VmSwap", "Threads"))
                    val st = ActivityManager.RunningAppProcessInfo()
                    ActivityManager.getMyMemoryState(st)
                    val thermal = if (Build.VERSION.SDK_INT >= 29) pm.currentThermalStatus else -1
                    val batt = context.registerReceiver(
                        null,
                        android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED)
                    )
                    val tempC = (batt?.getIntExtra("temperature", -1) ?: -1) / 10.0
                    val oomAdj = try { File("/proc/self/oom_score_adj").readText().trim() } catch (_: Throwable) { "?" }
                    val oomScore = try { File("/proc/self/oom_score").readText().trim() } catch (_: Throwable) { "?" }
                    val line = "t=" + ((System.currentTimeMillis() - t0) / 1000) + "s" +
                        " avail=" + (mem["MemAvailable"] ?: "?") + " free=" + (mem["MemFree"] ?: "?") +
                        " anon=" + (mem["AnonPages"] ?: "?") + " shmem=" + (mem["Shmem"] ?: "?") +
                        " swapfree=" + (mem["SwapFree"] ?: "?") +
                        " rss=" + (self["VmRSS"] ?: "?") + " vmswap=" + (self["VmSwap"] ?: "?") +
                        " thr=" + (self["Threads"] ?: "?") +
                        " importance=" + st.importance + " trim=" + st.lastTrimLevel +
                        " oom_adj=" + oomAdj + " oom_score=" + oomScore +
                        " thermal=" + thermal + " batt_c=" + tempC +
                        " " + cpuTop(prevTicks) + " " + freqInfo() + "\n"
                    out.write(line.toByteArray())
                    if (cur.length() > MAX_TRACE_BYTES) {
                        out.channel.truncate(0)
                    }
                    Thread.sleep(1000)
                }
            } catch (_: Throwable) {
            }
        }.apply { isDaemon = true; name = "dark-memtrace" }.start()
    }

    /** Top 4 threads by CPU use over the last second, as name:percent-of-one-core. */
    private fun cpuTop(prev: HashMap<String, Long>): String {
        return try {
            val dirs = File("/proc/self/task").listFiles() ?: return "cpu_top=?"
            val cur = HashMap<String, Long>()
            val usage = ArrayList<Pair<String, Long>>()
            var total = 0L
            for (d in dirs) {
                val txt = try { File(d, "stat").readText() } catch (_: Throwable) { continue }
                val open = txt.indexOf('(')
                val close = txt.lastIndexOf(')')
                if (open < 0 || close < open) continue
                val name = txt.substring(open + 1, close)
                val f = txt.substring(close + 2).split(' ')
                val ticks = (f.getOrNull(11)?.toLongOrNull() ?: 0L) + (f.getOrNull(12)?.toLongOrNull() ?: 0L)
                val key = d.name
                cur[key] = ticks
                val delta = ticks - (prev[key] ?: ticks)
                if (delta > 0) {
                    usage.add(Pair(name, delta))
                    total += delta
                }
            }
            prev.clear()
            prev.putAll(cur)
            val top = usage.sortedByDescending { it.second }.take(4)
                .joinToString(",") { it.first.replace(' ', '_') + ":" + it.second }
            "cpu_total=" + total + " cpu_top=" + top
        } catch (_: Throwable) {
            "cpu_top=?"
        }
    }

    /** Current CPU core frequencies (MHz) and GPU load/frequency when the kernel exposes them. */
    private fun freqInfo(): String {
        val sb = StringBuilder("freq_mhz=")
        for (i in 0 until 8) {
            val v = try {
                File("/sys/devices/system/cpu/cpu$i/cpufreq/scaling_cur_freq").readText().trim().toLong() / 1000
            } catch (_: Throwable) {
                -1L
            }
            sb.append(v).append(if (i < 7) "/" else "")
        }
        val gpuBusy = try { File("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage").readText().trim() } catch (_: Throwable) { "?" }
        val gpuClk = try { File("/sys/class/kgsl/kgsl-3d0/gpuclk").readText().trim() } catch (_: Throwable) { "?" }
        return sb.append(" gpu_busy=").append(gpuBusy).append(" gpuclk=").append(gpuClk).toString()
    }

    private fun readKv(path: String, keys: Set<String>): Map<String, String> {
        val result = HashMap<String, String>()
        try {
            File(path).forEachLine { l ->
                val i = l.indexOf(':')
                if (i > 0) {
                    val k = l.substring(0, i)
                    if (k in keys) result[k] = l.substring(i + 1).trim().replace(" ", "")
                }
            }
        } catch (_: Throwable) {
        }
        return result
    }

    private fun reasonName(r: Int): String = when (r) {
        ApplicationExitInfo.REASON_CRASH -> "CRASH_JAVA"
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "CRASH_NATIVE"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "LOW_MEMORY_KILL"
        ApplicationExitInfo.REASON_SIGNALED -> "SIGNALED"
        ApplicationExitInfo.REASON_ANR -> "ANR"
        ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "EXCESSIVE_RESOURCE_USAGE"
        ApplicationExitInfo.REASON_USER_REQUESTED -> "USER_REQUESTED"
        ApplicationExitInfo.REASON_EXIT_SELF -> "EXIT_SELF"
        ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "INIT_FAILURE"
        ApplicationExitInfo.REASON_DEPENDENCY_DIED -> "DEPENDENCY_DIED"
        else -> "OTHER($r)"
    }
}
