// SPDX-FileCopyrightText: 2026 Dark Switch Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Records why the previous app processes ended (native crash, low-memory kill, ANR...) into
 * files/log/exit_reasons.txt, so crashes can be diagnosed on the device without adb.
 */
object ExitReasonLogger {
    fun record(context: Context, filesDir: File) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        try {
            val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val exits = am.getHistoricalProcessExitReasons(context.packageName, 0, 5)
            if (exits.isEmpty()) return
            val fmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)
            val sb = StringBuilder()
            for (e in exits) {
                sb.append(fmt.format(Date(e.timestamp))).append("  ")
                    .append(reasonName(e.reason)).append("  status=").append(e.status)
                    .append("  pss=").append(e.pss).append("KB  rss=").append(e.rss).append("KB")
                    .append("  desc=").append(e.description ?: "").append('\n')
                if (e.reason == ApplicationExitInfo.REASON_CRASH_NATIVE) {
                    try {
                        e.traceInputStream?.bufferedReader()?.use { r ->
                            sb.append(r.readText().take(6000)).append('\n')
                        }
                    } catch (_: Exception) {
                    }
                }
            }
            val dir = File(filesDir, "log").apply { mkdirs() }
            File(dir, "exit_reasons.txt").writeText(sb.toString())
        } catch (_: Throwable) {
        }
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
