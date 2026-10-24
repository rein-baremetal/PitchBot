package com.pitchbot.app

import android.content.Context
import android.graphics.Bitmap
import android.util.DisplayMetrics
import android.view.WindowManager
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

fun Context.dp(v: Int): Int = (v * resources.displayMetrics.density + 0.5f).toInt()

fun realSize(ctx: Context): Pair<Int, Int> {
    val dm = DisplayMetrics()
    @Suppress("DEPRECATION")
    (ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.getRealMetrics(dm)
    return Pair(dm.widthPixels, dm.heightPixels)
}

data class Swatch(val rgb: Int, val blobs: Int, val cx: Float = 0.5f)

object Btn {
    const val NONE = 0
    const val TOP = 1
    const val THROUGH = 2
    const val PASS = 3
    const val DASH = 4
    const val SHOOT = 5
}

object ActionType {
    const val NONE = 0
    const val CARRY = 1
    const val PASS = 2
    const val THROUGH = 3
    const val SHOOT = 4
    const val CLEAR = 5
    const val SWITCH = 6
    const val TACKLE = 7
    const val PRESS = 8
    const val CHASE = 9
    const val LOFTED = 10
    const val MATCHUP = 11

    fun label(t: Int) = when (t) {
        PASS -> "Pass"
        THROUGH -> "Through"
        LOFTED -> "Lofted"
        SHOOT -> "Shoot"
        CLEAR -> "Clear"
        SWITCH -> "Switch"
        TACKLE -> "Tackle"
        PRESS -> "Press"
        MATCHUP -> "Match-up"
        else -> ""
    }
}

fun rgbHex(rgb: Int): String = String.format("#%06X", rgb and 0xFFFFFF)

internal fun rgbToHsvInt(r: Int, g: Int, b: Int, out: IntArray) {
    val mx = max(r, max(g, b))
    val mn = min(r, min(g, b))
    val d = mx - mn
    out[2] = mx
    out[1] = if (mx > 0) d * 255 / mx else 0
    if (d == 0) {
        out[0] = 0
        return
    }
    var h = when (mx) {
        r -> 60 * (g - b) / d
        g -> 120 + 60 * (b - r) / d
        else -> 240 + 60 * (r - g) / d
    }
    if (h < 0) h += 360
    if (h >= 360) h -= 360
    out[0] = h
}

private fun hueDist(a: Int, b: Int): Int {
    val d = abs(a - b)
    return if (d > 180) 360 - d else d
}

private const val MAX_ANALYSED_PIXELS = 1 shl 18
private const val MIN_V = 40
private const val ACHROMATIC_S = 56
private const val BG_HUE_BAND = 22
private const val BG_MIN_SHARE = 0.20f
private const val FALLBACK_GREEN_LO = 70
private const val FALLBACK_GREEN_HI = 170

fun extractDominantColors(bmp: Bitmap, maxColors: Int = 6): List<Swatch> {
    var src = bmp
    if (src.width.toLong() * src.height > MAX_ANALYSED_PIXELS) {
        val k = Math.sqrt(MAX_ANALYSED_PIXELS.toDouble() / (src.width.toDouble() * src.height)).coerceAtMost(1.0)
        src = Bitmap.createScaledBitmap(bmp, max(8, (src.width * k).toInt()), max(8, (src.height * k).toInt()), false)
    }
    val w = src.width
    val h = src.height
    if (w < 8 || h < 8) return emptyList()
    val px = IntArray(w * h)
    src.getPixels(px, 0, w, 0, 0, w, h)
    return extractDominantColors(px, w, h, maxColors)
}

fun extractDominantColors(px: IntArray, w: Int, h: Int, maxColors: Int = 6): List<Swatch> {
    if (w < 8 || h < 8 || maxColors <= 0 || px.size.toLong() < w.toLong() * h) return emptyList()
    val n = w * h
    val hsv = IntArray(3)

    val hist = IntArray(36)
    var chromatic = 0
    for (i in 0 until n) {
        val c = px[i]
        rgbToHsvInt((c shr 16) and 255, (c shr 8) and 255, c and 255, hsv)
        if (hsv[1] >= 60 && hsv[2] >= 60) {
            hist[hsv[0] / 10]++
            chromatic++
        }
    }
    var bestBin = 0
    var bestSum = -1
    for (b in 0 until 36) {
        val s = hist[(b + 35) % 36] + hist[b] + hist[(b + 1) % 36]
        if (s > bestSum) { bestSum = s; bestBin = b }
    }
    val pitchHue = bestBin * 10 + 5
    val adaptive = chromatic > 0 && bestSum.toFloat() / n >= BG_MIN_SHARE &&
        pitchHue in (FALLBACK_GREEN_LO - 20)..(FALLBACK_GREEN_HI + 20)

    val keys = IntArray(n)
    for (i in 0 until n) {
        val c = px[i]
        val r = (c shr 16) and 255
        val g = (c shr 8) and 255
        val b = c and 255
        rgbToHsvInt(r, g, b, hsv)
        val hh = hsv[0]
        val ss = hsv[1]
        val vv = hsv[2]
        keys[i] = when {
            vv < MIN_V -> -1
            ss >= 45 && adaptive && hueDist(hh, pitchHue) <= BG_HUE_BAND -> -1
            ss >= 45 && !adaptive && hh in FALLBACK_GREEN_LO..FALLBACK_GREEN_HI -> -1
            ss < ACHROMATIC_S -> 1000 + vv / 64
            else -> (hh / 20) * 4 + (if (ss >= 150) 2 else 0) + (if (vv >= 150) 1 else 0)
        }
    }

    val cells = HashMap<Int, LongArray>()
    for (i in 0 until n) {
        val k = keys[i]
        if (k < 0) continue
        val c = px[i]
        val a = cells.getOrPut(k) { LongArray(4) }
        a[0]++
        a[1] += ((c shr 16) and 255).toLong()
        a[2] += ((c shr 8) and 255).toLong()
        a[3] += (c and 255).toLong()
    }
    val ordered = cells.entries.filter { it.value[0] >= 3 }.sortedByDescending { it.value[0] }
    if (ordered.isEmpty()) return emptyList()
    val clusters = ArrayList<LongArray>()
    val clusterOfKey = HashMap<Int, Int>()
    val ha = IntArray(3)
    val hb = IntArray(3)
    for (e in ordered) {
        val v = e.value
        rgbToHsvInt((v[1] / v[0]).toInt(), (v[2] / v[0]).toInt(), (v[3] / v[0]).toInt(), ha)
        var target = -1
        for ((ci, m) in clusters.withIndex()) {
            rgbToHsvInt((m[1] / m[0]).toInt(), (m[2] / m[0]).toInt(), (m[3] / m[0]).toInt(), hb)
            val bothGrey = ha[1] < ACHROMATIC_S && hb[1] < ACHROMATIC_S
            val bothColour = ha[1] >= ACHROMATIC_S && hb[1] >= ACHROMATIC_S
            val same = (bothGrey && abs(ha[2] - hb[2]) < 48) ||
                (bothColour && hueDist(ha[0], hb[0]) < 15 && abs(ha[1] - hb[1]) < 80 && abs(ha[2] - hb[2]) < 80)
            if (same) { target = ci; break }
        }
        if (target < 0) {
            clusters.add(v.copyOf())
            target = clusters.size - 1
        } else {
            for (q in 0..3) clusters[target][q] += v[q]
        }
        clusterOfKey[e.key] = target
    }
    val cid = IntArray(n) { -1 }
    for (i in 0 until n) {
        val k = keys[i]
        if (k >= 0) cid[i] = clusterOfKey[k] ?: -1
    }

    var groups = collectBlobs(cid, px, w, h, erode = true, minArea = 1)
    if (groups.isEmpty()) groups = collectBlobs(cid, px, w, h, erode = false, minArea = 3)

    return groups.values
        .filter { it.blobs >= 1 }
        .sortedByDescending { it.blobs }
        .take(maxColors)
        .map { Swatch((it.r() shl 16) or (it.g() shl 8) or it.b(), it.blobs, it.side().coerceIn(0f, 1f)) }
}

internal fun isBallYellow(rgb: Int): Boolean {
    val hsv = IntArray(3)
    rgbToHsvInt((rgb shr 16) and 255, (rgb shr 8) and 255, rgb and 255, hsv)
    return hsv[0] in 40..70 && hsv[1] >= 100 && hsv[2] >= 150
}

fun extractTeamColors(px: IntArray, w: Int, h: Int): List<Swatch> {
    val all = extractDominantColors(px, w, h, 6)
    val teams = if (all.size >= 3) all.filterNot { isBallYellow(it.rgb) && it.blobs <= 2 } else all
    return teams.take(2).sortedBy { it.cx }
}

fun extractTeamColors(bmp: Bitmap): List<Swatch> {
    var src = bmp
    if (src.width.toLong() * src.height > MAX_ANALYSED_PIXELS) {
        val k = Math.sqrt(MAX_ANALYSED_PIXELS.toDouble() / (src.width.toDouble() * src.height)).coerceAtMost(1.0)
        src = Bitmap.createScaledBitmap(bmp, max(8, (src.width * k).toInt()), max(8, (src.height * k).toInt()), false)
    }
    val w = src.width
    val h = src.height
    if (w < 8 || h < 8) return emptyList()
    val px = IntArray(w * h)
    src.getPixels(px, 0, w, 0, 0, w, h)
    return extractTeamColors(px, w, h)
}

private class Acc(var blobs: Int, var area: Long, var sr: Long, var sg: Long, var sb: Long, var sx: Double = 0.0) {
    val xs = ArrayList<Float>()
    fun side(): Float {
        if (xs.isEmpty()) return 0.5f
        val s = xs.sorted(); val k = minOf(3, s.size)
        return (s.take(k).average().toFloat() + s.takeLast(k).average().toFloat()) / 2f
    }
    fun r() = if (area > 0) (sr / area).toInt() else 0
    fun g() = if (area > 0) (sg / area).toInt() else 0
    fun b() = if (area > 0) (sb / area).toInt() else 0
}

private fun collectBlobs(cid: IntArray, rgb: IntArray, w: Int, h: Int, erode: Boolean, minArea: Int): Map<Int, Acc> {
    val n = w * h
    val core = IntArray(n) { -1 }
    if (erode) {
        for (y in 1 until h - 1) {
            for (x in 1 until w - 1) {
                val i = y * w + x
                val k = cid[i]
                if (k < 0) continue
                if (cid[i - 1] == k && cid[i + 1] == k && cid[i - w] == k && cid[i + w] == k &&
                    cid[i - w - 1] == k && cid[i - w + 1] == k && cid[i + w - 1] == k && cid[i + w + 1] == k
                ) core[i] = k
            }
        }
    } else {
        System.arraycopy(cid, 0, core, 0, n)
    }

    val maxArea = if (erode) 400 else 600
    val seen = BooleanArray(n)
    val stack = IntArray(n)
    val byCluster = HashMap<Int, Acc>()
    for (start in 0 until n) {
        val k = core[start]
        if (k < 0 || seen[start]) continue
        var sp = 0
        stack[sp++] = start
        seen[start] = true
        var area = 0
        var sr = 0L
        var sg = 0L
        var sb = 0L
        var sx = 0.0
        var x0 = w; var x1 = 0; var y0 = h; var y1 = 0
        while (sp > 0) {
            val i = stack[--sp]
            area++
            val c = rgb[i]
            sr += (c shr 16) and 255
            sg += (c shr 8) and 255
            sb += c and 255
            val x = i % w
            val y = i / w
            sx += x + 0.5
            if (x < x0) x0 = x
            if (x > x1) x1 = x
            if (y < y0) y0 = y
            if (y > y1) y1 = y
            if (x > 0 && !seen[i - 1] && core[i - 1] == k) { seen[i - 1] = true; stack[sp++] = i - 1 }
            if (x < w - 1 && !seen[i + 1] && core[i + 1] == k) { seen[i + 1] = true; stack[sp++] = i + 1 }
            if (y > 0 && !seen[i - w] && core[i - w] == k) { seen[i - w] = true; stack[sp++] = i - w }
            if (y < h - 1 && !seen[i + w] && core[i + w] == k) { seen[i + w] = true; stack[sp++] = i + w }
        }
        if (area < minArea || area > maxArea) continue
        if (area >= 12) {
            val bw = x1 - x0 + 1
            val bh = y1 - y0 + 1
            val aspect = maxOf(bw, bh).toFloat() / minOf(bw, bh)
            if (aspect > 2.2f || area.toFloat() / (bw * bh) < 0.45f) continue
        }
        if (erode && area < 8) {
            val mr = (sr / area).toInt(); val mg = (sg / area).toInt(); val mb = (sb / area).toInt()
            val mx = maxOf(mr, mg, mb); val mn = minOf(mr, mg, mb)
            val sat = if (mx > 0) (mx - mn) * 255 / mx else 0
            if (sat < ACHROMATIC_S) continue
        }
        val acc = byCluster.getOrPut(k) { Acc(0, 0, 0, 0, 0) }
        acc.blobs++
        acc.area += area
        acc.sr += sr
        acc.sg += sg
        acc.sb += sb
        acc.sx += sx
        acc.xs.add((sx / area / w).toFloat())
    }
    return byCluster
}

private fun longestRun(n: Int, gap: Int, on: (Int) -> Boolean): Int {
    var best = 0
    var cur = 0
    var miss = 0
    for (i in 0 until n) {
        if (on(i)) {
            cur += if (cur > 0) 1 + miss else 1
            miss = 0
        } else if (cur > 0) {
            miss++
            if (miss > gap) { best = max(best, cur); cur = 0; miss = 0 }
        }
    }
    return max(best, cur)
}

private fun outlinePass(lum: IntArray, rw: Int, rh: Int, thr: Int, rad: Int): IntArray? {
    val hl = BooleanArray(rw * rh)
    val vl = BooleanArray(rw * rh)
    for (y in rad until rh - rad) for (x in 0 until rw) {
        val v = lum[y * rw + x]
        hl[y * rw + x] = v - max(lum[(y - rad) * rw + x], lum[(y + rad) * rw + x]) >= thr
    }
    for (y in 0 until rh) for (x in rad until rw - rad) {
        val v = lum[y * rw + x]
        vl[y * rw + x] = v - max(lum[y * rw + x - rad], lum[y * rw + x + rad]) >= thr
    }
    val hr = IntArray(rh) { y -> longestRun(rw, 5) { x -> hl[y * rw + x] } }
    val vr = IntArray(rw) { x -> longestRun(rh, 5) { y -> vl[y * rw + x] } }
    val hMax = hr.maxOrNull() ?: 0
    val vMax = vr.maxOrNull() ?: 0
    if (hMax < rw * 0.25f || vMax < rh * 0.25f) return null
    var top = -1; var bot = -1; var lef = -1; var rig = -1
    for (y in 0 until rh) if (hr[y] >= 0.4f * hMax) { if (top < 0) top = y; bot = y }
    for (x in 0 until rw) if (vr[x] >= 0.4f * vMax) { if (lef < 0) lef = x; rig = x }
    val ow = rig - lef
    val oh = bot - top
    if (top < 0 || lef < 0 || oh < 20 || ow < 30) return null
    val aspect = ow.toFloat() / oh
    if (aspect < 1.2f || aspect > 2.4f) return null

    fun hCover(row: Int): Float {
        var n = 0
        for (i in 0 until ow) {
            val x = lef + i
            var any = false
            for (yy in max(0, row - 1)..min(rh - 1, row + 1)) if (hl[yy * rw + x]) any = true
            if (any) n++
        }
        return n.toFloat() / ow
    }
    fun vCover(col: Int): Float {
        var n = 0
        for (i in 0 until oh) {
            val y = top + i
            var any = false
            for (xx in max(0, col - 1)..min(rw - 1, col + 1)) if (vl[y * rw + xx]) any = true
            if (any) n++
        }
        return n.toFloat() / oh
    }
    val conf = minOf(minOf(hCover(top), hCover(bot)), minOf(vCover(lef), vCover(rig)))
    return intArrayOf(lef, top, rig, bot, (conf * 100).toInt())
}

fun fitRadarOutline(px: IntArray, w: Int, h: Int, l0: Int, t0: Int, r0: Int, b0: Int): IntArray? {
    val l = l0.coerceIn(0, w - 1); val t = t0.coerceIn(0, h - 1)
    val r = r0.coerceIn(l + 1, w); val b = b0.coerceIn(t + 1, h)
    val rw = r - l
    val rh = b - t
    if (rw < 40 || rh < 30 || px.size < w * h) return null
    val lum = IntArray(rw * rh)
    for (y in 0 until rh) {
        for (x in 0 until rw) {
            val c = px[(t + y) * w + l + x]
            lum[y * rw + x] = (299 * ((c shr 16) and 255) + 587 * ((c shr 8) and 255) + 114 * (c and 255)) / 1000
        }
    }
    var best: IntArray? = null
    val ladder = intArrayOf(28, 2, 20, 2, 38, 2, 14, 2, 10, 2, 7, 2)
    var k = 0
    while (k < ladder.size) {
        val res = outlinePass(lum, rw, rh, ladder[k], ladder[k + 1])
        k += 2
        if (res == null) continue
        if (res[4] >= 70) { best = res; break }
        if (best == null || res[4] > best[4]) best = res
    }
    val o = best ?: return null
    return intArrayOf(l + o[0], t + o[1], l + o[2], t + o[3], o[4])
}
