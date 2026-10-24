package com.pitchbot.app

import android.content.Context
import android.content.SharedPreferences

data class Mapping(
    val joyX: Float, val joyY: Float, val joyR: Float,
    val thrX: Float, val thrY: Float,
    val passX: Float, val passY: Float,
    val radL: Float, val radT: Float, val radR: Float, val radB: Float,
    val shootX: Float = -1f, val shootY: Float = -1f,
    val dashX: Float = -1f, val dashY: Float = -1f
) {
    val hasShoot: Boolean get() = shootX >= 0f && shootY >= 0f
    val hasDash: Boolean get() = dashX >= 0f && dashY >= 0f
}

data class ScreenPoint(val x: Float, val y: Float)

data class HudPoint(val nx: Float, val ny: Float, val px: Float, val py: Float) {
    fun at(w: Int, h: Int) = ScreenPoint(nx * w, ny * h)
}

data class HudAnchors(
    val joystickCenter: HudPoint,
    val joystickRadiusN: Float,
    val joystickRadiusPx: Float,
    val passButtonCenter: HudPoint,
    val throughBallCenter: HudPoint,
    val shootButtonCenter: HudPoint?,
    val dashButtonCenter: HudPoint? = null,
    val calibW: Int,
    val calibH: Int
) {
    val stickCenter: HudPoint get() = joystickCenter
    val passCenter: HudPoint get() = passButtonCenter
    val throughCenter: HudPoint get() = throughBallCenter

    companion object {
        const val STICK_REACH = 0.85f

        fun fromMapping(m: Mapping, w: Int, h: Int) = HudAnchors(
            joystickCenter = HudPoint(m.joyX, m.joyY, m.joyX * w, m.joyY * h),
            joystickRadiusN = m.joyR,
            joystickRadiusPx = m.joyR * h,
            passButtonCenter = HudPoint(m.passX, m.passY, m.passX * w, m.passY * h),
            throughBallCenter = HudPoint(m.thrX, m.thrY, m.thrX * w, m.thrY * h),
            shootButtonCenter = if (m.hasShoot) HudPoint(m.shootX, m.shootY, m.shootX * w, m.shootY * h) else null,
            dashButtonCenter = if (m.hasDash) HudPoint(m.dashX, m.dashY, m.dashX * w, m.dashY * h) else null,
            calibW = w,
            calibH = h
        )
    }

    fun joystickPoint(w: Int, h: Int): ScreenPoint = joystickCenter.at(w, h)

    fun stickTarget(sx: Float, sy: Float, w: Int, h: Int): ScreenPoint {
        val c = joystickPoint(w, h)
        var x = if (sx.isFinite()) sx else 0f
        var y = if (sy.isFinite()) sy else 0f
        val len = Math.hypot(x.toDouble(), y.toDouble()).toFloat()
        if (len > 1f) { x /= len; y /= len }
        val reach = joystickRadiusN * h * STICK_REACH
        return ScreenPoint(c.x + x * reach, c.y + y * reach)
    }

    fun buttonPoint(btn: Int, w: Int, h: Int): ScreenPoint? = when (btn) {
        Btn.THROUGH -> throughBallCenter.at(w, h)
        Btn.PASS -> passButtonCenter.at(w, h)
        Btn.SHOOT -> shootButtonCenter?.at(w, h)
        Btn.DASH -> dashButtonCenter?.at(w, h)
        else -> null
    }
}

data class RadarRoi(
    val nl: Float, val nt: Float, val nr: Float, val nb: Float,
    val pl: Float, val pt: Float, val pr: Float, val pb: Float,
    val calibW: Int, val calibH: Int
) {
    fun rect(w: Int, h: Int) = android.graphics.RectF(nl * w, nt * h, nr * w, nb * h)
}

const val KIT_BOUNDS_INTS = 33

class Prefs(ctx: Context) {
    private val appCtx: Context = ctx.applicationContext
    val sp: SharedPreferences = appCtx.getSharedPreferences("pitchbot", Context.MODE_PRIVATE)

    var tactic: Int
        get() = sp.getInt("tactic", 1)
        set(v) { sp.edit().putInt("tactic", v).apply() }
    var aggression: Float
        get() = sp.getFloat("aggression", 0.72f)
        set(v) { sp.edit().putFloat("aggression", v).apply() }
    var shootReach: Float
        get() = sp.getFloat("shootReach", 0.32f)
        set(v) { sp.edit().putFloat("shootReach", v).apply() }
    var passSpeed: Float
        get() = sp.getFloat("passSpeed", 0.88f)
        set(v) { sp.edit().putFloat("passSpeed", v).apply() }
    var attackDir: Int
        get() = sp.getInt("attackDir", -1)
        set(v) { sp.edit().putInt("attackDir", v).apply() }
    var half: Int
        get() = sp.getInt("half", 0)
        set(v) { sp.edit().putInt("half", v).apply() }
    var invertX: Boolean
        get() = sp.getBoolean("invertX", false)
        set(v) { sp.edit().putBoolean("invertX", v).apply() }
    var invertY: Boolean
        get() = sp.getBoolean("invertY", false)
        set(v) { sp.edit().putBoolean("invertY", v).apply() }
    var dryRun: Boolean
        get() = sp.getBoolean("dryRun", false)
        set(v) { sp.edit().putBoolean("dryRun", v).apply() }
    // stunning shot: flick the Shoot button toward the open corner while holding it
    var stunFlick: Boolean
        get() = sp.getBoolean("stunFlick", true)
        set(v) { sp.edit().putBoolean("stunFlick", v).apply() }

    var simpleMove: Boolean
        get() = sp.getBoolean("simpleMove", false)
        set(v) { sp.edit().putBoolean("simpleMove", v).apply() }
    var showDebug: Boolean
        get() = sp.getBoolean("showDebug", false)
        set(v) { sp.edit().putBoolean("showDebug", v).apply() }

    var resumeMs: Int
        get() = sp.getInt("resumeMs", 204)
        set(v) = sp.edit().putInt("resumeMs", v.coerceIn(150, 2000)).apply()

    var holdAware: Boolean
        get() = sp.getBoolean("holdAware", true)
        set(v) = sp.edit().putBoolean("holdAware", v).apply()

    var radarResizePending: Boolean
        get() = sp.getBoolean("radarResizePending", false)
        set(v) { sp.edit().putBoolean("radarResizePending", v).apply() }

    var analytics: Boolean
        get() = sp.getBoolean("analytics", true)
        set(v) { sp.edit().putBoolean("analytics", v).apply() }
    var tempCounter: Float
        get() = sp.getFloat("tempCounter", 0.13f)
        set(v) { sp.edit().putFloat("tempCounter", v).apply() }
    var tempHold: Float
        get() = sp.getFloat("tempHold", 0.46f)
        set(v) { sp.edit().putFloat("tempHold", v).apply() }
    var recordSessions: Boolean
        get() = sp.getBoolean("recordSessions", true)
        set(v) { sp.edit().putBoolean("recordSessions", v).apply() }

    var legendary: Boolean
        get() = sp.getBoolean("legendary", true)
        set(v) { sp.edit().putBoolean("legendary", v).apply() }
    var keepBall: Boolean
        get() = sp.getBoolean("keepBall", true)
        set(v) { sp.edit().putBoolean("keepBall", v).apply() }
    var keepBias: Float
        get() = sp.getFloat("keepBias", 0.75f)
        set(v) { sp.edit().putFloat("keepBias", v).apply() }
    var colorOnly: Boolean
        get() = sp.getBoolean("colorOnly", true)
        set(v) { sp.edit().putBoolean("colorOnly", v).apply() }
    var virtualKeeper: Boolean
        get() = sp.getBoolean("virtualKeeper", false)
        set(v) { sp.edit().putBoolean("virtualKeeper", v).apply() }
    var trackAssist: Boolean
        get() = sp.getBoolean("trackAssist", true)
        set(v) { sp.edit().putBoolean("trackAssist", v).apply() }
    var adaptivePlay: Boolean
        get() = sp.getBoolean("adaptivePlay", true)
        set(v) { sp.edit().putBoolean("adaptivePlay", v).apply() }
    var autoDrift: Boolean
        get() = sp.getBoolean("autoDrift", true)
        set(v) { sp.edit().putBoolean("autoDrift", v).apply() }
    var shotHold: Boolean
        get() = sp.getBoolean("shotHold", true)
        set(v) { sp.edit().putBoolean("shotHold", v).apply() }
    var autoShoot: Boolean
        get() = sp.getBoolean("autoShoot", true)
        set(v) { sp.edit().putBoolean("autoShoot", v).apply() }
    var rescueRules: Boolean
        get() = sp.getBoolean("rescueRules", true)
        set(v) { sp.edit().putBoolean("rescueRules", v).apply() }
    // Upgrade pack: live opponent line, roles, lookahead, loose-ball interception, counter-press, recovery, in-match pass learning
    var upgradePack: Boolean
        get() = sp.getBoolean("upgradePack", true)
        set(v) { sp.edit().putBoolean("upgradePack", v).apply() }
    var shootHoldMs: Int
        get() = sp.getInt("shootHoldMs2", 160)
        set(v) { sp.edit().putInt("shootHoldMs2", v).apply() }
    var clutchPlan: Int
        get() = sp.getInt("clutchPlan", 0)
        set(v) { sp.edit().putInt("clutchPlan", v).apply() }
    var matchMinutes: Float
        get() = sp.getFloat("matchMinutes", 10f)
        set(v) { sp.edit().putFloat("matchMinutes", v).apply() }
    var clutchStartPct: Float
        get() = sp.getFloat("clutchStartPct", 0.80f)
        set(v) { sp.edit().putFloat("clutchStartPct", v).apply() }
    var wPress: Float
        get() = sp.getFloat("wPress", 0.05f)
        set(v) { sp.edit().putFloat("wPress", v).apply() }
    var wOffside: Float
        get() = sp.getFloat("wOffside", 0.30f)
        set(v) { sp.edit().putFloat("wOffside", v).apply() }

    var lastBackupMs: Long
        get() = sp.getLong("lastBackupMs", 0L)
        set(v) { sp.edit().putLong("lastBackupMs", v).apply() }

    val isCalibrated: Boolean get() = sp.getBoolean("mapped", false)

    fun mapping(): Mapping? {
        if (!sp.getBoolean("mapped", false)) return null
        fun g(k: String) = sp.getFloat(k, 0f)
        return Mapping(
            g("joyX"), g("joyY"), g("joyR"), g("thrX"), g("thrY"), g("passX"), g("passY"),
            g("radL"), g("radT"), g("radR"), g("radB"),
            sp.getFloat("shootX", -1f), sp.getFloat("shootY", -1f),
            sp.getFloat("dashX", -1f), sp.getFloat("dashY", -1f)
        )
    }

    fun outline(): FloatArray? {
        if (!sp.contains("olL")) return null
        return floatArrayOf(sp.getFloat("olL", 0f), sp.getFloat("olT", 0f), sp.getFloat("olR", 0f), sp.getFloat("olB", 0f))
    }
    fun saveOutline(l: Float, t: Float, r: Float, b: Float) {
        sp.edit().putFloat("olL", l).putFloat("olT", t).putFloat("olR", r).putFloat("olB", b).apply()
    }
    fun clearOutline() { sp.edit().remove("olL").remove("olT").remove("olR").remove("olB").apply() }

    fun saveMapping(m: Mapping) {
        val (sw, sh) = realSize(appCtx)
        sp.edit()
            .putBoolean("mapped", true)
            .putFloat("joyX", m.joyX).putFloat("joyY", m.joyY).putFloat("joyR", m.joyR)
            .putFloat("thrX", m.thrX).putFloat("thrY", m.thrY)
            .putFloat("passX", m.passX).putFloat("passY", m.passY)
            .putFloat("shootX", m.shootX).putFloat("shootY", m.shootY)
            .putFloat("dashX", m.dashX).putFloat("dashY", m.dashY)
            .putFloat("radL", m.radL).putFloat("radT", m.radT).putFloat("radR", m.radR).putFloat("radB", m.radB)
            .putFloat("roiPl", m.radL * sw).putFloat("roiPt", m.radT * sh)
            .putFloat("roiPr", m.radR * sw).putFloat("roiPb", m.radB * sh)
            .putInt("roiCalibW", sw).putInt("roiCalibH", sh)
            .apply()
        saveHudAnchors(HudAnchors.fromMapping(m, sw, sh))
    }

    fun radarRoi(): RadarRoi? {
        val m = mapping() ?: return null
        return RadarRoi(
            m.radL, m.radT, m.radR, m.radB,
            sp.getFloat("roiPl", 0f), sp.getFloat("roiPt", 0f), sp.getFloat("roiPr", 0f), sp.getFloat("roiPb", 0f),
            sp.getInt("roiCalibW", 0), sp.getInt("roiCalibH", 0)
        )
    }

    fun resetMapping() {
        sp.edit().putBoolean("mapped", false).apply()
    }

    fun saveHudAnchors(a: HudAnchors) {
        val e = sp.edit()
        fun put(name: String, p: HudPoint) {
            e.putFloat("hud${name}Nx", p.nx).putFloat("hud${name}Ny", p.ny)
            e.putFloat("hud${name}Px", p.px).putFloat("hud${name}Py", p.py)
        }
        put("Joystick", a.joystickCenter)
        put("Pass", a.passButtonCenter)
        put("Through", a.throughBallCenter)
        a.shootButtonCenter?.let { put("Shoot", it) }
        e.putBoolean("hudHasShoot", a.shootButtonCenter != null)
        a.dashButtonCenter?.let { put("Dash", it) }
        e.putBoolean("hudHasDash", a.dashButtonCenter != null)
        e.putFloat("hudJoystickRn", a.joystickRadiusN).putFloat("hudJoystickRpx", a.joystickRadiusPx)
        e.putInt("hudCalibW", a.calibW).putInt("hudCalibH", a.calibH)
        e.putBoolean("hudAnchored", true)
        e.apply()
    }

    fun hudAnchors(): HudAnchors? {
        if (!sp.getBoolean("mapped", false)) return null
        if (!sp.getBoolean("hudAnchored", false)) {
            val m = mapping() ?: return null
            val (w, h) = realSize(appCtx)
            val a = HudAnchors.fromMapping(m, w, h)
            saveHudAnchors(a)
            return a
        }
        fun get(name: String) = HudPoint(
            sp.getFloat("hud${name}Nx", 0f), sp.getFloat("hud${name}Ny", 0f),
            sp.getFloat("hud${name}Px", 0f), sp.getFloat("hud${name}Py", 0f)
        )
        return HudAnchors(
            joystickCenter = get("Joystick"),
            joystickRadiusN = sp.getFloat("hudJoystickRn", 0.1f),
            joystickRadiusPx = sp.getFloat("hudJoystickRpx", 0f),
            passButtonCenter = get("Pass"),
            throughBallCenter = get("Through"),
            shootButtonCenter = if (sp.getBoolean("hudHasShoot", false)) get("Shoot") else null,
            dashButtonCenter = if (sp.getBoolean("hudHasDash", false)) get("Dash") else null,
            calibW = sp.getInt("hudCalibW", 0),
            calibH = sp.getInt("hudCalibH", 0)
        )
    }

    var homeRgb: Int?
        get() = sp.getInt("kitHome", -1).takeIf { it >= 0 }
        private set(v) { sp.edit().putInt("kitHome", v ?: -1).apply() }
    var awayRgb: Int?
        get() = sp.getInt("kitAway", -1).takeIf { it >= 0 }
        private set(v) { sp.edit().putInt("kitAway", v ?: -1).apply() }
    var ballRgb: Int?
        get() = sp.getInt("kitBall", -1).takeIf { it >= 0 }
        private set(v) { sp.edit().putInt("kitBall", v ?: -1).apply() }

    val hasKit: Boolean get() = homeRgb != null && awayRgb != null && kitBounds() != null

    fun saveKit(home: Int, away: Int, ball: Int?, bounds: IntArray) {
        val e = sp.edit()
        e.putInt("kitHome", home and 0xFFFFFF).putInt("kitAway", away and 0xFFFFFF).putInt("kitBall", if (ball == null) -1 else ball and 0xFFFFFF)
        for (i in bounds.indices) e.putInt("kitB$i", bounds[i])
        e.putInt("kitBn", bounds.size)
        e.apply()
    }

    fun kitBounds(): IntArray? {
        val n = sp.getInt("kitBn", 0)
        if (n != KIT_BOUNDS_INTS) return null
        return IntArray(n) { sp.getInt("kitB$it", 0) }
    }

    fun clearKit() {
        val e = sp.edit().putInt("kitHome", -1).putInt("kitAway", -1).putInt("kitBall", -1).putInt("kitBn", 0)
        e.apply()
    }

    var stateEst: Boolean
        get() = sp.getBoolean("stateEst", true)
        set(v) { sp.edit().putBoolean("stateEst", v).apply() }

    private fun getD(k: String): Double = sp.getString(k, null)?.toDoubleOrNull() ?: 0.0
    private fun putD(ed: SharedPreferences.Editor, k: String, v: Double) { ed.putString(k, v.toString()) }

    var latMeasuredMs: Float
        get() = sp.getFloat("latMeasured", 40f)
        set(v) { sp.edit().putFloat("latMeasured", v).apply() }
    private val latFixedMs = 60f
    val kalLatencyMs: Float get() = (latMeasuredMs + latFixedMs).coerceIn(40f, 300f)

    val projGain: Float get() {
        val dd = getD("trkDD"); val ad = getD("trkAD")
        val lambda = 40.0
        return ((ad + lambda) / (dd + lambda)).coerceIn(0.6, 1.15).toFloat()
    }
    val radarNoise: Float get() {
        val fr = getD("trkFrames")
        return if (fr < 200.0) 0.6f else Math.sqrt(getD("trkR") / fr).coerceIn(0.25, 2.0).toFloat()
    }
    val trackerSamples: Int get() = getD("trkN").toInt()

    fun absorbTrackerStats(s: DoubleArray?) {
        val lat = Session.takeLatency()
        val ed = sp.edit()
        if (lat != null) ed.putFloat("latMeasured", (0.7f * latMeasuredMs + 0.3f * lat).coerceIn(5f, 240f))
        if (s != null && s.size >= 5 && s[2] >= 30.0 && s[4] >= 50.0) {
            val keep = 0.85
            putD(ed, "trkAD", keep * getD("trkAD") + s[0])
            putD(ed, "trkDD", keep * getD("trkDD") + s[1])
            putD(ed, "trkN", keep * getD("trkN") + s[2])
            putD(ed, "trkR", keep * getD("trkR") + s[3])
            putD(ed, "trkFrames", keep * getD("trkFrames") + s[4])
        }
        ed.apply()
    }
    fun resetTrackerLearning() {
        sp.edit().remove("trkAD").remove("trkDD").remove("trkN").remove("trkR").remove("trkFrames").remove("latMeasured").apply()
    }
    fun trackerSummary(): String =
        "Learned so far: projection keeps ${"%.0f".format(projGain * 100)}% of a runner's speed, radar noise ${"%.2f".format(radarNoise)} m, " +
        "pipeline delay ${"%.0f".format(kalLatencyMs)} ms (${trackerSamples} predictions checked). Updated after every match, stored on this phone."

    fun restoreKitToEngine(): Boolean {
        val home = homeRgb ?: return false
        val away = awayRgb ?: return false
        val b = kitBounds() ?: return false
        if (!Native.setKitBounds(b)) return false
        Native.nSetColors(home, away, ballRgb ?: 0xFFEB3B)
        return true
    }

    fun pushToEngine() {
        val dir = if (Session.dir >= 0) Session.dir else attackDir
        val halfSel = if (this.half == 0 && Session.half > 0) Session.half else this.half
        Native.nSetOptions(tactic, aggression, shootReach, passSpeed, dir, halfSel, invertX, invertY)
        Native.configurePitchEngine(wPress, wOffside, tempCounter, tempHold, dir, invertX, invertY, 150, 350, 0x1234ABCD)
        Native.setPitchExtras(legendary, false, shotHold, shootReach)
        Native.setShootConfig(autoShoot && (mapping()?.hasShoot == true), shootHoldMs, !dryRun)
        Native.setRescueRules(rescueRules)
        Native.setUpgradePack(upgradePack)
        Native.setKeepBall(keepBall, keepBias, colorOnly, virtualKeeper)
        Native.setAdaptive(trackAssist, trackAssist, adaptivePlay, adaptivePlay, adaptivePlay)
        Native.setStateEstimator(stateEst, kalLatencyMs, projGain, radarNoise)
        if (!analytics) Native.nSetObserveOnly(false)
    }
}

object Session {
    private var latSum = 0.0
    private var latN = 0
    @Synchronized fun addLatency(ms: Long) { if (ms in 0..1000) { latSum += ms; latN++ } }
    @Synchronized fun takeLatency(): Float? { val r = if (latN >= 5) (latSum / latN).toFloat() else null; latSum = 0.0; latN = 0; return r }
    @Volatile var dir = -1
    @Volatile var half = 0
    @Volatile var kickoffMs = 0L
    @Volatile var half2Ms = 0L
    fun reset() { dir = -1; half = 0; kickoffMs = 0L; half2Ms = 0L }
    fun pick(attackRight: Boolean?) {
        if (attackRight == null) return
        val d = if (attackRight) 1 else 0
        if (dir >= 0 && dir != d) { half = 2; half2Ms = System.currentTimeMillis() }
        else if (dir < 0) { half = 1; kickoffMs = System.currentTimeMillis() }
        dir = d
    }

    // Played fraction of the match (0..1), or -1 before kickoff is known.
    fun playedFraction(matchMinutes: Float): Float {
        val total = matchMinutes * 60_000f
        if (kickoffMs <= 0L || total <= 0f) return -1f
        val now = System.currentTimeMillis()
        val played = if (half == 2 && half2Ms > 0L) total * 0.5f + (now - half2Ms).toFloat() else (now - kickoffMs).toFloat()
        return (played / total).coerceIn(0f, 1f)
    }

    fun clutchMode(plan: Int, matchMinutes: Float, startPct: Float): Int {
        if (plan <= 0 || kickoffMs <= 0L) return 0
        val total = matchMinutes * 60_000f
        if (total <= 0f) return 0
        val now = System.currentTimeMillis()
        val played = if (half == 2 && half2Ms > 0L) total * 0.5f + (now - half2Ms).toFloat() else (now - kickoffMs).toFloat()
        return if (played / total >= startPct) plan else 0
    }
}
