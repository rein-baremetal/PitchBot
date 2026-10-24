package com.pitchbot.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.widget.Toast
import kotlin.math.abs

class BotService : Service(), OverlayHost {

    companion object {
        const val EXTRA_CODE = "code"
        const val EXTRA_DATA = "data"
        const val ACTION_STOP = "com.pitchbot.app.STOP"
        const val ACTION_PICK = "com.pitchbot.app.PICK_COLOURS"
        const val ACTION_FT = "com.pitchbot.app.FULL_TIME"
        @Volatile var alive = false
        private const val CHANNEL = "pitchbot_low"
        private const val DRIFT_AUTO_MAX = 8
        private const val DRIFT_AUTO_CAP = 12
        private const val NID = 7
    }

    private class Crop(val px: IntArray, val w: Int, val h: Int, val t: Long, val m: Mapping)

    private lateinit var prefs: Prefs
    private lateinit var store: MatchStore
    private lateinit var overlay: OverlayManager
    private val main = Handler(Looper.getMainLooper())
    private var thread: HandlerThread? = null
    private lateinit var bg: Handler

    private var projection: MediaProjection? = null
    private var vd: VirtualDisplay? = null
    private var reader: ImageReader? = null

    @Volatile private var running = false
    @Volatile private var colorsSet = false
    @Volatile private var capW = 0
    @Volatile private var capH = 0
    @Volatile private var curMapping: Mapping? = null
    @Volatile private var curAnchors: HudAnchors? = null
    @Volatile private var lastCrop: Crop? = null
    private var lastFrameT = 0L
    private var lastSizeCheck = 0L
    private var lastHealthAt = 0L
    private var badMapRuns = 0
    private var lastDebugPush = 0L
    @Volatile private var lastImageAt = 0L
    private var runSince = 0L
    private var stallAlerted = false
    private var tapAlerted = false
    private var lastDriftT = 0L
    private var lastDriftWarn = -1_000_000L
    private var driftStreak = 0
    private var driftBase: IntArray? = null
    private var driftOrig: IntArray? = null
    private val watchdog = object : Runnable {
        override fun run() {
            if (!alive) return
            try { watchdogTick() } catch (_: Exception) { }
            main.postDelayed(this, 2000)
        }
    }
    private var lastStatus = -1
    private var seq = 0L
    private var saved = false
    private var possTeam = 0L
    private var possOpp = 0L
    private var warnedCancel = false
    @Volatile private var fullAfter = 0L
    @Volatile private var legacyRect: IntArray? = null
    @Volatile private var fullCb: ((IntArray?, Int, Int) -> Unit)? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        prefs = Prefs(this)
        store = MatchStore(this)
        overlay = OverlayManager(this, prefs, this)
        Native.nInit()
        Native.initPitchEngine()
        loadV7()
        Native.nNewMatch()
        prefs.pushToEngine()
        curMapping = prefs.mapping()
        curAnchors = prefs.hudAnchors()
    }

    @Suppress("DEPRECATION")
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            shutdown()
            return START_NOT_STICKY
        }
        if (alive && intent?.action == ACTION_PICK) { onVolumeKey(android.view.KeyEvent.KEYCODE_VOLUME_UP); return START_NOT_STICKY }
        if (alive && intent?.action == ACTION_FT) { onVolumeKey(android.view.KeyEvent.KEYCODE_VOLUME_DOWN); return START_NOT_STICKY }
        if (alive) return START_NOT_STICKY

        try {
            startForegroundCompat()
        } catch (e: SecurityException) {
            Toast.makeText(this, "Screen capture permission missing", Toast.LENGTH_LONG).show()
            stopSelf()
            return START_NOT_STICKY
        }
        val code = intent?.getIntExtra(EXTRA_CODE, 0) ?: 0
        val data: Intent? = if (Build.VERSION.SDK_INT >= 33) intent?.getParcelableExtra(EXTRA_DATA, Intent::class.java)
        else @Suppress("DEPRECATION") intent?.getParcelableExtra<Intent>(EXTRA_DATA)
        if (data == null) {
            Toast.makeText(this, "Screen capture permission missing", Toast.LENGTH_LONG).show()
            stopSelf()
            return START_NOT_STICKY
        }
        thread = HandlerThread("pitchbot-vision").also { it.start() }
        bg = Handler(thread!!.looper)
        try {
            val mpm = getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
            projection = mpm.getMediaProjection(code, data)
            projection!!.registerCallback(object : MediaProjection.Callback() {
                override fun onStop() { main.post { shutdown() } }
            }, main)
            setupDisplay()
        } catch (e: Exception) {
            Toast.makeText(this, "Could not start screen capture: ${e.message}", Toast.LENGTH_LONG).show()
            shutdown()
            return START_NOT_STICKY
        }
        alive = true
        Diagnostics.meta = "Capture: ${capW}x${capH}"
        Diagnostics.note("service started (capture ${capW}x${capH})")
        main.postDelayed(watchdog, 3000)
        applyQuad()
        TapService.inst?.simpleMove = prefs.simpleMove
        TapService.inst?.anchors = curAnchors
        TapService.keyHandler = { key -> onVolumeKey(key) }
        overlay.show()
        Toast.makeText(this, "PitchBot ready. Open the match and press Volume Up to pick your team colour. Volume Down stops and saves the log.", Toast.LENGTH_LONG).show()
        return START_NOT_STICKY
    }

    private fun startForegroundCompat() {
        val nm = getSystemService(NotificationManager::class.java)
        val ch = NotificationChannel(CHANNEL, "PitchBot (background)", NotificationManager.IMPORTANCE_LOW)
        ch.setShowBadge(false)
        ch.lockscreenVisibility = Notification.VISIBILITY_SECRET
        nm.createNotificationChannel(ch)
        val stop = PendingIntent.getService(
            this, 1, Intent(this, BotService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val pick = PendingIntent.getService(
            this, 3, Intent(this, BotService::class.java).setAction(ACTION_PICK),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val ft = PendingIntent.getService(
            this, 4, Intent(this, BotService::class.java).setAction(ACTION_FT),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val open = PendingIntent.getActivity(
            this, 2, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE
        )
        val n = Notification.Builder(this, CHANNEL)
            .setContentTitle("PitchBot")
            .setContentText("Running. Use the side buttons or these actions in the game.")
            .addAction(Notification.Action.Builder(android.graphics.drawable.Icon.createWithResource(this, android.R.drawable.ic_menu_edit), "Pick colours", pick).build())
            .addAction(Notification.Action.Builder(android.graphics.drawable.Icon.createWithResource(this, android.R.drawable.ic_menu_save), "Full time", ft).build())
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentIntent(open)
            .setVisibility(Notification.VISIBILITY_SECRET)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(NID, n)
        }
    }

    private fun setupDisplay() {
        val (w, h) = realSize(this)
        capW = w
        capH = h
        val old = reader
        val nr = ImageReader.newInstance(w, h, PixelFormat.RGBA_8888, 2)
        nr.setOnImageAvailableListener({ onImage(it) }, bg)
        reader = nr
        val dpi = resources.displayMetrics.densityDpi
        val existing = vd
        if (existing == null) {
            vd = projection!!.createVirtualDisplay(
                "PitchBotCapture", w, h, dpi, DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, nr.surface, null, bg
            )
        } else {
            existing.resize(w, h, dpi)
            existing.surface = nr.surface
        }
        old?.close()
        // the radar map is stored in pixels of the capture size: rebuild it whenever the size changes (portrait -> landscape)
        if (alive) {
            try { applyQuad() } catch (_: Exception) { }
            Diagnostics.note("capture size now ${w}x${h}: radar map rebuilt")
        }
    }

    private fun onImage(r: ImageReader) {
        val img = try { r.acquireLatestImage() } catch (e: Exception) { null } ?: return
        try {
            val now = SystemClock.elapsedRealtime()
            lastImageAt = now
            if (now - lastSizeCheck > 1000) {
                lastSizeCheck = now
                val (w, h) = realSize(this)
                if (w != capW || h != capH) main.post { try { setupDisplay() } catch (_: Exception) { } }
            }
            if (now - lastFrameT < 75) return
            lastFrameT = now
            val fa = fullAfter
            if (fa > 0 && now >= fa) {
                val cb = fullCb
                fullAfter = 0; fullCb = null
                if (cb != null) {
                    val fp = grabFull(img)
                    val fw = img.width; val fh = img.height
                    main.post { cb(fp, fw, fh) }
                }
            }
            val m = curMapping ?: return

            val iw = img.width
            val ih = img.height
            val l = (m.radL * iw).toInt().coerceIn(0, iw - 20)
            val t = (m.radT * ih).toInt().coerceIn(0, ih - 20)
            val rr = (m.radR * iw).toInt().coerceIn(l + 16, iw)
            val bb = (m.radB * ih).toInt().coerceIn(t + 16, ih)
            val cw = rr - l
            val ch = bb - t
            val plane = img.planes[0]
            val buf = plane.buffer
            val rs = plane.rowStride
            val ps = plane.pixelStride
            val out = IntArray(cw * ch)
            for (y in 0 until ch) {
                var o = (t + y) * rs + l * ps
                val row = y * cw
                for (x in 0 until cw) {
                    out[row + x] = ((buf.get(o).toInt() and 255) shl 16) or
                            ((buf.get(o + 1).toInt() and 255) shl 8) or
                            (buf.get(o + 2).toInt() and 255)
                    o += ps
                }
            }
            lastCrop = Crop(out, cw, ch, now, m)
            if (!running || !colorsSet) return
            try { driftCheck(out, cw, ch, now) } catch (_: Exception) { }

            var gesture: Gesture? = null
            val tapSvc = TapService.inst
            if (tapSvc != null) {
                tapSvc.resumeMs = prefs.resumeMs.toLong()
                tapSvc.holdAware = prefs.holdAware
                if (prefs.holdAware) tapSvc.reportKnob(stickKnobDisplaced(buf, rs, ps, iw, ih))
            }
            if (prefs.analytics) {
                Native.setHudPossession(votePossession(hudPossessionRaw(buf, rs, ps, iw, ih)))
                Native.setHumanControl(tapSvc?.humanActive() == true)
                gesture = Gesture.unpack(Native.processPitchFrame(buf, rs, ps, l, t, cw, ch, now))
                Native.nSetObserveOnly(gesture.status == Gesture.ST_TEAM || gesture.status == Gesture.ST_OPP ||
                    (gesture.status == Gesture.ST_LOOSE && gesture.valid))
                trackBlind(gesture.status, now)
                try { mapHealth(now) } catch (_: Exception) { }
            }
            val lr = legacyRect
            if (lr != null && lr[2] - lr[0] >= 8 && lr[3] - lr[1] >= 8 && lr[2] <= cw && lr[3] <= ch) {
                val lw = lr[2] - lr[0]
                val lh = lr[3] - lr[1]
                val leg = IntArray(lw * lh)
                for (y in 0 until lh) System.arraycopy(out, (lr[1] + y) * cw + lr[0], leg, y * lw, lw)
                handleResult(Native.nProcess(leg, lw, lh, now), now, gesture)
            } else {
                handleResult(Native.nProcess(out, cw, ch, now), now, gesture)
            }
            try { observe(now, gesture, tapSvc) } catch (_: Exception) { }
        } finally {
            img.close()
        }
    }

    private fun observe(now: Long, g: Gesture?, tap: TapService?) {
        when (g?.status) {
            Gesture.ST_TEAM -> possTeam++
            Gesture.ST_OPP -> possOpp++
        }
        val rec = prefs.recordSessions
        if (!rec && Recorder.isOpen()) Recorder.stop()
        val wantRec = rec && prefs.analytics
        val wantDiag = Diagnostics.due(now)
        if (!wantRec && !wantDiag) return
        val dots = if (prefs.analytics) Native.pitchDebugDots() else null
        val st = g?.status ?: -1
        if (wantRec) {
            if (!Recorder.isOpen()) Recorder.start(this, now, recorderHeader())
            Recorder.add(now, st, dots)
        }
        if (wantDiag) {
            val info = if (prefs.analytics) Native.pitchInfo() else null
            Diagnostics.sample(now, st, dots, info, SystemClock.elapsedRealtime() - now, tap?.humanActive() == true, tap != null)
        }
    }

    private fun recorderHeader(): List<String> {
        val loc = java.util.Locale.US
        val h = ArrayList<String>()
        h += "pitchbot-track 1 (tracked dot positions and times only; no screenshots)"
        h += "started " + java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", loc).format(java.util.Date())
        h += "columns: t_ms since start, status (0 not live, 1 loose, 2 our ball, 3 opp ball, -1 unknown), nHome, nAway, ballSeen, ballX, ballY, then nHome x/y pairs, then nAway x/y pairs"
        h += "coordinates: thousandths of the pitch (0..1000) after the radar-to-pitch mapping; tracker output, about 8 rows per second"
        h += "capture ${capW}x${capH}"
        curMapping?.let { m -> h += String.format(loc, "radar box (screen fractions) l=%.4f t=%.4f r=%.4f b=%.4f", m.radL, m.radT, m.radR, m.radB) }
        prefs.outline()?.let { o -> h += String.format(loc, "saved outline (screen fractions) l=%.4f t=%.4f r=%.4f b=%.4f", o[0], o[1], o[2], o[3]) }
        h += String.format(loc, "kits home=#%06X away=#%06X", (prefs.homeRgb ?: 0) and 0xFFFFFF, (prefs.awayRgb ?: 0) and 0xFFFFFF)
        h += "smartTracking=${prefs.stateEst} " + prefs.trackerSummary()
        return h
    }

    private fun driftCheck(px: IntArray, cw: Int, ch: Int, now: Long) {
        if (now - lastDriftT < 20_000) return
        lastDriftT = now
        val o = fitRadarOutline(px, cw, ch, 0, 0, cw, ch) ?: return
        if (o[4] < 70) return
        val ref = legacyRect ?: driftBase
        if (ref == null) { driftBase = intArrayOf(o[0], o[1], o[2], o[3]); return }
        val dev = maxOf(abs(o[0] - ref[0]), abs(o[1] - ref[1]), abs(o[2] - ref[2]), abs(o[3] - ref[3]))
        Diagnostics.drift(dev)
        driftStreak = if (dev >= 3) driftStreak + 1 else 0
        if (prefs.autoDrift && driftStreak >= 2 && o[4] >= 80 && dev <= DRIFT_AUTO_MAX && nudgeOutline(o, dev)) return
        if (driftStreak >= 2 && now - lastDriftWarn > 300_000) {
            lastDriftWarn = now
            main.post {
                Diagnostics.alert(this, "PitchBot: radar may have moved",
                    "The radar outline is about $dev px off from setup. Pause (Volume Up) and press Auto-fit in the radar box editor.")
            }
        }
    }

    private fun nudgeOutline(o: IntArray, dev: Int): Boolean {
        val cur = legacyRect ?: return false
        val orig = driftOrig ?: intArrayOf(cur[0], cur[1], cur[2], cur[3]).also { driftOrig = it }
        val next = IntArray(4)
        var shift = 0
        for (i in 0..3) {
            next[i] = cur[i] + (o[i] - cur[i]).coerceIn(-1, 1)
            val total = abs(next[i] - orig[i])
            if (total > DRIFT_AUTO_CAP) return false
            if (total > shift) shift = total
        }
        if (next[2] - next[0] < 8 || next[3] - next[1] < 8) return false
        legacyRect = next
        Native.setPitchQuad(floatArrayOf(next[0].toFloat(), next[1].toFloat(), next[2].toFloat(), next[1].toFloat(),
            next[2].toFloat(), next[3].toFloat(), next[0].toFloat(), next[3].toFloat()))
        Diagnostics.driftFixed(dev, shift)
        return true
    }

    private fun watchdogTick() {
        val now = SystemClock.elapsedRealtime()
        overlay.ensure()
        if (!(running && colorsSet)) {
            runSince = 0L; stallAlerted = false; tapAlerted = false
            return
        }
        if (runSince == 0L) runSince = now
        val idle = now - maxOf(lastImageAt, runSince)
        if (idle > 6000) {
            if (!stallAlerted) {
                stallAlerted = true
                Diagnostics.alert(this, "PitchBot: no screen frames for ${idle / 1000} s",
                    "Capture may have stalled (or the game is on a still screen). If it stays like this, stop and start the overlay again.")
            }
        } else stallAlerted = false
        if (!prefs.dryRun && TapService.inst == null) {
            if (!tapAlerted) {
                tapAlerted = true
                Diagnostics.alert(this, "PitchBot: accessibility service is off",
                    "The bot cannot press anything. Turn PitchBot's accessibility service back on in Settings.")
            }
        } else tapAlerted = false
    }

    private val possVotes = IntArray(5)
    private var possIdx = 0

    // The same four HUD buttons change label with possession. "Dash & Pressure" has two text lines,
    // plain "Dash" has one, so the text-row pattern on the DASH button says who has the ball.
    // Returns 1 = we attack, 2 = we defend, 0 = not sure.
    private fun hudPossessionRaw(buf: java.nio.ByteBuffer, rs: Int, ps: Int, iw: Int, ih: Int): Int {
        val dc = curAnchors?.dashButtonCenter ?: return 0
        val c = dc.at(iw, ih)
        val s = ih / 720f
        val cx = c.x.toInt()
        val cy = c.y.toInt()
        val hw = (40 * s).toInt().coerceAtLeast(8)
        val hh = (34 * s).toInt().coerceAtLeast(8)
        val x0 = cx - hw
        val x1 = cx + hw
        val y0 = cy - hh
        val y1 = cy + hh
        if (x0 < 0 || y0 < 0 || x1 >= iw || y1 >= ih) return 0
        val cw = x1 - x0 + 1
        val ch = y1 - y0 + 1
        val lum = IntArray(cw * ch)
        val hist = IntArray(256)
        for (y in 0 until ch) {
            for (x in 0 until cw) {
                val o = (y0 + y) * rs + (x0 + x) * ps
                if (o + 2 >= buf.limit()) return 0
                val l = ((buf.get(o).toInt() and 255) + (buf.get(o + 1).toInt() and 255) + (buf.get(o + 2).toInt() and 255)) / 3
                lum[y * cw + x] = l
                hist[l]++
            }
        }
        var acc = 0
        var median = 0
        val half = lum.size / 2
        for (i in 0 until 256) { acc += hist[i]; if (acc >= half) { median = i; break } }
        val th = median + 10
        fun band(a: Int, b: Int): Float {
            val r0 = (hh + (a * s).toInt()).coerceIn(0, ch - 1)
            val r1 = (hh + (b * s).toInt()).coerceIn(0, ch - 1)
            var n = 0
            for (y in r0..r1) for (x in 0 until cw) if (lum[y * cw + x] >= th) n++
            return n / (s * s)
        }
        val up = band(-25, -12)
        val mid = band(-6, 6)
        val lo = band(12, 25)
        return when {
            up > 150f && lo > 150f && mid < 0.5f * minOf(up, lo) -> 2
            mid > 200f && up + lo < 1.2f * mid -> 1
            else -> 0
        }
    }

    private fun votePossession(raw: Int): Int {
        possVotes[possIdx] = raw
        possIdx = (possIdx + 1) % possVotes.size
        var a = 0
        var d = 0
        for (v in possVotes) { if (v == 1) a++ else if (v == 2) d++ }
        return if (d >= 3) 2 else if (a >= 3) 1 else 0
    }

    private fun stickKnobDisplaced(buf: java.nio.ByteBuffer, rs: Int, ps: Int, iw: Int, ih: Int): Boolean {
        val a = curAnchors ?: return false
        val c = a.joystickPoint(iw, ih)
        val r = a.joystickRadiusN * ih * 0.65f
        if (r < 8f) return false
        val step = maxOf(1, (r / 14f).toInt())
        var total = 0
        var bright = 0
        var y = (c.y - r).toInt()
        val y1 = (c.y + r).toInt()
        val x1 = (c.x + r).toInt()
        while (y <= y1) {
            var x = (c.x - r).toInt()
            while (x <= x1) {
                val dx = x - c.x
                val dy = y - c.y
                if (dx * dx + dy * dy <= r * r && x in 0 until iw && y in 0 until ih) {
                    val o = y * rs + x * ps
                    if (o + 2 < buf.limit()) {
                        val rr = buf.get(o).toInt() and 255
                        val gg = buf.get(o + 1).toInt() and 255
                        val bb = buf.get(o + 2).toInt() and 255
                        total++
                        val mx = maxOf(rr, gg, bb)
                        val mn = minOf(rr, gg, bb)
                        if (mx >= 130 && mx - mn < 45) bright++
                    }
                }
                x += step
            }
            y += step
        }
        return total >= 20 && bright.toFloat() / total < 0.88f
    }

    private fun grabFull(img: android.media.Image): IntArray {
        val iw = img.width; val ih = img.height
        val p0 = img.planes[0]
        val src = p0.buffer.duplicate()
        val rs = p0.rowStride; val ps = p0.pixelStride
        val out = IntArray(iw * ih)
        val row = ByteArray(maxOf(rs, iw * ps))
        for (y in 0 until ih) {
            src.position(y * rs)
            src.get(row, 0, minOf(iw * ps, src.remaining()))
            var o = 0
            val base = y * iw
            for (x in 0 until iw) {
                out[base + x] = (0xFF shl 24) or ((row[o].toInt() and 255) shl 16) or
                        ((row[o + 1].toInt() and 255) shl 8) or (row[o + 2].toInt() and 255)
                o += ps
            }
        }
        return out
    }

    override fun captureFullFrame(onReady: (IntArray?, Int, Int) -> Unit) {
        fullCb = onReady
        fullAfter = SystemClock.elapsedRealtime() + 300
        bg.postDelayed({
            if (fullCb === onReady) { fullCb = null; fullAfter = 0; main.post { onReady(null, 0, 0) } }
        }, 3500)
    }

    // Self-check of the radar-to-pitch map: tracked dots must lie on the pitch (0..1). If a quarter or more of them fall outside for
    // several checks in a row the stored map is stale or wrong: rebuild it from the current capture size.
    private fun mapHealth(now: Long) {
        if (now - lastHealthAt < 2000) return
        lastHealthAt = now
        val d = Native.pitchDebugDots()
        if (d.size < 5) return
        val nh = d[0].toInt()
        val na = d[1].toInt()
        val n = nh + na
        if (n < 8) { badMapRuns = 0; return }
        var out = 0
        var i = 5
        var k = 0
        while (k < n && i + 1 < d.size) {
            val x = d[i]; val y = d[i + 1]
            if (x < -0.08f || x > 1.08f || y < -0.08f || y > 1.08f) out++
            i += 2; k++
        }
        if (out * 4 >= n) badMapRuns++ else badMapRuns = 0
        if (badMapRuns >= 3) {
            badMapRuns = 0
            Diagnostics.note("radar map looks wrong ($out of $n dots off the pitch): rebuilding it")
            main.post { try { applyQuad() } catch (_: Exception) { } }
        }
    }

    private fun applyQuad() {
        driftOrig = null
        val m = curMapping
        val o = prefs.outline()
        if (m == null || o == null || capW < 100 || capH < 100) { Native.setPitchQuad(null); legacyRect = null; return }
        val l = (m.radL * capW).toInt().coerceIn(0, capW - 20)
        val t = (m.radT * capH).toInt().coerceIn(0, capH - 20)
        val cw = (m.radR * capW).toInt().coerceIn(l + 16, capW) - l
        val ch = (m.radB * capH).toInt().coerceIn(t + 16, capH) - t
        val ol = o[0] * capW - l; val ot = o[1] * capH - t
        val orr = o[2] * capW - l; val ob = o[3] * capH - t
        val ok = ol >= -1 && ot >= -1 && orr <= cw + 1 && ob <= ch + 1 && orr - ol > cw * 0.5f && ob - ot > ch * 0.5f
        if (!ok) { Native.setPitchQuad(null); legacyRect = null; return }
        Native.setPitchQuad(floatArrayOf(ol, ot, orr, ot, orr, ob, ol, ob))
        legacyRect = intArrayOf(ol.toInt().coerceIn(0, cw - 8), ot.toInt().coerceIn(0, ch - 8),
            orr.toInt().coerceIn(8, cw), ob.toInt().coerceIn(8, ch))
    }

    private var clutchSent = -1
    private var phaseSent = -2f
    private fun updateClutch() {
        val mode = Session.clutchMode(prefs.clutchPlan, prefs.matchMinutes, prefs.clutchStartPct)
        if (mode != clutchSent) {
            clutchSent = mode
            Native.setClutchMode(mode)
        }
        val ph = Session.playedFraction(prefs.matchMinutes)
        if (Math.abs(ph - phaseSent) >= 0.01f) {
            phaseSent = ph
            Native.setMatchPhase(ph)
        }
    }

    // The radar has shown no players for a long stretch while the game is running: usually the kit colours or the radar box
    // no longer match what is on screen. Say so once in a while instead of playing blind.
    private var blindSince = 0L
    private var blindWarnAt = 0L
    private fun trackBlind(status: Int, now: Long) {
        if (status != Gesture.ST_NOT_LIVE) { blindSince = 0L; return }
        if (blindSince == 0L) blindSince = now
        if (now - blindSince > 25_000L && now - blindWarnAt > 90_000L) {
            blindWarnAt = now
            Diagnostics.note("radar shows no players for 25 s")
            main.post { Toast.makeText(this, "PitchBot cannot see the players. Check the kit colours and the radar box.", Toast.LENGTH_LONG).show() }
        }
    }

    private fun handleResult(res: FloatArray, now: Long, g: Gesture?) {
        updateClutch()
        val peDrives = g != null && (g.status == Gesture.ST_TEAM || ((g.status == Gesture.ST_OPP || g.status == Gesture.ST_LOOSE) && g.valid))
        val status = if (peDrives) 2 else res[0].toInt()
        val sx = if (peDrives) g!!.stickX else res[1]
        val sy = if (peDrives) g!!.stickY else res[2]
        val rawBtn = if (peDrives) (if (g!!.valid) Gesture.buttonFor(g.action) else Btn.NONE) else res[3].toInt()
        var btn = if (rawBtn == Btn.PASS || rawBtn == Btn.THROUGH || rawBtn == Btn.SHOOT || rawBtn == Btn.DASH) rawBtn else Btn.NONE
        var hold = if (peDrives) g!!.durationMs else res[4].toInt()
        if (peDrives && g!!.valid) {
            // defending: hold Dash & Pressure while closing the ball carrier down
            if (g.action == Gesture.A_PRESS && !g.dash) btn = Btn.NONE
            // attacking: dash only when the engine sees open space (Legendary mode)
            if (g.action == Gesture.A_CARRY && g.dash && prefs.legendary && curAnchors?.dashButtonCenter != null) { btn = Btn.DASH; hold = 220 }
        }
        val type = if (peDrives) Gesture.overlayType(g!!.action) else res[6].toInt()
        val dry = prefs.dryRun
        val tap = TapService.inst
        if (tap != null && tap.anchors !== curAnchors) tap.anchors = curAnchors
        if (tap != null) tap.simpleMove = tap.simpleMove || prefs.simpleMove
        if (tap != null && tap.lastCancelWarning && !warnedCancel) {
            warnedCancel = true
            main.post { Toast.makeText(this, "Smooth stick not supported here: switched to simple movement.", Toast.LENGTH_LONG).show() }
        }

        if (status == 0) {
            if (lastStatus != 0) tap?.release()
        } else {
            if (btn != 0) { seq++; Session.addLatency(SystemClock.elapsedRealtime() - now) }
            if (!dry) {
                if (peDrives) tap?.submitGesture(g!!, seq, btn, hold, if (prefs.stunFlick && g!!.action == Gesture.A_SHOT) g.flick else 0)
                else tap?.submit(TapService.Cmd(sx, sy, btn, hold, seq))
            }
            val human = tap?.humanActive() == true
            if (!human) {
                if (btn != 0) overlay.press(btn, ActionType.label(type))
                overlay.stick(sx, sy)
            }
        }
        lastStatus = status
        if (prefs.showDebug && prefs.dryRun && now - lastDebugPush > 150) {
            lastDebugPush = now
            overlay.dots(if (prefs.analytics) Native.pitchDebugDots() else Native.nDebugDots())
        }
    }

    override fun isRunning() = running

    override fun toggleRun() {
        running = !running
        if (running) {
            prefs.pushToEngine()
            if (!colorsSet) {
                main.post { Toast.makeText(this, "Pick your team colour first (press Volume Up).", Toast.LENGTH_LONG).show() }
            } else if (prefs.dryRun) {
                main.post { Toast.makeText(this, "Running in DRY-RUN: nothing is tapped.", Toast.LENGTH_SHORT).show() }
            }
        } else {
            TapService.inst?.release()
        }
        overlay.setRunning(running)
    }

    override fun fullTime() {
        running = false
        TapService.inst?.release()
        saveMatch()
        learnV7()
        shutdown()
    }

    override fun captureRadarBitmap(onReady: (Bitmap?) -> Unit) {
        overlay.setCaptureMode(true)
        val startedAt = SystemClock.elapsedRealtime()
        fun finish(c: Crop?) {
            val bmp = try { c?.let { toBitmap(it) } } catch (_: Exception) { null }
            overlay.setCaptureMode(false)
            main.post { onReady(bmp) }
        }
        fun poll(attempt: Int) {
            val c = lastCrop
            val rightBox = c != null && c.m === curMapping
            val fresh = rightBox && c!!.t >= startedAt + 160
            when {
                fresh -> finish(c)
                attempt >= 25 -> finish(if (rightBox) c else null)
                else -> bg.postDelayed({ poll(attempt + 1) }, 80)
            }
        }
        bg.postDelayed({ poll(0) }, 200)
    }

    private fun toBitmap(c: Crop): Bitmap {
        val argb = IntArray(c.px.size) { c.px[it] or (0xFF shl 24) }
        return Bitmap.createBitmap(argb, c.w, c.h, Bitmap.Config.ARGB_8888)
    }

    override fun applyKitColors(home: Int, away: Int, ball: Int?, attackRight: Boolean?): Boolean {
        if (!Native.applyKit(home, away, ball)) return false
        Session.pick(attackRight)
        Native.resetPitchEngine()
        prefs.saveKit(home, away, ball, Native.getKitBounds())
        colorsSet = true
        return true
    }

    override fun startAutomatic() {
        prefs.pushToEngine()
        Diagnostics.note("run started (dry-run=${prefs.dryRun})")
        running = true
        overlay.setRunning(true)
        if (prefs.dryRun) {
            main.post { Toast.makeText(this, "Running in DRY-RUN: nothing is tapped. Stop the overlay, turn Dry-run off in the PitchBot app (Settings), then start the overlay again.", Toast.LENGTH_SHORT).show() }
        }
    }

    override fun volumeAction(up: Boolean) {
        onVolumeKey(if (up) android.view.KeyEvent.KEYCODE_VOLUME_UP else android.view.KeyEvent.KEYCODE_VOLUME_DOWN)
    }

    private fun onVolumeKey(code: Int): Boolean {
        if (!alive) return false
        when (code) {
            android.view.KeyEvent.KEYCODE_VOLUME_UP -> {
                Toast.makeText(this, "Scanning the radar for team colours...", Toast.LENGTH_SHORT).show()
                Diagnostics.note("Volume Up: rescan colours")
                running = false
                TapService.inst?.release()
                overlay.setRunning(false)
                overlay.startKitSelection()
            }
            android.view.KeyEvent.KEYCODE_VOLUME_DOWN -> {
                Toast.makeText(this, "Full time: saving the match.", Toast.LENGTH_SHORT).show()
                Diagnostics.note("Volume Down: match ended")
                Recorder.stop()
                running = false
                TapService.inst?.release()
                overlay.dismissFullStops()
                saveMatch()
                writeRescueLog()
                learnV7()
                saved = false
                Session.reset()
                clutchSent = -1
                Native.setClutchMode(0)
                Native.nNewMatch()
            }
            else -> return false
        }
        return true
    }

    override fun reuseSavedKit(): Boolean {
        if (!prefs.restoreKitToEngine()) return false
        colorsSet = true
        return true
    }

    override fun optionsChanged() {
        prefs.pushToEngine()
        TapService.inst?.simpleMove = prefs.simpleMove
        overlay.refreshFlags()
    }

    override fun mappingDone() {
        driftBase = null; driftStreak = 0
        curMapping = prefs.mapping()
        curAnchors = prefs.hudAnchors()
        TapService.inst?.anchors = curAnchors
        applyQuad()
    }

    private fun saveMatch(): Boolean {
        if (saved) return true
        val m = Native.nMatchStats()
        val liveSec = m[7].toInt()
        if (liveSec < 60) return false       // no real live play: nothing worth a point on the graph
        val kept = m[0].toInt()
        val lost = m[1].toInt()
        val rec = MatchRec(
            id = "m" + System.currentTimeMillis(), ts = System.currentTimeMillis(),
            poss = if (possTeam + possOpp >= 200L) 100f * possTeam / (possTeam + possOpp) else 100f * m[6],
            passAtt = kept + lost, passOk = kept, shots = m[3].toInt(), losses = m[2].toInt(),
            gf = m[4].toInt(), ga = m[5].toInt(), scored = true, learned = true, version = 0, liveSec = liveSec
        )
        // Keep the pitch engine's shot-level evidence inside the per-match log. The backup exports
        // logs/ but does not export externalFilesDir/RESCUE_ANALYTICS.txt, so previously the most
        // useful aim/power/timing evidence could be missing from a backup.
        val shotTelemetry = try { Native.exportShotLog() } catch (_: Throwable) { "" }
        val mapping = curMapping
        val (screenW, screenH) = realSize(this)
        val matchEvidence = buildString {
            append("\n# PITCHBOT V6.22 MATCH EVIDENCE\n")
            append("result_source=radar_estimate_not_official_score\n")
            append("screen=${screenW}x${screenH}; analytics=${prefs.analytics}; dryRun=${prefs.dryRun}; tapServiceConnected=${TapService.inst != null}\n")
            append("autoShoot=${prefs.autoShoot}; shootHoldMs=${prefs.shootHoldMs}; shotHold=${prefs.shotHold}; rescueRules=${prefs.rescueRules}; shootReach=${prefs.shootReach}\n")
            append("attackDir=${prefs.attackDir}; half=${prefs.half}; sessionDir=${Session.dir}; sessionHalf=${Session.half}; invertX=${prefs.invertX}; invertY=${prefs.invertY}\n")
            if (mapping == null) append("mapping=missing\n") else append(String.format(java.util.Locale.US,
                "mapping=joy(%.4f,%.4f,r=%.4f); pass(%.4f,%.4f); shoot(%.4f,%.4f); radar(%.4f,%.4f,%.4f,%.4f)\n",
                mapping.joyX, mapping.joyY, mapping.joyR, mapping.passX, mapping.passY, mapping.shootX, mapping.shootY,
                mapping.radL, mapping.radT, mapping.radR, mapping.radB))
            append("shot_telemetry_begin\n")
            append(if (shotTelemetry.isBlank()) "NO_SHOT_TELEMETRY_CAPTURED\n" else shotTelemetry)
            if (shotTelemetry.isNotBlank() && !shotTelemetry.endsWith("\n")) append('\n')
            append("shot_telemetry_end\n")
        }
        store.add(rec, Native.nExportLog() + matchEvidence)
        if (shotTelemetry.isNotBlank()) try { Native.clearShotLog() } catch (_: Throwable) { }
        prefs.absorbTrackerStats(Native.takeTrackerStats())
        prefs.pushToEngine()
        possTeam = 0L
        possOpp = 0L
        saved = true
        return true
    }

    // ---- learned memory: policy / sequence / defence / meta nets, replay buffer, opponent embeddings, timing and set-piece bandits.
    // One text file under models/ (so the backup zip carries it). Loaded once at start, trained and saved at the end of every match.
    private fun v7File(): java.io.File = java.io.File(java.io.File(filesDir, "models").apply { mkdirs() }, "brain617.txt")

    private fun loadV7() {
        try {
            val f = v7File()
            if (!f.exists()) return
            val ok = Native.v7Import(f.readText())
            Diagnostics.note("v6.17 memory " + (if (ok) "loaded" else "refused (damaged file, starting fresh)"))
        } catch (e: Throwable) {
            Diagnostics.note("v6.17 memory not loaded: " + e.message)
        }
    }

    private fun learnV7() {
        try {
            val rep = Native.v7EndMatch()
            Diagnostics.note(rep)
            if (rep.contains("no live play")) return
            val f = v7File()
            val tmp = java.io.File(f.parentFile, f.name + ".tmp")
            tmp.writeText(Native.v7Export())
            if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
            applyGate(f)
            val dir = getExternalFilesDir(null) ?: filesDir
            java.io.File(dir, "MATCH_SUMMARY.txt").appendText(rep + "\n")
        } catch (e: Throwable) {
            Diagnostics.note("v6.17 learning step failed: " + e.message)
        }
    }

    // regression gate: every fourth match is measured and never trained on. When the engine decides, the brain file is copied
    // to brain617.good.txt (promote) or replaced by it (rollback). A rollback restores the networks but keeps the gate history.
    private fun applyGate(f: java.io.File) {
        try {
            val g = Native.v8Gate().split("|", limit = 3)
            if (g.size < 3 || g[1] != "1") return
            val good = java.io.File(f.parentFile, "brain617.good.txt")
            val verdict = g[0].toIntOrNull() ?: return
            if (verdict == 1) {
                f.copyTo(good, overwrite = true)
                Diagnostics.note("v6.18 gate: promoted (" + g[2] + ")")
            } else if (verdict == 3) {
                if (good.exists()) {
                    val keepGate = Native.v8Export()
                    val ok = Native.v7Import(good.readText())
                    if (ok) {
                        Native.v8Import(keepGate)
                        val tmp = java.io.File(f.parentFile, f.name + ".tmp")
                        tmp.writeText(Native.v7Export())
                        if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
                        Diagnostics.note("v6.18 gate: ROLLED BACK to the known-good brain (" + g[2] + ")")
                    } else Diagnostics.note("v6.18 gate: rollback wanted but the known-good file is damaged; keeping the current brain")
                } else Diagnostics.note("v6.18 gate: regression seen but no known-good brain exists yet (" + g[2] + ")")
            }
            Native.v8GateAck()
            val dir = getExternalFilesDir(null) ?: filesDir
            java.io.File(dir, "MATCH_SUMMARY.txt").appendText("gate: " + g[2] + "\n")
        } catch (e: Throwable) {
            Diagnostics.note("v6.18 gate step failed: " + e.message)
        }
    }

    // Appends a short plain-language read of the match (possession, territory, pass keeping, opponent line, recoveries) to
    // MATCH_SUMMARY.txt. Skipped when there was no live play (the engine says "Not enough ...").
    private fun writeMatchSummary() {
        try {
            val sum = Native.matchSummary()
            if (sum.isBlank() || sum.startsWith("Not enough")) return
            val dir = getExternalFilesDir(null) ?: filesDir
            val f = java.io.File(dir, "MATCH_SUMMARY.txt")
            val head = "# match " + java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.US).format(java.util.Date()) + "\n"
            var old = if (f.exists()) f.readText() else ""
            if (old.length > 100_000) old = old.substring(old.length - 50_000)
            f.writeText(old + head + sum + buildMatchReview())
        } catch (e: Exception) {
            Diagnostics.note("match summary not written: " + e.message)
        }
    }

    // Statistical after-action review. Uses saved match records only; it does not invent a cause
    // (for example, a through-ball goal) unless the engine actually measured that event.
    private fun buildMatchReview(): String {
        return try {
            val all = store.list().sortedBy { it.ts }
            if (all.isEmpty()) return "\nSELF-REVIEW: No saved match record is available yet.\n"
            val current = all.last()
            val previous = all.dropLast(1).takeLast(5)
            val out = StringBuilder("\nSELF-REVIEW — MATCH ${all.size}\n")
            out.append("Radar-estimated result: ${current.gf}-${current.ga}; ")
            out.append(when {
                current.gf > current.ga -> "win.\n"
                current.gf < current.ga -> "loss.\n"
                else -> "draw.\n"
            })
            val mins = (current.liveSec / 60f).coerceAtLeast(1f)
            out.append("Recorded rates: ${(current.poss).toInt()}% possession estimate, ${current.passAcc().times(100).toInt()}% pass keeping, ${"%.2f".format(java.util.Locale.US, current.losses / mins)} tracked losses/min, ${current.shots} shot commands.\n")
            out.append("Score warning: radar-derived goals can be wrong when the ball is briefly lost or the radar mapping is off; verify the in-game scoreboard before using this result as ground truth.\n")
            if (current.ga >= 3) out.append("Priority: review defensive transitions and central protection; the available summary cannot attribute goals to a specific pass type.\n")
            if (current.passAtt >= 8 && current.passAcc() < 0.72f) out.append("Priority: raise pass safety; inspect receiver confidence, pressure and long/through-ball selection.\n")
            if (current.shots >= 5 && current.gf == 0) out.append("Priority: inspect shot quality and finishing execution; shot count alone does not establish chance quality.\n")
            if (current.poss >= 60f && current.shots <= 2) out.append("Priority: turn safe possession into more final-third entries and higher-quality chances.\n")
            if (previous.isNotEmpty()) {
                val n = previous.size.toFloat()
                val prevGf = previous.map { it.gf }.average()
                val prevGa = previous.map { it.ga }.average()
                val prevPoss = previous.map { it.poss.toDouble() }.average()
                val prevPass = previous.map { it.passAcc().toDouble() }.average()
                out.append("Before/after vs previous ${previous.size} match(es): goals for ${"%.2f".format(java.util.Locale.US, prevGf)} -> ${"%.2f".format(java.util.Locale.US, current.gf.toFloat())}; goals against ${"%.2f".format(java.util.Locale.US, prevGa)} -> ${current.ga}; possession ${"%.1f".format(java.util.Locale.US, prevPoss)}% -> ${"%.1f".format(java.util.Locale.US, current.poss)}%; pass keeping ${"%.1f".format(java.util.Locale.US, prevPass*100)}% -> ${"%.1f".format(java.util.Locale.US, current.passAcc()*100)}%.\n")
                val recentWins = previous.count { it.gf > it.ga }
                out.append("Recent baseline: $recentWins/${previous.size} wins. Treat this as a trend, not proof of causation; compare like-for-like opponents and settings.\n")
            } else out.append("Before/after: comparison will appear after another saved match.\n")
            out.toString()
        } catch (e: Exception) {
            "\nSELF-REVIEW unavailable: ${e.message ?: "record error"}.\n"
        }
    }

    // Appends the shot-suppression telemetry of this session to RESCUE_ANALYTICS.txt (capped at ~400 KB).
    private fun writeRescueLog() {
        writeMatchSummary()
        try {
            val log = Native.exportShotLog()
            if (log.isBlank()) return
            val dir = getExternalFilesDir(null) ?: filesDir
            val f = java.io.File(dir, "RESCUE_ANALYTICS.txt")
            val head = "# session " + java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.US).format(java.util.Date()) +
                " (columns: ms-since-start event dist nearestOpp pressure)\n"
            var old = if (f.exists()) f.readText() else ""
            if (old.length > 400_000) old = old.substring(old.length - 200_000)
            f.writeText(old + head + log)
            Native.clearShotLog()
        } catch (e: Exception) {
            Diagnostics.note("rescue log not written: " + e.message)
        }
    }

    private fun shutdown() {
        if (!alive && projection == null && vd == null) {
            stopSelf()
            return
        }
        val wasRunning = running
        // Save before writeRescueLog clears the shot-event buffer, so a service stop mid-match
        // still preserves the per-shot evidence in the backup-exported match log.
        if (wasRunning || !saved) saveMatch()
        writeRescueLog()
        main.removeCallbacks(watchdog)
        Recorder.stop()
        Diagnostics.note("service stopped")
        Diagnostics.saveSnapshot(this)
        TapService.keyHandler = null
        running = false
        TapService.inst?.release()
        if (wasRunning || !saved) learnV7()
        overlay.hide()
        try { vd?.release() } catch (_: Exception) { }
        vd = null
        try { reader?.close() } catch (_: Exception) { }
        reader = null
        try { projection?.stop() } catch (_: Exception) { }
        projection = null
        thread?.quitSafely()
        thread = null
        alive = false
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        if (alive) shutdown()
        super.onDestroy()
    }
}
