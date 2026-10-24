package com.pitchbot.app

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.accessibilityservice.GestureDescription.StrokeDescription
import android.content.Intent
import android.graphics.Path
import android.graphics.PixelFormat
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.view.accessibility.AccessibilityEvent
import kotlin.math.hypot
import kotlin.math.max

class TapService : AccessibilityService() {

    data class Cmd(
        val sx: Float, val sy: Float, val button: Int, val holdMs: Int,
        val seq: Long, val t: Long = SystemClock.uptimeMillis(),
        val screen: ScreenGesture? = null,
        val flick: Int = 0
    )

    companion object {
        @Volatile var inst: TapService? = null

        const val RESUME_MS = 300L

        private const val RELEASE_SETTLE_MS = 220L

        private const val HOLD_CAP_MS = 15000L

        private const val SELF_TOUCH_GUARD_MS = 180L

        // the bot's own touch can reach the screen later than 180 ms on a busy phone; while a
        // new stroke or button tap is still in flight, keep ignoring it (ends at onCompleted/onCancelled)
        private const val SELF_TOUCH_INFLIGHT_MS = 700L

        // after the bot lets go of the stick the knob animates back and screen frames arrive late;
        // ignore "knob looks displaced" readings for this long so the bot never mistakes itself for you
        private const val KNOB_SELF_IGNORE_MS = 600L

        @Volatile var keyHandler: ((Int) -> Boolean)? = null
    }

    private val main = Handler(Looper.getMainLooper())
    @Volatile private var latest: Cmd? = null
    @Volatile private var pending: Cmd? = null

    @Volatile var anchors: HudAnchors? = null
    @Volatile var simpleMove = false
    @Volatile var lastCancelWarning = false

    @Volatile private var lastHumanTouch = 0L
    @Volatile private var lastSelfInject = 0L
    @Volatile private var startInFlight = false

    @Volatile private var busy = false
    private var doneSeq = 0L
    @Volatile private var heldStick: StrokeDescription? = null
    @Volatile private var lastGestureEnd = 0L

    @Volatile var resumeMs = RESUME_MS
    @Volatile var holdAware = true
    @Volatile private var knobDisplacedAt = 0L

    private fun injectingStick(now: Long = SystemClock.uptimeMillis()) = busy || heldStick != null || now - lastGestureEnd < KNOB_SELF_IGNORE_MS

    fun reportKnob(displaced: Boolean) {
        if (displaced && !injectingStick()) knobDisplacedAt = SystemClock.uptimeMillis()
    }
    private var stickX = 0f
    private var stickY = 0f
    private var cancels = 0

    private var watcher: View? = null

    override fun onServiceConnected() {
        inst = this
        installTouchWatcher()
    }

    override fun onUnbind(intent: Intent?): Boolean {
        inst = null
        removeTouchWatcher()
        return super.onUnbind(intent)
    }

    override fun onDestroy() {
        inst = null
        removeTouchWatcher()
        super.onDestroy()
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {}
    override fun onInterrupt() {}

    override fun onKeyEvent(event: KeyEvent): Boolean {
        val code = event.keyCode
        if (code != KeyEvent.KEYCODE_VOLUME_UP && code != KeyEvent.KEYCODE_VOLUME_DOWN) return false
        val handler = keyHandler ?: return false
        if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
            main.post { try { handler(code) } catch (_: Exception) { } }
        }
        return true
    }

    private fun installTouchWatcher() {
        if (watcher != null) return
        val wm = getSystemService(WINDOW_SERVICE) as WindowManager
        val v = object : View(this) {
            override fun onTouchEvent(e: MotionEvent): Boolean {
                if (e.actionMasked == MotionEvent.ACTION_OUTSIDE) onOutsideTouch()
                return false
            }
        }
        val lp = WindowManager.LayoutParams(
            1, 1, WindowManager.LayoutParams.TYPE_ACCESSIBILITY_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH,
            PixelFormat.TRANSLUCENT
        )
        lp.gravity = Gravity.TOP or Gravity.START
        try {
            wm.addView(v, lp)
            watcher = v
        } catch (_: Exception) {
            watcher = null
        }
    }

    private fun removeTouchWatcher() {
        val v = watcher ?: return
        watcher = null
        try { (getSystemService(WINDOW_SERVICE) as WindowManager).removeView(v) } catch (_: Exception) { }
    }

    private fun onOutsideTouch() {
        val now = SystemClock.uptimeMillis()
        val guard = if (startInFlight) SELF_TOUCH_INFLIGHT_MS else SELF_TOUCH_GUARD_MS
        if (now - lastSelfInject < guard) return
        lastHumanTouch = now
        main.post { next() }
    }

    fun humanActive(now: Long = SystemClock.uptimeMillis()): Boolean {
        val t = lastHumanTouch
        if (t == 0L) return false
        if (now - t < resumeMs) return true
        return holdAware && now - t < HOLD_CAP_MS && now - knobDisplacedAt < RELEASE_SETTLE_MS
    }

    fun submit(c: Cmd) {
        latest = c
        if (c.button != 0) pending = c
        main.post { if (!busy) next() }
    }

    fun submitGesture(g: Gesture, seq: Long, button: Int = -1, holdMs: Int = -1, flick: Int = 0) {
        val a = anchors
        val (w, h) = realSize(this)
        val b = if (button >= 0) button else (Gesture.buttonFor(g.action).takeIf { g.valid } ?: Btn.NONE)
        val hold = if (holdMs >= 0) holdMs else g.durationMs
        val sg = if (a == null) null else if (button >= 0) a.resolve(g.stickX, g.stickY, b, hold, w, h) else g.toScreen(a, w, h)
        submit(Cmd(g.stickX, g.stickY, b, hold, seq, screen = sg, flick = if (b == Btn.SHOOT) flick else 0))
    }

    fun release() {
        latest = Cmd(0f, 0f, 0, 0, doneSeq)
        pending = null
        main.post { if (!busy) next() }
    }

    private fun tapPath(x: Float, y: Float): Path = Path().apply { moveTo(x, y) }

    private fun linePath(x0: Float, y0: Float, x1: Float, y1: Float): Path {
        val p = Path()
        p.moveTo(x0, y0)
        if (hypot(x1 - x0, y1 - y0) < 1f) p.lineTo(x1 + 1f, y1) else p.lineTo(x1, y1)
        return p
    }

    private fun next() {
        val a = anchors ?: return
        val (w, h) = realSize(this)
        val jc = a.joystickPoint(w, h)
        val jx = jc.x
        val jy = jc.y

        val paused = humanActive()
        val c = if (paused) Cmd(0f, 0f, 0, 0, doneSeq) else (latest ?: return)
        val wantStick = hypot(c.sx, c.sy) > 0.05f
        val pb = if (paused) null else pending
        val newButton = pb != null && pb.seq > doneSeq && SystemClock.uptimeMillis() - pb.t < 400
        val cont = !simpleMove
        if (!cont) heldStick = null

        if (!wantStick && !newButton && heldStick == null) return

        val seg = if (cont) 110L else 240L
        val g = GestureDescription.Builder()
        var addedAny = false

        var newStick: StrokeDescription? = null
        var endSX = stickX
        var endSY = stickY
        if (wantStick || heldStick != null) {
            val target = if (wantStick) (c.screen?.stickTo ?: a.stickTarget(c.sx, c.sy, w, h)) else null
            val tx = target?.x ?: stickX
            val ty = target?.y ?: stickY
            val x0 = if (heldStick != null) stickX else jx
            val y0 = if (heldStick != null) stickY else jy
            val path = linePath(x0, y0, tx, ty)
            val shootHold = if (newButton && pb != null && pb.button == Btn.SHOOT) 50L + pb.holdMs else 0L
            val willContinue = cont && wantStick
            val s = heldStick
            val stroke = if (cont && s != null)
                s.continueStroke(path, 0, if (wantStick) max(seg, shootHold) else 30L, willContinue)
            else StrokeDescription(path, 0, max(seg, shootHold), willContinue)
            g.addStroke(stroke)
            addedAny = true
            if (willContinue) newStick = stroke
            endSX = tx
            endSY = ty
        }

        var sentButton = false
        if (newButton && pb != null) {
            val bp = pb.screen?.buttonAt ?: a.buttonPoint(pb.button, w, h)
            if (bp != null) {
                val start = if (wantStick || heldStick != null) 50L else 0L
                // stunning shot: drag the finger sideways across the Shoot button while holding it (curl toward the open corner)
                val path = if (pb.flick != 0 && pb.button == Btn.SHOOT)
                    linePath(bp.x, bp.y, bp.x + (if (pb.flick == 2) 1f else -1f) * 0.075f * h, bp.y)
                else tapPath(bp.x, bp.y)
                g.addStroke(StrokeDescription(path, start, max(Gesture.MIN_TAP_MS, pb.holdMs).toLong()))
                addedAny = true
                sentButton = true
            }
        }
        if (!addedAny) return

        val sentSeq = pb?.seq ?: c.seq
        val startsNewTouch = heldStick == null || sentButton
        if (startsNewTouch) { lastSelfInject = SystemClock.uptimeMillis(); startInFlight = true }
        val tDispatch = SystemClock.uptimeMillis()
        // latency is tracked per command kind (move / pass / shot) and split into queue wait and dispatch overhead
        val latSrc = pb ?: c
        val latKind = when (latSrc.button) { Btn.PASS, Btn.THROUGH, Btn.TOP -> 1; Btn.SHOOT -> 2; else -> 0 }
        val latQueue = (tDispatch - latSrc.t).toFloat().coerceAtLeast(0f)
        val ok = dispatchGesture(g.build(), object : GestureResultCallback() {
            override fun onCompleted(gestureDescription: GestureDescription?) {
                lastGestureEnd = SystemClock.uptimeMillis()
                // dispatch overhead = wall time minus the time the strokes themselves last (feeds the engine's latency lead)
                try {
                    var planned = 0L
                    if (gestureDescription != null) for (i in 0 until gestureDescription.strokeCount) {
                        val st = gestureDescription.getStroke(i)
                        planned = max(planned, st.startTime + st.duration)
                    }
                    val over = (lastGestureEnd - tDispatch - planned).toFloat()
                    if (planned > 0L && over >= 0f && over < 600f) Native.noteTapLatency(over)
                    if (planned > 0L) Native.v8Latency(latKind, latQueue, over.coerceIn(0f, 4000f), if (latQueue > 400f) 1 else 0)
                } catch (_: Throwable) { }
                startInFlight = false
                busy = false
                cancels = 0
                heldStick = newStick
                stickX = endSX
                stickY = endSY
                if (sentButton) doneSeq = max(doneSeq, sentSeq)
                next()
            }

            override fun onCancelled(gestureDescription: GestureDescription?) {
                lastGestureEnd = SystemClock.uptimeMillis()
                try { Native.v8Latency(latKind, latQueue, 0f, 2) } catch (_: Throwable) { }
                startInFlight = false
                busy = false
                heldStick = null
                cancels++
                if (cancels >= 4 && !simpleMove) {
                    simpleMove = true
                    lastCancelWarning = true
                }
                main.postDelayed({ next() }, 20)
            }
        }, main)
        if (ok) busy = true else { startInFlight = false; main.postDelayed({ next() }, 40) }
    }
}
