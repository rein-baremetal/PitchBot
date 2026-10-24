package com.pitchbot.app

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast

interface OverlayHost {
    fun isRunning(): Boolean
    fun toggleRun()
    fun fullTime()

    fun captureRadarBitmap(onReady: (Bitmap?) -> Unit)

    fun applyKitColors(home: Int, away: Int, ball: Int?, attackRight: Boolean? = null): Boolean

    fun reuseSavedKit(): Boolean

    fun startAutomatic()

    fun captureFullFrame(onReady: (IntArray?, Int, Int) -> Unit)

    fun optionsChanged()
    fun mappingDone()

    // same as the hardware Volume Up (pick colours) / Volume Down (full time) keys
    fun volumeAction(up: Boolean)
}

class OverlayManager(private val ctx: Context, private val prefs: Prefs, private val host: OverlayHost) {

    private val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager
    private val ui = Handler(Looper.getMainLooper())

    private var flash: FlashView? = null
    private var wizard: View? = null
    private var stopLeft: View? = null
    private var stopRight: View? = null
    private var capturing = false
    private var setupScheduled = false
    private var bubble: View? = null
    private var bubbleLp: WindowManager.LayoutParams? = null

    private companion object {
        const val SETUP_DELAY_MS = 2 * 60 * 1000L   // 2 minutes before the mapping overlay appears
    }

    private fun lp(w: Int, h: Int, touchable: Boolean): WindowManager.LayoutParams {
        var flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
        if (!touchable) flags = flags or WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
        val p = WindowManager.LayoutParams(w, h, WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY, flags, PixelFormat.TRANSLUCENT)
        p.gravity = Gravity.TOP or Gravity.START
        if (Build.VERSION.SDK_INT >= 28) {
            p.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }
        return p
    }

    fun show() {
        ui.post {
            if (flash == null) {
                val f = FlashView(ctx)
                f.mapping = prefs.mapping()
                f.dry = prefs.dryRun
                f.showDebug = prefs.showDebug
                flash = f
                wm.addView(f, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, false))
            }
            addBubble()
            if (needsSetup()) scheduleSetup()
        }
    }

    // Small always-on-top control strip: works even when the volume keys do not reach the app (some games / phones).
    private fun buildBubble(): View {
        val sp = ctx.getSharedPreferences("pitchbot_bubble", Context.MODE_PRIVATE)
        val col = LinearLayout(ctx)
        col.orientation = LinearLayout.VERTICAL
        val bg = GradientDrawable()
        bg.cornerRadius = ctx.dp(14).toFloat()
        bg.setColor(Color.argb(190, 20, 30, 40))
        col.background = bg
        col.setPadding(ctx.dp(4), ctx.dp(2), ctx.dp(4), ctx.dp(4))

        val p = lp(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, true)
        p.x = sp.getInt("x", ctx.dp(6))
        p.y = sp.getInt("y", ctx.dp(110))
        bubbleLp = p

        val grip = TextView(ctx)
        grip.text = "::::"
        grip.setTextColor(Color.LTGRAY)
        grip.textSize = 12f
        grip.gravity = Gravity.CENTER
        grip.layoutParams = LinearLayout.LayoutParams(ctx.dp(56), ctx.dp(22))
        var sx = 0f
        var sy = 0f
        var ox = 0
        var oy = 0
        grip.setOnTouchListener { _, e ->
            val lpp = bubbleLp
            if (lpp == null) {
                false
            } else {
                when (e.actionMasked) {
                    MotionEvent.ACTION_DOWN -> { sx = e.rawX; sy = e.rawY; ox = lpp.x; oy = lpp.y }
                    MotionEvent.ACTION_MOVE -> {
                        lpp.x = ox + (e.rawX - sx).toInt()
                        lpp.y = oy + (e.rawY - sy).toInt()
                        try { wm.updateViewLayout(col, lpp) } catch (_: Exception) { }
                    }
                    MotionEvent.ACTION_UP -> sp.edit().putInt("x", lpp.x).putInt("y", lpp.y).apply()
                }
                true
            }
        }
        col.addView(grip)

        fun chip(label: String, color: Int, onClick: () -> Unit) {
            val t = TextView(ctx)
            t.text = label
            t.setTextColor(Color.WHITE)
            t.textSize = 10f
            t.gravity = Gravity.CENTER
            val b = GradientDrawable()
            b.cornerRadius = ctx.dp(10).toFloat()
            b.setColor(color)
            t.background = b
            val lpp = LinearLayout.LayoutParams(ctx.dp(56), ctx.dp(34))
            lpp.topMargin = ctx.dp(3)
            t.layoutParams = lpp
            t.setOnClickListener { onClick() }
            col.addView(t)
        }
        chip("COLOUR", Color.parseColor("#2E7D32")) { host.volumeAction(true) }
        chip("PAUSE/GO", Color.parseColor("#1565C0")) { host.toggleRun() }
        chip("FULL TIME", Color.parseColor("#C62828")) { host.volumeAction(false) }
        return col
    }

    private fun addBubble() {
        val v = bubble ?: buildBubble().also { bubble = it }
        if (v.isAttachedToWindow) return
        val p = bubbleLp ?: return
        try { wm.addView(v, p) } catch (_: Exception) { }
    }

    // Called every couple of seconds by the service: puts back any overlay window the system dropped.
    fun ensure() {
        ui.post {
            val f = flash ?: return@post
            if (!f.isAttachedToWindow) {
                try { wm.addView(f, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, false)) } catch (_: Exception) { }
            }
            addBubble()
        }
    }

    // Control-mapping / radar setup screens only pop up after a delay, so the game can be opened first.
    private val setupRunnable = Runnable {
        setupScheduled = false
        if (flash == null || wizard != null) return@Runnable
        if (prefs.mapping() == null) startWizard()
        else if (prefs.mapping()?.hasShoot == false) startShootWizard()
        else if (prefs.mapping()?.hasDash == false) startDashWizard(null)
        else if (prefs.radarResizePending) { prefs.radarResizePending = false; startRadarResize() }
    }

    private fun needsSetup(): Boolean =
        prefs.mapping() == null || prefs.mapping()?.hasShoot == false || prefs.mapping()?.hasDash == false || prefs.radarResizePending

    private fun scheduleSetup() {
        if (setupScheduled || wizard != null) return
        setupScheduled = true
        ui.postDelayed(setupRunnable, SETUP_DELAY_MS)
        Toast.makeText(ctx, "Open your game now. The control-mapping screen will appear in 2 minutes.", Toast.LENGTH_LONG).show()
    }

    fun hide() {
        ui.removeCallbacks(setupRunnable)
        setupScheduled = false
        ui.post {
            removeFullStops()
            safeRemove(wizard); wizard = null
            safeRemove(flash); flash = null
            safeRemove(bubble); bubble = null; bubbleLp = null
        }
    }

    private fun safeRemove(v: View?) {
        if (v == null) return
        try { wm.removeView(v) } catch (_: Exception) { }
    }

    fun press(button: Int, label: String) { ui.post { flash?.press(button, label) } }
    fun stick(sx: Float, sy: Float) { ui.post { flash?.stick(sx, sy) } }
    fun dots(d: FloatArray?) { ui.post { if (capturing) return@post; flash?.dots = d; if (prefs.showDebug) flash?.postInvalidateOnAnimation() } }

    fun setCaptureMode(hide: Boolean) {
        ui.post {
            capturing = hide
            bubble?.visibility = if (hide) View.INVISIBLE else View.VISIBLE
            flash?.let { it.captureHidden = hide; if (hide) it.dots = null }
        }
    }
    fun setRunning(r: Boolean) { }
    fun refreshFlags() { ui.post { flash?.dry = prefs.dryRun; flash?.showDebug = prefs.showDebug; flash?.invalidate() } }

    private val fullSteps = listOf(
        "Tap the CENTRE of the joystick",
        "Tap the OUTER EDGE of the joystick ring",
        "Tap the THROUGH button (Through / Match-up)",
        "Tap the PASS button (Pass / Switch)",
        "Tap the SHOOT button",
        "Tap the radar's TOP-LEFT corner (the pitch outline)",
        "Tap the radar's BOTTOM-RIGHT corner"
    )

    private fun mappingFromPoints(pts: List<Pair<Float, Float>>, w: Int, h: Int): Mapping {
        fun fx(i: Int) = pts[i].first / w
        fun fy(i: Int) = pts[i].second / h
        val joyR = Math.hypot((pts[1].first - pts[0].first).toDouble(), (pts[1].second - pts[0].second).toDouble()).toFloat() / h
        val l = minOf(fx(5), fx(6)); val r = maxOf(fx(5), fx(6))
        val t = minOf(fy(5), fy(6)); val b = maxOf(fy(5), fy(6))
        return Mapping(fx(0), fy(0), joyR.coerceAtLeast(0.04f), fx(2), fy(2), fx(3), fy(3), l, t, r, b, fx(4), fy(4))
    }

    private fun startWizard() {
        if (wizard != null) return
        val (w, h) = realSize(ctx)
        val v = MappingWizard(ctx, w, h, fullSteps, null) { pts ->
            val m = mappingFromPoints(pts, w, h)
            prefs.saveMapping(m)
            safeRemove(wizard); wizard = null
            flash?.mapping = m
            host.mappingDone()
            startDashWizard { startRadarResize() }
        }
        wizard = v
        wm.addView(v, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, true))
    }

    private fun startShootWizard() {
        if (wizard != null) return
        val old = prefs.mapping() ?: return
        val (w, h) = realSize(ctx)
        val v = MappingWizard(ctx, w, h, listOf("Tap the SHOOT button"), null) { pts ->
            val m = old.copy(shootX = pts[0].first / w, shootY = pts[0].second / h)
            prefs.saveMapping(m)
            safeRemove(wizard); wizard = null
            flash?.mapping = m
            host.mappingDone()
            prefs.pushToEngine()
            startDashWizard(null)
        }
        wizard = v
        wm.addView(v, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, true))
    }

    // The DASH / "Dash & Pressure" button is one round button in the lower right. It lets the bot press
    // when defending and dash into open space.
    private fun startDashWizard(then: (() -> Unit)?) {
        if (wizard != null) return
        val old = prefs.mapping()
        if (old == null || old.hasDash) { then?.invoke(); return }
        val (w, h) = realSize(ctx)
        val v = MappingWizard(ctx, w, h, listOf("Tap the DASH button (round button, lower right, says Dash or Dash & Pressure)"), null) { pts ->
            val m = old.copy(dashX = pts[0].first / w, dashY = pts[0].second / h)
            prefs.saveMapping(m)
            safeRemove(wizard); wizard = null
            flash?.mapping = m
            host.mappingDone()
            prefs.pushToEngine()
            then?.invoke()
        }
        wizard = v
        wm.addView(v, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, true))
    }

    private fun startRadarResize() {
        if (wizard != null) return
        val old = prefs.mapping()
        if (old == null) { startWizard(); return }
        val (w, h) = realSize(ctx)
        val start = RectF(old.radL * w, old.radT * h, old.radR * w, old.radB * h)
        val v = RadarCropView(ctx, w, h, start) { box, outline ->
            safeRemove(wizard); wizard = null
            if (box == null) return@RadarCropView
            val m = old.copy(radL = box.left / w, radT = box.top / h, radR = box.right / w, radB = box.bottom / h)
            prefs.saveMapping(m)
            if (outline != null) prefs.saveOutline(outline.left / w, outline.top / h, outline.right / w, outline.bottom / h)
            else prefs.clearOutline()
            flash?.mapping = m
            host.mappingDone()
            startKitSelection()
        }
        wizard = v
        wm.addView(v, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, true))
    }

    private inner class RadarCropView(
        ctx: Context, private val w: Int, private val h: Int, start: RectF,
        private val done: (RectF?, RectF?) -> Unit
    ) : View(ctx) {
        private val box = RectF(start)
        private val p = Paint(Paint.ANTI_ALIAS_FLAG)
        private val bmpPaint = Paint().apply { isFilterBitmap = false }
        private val handleR = ctx.dp(30).toFloat()
        private val minSide = ctx.dp(40).toFloat()
        private var mode = 0
        private var corner = 0
        private var lx = 0f
        private var ly = 0f
        private var downBtn = 0
        private val okRect = RectF()
        private val cancelRect = RectF()
        private val autoRect = RectF()

        private var snapPx: IntArray? = null
        private var snap: Bitmap? = null
        private var snapW = 0
        private var snapH = 0
        private var outline: RectF? = null
        private var busy = false
        private var note = ""
        private var fitText = ""
        private var kitText = ""
        private var good = false
        private var conf = 0
        private var fromSaved = false
        private var retried = false

        private val pinch = ScaleGestureDetector(ctx, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(d: ScaleGestureDetector): Boolean {
                val f = d.scaleFactor
                val cx = box.centerX(); val cy = box.centerY()
                val nw = (box.width() * f).coerceIn(minSide, w.toFloat())
                val nh = (box.height() * f).coerceIn(minSide, h.toFloat())
                box.set(cx - nw / 2, cy - nh / 2, cx + nw / 2, cy + nh / 2)
                clampInside()
                invalidate()
                return true
            }
            override fun onScaleEnd(d: ScaleGestureDetector) { evaluate(); invalidate() }
        })

        override fun onAttachedToWindow() {
            super.onAttachedToWindow()
            post { refresh(true) }
        }

        private fun clampInside() {
            var dx = 0f; var dy = 0f
            if (box.left < 0) dx = -box.left
            if (box.right > w) dx = w - box.right
            if (box.top < 0) dy = -box.top
            if (box.bottom > h) dy = h - box.bottom
            box.offset(dx, dy)
        }

        private fun cornerX(i: Int) = if (i == 0 || i == 3) box.left else box.right
        private fun cornerY(i: Int) = if (i < 2) box.top else box.bottom

        private fun refresh(fit: Boolean) {
            if (busy) return
            busy = true
            note = "Looking at the game..."
            setCaptureMode(true)
            visibility = INVISIBLE
            host.captureFullFrame { px, fw, fh ->
                setCaptureMode(false)
                visibility = VISIBLE
                busy = false
                if (px == null || fw < 50 || fh < 50) {
                    if (!retried) { retried = true; note = "No frame yet, trying again..."; invalidate(); postDelayed({ refresh(fit) }, 600); return@captureFullFrame }
                    note = "Could not grab a frame. Make sure the match is on screen, then press Auto-fit."
                    evaluate()
                    invalidate()
                    return@captureFullFrame
                }
                retried = false
                snapPx = px; snapW = fw; snapH = fh
                snap = try { Bitmap.createBitmap(px, fw, fh, Bitmap.Config.ARGB_8888) } catch (_: Exception) { null }
                detect()
                val o = outline
                if (o != null && !fromSaved && fit && conf >= 50) fitToOutline()
                note = when {
                    o == null -> "Radar outline not found: fit the box by hand (pinch / drag)."
                    fromSaved -> "Outline not detected now; showing the saved one."
                    conf < 50 -> "Outline found but unsure ($conf%): check the cyan rectangle, adjust by hand."
                    fit -> "Auto-fitted (confidence $conf%)."
                    else -> "Outline found ($conf%)."
                }
                evaluate()
                invalidate()
            }
        }

        private fun sc() = if (snapW > 0) snapW.toFloat() / w else 1f

        private fun detect() {
            val px = snapPx ?: return
            val s = sc()
            val mx = maxOf(ctx.dp(20).toFloat(), 0.3f * box.width())
            val my = maxOf(ctx.dp(20).toFloat(), 0.3f * box.height())
            val o = fitRadarOutline(px, snapW, snapH,
                ((box.left - mx) * s).toInt(), ((box.top - my) * s).toInt(),
                ((box.right + mx) * s).toInt(), ((box.bottom + my) * s).toInt())
            fromSaved = false
            conf = o?.get(4) ?: 0
            outline = o?.let { RectF(it[0] / s, it[1] / s, it[2] / s, it[3] / s) }
            if (outline == null) {
                val sv = prefs.outline()
                if (sv != null && sv[2] > sv[0] && sv[3] > sv[1]) {
                    val r = RectF(sv[0] * w, sv[1] * h, sv[2] * w, sv[3] * h)
                    if (r.centerX() > box.left - mx && r.centerX() < box.right + mx && r.centerY() > box.top - my && r.centerY() < box.bottom + my) {
                        outline = r; fromSaved = true; conf = 0
                    }
                }
            }
        }

        private fun fitToOutline() {
            val o = outline ?: return
            val m = maxOf(4f, 0.005f * snapW) / sc()
            box.set(o.left - m, o.top - m, o.right + m, o.bottom + m)
            clampInside()
        }

        private fun evaluate() {
            val px = snapPx
            val s = sc()
            val o = outline
            good = false
            fitText = if (o == null) "Fit: outline unknown" else {
                val ml = (o.left - box.left) * s; val mt = (o.top - box.top) * s
                val mr = (box.right - o.right) * s; val mb = (box.bottom - o.bottom) * s
                val lo = minOf(minOf(ml, mt), minOf(mr, mb))
                val hi = maxOf(maxOf(ml, mt), maxOf(mr, mb))
                when {
                    lo < 3f -> { "Too tight: grow the box (the pitch edge or a ball on the line can be cut)" }
                    hi > 16f -> { "Too loose: shrink the box" }
                    else -> { good = true; "Fit OK" }
                }
            }
            kitText = "Kits: -"
            if (px != null && snapW > 0) {
                val cl = (box.left * s).toInt().coerceIn(0, snapW - 8)
                val ct = (box.top * s).toInt().coerceIn(0, snapH - 8)
                val cw = ((box.right * s).toInt() - cl).coerceIn(8, snapW - cl)
                val ch = ((box.bottom * s).toInt() - ct).coerceIn(8, snapH - ct)
                val crop = IntArray(cw * ch)
                for (y in 0 until ch) System.arraycopy(px, (ct + y) * snapW + cl, crop, y * cw, cw)
                val teams = try { extractTeamColors(crop, cw, ch) } catch (_: Exception) { emptyList() }
                kitText = if (teams.size >= 2)
                    "Kits found: ${rgbHex(teams[0].rgb)} x${teams[0].blobs}  |  ${rgbHex(teams[1].rgb)} x${teams[1].blobs}"
                else "Kits: fewer than two colours found (is play on screen?)"
                if (teams.size < 2) good = false
            }
        }

        override fun onTouchEvent(e: MotionEvent): Boolean {
            pinch.onTouchEvent(e)
            if (pinch.isInProgress || e.pointerCount > 1) { mode = 0; downBtn = 0; return true }
            val x = e.rawX
            val y = e.rawY
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    lx = x; ly = y; mode = 0; downBtn = 0
                    if (okRect.contains(x, y)) downBtn = 1
                    else if (cancelRect.contains(x, y)) downBtn = 2
                    else if (autoRect.contains(x, y)) downBtn = 3
                    else {
                        var best = -1; var bd = handleR
                        for (i in 0..3) {
                            val d = Math.hypot((x - cornerX(i)).toDouble(), (y - cornerY(i)).toDouble()).toFloat()
                            if (d < bd) { bd = d; best = i }
                        }
                        if (best >= 0) { mode = 2; corner = best }
                        else if (box.contains(x, y)) mode = 1
                    }
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = x - lx; val dy = y - ly
                    lx = x; ly = y
                    when (mode) {
                        1 -> { box.offset(dx, dy); clampInside() }
                        2 -> {
                            if (corner == 0 || corner == 3) box.left = (box.left + dx).coerceIn(0f, box.right - minSide)
                            else box.right = (box.right + dx).coerceIn(box.left + minSide, w.toFloat())
                            if (corner < 2) box.top = (box.top + dy).coerceIn(0f, box.bottom - minSide)
                            else box.bottom = (box.bottom + dy).coerceIn(box.top + minSide, h.toFloat())
                        }
                    }
                    invalidate()
                }
                MotionEvent.ACTION_UP -> {
                    when {
                        downBtn == 1 && okRect.contains(x, y) -> done(RectF(box), if (conf >= 50 || fromSaved) outline?.let { RectF(it) } else null)
                        downBtn == 2 && cancelRect.contains(x, y) -> done(null, null)
                        downBtn == 3 && autoRect.contains(x, y) -> refresh(true)
                        mode != 0 -> { evaluate(); invalidate() }
                    }
                    mode = 0; downBtn = 0
                }
                MotionEvent.ACTION_CANCEL -> { mode = 0; downBtn = 0 }
            }
            return true
        }

        override fun onDraw(c: Canvas) {
            val sd = resources.displayMetrics.scaledDensity
            p.style = Paint.Style.FILL
            p.color = Color.argb(120, 0, 0, 0)
            c.drawRect(0f, 0f, width.toFloat(), box.top, p)
            c.drawRect(0f, box.bottom, width.toFloat(), height.toFloat(), p)
            c.drawRect(0f, box.top, box.left, box.bottom, p)
            c.drawRect(box.right, box.top, width.toFloat(), box.bottom, p)
            p.style = Paint.Style.STROKE
            p.strokeWidth = 1.5f
            p.color = Color.argb(110, 255, 255, 255)
            for (k in 1..2) {
                val gx = box.left + box.width() * k / 3f
                val gy = box.top + box.height() * k / 3f
                c.drawLine(gx, box.top, gx, box.bottom, p)
                c.drawLine(box.left, gy, box.right, gy, p)
            }
            outline?.let {
                p.strokeWidth = 2f
                p.color = Color.parseColor("#40E0FF")
                c.drawRect(it, p)
            }
            p.strokeWidth = 3f
            p.color = if (good) Color.parseColor("#80FFA0") else Color.parseColor("#FFC857")
            c.drawRect(box, p)
            p.style = Paint.Style.FILL
            for (i in 0..3) c.drawCircle(cornerX(i), cornerY(i), ctx.dp(9).toFloat(), p)

            val bm = snap
            if (bm != null) {
                val s = sc()
                val pw = width * 0.24f
                val ph = minOf(pw * box.height() / box.width(), height * 0.38f)
                val pl = ctx.dp(12).toFloat(); val pt = ctx.dp(12).toFloat()
                val src = android.graphics.Rect((box.left * s).toInt().coerceIn(0, snapW - 1), (box.top * s).toInt().coerceIn(0, snapH - 1),
                    (box.right * s).toInt().coerceIn(1, snapW), (box.bottom * s).toInt().coerceIn(1, snapH))
                p.color = Color.BLACK
                c.drawRect(pl - 3f, pt - 3f, pl + pw + 3f, pt + ph + 3f, p)
                c.drawBitmap(bm, src, RectF(pl, pt, pl + pw, pt + ph), bmpPaint)
                p.color = Color.WHITE
                p.textAlign = Paint.Align.LEFT
                p.textSize = 11f * sd
                c.drawText("What the bot sees", pl, pt + ph + 16f * sd, p)
            }

            p.textAlign = Paint.Align.CENTER
            p.color = Color.WHITE
            p.textSize = 17f * sd
            val atTop = box.centerY() > height * 0.45f
            val cx = if (atTop) width * 0.62f else width * 0.5f
            val maxW = if (atTop) width * 0.70f else width * 0.9f
            val y0 = if (atTop) height * 0.10f else height * 0.66f
            fun fitSize(txt: String, size: Float): Float {
                p.textSize = size
                val tw = p.measureText(txt)
                return if (tw > maxW) size * maxW / tw else size
            }
            val title = "Fit the box just OUTSIDE the radar outline"
            p.textSize = fitSize(title, 17f * sd)
            c.drawText(title, cx, y0, p)
            p.color = Color.LTGRAY
            val sub = "Auto-fit does it for you  -  or drag inside / drag a corner / pinch"
            p.textSize = fitSize(sub, 12f * sd)
            c.drawText(sub, cx, y0 + 20f * sd, p)

            val bw = 104f * sd; val bh = 38f * sd; val gap = 8f * sd
            val by = y0 + 34f * sd
            autoRect.set(cx - bw / 2, by, cx + bw / 2, by + bh)
            cancelRect.set(autoRect.left - gap - bw, by, autoRect.left - gap, by + bh)
            okRect.set(autoRect.right + gap, by, autoRect.right + gap + bw, by + bh)
            p.color = Color.parseColor("#2E6FD0"); c.drawRoundRect(autoRect, 12f * sd, 12f * sd, p)
            p.color = Color.parseColor("#555555"); c.drawRoundRect(cancelRect, 12f * sd, 12f * sd, p)
            p.color = Color.parseColor("#2E9E55"); c.drawRoundRect(okRect, 12f * sd, 12f * sd, p)
            p.color = Color.WHITE
            p.textSize = 15f * sd
            c.drawText("Auto-fit", autoRect.centerX(), autoRect.centerY() + 5f * sd, p)
            c.drawText("Cancel", cancelRect.centerX(), cancelRect.centerY() + 5f * sd, p)
            c.drawText("Done", okRect.centerX(), okRect.centerY() + 5f * sd, p)

            var ty = by + bh + 22f * sd
            p.textSize = 13f * sd
            p.color = if (good) Color.parseColor("#80FFA0") else Color.parseColor("#FFC857")
            p.textSize = fitSize(fitText, 13f * sd)
            c.drawText(fitText, cx, ty, p); ty += 18f * sd
            p.color = Color.WHITE
            p.textSize = fitSize(kitText, 13f * sd)
            c.drawText(kitText, cx, ty, p); ty += 18f * sd
            p.color = Color.LTGRAY
            val nt = "$note   (${box.width().toInt()} x ${box.height().toInt()} px)"
            p.textSize = fitSize(nt, 12f * sd)
            c.drawText(nt, cx, ty, p)
        }
    }

    private class MappingWizard(
        ctx: Context, private val w: Int, private val h: Int,
        private val steps: List<String>, private val hint: RectF?,
        private val done: (List<Pair<Float, Float>>) -> Unit
    ) : View(ctx) {
        private val pts = ArrayList<Pair<Float, Float>>()
        private val p = Paint(Paint.ANTI_ALIAS_FLAG)

        override fun onTouchEvent(e: MotionEvent): Boolean {
            if (e.action == MotionEvent.ACTION_UP) {
                pts.add(Pair(e.rawX.coerceIn(0f, w.toFloat()), e.rawY.coerceIn(0f, h.toFloat())))
                if (pts.size == steps.size) done(ArrayList(pts)) else invalidate()
            }
            return true
        }

        override fun onDraw(c: Canvas) {
            c.drawColor(Color.argb(140, 0, 0, 0))
            val sd = resources.displayMetrics.scaledDensity
            hint?.let {
                p.style = Paint.Style.STROKE
                p.strokeWidth = 3f
                p.color = Color.argb(200, 255, 255, 255)
                p.pathEffect = DashPathEffect(floatArrayOf(18f, 12f), 0f)
                c.drawRect(it, p)
                p.pathEffect = null
            }
            p.style = Paint.Style.FILL
            p.color = Color.WHITE
            p.textAlign = Paint.Align.CENTER
            p.textSize = 22f * sd
            val i = pts.size.coerceAtMost(steps.size - 1)
            c.drawText("Step ${i + 1} of ${steps.size}", width / 2f, height * 0.22f, p)
            p.color = Color.parseColor("#80FFA0")
            c.drawText(steps[i], width / 2f, height * 0.32f, p)
            p.color = Color.LTGRAY
            p.textSize = 13f * sd
            c.drawText(
                if (hint != null) "The dashed box is the current radar box. Tap two opposite corners of the new one."
                else "Do this once. The game must be open behind this screen, in landscape.",
                width / 2f, height * 0.4f, p
            )
            p.color = Color.parseColor("#80FFA0")
            for ((k, pt) in pts.withIndex()) {
                c.drawCircle(pt.first, pt.second, 10f, p)
                p.textSize = 12f * sd
                c.drawText("${k + 1}", pt.first, pt.second - 16f, p)
            }
        }
    }

    private class FullStopView(ctx: Context, private val rgb: Int) : View(ctx) {
        private val p = Paint(Paint.ANTI_ALIAS_FLAG)
        override fun onDraw(c: Canvas) {
            val cx = width / 2f
            val cy = height / 2f
            val r = minOf(cx, cy)
            p.style = Paint.Style.FILL
            p.color = Color.argb(200, 0, 0, 0)
            c.drawCircle(cx, cy, r, p)
            p.color = Color.WHITE
            c.drawCircle(cx, cy, r * 0.88f, p)
            p.color = Color.rgb((rgb shr 16) and 255, (rgb shr 8) and 255, rgb and 255) or (0xFF shl 24)
            c.drawCircle(cx, cy, r * 0.72f, p)
        }
    }

    fun dismissFullStops() { ui.post { removeFullStops() } }

    private fun removeFullStops() {
        for (v in listOf(stopLeft, stopRight)) {
            if (v == null) continue
            v.visibility = View.GONE
            safeRemove(v)
        }
        stopLeft = null
        stopRight = null
    }

    fun startKitSelection() {
        ui.post {
            removeFullStops()
            if (prefs.mapping() == null || wizard != null) {
                Toast.makeText(ctx, "Set up the radar box first.", Toast.LENGTH_SHORT).show()
                return@post
            }
            host.captureRadarBitmap { bmp ->
                if (bmp == null) {
                    Toast.makeText(ctx, "Could not capture the radar. Open the match, then press Volume Up again.", Toast.LENGTH_LONG).show()
                    return@captureRadarBitmap
                }
                Thread({
                    val found = try { extractTeamColors(bmp) } catch (_: Exception) { emptyList() }
                    ui.post {
                        if (found.size < 2) {
                            Toast.makeText(ctx, "Could not find two team colours on the radar. Press Volume Up again once play is on screen.", Toast.LENGTH_LONG).show()
                        } else {
                            showFullStops(found[0], found[1])
                        }
                    }
                }, "pitchbot-kit-scan").start()
            }
        }
    }

    private fun showFullStops(left: Swatch, right: Swatch) {
        removeFullStops()
        val roi = prefs.radarRoi() ?: return
        val (w, h) = realSize(ctx)
        val rect = roi.rect(w, h)
        val size = ctx.dp(44)
        val gap = ctx.dp(6)
        val clear = (right.cx - left.cx) >= 0.08f

        fun place(v: View, x: Int, y: Int) {
            val p = lp(size, size, true)
            p.x = x.coerceIn(0, (w - size).coerceAtLeast(0))
            p.y = y.coerceIn(0, (h - size).coerceAtLeast(0))
            wm.addView(v, p)
        }

        val tlx = if (rect.left - size - gap >= 0) rect.left - size - gap else rect.left + gap
        val tly = if (rect.top - size - gap >= 0) rect.top - size - gap else rect.top + gap
        val brx = if (rect.right + gap + size <= w) rect.right + gap else rect.right - size - gap
        val bry = if (rect.bottom + gap + size <= h) rect.bottom + gap else rect.bottom - size - gap

        val lv = FullStopView(ctx, left.rgb)
        val rv = FullStopView(ctx, right.rgb)
        lv.setOnClickListener { onFullStopTapped(left, right, if (clear) true else null) }
        rv.setOnClickListener { onFullStopTapped(right, left, if (clear) false else null) }
        try {
            place(lv, tlx.toInt(), tly.toInt())
            place(rv, brx.toInt(), bry.toInt())
        } catch (e: Exception) {
            safeRemove(lv); safeRemove(rv)
            Toast.makeText(ctx, "Cannot show the colour dots: ${e.message}", Toast.LENGTH_LONG).show()
            return
        }
        stopLeft = lv
        stopRight = rv
        Toast.makeText(ctx, if (clear) "Tap YOUR colour: top-left = you are on the left, bottom-right = on the right"
                            else "Tap the dot in YOUR team's colour", Toast.LENGTH_SHORT).show()
    }

    private fun onFullStopTapped(home: Swatch, away: Swatch, attackRight: Boolean?) {
        removeFullStops()
        val ok = host.applyKitColors(home.rgb, away.rgb, null, attackRight)
        if (ok) {
            host.startAutomatic()
            val side = when (attackRight) { true -> "  attack >"; false -> "  < attack"; null -> "" }
            Toast.makeText(ctx, "Home ${rgbHex(home.rgb)}  /  Away ${rgbHex(away.rgb)}$side", Toast.LENGTH_SHORT).show()
        } else {
            Toast.makeText(ctx, "Those two colours are too close to separate on the radar.", Toast.LENGTH_LONG).show()
        }
    }
}
