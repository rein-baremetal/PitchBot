package com.pitchbot.app

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.os.SystemClock
import android.view.View

class FlashView(ctx: Context) : View(ctx) {
    var mapping: Mapping? = null
    var dry = false
    var showDebug = false
    var dots: FloatArray? = null

    @Volatile var captureHidden = false
        set(v) {
            field = v
            if (v) hits.clear()
            postInvalidateOnAnimation()
        }

    private class Hit(val x: Float, val y: Float, val t: Long, val label: String, val life: Long = 450L)

    private val hits = ArrayList<Hit>()
    private var stickX = 0f
    private var stickY = 0f
    private var stickT = 0L
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)

    fun press(button: Int, label: String) {
        if (!dry) return
        val m = mapping ?: return
        val (x, y) = when (button) {
            Btn.THROUGH -> Pair(m.thrX, m.thrY)
            Btn.PASS -> Pair(m.passX, m.passY)
            Btn.SHOOT -> if (m.hasShoot) Pair(m.shootX, m.shootY) else return
            Btn.DASH -> if (m.hasDash) Pair(m.dashX, m.dashY) else return
            else -> return
        }
        hits.add(Hit(x, y, SystemClock.uptimeMillis(), label, if (button == Btn.SHOOT) 1200L else 450L))
        postInvalidateOnAnimation()
    }

    fun stick(sx: Float, sy: Float) {
        if (!dry) return
        stickX = sx
        stickY = sy
        stickT = SystemClock.uptimeMillis()
        postInvalidateOnAnimation()
    }

    override fun onDraw(c: Canvas) {
        if (captureHidden) return
        val m = mapping ?: return
        val w = width.toFloat()
        val h = height.toFloat()
        val now = SystemClock.uptimeMillis()
        var animating = false

        hits.removeAll { now - it.t > it.life }
        for (hit in hits) {
            val a = 1f - (now - hit.t) / hit.life.toFloat()
            p.style = Paint.Style.STROKE
            p.strokeWidth = 5f
            p.color = if (dry) Color.argb((a * 200).toInt(), 255, 193, 7) else Color.argb((a * 220).toInt(), 80, 255, 160)
            c.drawCircle(hit.x * w, hit.y * h, 26f * resources.displayMetrics.density * (1f + 0.2f * (1f - a)), p)
            if (hit.label.isNotEmpty()) {
                p.style = Paint.Style.FILL
                p.textSize = 11f * resources.displayMetrics.scaledDensity
                p.textAlign = Paint.Align.CENTER
                c.drawText(hit.label, hit.x * w, hit.y * h + p.textSize / 3, p)
            }
            animating = true
        }

        if (now - stickT < 250 && (stickX != 0f || stickY != 0f)) {
            val jx = m.joyX * w
            val jy = m.joyY * h
            val r = m.joyR * h
            p.style = Paint.Style.STROKE
            p.strokeWidth = 6f
            p.color = if (dry) Color.argb(170, 255, 193, 7) else Color.argb(190, 80, 255, 160)
            c.drawLine(jx, jy, jx + stickX * r, jy + stickY * r, p)
            p.style = Paint.Style.FILL
            c.drawCircle(jx + stickX * r, jy + stickY * r, 9f, p)
            animating = true
        }

        val d = dots
        if (dry && showDebug && d != null && d.size >= 5) {
            val rect = RectF(m.radL * w, m.radT * h, m.radR * w, m.radB * h)
            p.style = Paint.Style.STROKE
            p.strokeWidth = 2f
            p.color = Color.argb(120, 255, 255, 255)
            c.drawRect(rect, p)
            p.style = Paint.Style.FILL
            val nT = d[0].toInt()
            val nO = d[1].toInt()
            var i = 5
            p.color = Color.argb(230, 255, 64, 160)
            for (k in 0 until nT) {
                if (i + 1 >= d.size) break
                c.drawCircle(rect.left + d[i] * rect.width(), rect.top + d[i + 1] * rect.height(), 5f, p); i += 2
            }
            p.color = Color.argb(230, 255, 255, 255)
            for (k in 0 until nO) {
                if (i + 1 >= d.size) break
                c.drawCircle(rect.left + d[i] * rect.width(), rect.top + d[i + 1] * rect.height(), 5f, p); i += 2
            }
            if (d[2] > 0.5f) {
                p.color = Color.argb(255, 255, 235, 59)
                c.drawCircle(rect.left + d[3] * rect.width(), rect.top + d[4] * rect.height(), 7f, p)
            }
        }
        if (animating) postInvalidateOnAnimation()
    }
}

class PitchMapView(ctx: Context) : View(ctx) {
    private var rate = FloatArray(GX * GY) { 0.2f }
    private var visits = FloatArray(GX * GY)
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)

    companion object { const val GX = 6; const val GY = 4 }

    fun setGrid(grid: FloatArray) {
        if (grid.size >= GX * GY * 2) {
            rate = grid.copyOfRange(0, GX * GY)
            visits = grid.copyOfRange(GX * GY, GX * GY * 2)
            invalidate()
        }
    }

    override fun onMeasure(w: Int, h: Int) {
        val ww = MeasureSpec.getSize(w)
        setMeasuredDimension(ww, (ww * 0.62f).toInt())
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        p.style = Paint.Style.FILL
        p.color = Color.parseColor("#2E7D32")
        c.drawRoundRect(0f, 0f, w, h, 12f, 12f, p)
        val cw = w / GX
        val ch = h / GY
        for (gy in 0 until GY) for (gx in 0 until GX) {
            val i = gy * GX + gx
            val v = visits[i]
            if (v >= 3) {
                val a = ((rate[i] - 0.1f) / 0.5f).coerceIn(0f, 1f)
                p.style = Paint.Style.FILL
                p.color = Color.argb((a * 200).toInt(), 229, 57, 53)
                c.drawRect(gx * cw, gy * ch, (gx + 1) * cw, (gy + 1) * ch, p)
                p.color = Color.WHITE
                p.textAlign = Paint.Align.CENTER
                p.textSize = 12f * resources.displayMetrics.scaledDensity
                c.drawText("${(rate[i] * 100).toInt()}%", gx * cw + cw / 2, gy * ch + ch / 2 + p.textSize / 3, p)
            }
        }
        p.style = Paint.Style.STROKE
        p.strokeWidth = 3f
        p.color = Color.argb(200, 255, 255, 255)
        c.drawRoundRect(1.5f, 1.5f, w - 1.5f, h - 1.5f, 12f, 12f, p)
        c.drawLine(w / 2, 0f, w / 2, h, p)
        c.drawCircle(w / 2, h / 2, h * 0.14f, p)
        c.drawRect(w - w * 0.14f, h * 0.25f, w, h * 0.75f, p)
        p.style = Paint.Style.FILL
        p.textSize = 10f * resources.displayMetrics.scaledDensity
        p.textAlign = Paint.Align.RIGHT
        c.drawText("attack →", w - 8f, 16f * resources.displayMetrics.density, p)
    }
}

class ProgressChartView(ctx: Context) : View(ctx) {
    private var values: List<Float> = emptyList()
    private var trend: List<Float> = emptyList()
    private var learned: List<Boolean> = emptyList()
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)

    fun setData(matches: List<MatchRec>) {
        values = matches.map { it.quality() }
        learned = matches.map { it.learned }
        trend = movingAverage(values, 3)
        invalidate()
    }

    override fun onMeasure(w: Int, h: Int) {
        val ww = MeasureSpec.getSize(w)
        setMeasuredDimension(ww, (ww * 0.50f).toInt())
    }

    private fun xAt(i: Int, n: Int, left: Float, right: Float) =
        if (n <= 1) (left + right) / 2f else left + (right - left) * i / (n - 1).toFloat()

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val dens = resources.displayMetrics.density
        val sd = resources.displayMetrics.scaledDensity
        val left = 30f * dens
        val right = w - 12f * dens
        val top = 12f * dens
        val bottom = h - 22f * dens
        p.style = Paint.Style.STROKE
        p.strokeWidth = 1.5f
        p.color = Color.parseColor("#CFD8DC")
        for (i in 0..4) {
            val y = top + (bottom - top) * i / 4f
            c.drawLine(left, y, right, y, p)
        }
        p.style = Paint.Style.FILL
        p.color = Color.GRAY
        p.textSize = 10f * sd
        p.textAlign = Paint.Align.RIGHT
        c.drawText("1.0", left - 4f * dens, top + 4f * dens, p)
        c.drawText("0.5", left - 4f * dens, (top + bottom) / 2f + 4f * dens, p)
        c.drawText("0", left - 4f * dens, bottom + 4f * dens, p)
        if (values.size < 2) {
            p.textAlign = Paint.Align.CENTER
            p.textSize = 12f * sd
            c.drawText("Play 2+ games to see the graph", w / 2, h / 2, p)
            return
        }
        val n = values.size
        fun yOf(v: Float) = top + (bottom - top) * (1f - v.coerceIn(0f, 1f))

        p.style = Paint.Style.STROKE
        p.strokeWidth = 3f
        p.color = Color.parseColor("#0B7A4B")
        for (i in 1 until n) c.drawLine(xAt(i - 1, n, left, right), yOf(values[i - 1]), xAt(i, n, left, right), yOf(values[i]), p)
        p.style = Paint.Style.FILL
        for (i in 0 until n) c.drawCircle(xAt(i, n, left, right), yOf(values[i]), 5f, p)

        p.style = Paint.Style.STROKE
        p.strokeWidth = 5f
        p.color = Color.parseColor("#FB8C00")
        for (i in 1 until n) c.drawLine(xAt(i - 1, n, left, right), yOf(trend[i - 1]), xAt(i, n, left, right), yOf(trend[i]), p)

        p.strokeWidth = 2.5f
        p.color = Color.parseColor("#1565C0")
        for (i in 0 until n) if (learned.getOrElse(i) { false }) c.drawCircle(xAt(i, n, left, right), yOf(values[i]), 10f, p)

        p.style = Paint.Style.FILL
        p.textSize = 10f * sd
        p.textAlign = Paint.Align.LEFT
        var lx = left
        val ly = h - 6f * dens
        for ((label, col) in listOf("match" to "#0B7A4B", "trend (3)" to "#FB8C00", "○ learned from" to "#1565C0")) {
            p.color = Color.parseColor(col)
            c.drawText(label, lx, ly, p)
            lx += p.measureText(label) + 14f * dens
        }
    }
}

fun movingAverage(v: List<Float>, k: Int): List<Float> =
    v.indices.map { i ->
        val from = maxOf(0, i - k + 1)
        v.subList(from, i + 1).average().toFloat()
    }

fun gamesVerdict(scored: List<MatchRec>): String {
    val n = scored.size
    if (n < 10) return "Too early to tell: ${10 - n} more game(s) are needed before a trend means anything. " +
            "A few matches vary by luck alone."
    val k = minOf(5, n / 2)
    val q = scored.map { it.quality() }
    val recent = q.takeLast(k).average().toFloat()
    val before = q.subList(n - 2 * k, n - k).average().toFloat()
    val d = recent - before
    val first = q.take(k).average().toFloat()
    val last5 = scored.takeLast(5)
    val w = last5.count { it.gf > it.ga }
    val dr = last5.count { it.gf == it.ga }
    val l = last5.count { it.gf < it.ga }
    val mean = q.average().toFloat()
    val sd = Math.sqrt(q.sumOf { ((it - mean) * (it - mean)).toDouble() } / (n - 1)).toFloat()
    val need = maxOf(0.03f, 1.5f * sd * Math.sqrt(2.0 / k).toFloat())
    val trend = when {
        d > need -> "IMPROVING"
        d < -need -> "GETTING WORSE"
        else -> "NO CLEAR CHANGE YET (differences this size happen by luck)"
    }
    return String.format(
        java.util.Locale.US,
        "Trend: %s. Last %d matches %.2f vs %.2f for the %d before (%+.2f, needs more than %.2f to count); since the first %d: %+.2f. Last %d results: %dW %dD %dL.",
        trend, k, recent, before, k, d, need, k, recent - first, last5.size, w, dr, l
    )
}


// dashboard: three small graphs fed by the engine (one point per finished match, plus the running match on the right).
// 0 = defence (clean sheet and goal difference; eval matches ringed), 1 = what the opponent does (pattern rate per minute and how often it worked),
// 2 = learning (profile confidence, evidence sufficiency, change against the known-good brain).
class DashboardGraphView(ctx: Context, private val kind: Int) : View(ctx) {
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private var rows = 0
    private var d = FloatArray(0)
    private var live = FloatArray(0)
    private val tick = object : Runnable {
        override fun run() { refresh(); postDelayed(this, 1500) }
    }

    fun refresh() {
        try { d = Native.v8DashSeries(); rows = d.size / STRIDE; live = Native.v8LiveSeries() } catch (_: Throwable) { rows = 0 }
        invalidate()
    }

    override fun onAttachedToWindow() { super.onAttachedToWindow(); post(tick) }
    override fun onDetachedFromWindow() { removeCallbacks(tick); super.onDetachedFromWindow() }
    override fun onMeasure(w: Int, h: Int) = setMeasuredDimension(MeasureSpec.getSize(w), (MeasureSpec.getSize(w) * 0.42f).toInt())

    private fun col(i: Int) = d[i]
    override fun onDraw(c: Canvas) {
        val w = width.toFloat(); val h = height.toFloat()
        val dens = resources.displayMetrics.density; val sd = resources.displayMetrics.scaledDensity
        val left = 30f * dens; val right = w - 10f * dens; val top = 10f * dens; val bottom = h - 22f * dens
        p.style = Paint.Style.STROKE; p.strokeWidth = 1.2f; p.color = Color.parseColor("#CFD8DC")
        for (i in 0..4) { val y = top + (bottom - top) * i / 4f; c.drawLine(left, y, right, y, p) }
        p.style = Paint.Style.FILL; p.color = Color.GRAY; p.textSize = 10f * sd; p.textAlign = Paint.Align.LEFT
        val title = when (kind) { 0 -> "Defence: clean sheets (green), goal difference (blue)"; 1 -> "Opponent patterns per minute (lines), success when thick"; else -> "Learning: confidence, evidence, change vs known-good" }
        c.drawText(title, left, h - 6f * dens, p)
        if (rows < 1) { p.textAlign = Paint.Align.CENTER; c.drawText("Play a match to fill this graph", w / 2, h / 2, p); return }
        val n = rows
        fun xAt(i: Int) = if (n <= 1) (left + right) / 2f else left + (right - left) * i / (n - 1).toFloat()
        fun y01(v: Float) = top + (bottom - top) * (1f - v.coerceIn(0f, 1f))
        fun line(color: String, width: Float, f: (Int) -> Float?) {
            p.style = Paint.Style.STROKE; p.strokeWidth = width; p.color = Color.parseColor(color)
            var px = 0f; var py = 0f; var have = false
            for (i in 0 until n) {
                val v = f(i)
                if (v == null) { have = false; continue }
                val x = xAt(i); val y = y01(v)
                if (have) c.drawLine(px, py, x, y, p)
                px = x; py = y; have = true
            }
        }
        when (kind) {
            0 -> {
                line("#0B7A4B", 3f) { col(it * STRIDE + 4) }
                line("#1565C0", 2.5f) { 0.5f + col(it * STRIDE + 3) / 6f }          // goal difference -3..+3 mapped to 0..1
                line("#E53935", 1.5f) { (col(it * STRIDE + 6) / 6f) }                // dangerous chances against
                p.style = Paint.Style.STROKE; p.strokeWidth = 2f; p.color = Color.parseColor("#6A1B9A")
                for (i in 0 until n) if (col(i * STRIDE + 1) > 0.5f) c.drawCircle(xAt(i), y01(col(i * STRIDE + 4)), 9f, p)
            }
            1 -> {
                val cols = listOf("#E53935", "#FB8C00", "#8E24AA", "#00897B", "#546E7A")
                for (k in 0 until 5) {
                    val rate = { i: Int -> (col(i * STRIDE + 8 + k) / 4f) }
                    line(cols[k], 2f, rate)
                    p.style = Paint.Style.FILL; p.color = Color.parseColor(cols[k])
                    for (i in 0 until n) { val v = col(i * STRIDE + 13 + k); if (v >= 0f) c.drawCircle(xAt(i), y01(rate(i)), 2f + 5f * v, p) }
                }
            }
            else -> {
                line("#0B7A4B", 3f) { col(it * STRIDE + 18) }
                line("#FB8C00", 2.5f) { col(it * STRIDE + 19) }
                line("#E53935", 2f) { 0.5f + col(it * STRIDE + 20) }               // 0.5 = equal to the known-good brain
            }
        }
        if (live.size >= STRIDE) {
            p.style = Paint.Style.FILL; p.color = Color.DKGRAY; p.textSize = 9f * sd; p.textAlign = Paint.Align.RIGHT
            val extra = when (kind) {
                0 -> "now: against ${live[5].toInt()} shots, ${live[6].toInt()} dangerous"
                1 -> "now: " + (0 until 5).joinToString(" ") { live[8 + it].toInt().toString() }
                else -> "now: conf ${(live[18] * 100).toInt()}%  evidence ${(live[19] * 100).toInt()}%" + (if (live[23] > 0.5f) "  (evaluation match)" else "")
            }
            c.drawText(extra, right, top + 8f * dens, p)
        }
    }

    companion object { const val STRIDE = 24 }
}
