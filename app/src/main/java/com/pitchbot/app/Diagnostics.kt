package com.pitchbot.app

import android.app.ActivityManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.widget.Toast
import java.io.File
import java.text.SimpleDateFormat
import java.util.ArrayDeque
import java.util.Date
import java.util.Locale

object Diagnostics {
    private class Sample(
        val wall: Long, val status: Int, val nHome: Int, val nAway: Int, val ball: Int,
        val engineUs: Int, val procMs: Int, val human: Boolean, val tapUp: Boolean
    )

    private const val MAX_SAMPLES = 600
    private const val MAX_EVENTS = 150
    private const val SAMPLE_GAP_MS = 500L
    private const val ALERT_CHANNEL = "pitchbot_alert"
    private const val ALERT_NID = 8
    private const val SNAPSHOT = "last_session.txt"

    private val samples = ArrayDeque<Sample>()
    private val events = ArrayDeque<String>()
    private var lastSample = -1_000_000L
    private var lastAlert = -1_000_000L

    @Volatile var meta: String = ""

    private fun stamp(ms: Long): String = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date(ms))

    @Synchronized fun note(msg: String) {
        if (events.size >= MAX_EVENTS) events.removeFirst()
        events.addLast(stamp(System.currentTimeMillis()) + "  " + msg)
    }

    @Synchronized fun due(nowMono: Long): Boolean = nowMono - lastSample >= SAMPLE_GAP_MS

    @Synchronized fun sample(
        nowMono: Long, status: Int, dots: FloatArray?, info: FloatArray?, procMs: Long, human: Boolean, tapUp: Boolean
    ) {
        if (nowMono - lastSample < SAMPLE_GAP_MS) return
        lastSample = nowMono
        val nh = if (dots != null && dots.size >= 5) dots[0].toInt() else -1
        val na = if (dots != null && dots.size >= 5) dots[1].toInt() else -1
        val ball = if (dots != null && dots.size >= 5) (if (dots[2] > 0.5f) 1 else 0) else -1
        val us = if (info != null && info.size >= 2) info[1].toInt() else -1
        if (samples.size >= MAX_SAMPLES) samples.removeFirst()
        samples.addLast(Sample(System.currentTimeMillis(), status, nh, na, ball, us, procMs.toInt(), human, tapUp))
    }

    fun drift(devPx: Int) { note("radar outline check: shift $devPx px") }

    fun driftFixed(devPx: Int, shiftPx: Int) { note("radar outline re-fitted: was $devPx px off, total shift $shiftPx px") }

    fun alert(ctx: Context, title: String, text: String) {
        note("ALERT: $title")
        val now = SystemClock.elapsedRealtime()
        if (now - lastAlert < 10_000) return
        lastAlert = now
        try { Toast.makeText(ctx, "$title\n$text", Toast.LENGTH_LONG).show() } catch (_: Exception) { }
        try {
            val nm = ctx.getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(NotificationChannel(ALERT_CHANNEL, "PitchBot alerts", NotificationManager.IMPORTANCE_DEFAULT))
            val open = PendingIntent.getActivity(ctx, 3, Intent(ctx, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
            val n = Notification.Builder(ctx, ALERT_CHANNEL)
                .setContentTitle(title)
                .setContentText(text)
                .setStyle(Notification.BigTextStyle().bigText(text))
                .setSmallIcon(android.R.drawable.stat_notify_error)
                .setContentIntent(open)
                .setAutoCancel(true)
                .build()
            nm.notify(ALERT_NID, n)
        } catch (_: Exception) { }
        try {
            val vib: Vibrator? = if (Build.VERSION.SDK_INT >= 31) {
                (ctx.applicationContext.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager)?.defaultVibrator
            } else {
                @Suppress("DEPRECATION")
                ctx.applicationContext.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
            }
            if (vib != null && vib.hasVibrator()) {
                vib.vibrate(VibrationEffect.createWaveform(longArrayOf(0, 200, 120, 200, 120, 200), -1))
            }
        } catch (_: Exception) { }
    }

    private fun stateName(s: Int): String = when (s) {
        Gesture.ST_NOT_LIVE -> "NOT_LIVE"
        Gesture.ST_LOOSE -> "LOOSE"
        Gesture.ST_TEAM -> "OUR_BALL"
        Gesture.ST_OPP -> "OPP_BALL"
        else -> "-"
    }

    private fun exitReason(r: Int): String = when (r) {
        1 -> "EXIT_SELF"; 2 -> "SIGNALED"; 3 -> "LOW_MEMORY"; 4 -> "CRASH (Java)"; 5 -> "CRASH_NATIVE"
        6 -> "ANR"; 7 -> "INITIALIZATION_FAILURE"; 8 -> "PERMISSION_CHANGE"; 9 -> "EXCESSIVE_RESOURCE_USAGE"
        10 -> "USER_REQUESTED"; 11 -> "USER_STOPPED"; 12 -> "DEPENDENCY_DIED"; 13 -> "OTHER"; 14 -> "FREEZER"
        15 -> "PACKAGE_STATE_CHANGE"; 16 -> "PACKAGE_UPDATED"; else -> "UNKNOWN($r)"
    }

    @Synchronized private fun sessionText(): String {
        val sb = StringBuilder()
        sb.append("Events (oldest first):\n")
        if (events.isEmpty()) sb.append("  (none)\n") else for (e in events) sb.append("  ").append(e).append('\n')
        sb.append("\nStatus samples, about every 0.5 s (oldest first):\n")
        if (samples.isEmpty()) {
            sb.append("  (none)\n")
            return sb.toString()
        }
        val n = samples.size
        val notLive = samples.count { it.status == Gesture.ST_NOT_LIVE }
        val proc = samples.map { it.procMs }.sorted()
        val low = samples.count { it.nHome in 0..5 || it.nAway in 0..5 }
        sb.append("  summary: ").append(n).append(" samples, not live ").append(notLive * 100 / n).append("%, frames with a team under 6 dots ")
            .append(low * 100 / n).append("%, processing median ").append(proc[n / 2]).append(" ms, max ").append(proc[n - 1]).append(" ms\n")
        sb.append("  time      state     home away ball  engine_us proc_ms human accessibility\n")
        for (s in samples) {
            sb.append("  ").append(stamp(s.wall)).append("  ")
                .append(stateName(s.status).padEnd(8)).append("  ")
                .append(s.nHome.toString().padStart(4)).append(' ')
                .append(s.nAway.toString().padStart(4)).append(' ')
                .append(s.ball.toString().padStart(4)).append(' ')
                .append(s.engineUs.toString().padStart(10)).append(' ')
                .append(s.procMs.toString().padStart(7)).append(' ')
                .append((if (s.human) "yes" else "no").padStart(5)).append(' ')
                .append(if (s.tapUp) "connected" else "OFF").append('\n')
        }
        return sb.toString()
    }

    fun saveSnapshot(ctx: Context) {
        try { File(ctx.filesDir, SNAPSHOT).writeText(sessionText()) } catch (_: Exception) { }
    }

    fun report(ctx: Context): String {
        val sb = StringBuilder()
        val prefs = Prefs(ctx)
        sb.append("PitchBot error report\n")
        sb.append("Created: ").append(SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())).append('\n')
        try {
            val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
            sb.append("App: ").append(pi.versionName).append(" (").append(if (Build.VERSION.SDK_INT >= 28) pi.longVersionCode else @Suppress("DEPRECATION") pi.versionCode.toLong()).append(")\n")
        } catch (_: Exception) { }
        sb.append("Phone: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
            .append(", Android ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n")
        sb.append("Bot service running: ").append(BotService.alive)
            .append(" | accessibility service connected: ").append(TapService.inst != null).append('\n')
        sb.append("Settings: dryRun=").append(prefs.dryRun).append(" analytics=").append(prefs.analytics)
            .append(" smartTracking=").append(prefs.stateEst).append(" simpleJoystick=").append(prefs.simpleMove)
            .append(" holdAware=").append(prefs.holdAware).append(" recordSessions=").append(prefs.recordSessions).append('\n')
        sb.append("Radar box saved: ").append(prefs.mapping() != null).append(" | outline saved: ").append(prefs.outline() != null).append('\n')
        if (meta.isNotEmpty()) sb.append(meta).append('\n')
        sb.append(prefs.trackerSummary()).append("\n\n")

        if (Build.VERSION.SDK_INT >= 30) {
            try {
                val am = ctx.getSystemService(ActivityManager::class.java)
                val list = am.getHistoricalProcessExitReasons(ctx.packageName, 0, 5)
                sb.append("Previous process exits (newest first; says why Android ended PitchBot):\n")
                if (list.isEmpty()) sb.append("  (none recorded)\n")
                for (e in list) {
                    sb.append("  ").append(SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date(e.timestamp)))
                        .append("  ").append(exitReason(e.reason))
                    val d = e.description
                    if (!d.isNullOrEmpty()) sb.append("  - ").append(d)
                    sb.append('\n')
                }
                sb.append('\n')
            } catch (_: Exception) { }
        }

        val live = sessionText()
        if (samples.isEmpty() && events.isEmpty()) {
            val f = File(ctx.filesDir, SNAPSHOT)
            if (f.exists()) {
                sb.append("No session in memory; last saved session:\n")
                sb.append(try { f.readText() } catch (_: Exception) { "(unreadable)\n" })
                return sb.toString()
            }
        }
        sb.append(live)
        return sb.toString()
    }

    fun exportTo(ctx: Context, uri: Uri) {
        val bytes = report(ctx).toByteArray(Charsets.UTF_8)
        ctx.contentResolver.openOutputStream(uri)?.use { it.write(bytes) }
    }
}
