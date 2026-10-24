package com.pitchbot.app

import android.content.Context
import java.io.BufferedWriter
import java.io.File
import java.io.FileWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object Recorder {
    private const val MAX_BYTES = 12L * 1024 * 1024
    private const val MIN_GAP_MS = 120L

    private var out: BufferedWriter? = null
    private var t0 = 0L
    private var lastRow = -1_000_000L
    private var lastFlush = 0L
    private var bytes = 0L
    private val sb = StringBuilder(512)

    fun dir(ctx: Context): File = File(ctx.filesDir, "tracks").also { it.mkdirs() }

    @Synchronized fun isOpen(): Boolean = out != null

    @Synchronized fun start(ctx: Context, nowMs: Long, header: List<String>) {
        if (out != null) return
        try {
            val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
            val w = BufferedWriter(FileWriter(File(dir(ctx), "track-$stamp.csv")), 16 * 1024)
            bytes = 0L
            for (h in header) {
                w.write("# "); w.write(h); w.newLine()
                bytes += h.length + 3
            }
            w.write("t_ms,status,nHome,nAway,ballSeen,bx,by,dots"); w.newLine()
            out = w
            t0 = nowMs
            lastRow = -1_000_000L
            lastFlush = nowMs
            Diagnostics.note("recording started")
        } catch (_: Exception) {
            out = null
        }
    }

    @Synchronized fun add(nowMs: Long, status: Int, d: FloatArray?) {
        val w = out ?: return
        if (d == null || d.size < 5) return
        if (nowMs - lastRow < MIN_GAP_MS) return
        lastRow = nowMs
        try {
            sb.setLength(0)
            sb.append(nowMs - t0).append(',').append(status)
            sb.append(',').append(d[0].toInt()).append(',').append(d[1].toInt())
            sb.append(',').append(if (d[2] > 0.5f) 1 else 0)
            sb.append(',').append(milli(d[3])).append(',').append(milli(d[4]))
            var i = 5
            while (i + 1 < d.size) {
                sb.append(',').append(milli(d[i])).append(',').append(milli(d[i + 1]))
                i += 2
            }
            sb.append('\n')
            w.write(sb.toString())
            bytes += sb.length
            if (nowMs - lastFlush > 2000) { w.flush(); lastFlush = nowMs }
            if (bytes >= MAX_BYTES) {
                Diagnostics.note("recording stopped: file size limit")
                stop()
            }
        } catch (_: Exception) {
            stop()
        }
    }

    @Synchronized fun stop() {
        val w = out ?: return
        out = null
        try { w.flush(); w.close() } catch (_: Exception) { }
    }

    private fun milli(v: Float): Int = if (v.isNaN()) 0 else (v * 1000f).toInt().coerceIn(-1000, 2000)

    fun summary(ctx: Context): String {
        val fs = dir(ctx).listFiles()?.filter { it.isFile } ?: emptyList()
        if (fs.isEmpty()) return "No recordings yet. They are saved on this phone and included in Export backup."
        val mb = fs.sumOf { it.length() } / 1048576.0
        return "${fs.size} recording(s), ${"%.1f".format(Locale.US, mb)} MB on this phone; included in Export backup (folder tracks/)."
    }

    fun deleteAll(ctx: Context) {
        stop()
        dir(ctx).listFiles()?.forEach { try { it.delete() } catch (_: Exception) { } }
    }
}
