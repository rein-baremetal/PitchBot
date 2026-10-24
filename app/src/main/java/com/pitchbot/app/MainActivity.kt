package com.pitchbot.app

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.res.ColorStateList
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.media.projection.MediaProjectionManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.text.InputType
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : Activity() {

    private lateinit var prefs: Prefs
    private lateinit var store: MatchStore
    private lateinit var root: LinearLayout

    private val reqCapture = 11
    private val reqExport = 12
    private val reqImport = 13
    private val reqReport = 14
    private val reqNotif = 14

    private val green = Color.parseColor("#0B7A4B")
    private val dark = Color.parseColor("#0B3D2E")
    private val ink = Color.parseColor("#1C1C1E")
    private val iosGray = Color.parseColor("#8E8E93")
    private val pageBg = Color.parseColor("#F2F2F7")
    private val hairline = Color.parseColor("#E5E5EA")
    private val tint = Color.parseColor("#E3F1E9")
    private val switchOn = Color.parseColor("#34C759")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = Prefs(this)
        store = MatchStore(this)
        Native.nInit()

        window.statusBarColor = pageBg
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR
        val sv = ScrollView(this)
        sv.setBackgroundColor(pageBg)
        sv.isVerticalScrollBarEnabled = false
        root = LinearLayout(this)
        root.orientation = LinearLayout.VERTICAL
        root.setPadding(dp(16), dp(24), dp(16), dp(40))
        sv.addView(root)
        setContentView(sv)
    }

    override fun onResume() {
        super.onResume()
        store.reload()
        render()
    }

    private fun card(title: String): LinearLayout {
        val hdr = TextView(this)
        hdr.text = title.uppercase(Locale.getDefault())
        hdr.textSize = 13f
        hdr.letterSpacing = 0.04f
        hdr.setTextColor(iosGray)
        hdr.setPadding(dp(16), dp(24), 0, dp(8))
        root.addView(hdr)
        val c = LinearLayout(this)
        c.orientation = LinearLayout.VERTICAL
        c.setPadding(dp(16), dp(8), dp(16), dp(16))
        val bg = GradientDrawable()
        bg.cornerRadius = dp(16).toFloat()
        bg.setColor(Color.WHITE)
        c.background = bg
        c.elevation = dp(1).toFloat()
        c.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        root.addView(c)
        return c
    }

    private fun text(parent: LinearLayout, s: String, size: Float = 14f, color: Int = Color.parseColor("#3C3C43")): TextView {
        val t = TextView(this)
        t.text = s
        t.textSize = size
        t.setTextColor(color)
        t.setPadding(0, dp(6), 0, 0)
        parent.addView(t)
        return t
    }

    private fun styledButton(parent: LinearLayout, s: String, enabled: Boolean, primary: Boolean, onClick: () -> Unit): Button {
        val b = Button(this)
        b.text = s
        b.isAllCaps = false
        b.isEnabled = enabled
        b.textSize = 16f
        b.typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        b.stateListAnimator = null
        b.setTextColor(if (primary) Color.WHITE else if (enabled) green else iosGray)
        val bg = GradientDrawable()
        bg.cornerRadius = dp(14).toFloat()
        bg.setColor(if (primary) (if (enabled) green else Color.parseColor("#C7C7CC")) else tint)
        b.background = bg
        val lp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(50))
        lp.topMargin = dp(10)
        b.layoutParams = lp
        b.setOnTouchListener { v, e ->
            when (e.action) {
                MotionEvent.ACTION_DOWN -> v.animate().scaleX(0.97f).scaleY(0.97f).alpha(0.85f).setDuration(80).start()
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> v.animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(120).start()
            }
            false
        }
        b.setOnClickListener { onClick() }
        parent.addView(b)
        return b
    }

    private fun button(parent: LinearLayout, s: String, enabled: Boolean = true, onClick: () -> Unit): Button =
        styledButton(parent, s, enabled, true, onClick)

    private fun secondaryButton(parent: LinearLayout, s: String, onClick: () -> Unit): Button =
        styledButton(parent, s, true, false, onClick)

    private fun switchRow(parent: LinearLayout, s: String, value: Boolean, onChange: (Boolean) -> Unit) {
        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        row.gravity = Gravity.CENTER_VERTICAL
        row.setPadding(0, dp(10), 0, dp(10))
        val label = TextView(this)
        label.text = s
        label.textSize = 15f
        label.setTextColor(ink)
        label.setPadding(0, 0, dp(12), 0)
        label.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        val sw = Switch(this)
        sw.isChecked = value
        sw.thumbTintList = ColorStateList.valueOf(Color.WHITE)
        sw.trackTintList = ColorStateList(
            arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
            intArrayOf(switchOn, Color.parseColor("#D1D1D6")))
        sw.setOnCheckedChangeListener { _, v -> onChange(v) }
        row.setOnClickListener { sw.toggle() }
        row.addView(label)
        row.addView(sw)
        parent.addView(row)
        val line = View(this)
        line.setBackgroundColor(hairline)
        line.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1)
        parent.addView(line)
    }

    private fun slider(parent: LinearLayout, s: String, min: Float, max: Float, cur: Float, fmt: String, onChange: (Float) -> Unit) {
        val label = text(parent, String.format(Locale.US, "$s: $fmt", cur), 15f, ink)
        label.setPadding(0, dp(12), 0, 0)
        val sb = SeekBar(this)
        sb.max = 100
        sb.progress = (((cur - min) / (max - min)) * 100).toInt().coerceIn(0, 100)
        sb.progressTintList = ColorStateList.valueOf(green)
        sb.progressBackgroundTintList = ColorStateList.valueOf(Color.parseColor("#D1D1D6"))
        sb.thumbTintList = ColorStateList.valueOf(green)
        sb.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(p0: SeekBar?, p: Int, fromUser: Boolean) {
                val v = min + (max - min) * p / 100f
                label.text = String.format(Locale.US, "$s: $fmt", v)
                if (fromUser) onChange(v)
            }
            override fun onStartTrackingTouch(p0: SeekBar?) {}
            override fun onStopTrackingTouch(p0: SeekBar?) {}
        })
        parent.addView(sb)
    }

    private fun overlayOk() = Settings.canDrawOverlays(this)

    private fun accessibilityOk(): Boolean {
        val s = Settings.Secure.getString(contentResolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES) ?: return false
        return s.contains("$packageName/${TapService::class.java.name}") || s.contains("$packageName/.TapService")
    }

    private fun notifOk() =
        Build.VERSION.SDK_INT < 33 || checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED

    private var advOpen = false

    private fun render() {
        root.removeAllViews()

        val h = TextView(this)
        h.text = "PitchBot"
        h.textSize = 34f
        h.setTextColor(ink)
        h.setPadding(dp(4), 0, 0, 0)
        h.setTypeface(h.typeface, android.graphics.Typeface.BOLD)
        root.addView(h)
        text(root, "Offline overlay that plays eFootball offline AI-vs-AI matches by reading the radar. Nothing leaves your phone.", 15f, iosGray).setPadding(dp(4), dp(4), dp(4), 0)

        val setup = card("1  Setup" + if (overlayOk() && accessibilityOk() && notifOk()) "  ✓" else "")
        text(setup, (if (overlayOk()) "✅" else "❌") + "  Draw over other apps")
        if (!overlayOk()) button(setup, "Allow overlay") {
            startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")))
        }
        text(setup, (if (accessibilityOk()) "✅" else "❌") + "  Controller taps (Accessibility service \"PitchBot controller taps\")")
        if (!accessibilityOk()) button(setup, "Open Accessibility settings") {
            startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
        }
        if (Build.VERSION.SDK_INT >= 33) {
            text(setup, (if (notifOk()) "✅" else "❌") + "  Notifications (Android requires a silent background notification while capturing)")
            if (!notifOk()) button(setup, "Allow notifications") {
                requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), reqNotif)
            }
        }

        val run = card("2  Play")
        val mapped = prefs.mapping() != null
        text(run, if (mapped) "Controls mapped ✅ (kept until you reset)" else "Controls not mapped yet. A short one-time wizard starts on the game screen (joystick, 4 buttons, radar corners).")
        val ready = overlayOk() && accessibilityOk()
        button(run, if (BotService.alive) "Overlay is running" else "Start overlay", ready && !BotService.alive) { requestCapture() }
        if (BotService.alive) button(run, "Stop overlay (saves the match)") {
            startService(Intent(this, BotService::class.java).setAction(BotService.ACTION_STOP))
            root.postDelayed({ render() }, 600)
        }
        val legBtn = button(run, "", true) { }
        fun paintLegendary() {
            legBtn.text = if (prefs.legendary) "⚡ Legendary mode: ON  (tap to turn off)" else "⚡ Legendary mode: OFF  (tap to turn on)"
            val bg = GradientDrawable()
            bg.cornerRadius = dp(14).toFloat()
            bg.setColor(if (prefs.legendary) Color.parseColor("#FF3B30") else Color.parseColor("#8E8E93"))
            legBtn.background = bg
        }
        legBtn.setOnClickListener {
            prefs.legendary = !prefs.legendary
            prefs.pushToEngine()
            paintLegendary()
        }
        paintLegendary()
        switchRow(run, "Test mode (yellow flashes only, no real taps). Keep OFF to let the bot play.", prefs.dryRun) { prefs.dryRun = it; prefs.pushToEngine() }
        text(run, "In the game press Volume Up (kick-off and 2nd half) and tap the dot in your team's colour. Volume Down at full time saves the match. The bot plays with and without the ball: pass, through ball, dash, press, switch, match-up and tackle. You take the shots. Touch the screen and it steps aside; let go and it carries on.", 12f, iosGray)
        secondaryButton(run, "Keep PitchBot alive (turn off battery saving for it)") {
            try { startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)) } catch (_: Exception) { }
        }
        if (mapped) secondaryButton(run, "Reset control mapping") {
            prefs.resetMapping()
            Toast.makeText(this, "Mapping cleared. You will be asked again on next start.", Toast.LENGTH_SHORT).show()
            render()
        }
        secondaryButton(run, "Export error report (text file)") {
            val i = Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                .setType("text/plain").putExtra(Intent.EXTRA_TITLE, "pitchbot-report.txt")
            startActivityForResult(i, reqReport)
        }
        secondaryButton(run, if (advOpen) "Hide advanced settings" else "Advanced settings") { advOpen = !advOpen; render() }

        if (advOpen) {
        val set = card("Advanced settings")
        switchRow(set, "Smart tracking: Kalman filter + latency lead, learns from its own mistakes after every match", prefs.stateEst) { prefs.stateEst = it; prefs.pushToEngine() }
        text(set, prefs.trackerSummary(), 12f, iosGray)
        secondaryButton(set, "Reset tracker learning") { prefs.resetTrackerLearning(); prefs.pushToEngine(); Toast.makeText(this, "Tracker learning cleared.", Toast.LENGTH_SHORT).show(); render() }
        switchRow(set, "Record radar sessions: tracked dot positions + times only (no screenshots), for tuning", prefs.recordSessions) { prefs.recordSessions = it }
        text(set, Recorder.summary(this), 12f, iosGray)
        secondaryButton(set, "Delete recordings") {
            AlertDialog.Builder(this).setTitle("Delete all recordings?")
                .setMessage("Removes the recorded dot-position files from this phone. Match logs and learning stay.")
                .setPositiveButton("Delete") { _, _ -> Recorder.deleteAll(this); render() }
                .setNegativeButton("Cancel", null).show()
        }
        secondaryButton(set, "Export error report (text file)") {
            val i = Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                .setType("text/plain").putExtra(Intent.EXTRA_TITLE, "pitchbot-report.txt")
            startActivityForResult(i, reqReport)
        }
        text(set, "The report holds the last ~5 minutes of status (dots found, live or not, delays), events and why Android last ended the app. No screenshots.", 12f, iosGray)
        switchRow(set, "Keep the ball: in a crowd, pass to an open player instead of dribbling", prefs.keepBall) { prefs.keepBall = it; prefs.pushToEngine() }
        slider(set, "Keep-ball strength", 0f, 1f, prefs.keepBias, "%.2f") { prefs.keepBias = it; prefs.pushToEngine() }
        text(set, "Higher = releases the ball sooner and avoids risky passes when opponents crowd you. Lower = more dribbling and forward gambles.", 12f, iosGray)
        switchRow(set, "Colour-only: ignore every colour except the two kits I picked (referee, keeper in a third colour)", prefs.colorOnly) { prefs.colorOnly = it; prefs.pushToEngine() }
        switchRow(set, "Back-pass to the keeper when crowded deep in my half (keeper not on the radar)", prefs.virtualKeeper) { prefs.virtualKeeper = it; prefs.pushToEngine() }
        switchRow(set, "Steady tracking: keep following the ball when it is hidden and players when their dots merge", prefs.trackAssist) { prefs.trackAssist = it; prefs.pushToEngine() }
        switchRow(set, "Adaptive play: read the opponent's shape, vary the style, add controlled randomness once they adjust", prefs.adaptivePlay) { prefs.adaptivePlay = it; prefs.pushToEngine() }
        switchRow(set, "Radar auto-correct: nudge the saved radar outline back when it slowly drifts", prefs.autoDrift) { prefs.autoDrift = it }
        text(set, "Steady tracking and adaptive play only change decisions when the picture is imperfect or the opponent changes behaviour; turn them off to get the plain engine.", 12f, iosGray)
        text(set, "If the attack direction wobbles, set it by hand in this card (left / right); with colour-only the keeper's dot is not used to find the direction.", 12f, iosGray)
        switchRow(set, "Auto-shoot: fire a power shot on the strongest chance (re-arms 1.5 s after a shot or a cancelled attempt)", prefs.autoShoot) { prefs.autoShoot = it; prefs.pushToEngine() }
        switchRow(set, "Rescue rules: panic clearances, forward-only build-up, one-touch timing, instant shots, box lockout, containment, wing switch", prefs.rescueRules) { prefs.rescueRules = it; prefs.pushToEngine() }
        switchRow(set, "Upgrade pack: live opponent line, roles, two-step passing value, loose-ball interception, counter-press, auto-recovery, in-match pass learning", prefs.upgradePack) { prefs.upgradePack = it; prefs.pushToEngine() }
        text(set, "Shots that were suppressed (pressure, blocked line, cooldown) are written to RESCUE_ANALYTICS.txt in the app's files folder (Android/data/com.pitchbot.app/files) after every match.", 12f, iosGray)
        slider(set, "Shot hold (ms)", 100f, 500f, prefs.shootHoldMs.toFloat(), "%.0f") { prefs.shootHoldMs = it.toInt(); prefs.pushToEngine() }
        text(set, "Needs the SHOOT button mapped (setup asks for it). Aims at a side of the goal, never at the keeper, holds the button for a power shot. Dry-run first: a SHOOT ring shows where it would fire. Your touch always overrides it.", 12f, iosGray)
        val plans = listOf("Off", "Protect the lead", "Chase a goal")
        val planBtn = secondaryButton(set, "") { }
        fun paintPlan() { planBtn.text = "Late-game plan: " + plans[prefs.clutchPlan.coerceIn(0, 2)] + "  (tap to change)" }
        planBtn.setOnClickListener { prefs.clutchPlan = (prefs.clutchPlan + 1) % 3; paintPlan() }
        paintPlan()
        slider(set, "Match length (real minutes, whole match)", 4f, 20f, prefs.matchMinutes, "%.0f") { prefs.matchMinutes = it }
        slider(set, "Plan starts at (share of the match played)", 0.5f, 0.95f, prefs.clutchStartPct, "%.2f") { prefs.clutchStartPct = it }
        text(set, "Protect: safe lateral / back passes and only sure forward balls. Chase: progress and through balls, no sideways recycling. The clock starts at your first colour tap (Volume Up) and the second-half tap.", 12f, iosGray)

        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        row.setPadding(dp(3), dp(3), dp(3), dp(3))
        val track = GradientDrawable()
        track.cornerRadius = dp(11).toFloat()
        track.setColor(Color.parseColor("#E5E5EA"))
        row.background = track
        row.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).also { it.topMargin = dp(14) }
        val names = listOf("Possession", "Fast triangle", "Counter")
        val btns = ArrayList<Button>()
        for (n in names) {
            val b = Button(this)
            b.text = n
            b.isAllCaps = false
            b.textSize = 13f
            b.stateListAnimator = null
            b.layoutParams = LinearLayout.LayoutParams(0, dp(38), 1f)
            row.addView(b)
            btns.add(b)
        }
        fun paint() {
            for ((i, b) in btns.withIndex()) {
                val bg = GradientDrawable()
                bg.cornerRadius = dp(9).toFloat()
                bg.setColor(if (prefs.tactic == i) Color.WHITE else Color.TRANSPARENT)
                b.background = bg
                b.elevation = if (prefs.tactic == i) dp(1).toFloat() else 0f
                b.setTextColor(if (prefs.tactic == i) ink else iosGray)
                b.typeface = Typeface.create("sans-serif-medium", if (prefs.tactic == i) Typeface.BOLD else Typeface.NORMAL)
            }
        }
        for ((i, b) in btns.withIndex()) b.setOnClickListener { prefs.tactic = i; paint() }
        paint()
        set.addView(row)
        slider(set, "Aggression", 0f, 1f, prefs.aggression, "%.2f") { prefs.aggression = it }
        slider(set, "Shooting range", 0.15f, 0.40f, prefs.shootReach, "%.2f") { prefs.shootReach = it }
        slider(set, "Pass speed (higher = fewer touches before passing)", 0f, 1f, prefs.passSpeed, "%.2f") { prefs.passSpeed = it }
        switchRow(set, "Analytics engine (xT / xP attack decisions)", prefs.analytics) { prefs.analytics = it }
        slider(set, "Temperature during counter-attacks (low = sharper)", 0.05f, 1f, prefs.tempCounter, "%.2f") { prefs.tempCounter = it }
        slider(set, "Temperature while holding the ball (high = more varied)", 0.05f, 1.5f, prefs.tempHold, "%.2f") { prefs.tempHold = it }
        slider(set, "Take over again after you touch (ms) - short = bot comes back almost as soon as you let go", 150f, 1500f, prefs.resumeMs.toFloat(), "%.0f") { prefs.resumeMs = it.toInt() }
        switchRow(set, "Hold-aware hand-over (waits while your finger is on the stick, resumes right after you let go)", prefs.holdAware) { prefs.holdAware = it }
        switchRow(set, "Dry-run (flashes only, no taps)", prefs.dryRun) { prefs.dryRun = it; prefs.pushToEngine() }
        switchRow(set, "Show debug dots on radar (Dry-run only; live play is always clean)", prefs.showDebug) { prefs.showDebug = it }
        switchRow(set, "Flick the Shoot button on long power shots (curl toward the open corner)", prefs.stunFlick) { prefs.stunFlick = it }
        switchRow(set, "Simple joystick mode (use if movement is jerky or cancelled)", prefs.simpleMove) { prefs.simpleMove = it }
        switchRow(set, "Flip stick left/right (if the bot runs the wrong way)", prefs.invertX) { prefs.invertX = it }
        switchRow(set, "Flip stick up/down", prefs.invertY) { prefs.invertY = it }
        fun dirText(d: Int) = when (d) { 0 -> "left"; 1 -> "right"; else -> "auto" }
        fun halfText(h: Int) = when (h) { 1 -> "1st"; 2 -> "2nd"; else -> "auto" }
        val dirBtn = secondaryButton(set, "Attack direction: " + dirText(prefs.attackDir)) { }
        dirBtn.setOnClickListener {
            prefs.attackDir = when (prefs.attackDir) { -1 -> 1; 1 -> 0; else -> -1 }
            dirBtn.text = "Attack direction: " + dirText(prefs.attackDir)
        }
        val halfBtn = secondaryButton(set, "Half: " + halfText(prefs.half)) { }
        halfBtn.setOnClickListener {
            prefs.half = (prefs.half + 1) % 3
            halfBtn.text = "Half: " + halfText(prefs.half)
        }
        if (mapped) secondaryButton(set, "Re-draw radar box (next start)") {
            prefs.radarResizePending = true
            Toast.makeText(this, "On the next overlay start you will tap the two radar corners again.", Toast.LENGTH_LONG).show()
        }
        text(set, "Dry-run is OFF by default, so the bot taps for real (live flashes are green). Switch it ON to only see yellow flashes with nothing tapped.", 12f, iosGray)

        }

        renderGames()
    }

    private fun renderGames() {
        val all = store.list().filter { it.scored }
        val card = card("3  Games")
        text(card, "Games: ${all.size}. Every number is measured by PitchBot during the game, there is nothing to enter.")

        val chart = ProgressChartView(this)
        chart.setData(all.takeLast(20))
        chart.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).also { it.topMargin = dp(8) }
        card.addView(chart)
        text(card, "Game score per match (goals, pass keeping, possession, ball security). Orange = trend.", 11f, iosGray)
        text(card, gamesVerdict(all), 12f, dark)
        for (k in 0..2) {
            val g = DashboardGraphView(this, k)
            g.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).also { it.topMargin = dp(8) }
            card.addView(g)
        }
        text(card, "Graphs update live. Ringed points are evaluation matches: they are measured, never trained on.", 11f, iosGray)
        text(card, "Goals are read from the radar (the ball seen in a goal mouth before a stoppage), so one can be missed.", 11f, iosGray)

        for (m in all.takeLast(12).reversed()) {
            val d = SimpleDateFormat("d MMM HH:mm", Locale.getDefault()).format(Date(m.ts))
            val info = TextView(this)
            val mins = if (m.liveSec > 0) "  |  ${m.liveSec / 60} min live" else ""
            info.text = "$d   ${m.gf} : ${m.ga}\nposs ${m.poss.toInt()}%  |  passes ${m.passOk}/${m.passAtt}\nshots ${m.shots}  |  lost ${m.losses}$mins"
            info.textSize = 12f
            info.setTextColor(Color.parseColor("#37474F"))
            info.setPadding(0, dp(6), 0, dp(6))
            card.addView(info)
        }

        val r = LinearLayout(this)
        r.orientation = LinearLayout.HORIZONTAL
        card.addView(r)
        fun small(s: String, onClick: () -> Unit) {
            val b = Button(this)
            b.text = s
            b.isAllCaps = false
            b.textSize = 14f
            b.stateListAnimator = null
            b.setTextColor(green)
            val bg = GradientDrawable()
            bg.cornerRadius = dp(12).toFloat()
            bg.setColor(tint)
            b.background = bg
            val lp = LinearLayout.LayoutParams(0, dp(46), 1f)
            lp.topMargin = dp(8); lp.rightMargin = dp(4)
            b.layoutParams = lp
            b.setOnClickListener { onClick() }
            r.addView(b)
        }
        small("Export backup") {
            val stamp = SimpleDateFormat("yyyy-MM-dd", Locale.US).format(Date())
            val i = Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                .setType("application/zip").putExtra(Intent.EXTRA_TITLE, "pitchbot-backup-$stamp.zip")
            startActivityForResult(i, reqExport)
        }
        small("Import backup") {
            val i = Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
            startActivityForResult(i, reqImport)
        }
        val last = prefs.lastBackupMs
        text(card, if (last > 0L) "Last backup: " + SimpleDateFormat("d MMM yyyy, HH:mm", Locale.getDefault()).format(Date(last)) +
            ". Keep the zip somewhere safe (Drive, PC). A new phone only needs: install PitchBot, Import backup."
        else "No backup yet. Export one now and then, and keep the zip off the phone (Drive, PC) so a new phone can continue.",
            12f, if (last > 0L) dark else Color.parseColor("#E65100"))
    }

    private fun confirmImport(uri: Uri) {
        val info = try { Backup.inspect(this, uri) } catch (e: Exception) {
            Toast.makeText(this, "Cannot read backup: ${e.message}", Toast.LENGTH_LONG).show(); return
        }
        val whenMade = if (info.created > 0L) "\nMade ${SimpleDateFormat("d MMM yyyy, HH:mm", Locale.getDefault()).format(Date(info.created))}" +
            (if (info.device.isNotBlank()) " on ${info.device}" else "") else ""
        val body = "Contains ${info.summary()}.$whenMade\n\n" +
            "This replaces the match history on this phone.\n\n" +
            "New phone or different screen: choose \"History only\" and set up the buttons and radar again.\n" +
            "Same phone (reinstall): choose \"Everything\"."
        AlertDialog.Builder(this).setTitle("Restore backup?").setMessage(body)
            .setPositiveButton("History only") { _, _ -> doImport(uri, true) }
            .setNeutralButton("Everything") { _, _ -> doImport(uri, false) }
            .setNegativeButton("Cancel", null).show()
    }

    private fun doImport(uri: Uri, learningOnly: Boolean) {
        try {
            val info = Backup.import(this, uri, learningOnly)
            store.reload()
            if (!learningOnly) prefs.restoreKitToEngine()
            prefs.pushToEngine()
            val extra = if (learningOnly) " Set up buttons and radar on this phone before playing." else ""
            Toast.makeText(this, "Restored: ${info.summary()}.$extra", Toast.LENGTH_LONG).show()
            render()
        } catch (e: Exception) {
            Toast.makeText(this, "Import failed, nothing was changed: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    private fun requestCapture() {
        val mpm = getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        startActivityForResult(mpm.createScreenCaptureIntent(), reqCapture)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        when (requestCode) {
            reqCapture -> if (resultCode == RESULT_OK && data != null) {
                val i = Intent(this, BotService::class.java)
                    .putExtra(BotService.EXTRA_CODE, resultCode)
                    .putExtra(BotService.EXTRA_DATA, data)
                startForegroundService(i)
                Toast.makeText(this, "Open the game now. The overlay is starting.", Toast.LENGTH_LONG).show()
                moveTaskToBack(true)
            } else Toast.makeText(this, "Screen capture permission is needed to read the radar.", Toast.LENGTH_LONG).show()
            reqExport -> data?.data?.let {
                try { val i = Backup.export(this, it); Toast.makeText(this, "Backup saved: ${i.summary()}", Toast.LENGTH_LONG).show(); render() }
                catch (e: Exception) { Toast.makeText(this, "Export failed: ${e.message}", Toast.LENGTH_LONG).show() }
            }
            reqReport -> data?.data?.let {
                try { Diagnostics.exportTo(this, it); Toast.makeText(this, "Report saved", Toast.LENGTH_SHORT).show() }
                catch (e: Exception) { Toast.makeText(this, "Report failed: ${e.message}", Toast.LENGTH_LONG).show() }
            }
            reqImport -> data?.data?.let { confirmImport(it) }
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        render()
    }
}
