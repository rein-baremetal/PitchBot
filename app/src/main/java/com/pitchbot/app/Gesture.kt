package com.pitchbot.app

import kotlin.math.cos
import kotlin.math.sin

const val ACTION_NONE = 0
const val ACTION_STICK = 1
const val ACTION_PASS = 2
const val ACTION_THROUGH = 3
const val ACTION_SHOOT = 4
const val ACTION_TACKLE = 5
const val ACTION_SWITCH = 6
const val ACTION_MATCHUP = 7
const val ACTION_PRESS = 8

data class ScreenGesture(
    val kind: Int,
    val stickFrom: ScreenPoint?,
    val stickTo: ScreenPoint?,
    val button: Int,
    val buttonAt: ScreenPoint?,
    val holdMs: Int
) {
    val hasStick: Boolean get() = stickTo != null
    val hasButton: Boolean get() = buttonAt != null
    val isEmpty: Boolean get() = stickTo == null && buttonAt == null

    companion object {
        val EMPTY = ScreenGesture(ACTION_NONE, null, null, Btn.NONE, null, 0)
    }
}

data class Gesture(
    val theta: Float,
    val r: Float,
    val durationMs: Int,
    val action: Int,
    val dash: Boolean,
    val valid: Boolean,
    val status: Int,
    val flick: Int = 0          // stunning shot: 0 none, 1 flick the shoot button left, 2 right
) {
    companion object {
        const val A_NONE = 0
        const val A_CARRY = 1
        const val A_PASS = 2
        const val A_THROUGH = 3
        const val A_LOFTED = 4
        const val A_SHOOT = 5
        const val A_SHOT = 6
        const val A_TACKLE = 7
        const val A_SWITCH = 8
        const val A_MATCHUP = 9
        const val A_PRESS = 10
        const val A_STEPUP = 11

        const val ST_NOT_LIVE = 0
        const val ST_LOOSE = 1
        const val ST_TEAM = 2
        const val ST_OPP = 3

        const val MIN_TAP_MS = 40
        const val MAX_TAP_MS = 260
        const val MAX_SHOOT_MS = 950

        fun unpack(v: Long): Gesture = Gesture(
            theta = ((v shr 24) and 0xFFF).toInt() / 10f,
            r = ((v shr 16) and 0xFF).toInt() / 255f,
            durationMs = ((v shr 4) and 0xFFF).toInt(),
            action = (v and 0xF).toInt(),
            dash = ((v shr 39) and 1L) != 0L,
            valid = ((v shr 36) and 1L) != 0L,
            status = ((v shr 37) and 3L).toInt(),
            flick = ((v shr 40) and 3L).toInt()
        )

        fun tacticalKind(action: Int): Int = when (action) {
            A_CARRY, A_SHOOT, A_STEPUP -> ACTION_STICK
            A_PASS, A_LOFTED -> ACTION_PASS
            A_THROUGH -> ACTION_THROUGH
            A_SHOT -> ACTION_SHOOT
            A_TACKLE -> ACTION_TACKLE
            A_SWITCH -> ACTION_SWITCH
            A_MATCHUP -> ACTION_MATCHUP
            A_PRESS -> ACTION_PRESS
            else -> ACTION_NONE
        }

        fun buttonFor(action: Int): Int = when (tacticalKind(action)) {
            ACTION_PASS -> Btn.PASS
            ACTION_THROUGH -> Btn.THROUGH
            ACTION_SHOOT -> Btn.SHOOT
            ACTION_TACKLE -> Btn.SHOOT
            ACTION_SWITCH -> Btn.PASS
            ACTION_MATCHUP -> Btn.THROUGH
            ACTION_PRESS -> Btn.DASH
            else -> Btn.NONE
        }

        fun overlayType(action: Int): Int = when (tacticalKind(action)) {
            ACTION_STICK -> ActionType.CARRY
            ACTION_PASS -> ActionType.PASS
            ACTION_THROUGH -> ActionType.THROUGH
            ACTION_SHOOT -> ActionType.SHOOT
            ACTION_TACKLE -> ActionType.TACKLE
            ACTION_SWITCH -> ActionType.SWITCH
            ACTION_MATCHUP -> ActionType.MATCHUP
            ACTION_PRESS -> ActionType.PRESS
            else -> ActionType.NONE
        }
    }

    val kind: Int get() = if (valid) tacticalKind(action) else ACTION_NONE

    val stickX: Float get() = if (valid) (r * cos(Math.toRadians(theta.toDouble()))).toFloat() else 0f
    val stickY: Float get() = if (valid) (r * sin(Math.toRadians(theta.toDouble()))).toFloat() else 0f

    fun toScreen(anchors: HudAnchors, w: Int, h: Int): ScreenGesture {
        if (kind == ACTION_NONE || w <= 0 || h <= 0) return ScreenGesture.EMPTY
        return anchors.resolve(stickX, stickY, buttonFor(action), durationMs, w, h)
    }
}

fun HudAnchors.resolve(sx: Float, sy: Float, button: Int, holdMs: Int, w: Int, h: Int): ScreenGesture {
    val wantStick = Math.hypot(sx.toDouble(), sy.toDouble()) > 0.05
    val stickFrom = if (wantStick) joystickPoint(w, h) else null
    val stickTo = if (wantStick) stickTarget(sx, sy, w, h) else null
    val buttonAt = buttonPoint(button, w, h)
    val kind = when {
        buttonAt != null && button == Btn.THROUGH -> ACTION_THROUGH
        buttonAt != null && button == Btn.SHOOT -> ACTION_SHOOT
        buttonAt != null -> ACTION_PASS
        wantStick -> ACTION_STICK
        else -> ACTION_NONE
    }
    val hold = if (buttonAt != null) holdMs.coerceIn(Gesture.MIN_TAP_MS, if (button == Btn.SHOOT) Gesture.MAX_SHOOT_MS else Gesture.MAX_TAP_MS) else 0
    return ScreenGesture(kind, stickFrom, stickTo, if (buttonAt != null) button else Btn.NONE, buttonAt, hold)
}
