package com.pitchbot.app

object Native {
    init {
        System.loadLibrary("pitchbot")
    }

    external fun nInit()
    external fun nNewMatch()

    external fun nScan(px: IntArray, w: Int, h: Int): IntArray

    external fun nSetColors(team: Int, opp: Int, ball: Int)
    external fun nSetOptions(
        tactic: Int, aggression: Float, shootReach: Float, passSpeed: Float,
        attackDir: Int, half: Int, invertX: Boolean, invertY: Boolean
    )

    external fun nProcess(px: IntArray, w: Int, h: Int, tMs: Long): FloatArray

    external fun nDebugDots(): FloatArray

    external fun nStats(): FloatArray

    external fun nExportLog(): String
    external fun nSetWeights(s: String): Boolean
    external fun nExportWeights(): String
    external fun nLearn(logs: String): String
    external fun attackCalibration(logs: String): String

    external fun nLossGrid(): FloatArray

    external fun nProfile(): FloatArray

    external fun nSetObserveOnly(v: Boolean)

    external fun initPitchEngine()
    external fun resetPitchEngine()

    external fun configurePitchEngine(
        wPress: Float, wOffside: Float, tempCounter: Float, tempHold: Float, attackDir: Int,
        invertX: Boolean, invertY: Boolean, decisionMs: Int, cooldownMs: Int, seed: Int
    )

    external fun setPitchQuad(quad: FloatArray?): Boolean

    external fun setTeamColors(homeRgb: Int, awayRgb: Int): Boolean

    external fun setBallColor(ballRgb: Int)

    external fun clearBallColor()

    external fun getKitBounds(): IntArray

    external fun setKitBounds(bounds: IntArray): Boolean

    external fun processPitchFrame(
        buf: java.nio.ByteBuffer?, rowStride: Int, pixelStride: Int,
        roiL: Int, roiT: Int, roiW: Int, roiH: Int, tMs: Long
    ): Long

    external fun setPitchExtras(legendary: Boolean, shotAlert: Boolean, shotHold: Boolean, shootReach: Float)

    external fun setStateEstimator(enabled: Boolean, latencyMs: Float, projGain: Float, r0: Float)

    external fun takeTrackerStats(): DoubleArray?

    external fun setKeepBall(keepBall: Boolean, keepBias: Float, colorOnly: Boolean, virtualKeeper: Boolean)

    external fun setAdaptive(ballCoast: Boolean, occHold: Boolean, context: Boolean, styles: Boolean, antiAdapt: Boolean)

    external fun setClutchMode(mode: Int)
    external fun setMatchPhase(frac: Float)
    external fun nMatchStats(): FloatArray

    external fun setShootConfig(autoShoot: Boolean, holdMs: Int, live: Boolean)

    external fun setRescueRules(on: Boolean)
    external fun setUpgradePack(on: Boolean)
    external fun matchSummary(): String

    // layer: long-term memory of the learned heads, end-of-match training, measured tap latency, host-forced play style
    external fun v7Export(): String
    external fun v7Import(s: String): Boolean
    external fun v7EndMatch(): String
    external fun v7SetStyle(style: Int, lockSeconds: Int)
    external fun noteTapLatency(ms: Float)

    // layer: latency per command kind (0 move, 1 pass, 2 shot, 3 switch, 4 other; flags 1 late, 2 dropped, 4 timeout), regression gate, dashboards, replay
    external fun v8Latency(kind: Int, queueMs: Float, overheadMs: Float, flags: Int)
    external fun v8LatencyReport(): String
    external fun v8Export(): String
    external fun v8Import(s: String): Boolean
    external fun v8Gate(): String
    external fun v8GateAck()
    external fun v8DashSeries(): FloatArray
    external fun v8LiveSeries(): FloatArray
    external fun v8SetRecording(on: Boolean)
    external fun v8Replay(): String
    external fun v8GkReliable(on: Boolean)

    external fun exportShotLog(): String

    external fun clearShotLog()

    external fun setHudPossession(v: Int)

    external fun setHumanControl(on: Boolean)

    external fun pitchDebugDots(): FloatArray

    external fun pitchInfo(): FloatArray

    private const val LEGACY_BALL_HINT = 0xFFEB3B

    fun applyKit(homeRgb: Int, awayRgb: Int, ballRgb: Int?): Boolean {
        if (!setTeamColors(homeRgb, awayRgb)) return false
        if (ballRgb != null) setBallColor(ballRgb) else clearBallColor()
        nSetColors(homeRgb, awayRgb, ballRgb ?: LEGACY_BALL_HINT)
        return true
    }
}
