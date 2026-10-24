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
 *  - mem_trace.txt / mem_trace.old.txt : once per second memory samples of the running app
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

        startMemoryTrace(logDir)
    }

    private fun startMemoryTrace(logDir: File) {
        Thread {
            try {
                val cur = File(logDir, "mem_trace.txt")
                val old = File(logDir, "mem_trace.old.txt")
                if (cur.exists()) {
                    old.delete()
                    cur.renameTo(old)
                }
                val out = FileOutputStream(cur, true)
                val t0 = System.currentTimeMillis()
                while (true) {
                    val mem = readKv("/proc/meminfo", setOf("MemAvailable", "SwapFree", "Cached"))
                    val self = readKv("/proc/self/status", setOf("VmRSS", "VmSwap", "Threads"))
                    val psi = try {
                        File("/proc/pressure/memory").readText().lineSequence().firstOrNull() ?: ""
                    } catch (_: Throwable) {
                        ""
                    }
                    val line = "t=" + ((System.currentTimeMillis() - t0) / 1000) + "s " +
                        "avail=" + (mem["MemAvailable"] ?: "?") + " swapfree=" + (mem["SwapFree"] ?: "?") +
                        " rss=" + (self["VmRSS"] ?: "?") + " vmswap=" + (self["VmSwap"] ?: "?") +
                        " thr=" + (self["Threads"] ?: "?") + " psi[" + psi + "]\n"
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
