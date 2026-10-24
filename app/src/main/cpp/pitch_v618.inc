// layer: predictive threat engine, last-man protection and defensive shape, multi-factor defender assignment with a
// commitment period, recovery-probability gated counter-press, plan validity, counter-exposure risk, a seven-state tactical
// machine with kick-off protection, opponent pattern profiles with evidence-weighted confidence, shot / cutback prevention,
// per-kind input latency statistics, a radar guard with safe mode and a command sanitiser, and the benchmarking layer
// (per-match metrics, training / evaluation split, regression gate, frame recorder). Included at the end of pitch_engine.cpp
// after pitch_v617.inc (inside namespace pe).
//
// Everything here is radar-only and offline. Every feature has its own switch in TacticalConfig (v8*), and with `v8 = false`
// the engine behaves exactly like every hook below is guarded by cfg_.v8 first.

namespace {
inline float v8Sat(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
inline MetricVector2D v8Unit(const MetricVector2D& v, const MetricVector2D& fallback) {
    const float l = v.length();
    return l > 1e-4f ? v * (1.f / l) : fallback;
}
}  // namespace

void V8Mem::clear() {
    for (int i = 0; i < V8_PROF; i++) prof[i] = V8OppProfile();
    nProf = 0;
    for (int i = 0; i < V8_HIST; i++) hist[i] = V8MatchRec();
    nHist = hHead = 0;
    seen = gen = 0;
    good = V8Agg();
    cand = V8Agg();
    haveGood = false;
    sinceGood = 0;
    verdict = GV_INSUFFICIENT;
    verdictPending = false;
    reason.clear();
    lastReplay.clear();
    promotions = rollbacks = 0;
}

const char* PitchEngine::tacticalName(int s) {
    static const char* N[TS_N] = {"DEFEND", "PRESS", "RECOVER", "COUNTER", "CONTROL", "PROTECT LEAD", "KICK-OFF PROTECT"};
    return s >= 0 && s < TS_N ? N[s] : "?";
}

// ---------------------------------------------------------------------------------------------------------------------
// Per-frame driver
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8Frame(double dtSec) {
    V8State& S = v8_;
    const double now = state_.timeMs;
    S.frames++;
    if (state_.status != ST_NOT_LIVE) { S.liveFrames++; S.liveMs += dtSec * 1000.0; }
    if (state_.ballSeen) S.lastBallSeenAt = now;
    if (cfg_.v8Guard) v8GuardUpdate(dtSec);
    else { S.trackConf = 1.f; S.safe = false; }
    S.confSum += S.trackConf;
    if (cfg_.v8Threat) v8ThreatUpdate(); else { S.preempt = false; S.top = -1; S.topScore = 0.f; }
    if (cfg_.v8ShotGuard) v8ShotUpdate(); else { S.sk = SK_NONE; S.resp = SR_NONE; }
    if (cfg_.v8Shape) v8ShapeUpdate();
    if (cfg_.v8Press) v8PressUpdate(); else S.pressOk = true;
    if (cfg_.v8Fsm) v8FsmUpdate();
    if (cfg_.v8Assign) v8AssignUpdate();
    if (cfg_.v8OppModel) v8OppObserve();
    if (cfg_.v8PlanValid) v8PlanValidate();
    v8NoteDecision();

    // observed effect: the decision to pass or shoot became a ball leaving the carrier (includes the game's own animation)
    const float bsp = state_.ballVel.length();
    if (S.pendKind >= 0) {
        if (now - S.pendAt > 1500.0) { S.lat[S.pendKind].effMiss++; S.pendKind = -1; }
        else if (state_.ballSeen && bsp >= 9.f && S.prevBallSp < 7.f) {
            V8LatKind& L = S.lat[S.pendKind];
            const float x = (float)(now - S.pendAt);
            L.effN++;
            L.effMean += (x - L.effMean) / (float)std::min(L.effN, 100);
            S.pendKind = -1;
        }
    }
    S.prevBallSp = state_.ballSeen ? bsp : S.prevBallSp * 0.9f;
}

// ---------------------------------------------------------------------------------------------------------------------
// 11. Radar guard: validation, tracking confidence, safe mode
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8GuardUpdate(double dtSec) {
    (void)dtSec;
    V8State& S = v8_;
    const double now = state_.timeMs;
    const float s = (float)state_.attackSign;
    int nh = 0, na = 0, nOverlap = 0, nFast = 0;
    float xmin = 1e9f, xmax = -1e9f, ymax = 0.f;
    float depthMin = 1e9f, oppDepthMax = -1e9f;
    for (int t = 0; t < 2; t++) {
        PlayerState* arr = t == 0 ? state_.team : state_.opp;
        float* cf = t == 0 ? S.cfOwn : S.cfOpp;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& p = arr[i];
            if (!p.active || !p.visible) { cf[i] += 0.12f * (0.f - cf[i]); continue; }
            if (t == 0) nh++; else na++;
            float target = 1.f - clampf((float)p.missed / 6.f, 0.f, 1.f);
            if (p.vel.length() > 13.f) { target *= 0.3f; nFast++; }
            cf[i] += 0.15f * (target - cf[i]);
            xmin = std::min(xmin, p.pos.x); xmax = std::max(xmax, p.pos.x);
            ymax = std::max(ymax, std::fabs(p.pos.y));
            const float dg = goalDistance(p.pos);
            if (t == 0) depthMin = std::min(depthMin, dg); else oppDepthMax = std::max(oppDepthMax, dg);
            for (int j = i + 1; j < TEAM_SIZE; j++) {
                const PlayerState& q = arr[j];
                if (!q.active || !q.visible) continue;
                if (distance(p.pos, q.pos) < 0.35f) nOverlap++;
            }
        }
    }
    S.implausible = nFast;
    S.overlapPairs = nOverlap;
    S.visHome += 0.1f * ((float)nh / (float)TEAM_SIZE - S.visHome);
    S.visAway += 0.1f * ((float)na / (float)TEAM_SIZE - S.visAway);
    S.ballSeenEma += 0.05f * ((state_.ballSeen ? 1.f : 0.f) - S.ballSeenEma);
    S.jumpEma += 0.08f * ((trackJumps_ > 0 ? 1.f : 0.f) - S.jumpEma);
    S.overlapEma += 0.1f * (std::min(1.f, (float)nOverlap / 3.f) - S.overlapEma);
    // scale: everything must sit inside the pitch (with margin) and spread over a plausible width
    const bool scaleOut = (nh + na) >= 8 && (xmax > 62.f || xmin < -62.f || ymax > 40.f || (nh + na >= 14 && xmax - xmin < 12.f));
    S.scaleBadRun = scaleOut ? S.scaleBadRun + 1 : std::max(0, S.scaleBadRun - 1);
    S.scaleBad = S.scaleBadRun >= 10;
    // attack direction: independent cues (kick-off layout is added by the state machine; here the keeper depth test)
    const bool dirOdd = nh >= 8 && na >= 8 && (depthMin > 45.f || oppDepthMax < 60.f);
    S.dirBad = dirOdd ? S.dirBad + 1 : std::max(0, S.dirBad - 2);
    S.dirSuspect = S.dirBad >= 90;
    (void)s;
    // ownership: the carrier must be next to the ball, and the HUD (when the host supplies it) must agree
    bool own = true;
    if (state_.status == ST_TEAM) {
        own = state_.carrier >= 0 && state_.carrier < TEAM_SIZE && distance(state_.team[state_.carrier].pos, state_.ball) <= 3.2f;
    } else if (state_.status == ST_OPP) {
        float d = 1e9f;
        for (int i = 0; i < TEAM_SIZE; i++) if (state_.opp[i].active && state_.opp[i].visible) d = std::min(d, distance(state_.opp[i].pos, state_.ball));
        own = d <= 3.6f;
    }
    if (hudPoss_ != 0 && frameTimeMs_ - hudPossMs_ < 600.0 && (state_.status == ST_TEAM || state_.status == ST_OPP)) {
        const int want = hudPoss_ == 1 ? ST_TEAM : ST_OPP;
        S.hudMismatch = state_.status != want ? S.hudMismatch + 1 : 0;
        if (S.hudMismatch >= 30) own = false;
    } else S.hudMismatch = 0;
    S.ownConfirmed = own;
    // tracking confidence
    const float vis = std::min(S.visHome, S.visAway);
    const float visScore = v8Sat((vis - 0.55f) / 0.35f);
    const float ballScore = v8Sat((S.ballSeenEma - 0.3f) / 0.6f);
    const float jumpScore = 1.f - v8Sat(S.jumpEma * 4.f);
    const float ovScore = 1.f - S.overlapEma;
    float raw = 0.32f * visScore + 0.22f * ballScore + 0.16f * jumpScore + 0.10f * ovScore + 0.10f * (S.dirSuspect ? 0.2f : 1.f) + 0.10f * (S.scaleBad ? 0.3f : 1.f);
    if (!S.ownConfirmed) raw -= 0.15f;
    if (S.scaleBad || S.dirSuspect) raw = std::min(raw, 0.35f);          // a wrong scale or direction invalidates everything else
    if (S.overlapEma > 0.6f) raw = std::min(raw, 0.4f);                  // players stacked on one point: the detector is returning nonsense
    S.trackConf += 0.2f * (v8Sat(raw) - S.trackConf);
    // safe mode with hysteresis
    const bool ballLost = state_.status != ST_NOT_LIVE && S.lastBallSeenAt > 0.0 && now - S.lastBallSeenAt > 700.0;
    if (!S.safe) {
        if (S.trackConf < cfg_.v8SafeEnter || ballLost || S.dirSuspect || S.scaleBad) { S.safe = true; S.safeSince = now; }
    } else if (S.trackConf > cfg_.v8SafeExit && !ballLost && !S.dirSuspect && !S.scaleBad && now - S.safeSince > 800.0) {
        S.safe = false;
    }
    if (S.safe) S.safeFrames++;
}

// ---------------------------------------------------------------------------------------------------------------------
// 1. Predictive threat detection
// ---------------------------------------------------------------------------------------------------------------------

// Chance that a ball played from -> to is NOT cut out by one of our players (the opposition's pass).
float PitchEngine::v8LaneOpen(const MetricVector2D& from, const MetricVector2D& to) const {
    const MetricVector2D seg = to - from;
    const float len2 = std::max(seg.x * seg.x + seg.y * seg.y, 1e-6f);
    const float len = std::sqrt(len2);
    const float bs = std::max(cfg_.passSpeed, 1.f);
    float worst = 50.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& q = state_.team[i];
        if (!q.active || !q.visible) continue;
        const MetricVector2D rel = q.pos - from;
        const float t = clampf((rel.x * seg.x + rel.y * seg.y) / len2, 0.f, 1.f);
        const MetricVector2D pt = from + seg * t;
        const float reach = cfg_.defSpeed * (len * t / bs + defReactEff()) + cfg_.defBody;
        worst = std::min(worst, distance(q.pos, pt) - reach);
    }
    return v8Sigmoid(worst);
}

void PitchEngine::v8ThreatUpdate() {
    V8State& S = v8_;
    const double now = state_.timeMs;
    if (state_.status == ST_NOT_LIVE || !ballUsable()) { S.preempt = false; S.top = -1; S.topScore = 0.f; S.laneScore = 0.f; return; }
    const float s = (float)state_.attackSign;
    const float own = -s;
    const MetricVector2D B = state_.ball;
    int car = -1;
    float cd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, B);
        if (d < cd) { cd = d; car = i; }
    }
    if (cd > 4.5f) car = -1;
    float lastDepth = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& q = state_.team[i];
        if (q.active && q.visible && !isKeeperIdx(i)) lastDepth = std::min(lastDepth, goalDistance(q.pos));
    }
    if (lastDepth > 1e8f) lastDepth = PITCH_HALF_X * 0.5f;
    const MetricVector2D carPos = car >= 0 ? state_.opp[car].pos : B;
    float best = 0.f, bestAll = 0.f;
    int bi = -1, ba = -1;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) { S.thr[i] *= 0.9f; S.run[i] = RK_NONE; continue; }
        const float conf = S.cfOpp[i];
        const MetricVector2D fut = o.future;
        const float gd = goalDistance(fut);
        const float spd = o.vel5.length();
        const float vF = own * o.vel5.x;                                // speed toward our goal
        const float central = 1.f - clampf(std::fabs(fut.y) / PITCH_HALF_Y, 0.f, 1.f);
        const float prox = std::exp(-std::max(gd, 0.f) / 22.f) * (0.75f + 0.25f * central);
        const float lane = (i == car) ? 1.f : v8LaneOpen(B, fut);
        const float move = v8Sat(vF / 6.f);
        float nd = 1e9f;
        for (int k = 0; k < TEAM_SIZE; k++) {
            const PlayerState& q = state_.team[k];
            if (!q.active || !q.visible || isKeeperIdx(k)) continue;
            nd = std::min(nd, distance(q.pos, fut));
        }
        const float unc = nd > 1e8f ? 1.f : v8Sat(nd / 8.f);
        const float raw = v8Sat(1.6f * prox * (0.35f + 0.65f * lane) * (0.55f + 0.45f * unc) * (0.70f + 0.30f * move));
        S.thr[i] += 0.5f * (raw * (0.35f + 0.65f * conf) - S.thr[i]);
        int rk = RK_NONE;
        if (i == car) {
            if (gd < 30.f) rk = RK_SHOT; else if (vF > 2.f) rk = RK_FORWARD;
        } else if (vF > 3.5f) {
            if (goalDistance(o.pos) < lastDepth + 2.f) rk = RK_THROUGH;
            else if (car >= 0 && std::fabs(o.pos.y - carPos.y) > 6.f && o.pos.y * carPos.y > 0.f && own * o.pos.x <= own * carPos.x + 4.f && std::fabs(o.pos.y) > 12.f) rk = RK_OVERLAP;
            else rk = RK_FORWARD;
        } else if (std::fabs(fut.y) > 18.f && gd < 24.f) rk = RK_CROSS;
        (void)spd;
        S.run[i] = rk;
        if (S.thr[i] > bestAll) { bestAll = S.thr[i]; ba = i; }
        if (i != car && S.thr[i] > best) { best = S.thr[i]; bi = i; }
    }
    S.top = bi;
    S.topScore = best;
    if (bi >= 0) {
        const PlayerState& o = state_.opp[bi];
        S.laneFrom = B;
        S.dest = o.future + o.vel5 * 0.25f;                              // the ball arrives where he will be
        S.laneTo = S.dest;
        S.laneScore = best;
        S.laneRecv = bi;
    } else { S.laneScore = 0.f; S.laneRecv = -1; }
    // pre-emptive trigger: only while the opposition attacks, only for dots we trust
    const bool attacked = state_.status != ST_TEAM;
    if (attacked && ba >= 0 && bestAll >= cfg_.v8ThreatTrig && S.cfOpp[ba] >= cfg_.v8ConfMin && !S.safe) {
        if (!S.preempt) S.preemptCount++;
        S.preempt = true;
        S.preemptUntil = now + 800.0;
    } else if (now > S.preemptUntil) S.preempt = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. Defensive shape: last-man protection, beaten-line prediction, depth by runner speed, de-conflicted points
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8ShapeUpdate() {
    V8State& S = v8_;
    S.holdOk = false;
    S.beatable = false;
    S.runnerIdx = -1;
    S.lineScale = 1.f;
    if (!ballUsable() || state_.status == ST_NOT_LIVE) return;
    const float s = (float)state_.attackSign;
    const float own = -s;
    int ord[TEAM_SIZE], n = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& q = state_.team[i];
        if (q.active && q.visible && !isKeeperIdx(i)) ord[n++] = i;
    }
    if (n < 2) return;
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (goalDistance(state_.team[ord[b]].pos) < goalDistance(state_.team[ord[a]].pos)) std::swap(ord[a], ord[b]);
    S.lastMan = ord[0];
    const PlayerState& lm = state_.team[ord[0]];
    const float lmDepth = goalDistance(lm.pos);
    S.lmCover = false;
    for (int k = 1; k < n; k++) {
        const PlayerState& q = state_.team[ord[k]];
        if (std::fabs(goalDistance(q.pos) - lmDepth) <= 5.f && std::fabs(q.pos.y - lm.pos.y) <= 14.f) { S.lmCover = true; break; }
    }
    // the most dangerous runner at or behind the line (the carrier counts: a breakaway)
    int runner = -1;
    float rs = 0.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible || S.cfOpp[i] < cfg_.v8ConfMin) continue;
        const int rk = S.run[i];
        if (rk != RK_THROUGH && rk != RK_FORWARD && rk != RK_OVERLAP && rk != RK_SHOT) continue;
        if (goalDistance(o.future) > lmDepth + 14.f) continue;
        if (S.thr[i] > rs) { rs = S.thr[i]; runner = i; }
    }
    S.runnerIdx = runner;
    if (runner >= 0 && state_.status != ST_TEAM) {
        const PlayerState& r = state_.opp[runner];
        const MetricVector2D rf = r.future;
        const float tBall = distance(state_.ball, rf) / std::max(cfg_.passSpeed, 8.f) + 0.25f;
        float tDef = 1e9f;
        for (int k = 0; k < n; k++) tDef = std::min(tDef, plusTta(state_.team[ord[k]], rf, 7.5f));
        S.beatable = tDef > tBall + 0.05f && goalDistance(rf) < 30.f;
        S.beatPt = rf;
        const float spd = r.vel5.length();
        const MetricVector2D rfL = rf + r.vel5 * 0.4f;
        const float buf = clampf(1.5f + 0.35f * spd + 0.08f * distance(lm.pos, rf), 2.f, 7.f);   // deeper when he is quick and far
        float dStar = std::max(goalDistance(rfL) - buf, 3.f);
        const int ci = controlledIdx();
        if (!S.lmCover && ci == ord[0]) dStar = std::min(dStar, lmDepth + 0.3f);                  // nobody covers him: no step forward
        const float yStar = rfL.y * 0.65f;                                                        // shade toward the middle
        S.holdPt = MetricVector2D(own * (PITCH_HALF_X - dStar), clampf(yStar, -PITCH_HALF_Y + 2.f, PITCH_HALF_Y - 2.f));
        S.holdDepth = dStar;
        S.holdOk = S.beatable || S.preempt || rs >= 0.4f;
    }
    float sc = 1.f;
    if (S.beatable) sc = 0.f;
    else if (!S.lmCover && (S.preempt || runner >= 0)) sc = 0.f;
    else if (S.preempt) sc = 0.5f;
    if (cfg_.v8Fsm && S.lineAdd < 0.f) sc *= std::max(0.f, 1.f + S.lineAdd / 6.f);
    S.lineScale = sc;
}

float PitchEngine::v8LineScale() const {
    if (!cfg_.v8 || !cfg_.v8Shape) return 1.f;
    const float bias = cfg_.v8OppModel ? std::min(0.f, v8_.oppLineBias) : 0.f;
    return clampf(v8_.lineScale * (1.f + bias), 0.f, 1.2f);
}

// Moves a target point out of the way when a team-mate is clearly closer to it, so two defenders never take the same spot.
MetricVector2D PitchEngine::v8Deconflict(const MetricVector2D& p, int self) const {
    MetricVector2D out = p;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < TEAM_SIZE; i++) {
            if (i == self) continue;
            const PlayerState& q = state_.team[i];
            if (!q.active || !q.visible || isKeeperIdx(i)) continue;
            const float d = distance(q.pos, out);
            if (d >= 2.2f) continue;
            if (self >= 0 && d >= distance(state_.team[self].pos, out) - 0.4f) continue;          // he does not own the point
            const MetricVector2D away = v8Unit(out - q.pos, MetricVector2D(0.f, out.y >= 0.f ? 1.f : -1.f));
            out = out + away * (2.4f - d);
        }
    }
    out.x = clampf(out.x, -PITCH_HALF_X + 1.f, PITCH_HALF_X - 1.f);
    out.y = clampf(out.y, -PITCH_HALF_Y + 1.f, PITCH_HALF_Y - 1.f);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. Defender assignment: press / lane / runner / line, with a commitment period and an override for a clearly bigger threat
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8AssignUpdate() {
    V8State& S = v8_;
    const double now = state_.timeMs;
    if (state_.status != ST_OPP && state_.status != ST_LOOSE) {
        for (int i = 0; i < TEAM_SIZE; i++) S.jobOf[i] = -1;
        for (int k = 0; k < JOB_N; k++) { S.jobHolder[k] = -1; S.jobOn[k] = false; }
        return;
    }
    if (!ballUsable() || now - S.assignAt < 120.0) return;
    S.assignAt = now;
    const float s = (float)state_.attackSign;
    const float own = -s;
    const MetricVector2D B = state_.ball;
    const MetricVector2D ownGoal(own * PITCH_HALF_X, 0.f);
    int car = -1;
    float cd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, B);
        if (d < cd) { cd = d; car = i; }
    }
    if (cd > 4.5f) car = -1;
    // candidate defenders
    int cand[TEAM_SIZE], n = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& q = state_.team[i];
        if (q.active && q.visible && !isKeeperIdx(i)) cand[n++] = i;
    }
    if (n < 2) return;
    // jobs and their points
    bool on[JOB_N] = {true, false, false, false};
    MetricVector2D pt[JOB_N];
    float thr[JOB_N] = {0, 0, 0, 0};
    pt[JOB_PRESS] = B + state_.ballVel * 0.3f + MetricVector2D(own * 1.5f, 0.f);
    thr[JOB_PRESS] = std::max(0.4f, car >= 0 ? S.thr[car] : 0.f);
    if (S.laneRecv >= 0 && S.laneScore >= 0.25f && S.laneRecv != S.runnerIdx && S.laneRecv != car) {
        on[JOB_LANE] = true;
        pt[JOB_LANE] = S.laneFrom + (S.laneTo - S.laneFrom) * 0.55f;
        thr[JOB_LANE] = S.laneScore;
    }
    if (S.runnerIdx >= 0 && S.runnerIdx != car) {
        const PlayerState& r = state_.opp[S.runnerIdx];
        const MetricVector2D toG = v8Unit(ownGoal - r.future, MetricVector2D(own, 0.f));
        on[JOB_RUNNER] = true;
        pt[JOB_RUNNER] = r.future + toG * 1.8f;
        thr[JOB_RUNNER] = S.thr[S.runnerIdx];
    }
    if (S.holdOk) {
        on[JOB_LINE] = true;
        pt[JOB_LINE] = S.holdPt;
        thr[JOB_LINE] = S.beatable ? 0.8f : 0.5f;
    }
    int jobs[JOB_N], m = 0;
    for (int k = 0; k < JOB_N; k++) if (on[k]) jobs[m++] = k;
    while (m > n) {                                                    // fewer defenders than jobs: drop the least dangerous
        int w = 0;
        for (int k = 1; k < m; k++) if (thr[jobs[k]] < thr[jobs[w]]) w = k;
        jobs[w] = jobs[--m];
    }
    // overrides: a clearly bigger threat, a new carrier, or a shot threat releases the commitment
    bool release[JOB_N] = {false, false, false, false};
    const bool carChanged = car != S.prevOppCarrier;
    for (int k = 0; k < JOB_N; k++) {
        if (S.jobHolder[k] < 0) { release[k] = true; continue; }
        if (!on[k]) { release[k] = true; continue; }
        const PlayerState& h = state_.team[S.jobHolder[k]];
        if (!h.active || !h.visible) { release[k] = true; continue; }
        if (now - S.jobSince[k] >= (double)cfg_.v8CommitMs) { release[k] = true; continue; }
        if ((k == JOB_PRESS && carChanged) || S.sk != SK_NONE || thr[k] > S.jobThreat[k] * cfg_.v8OverrideGain + 0.05f) { release[k] = true; S.overrideCount++; }
    }
    // cost matrix
    const int N = n;
    static float cost[HUNGARIAN_MAX * HUNGARIAN_MAX];
    int rowToCol[HUNGARIAN_MAX];
    const int ci = controlledIdx();
    for (int r = 0; r < N; r++) {
        const int j = cand[r];
        const PlayerState& q = state_.team[j];
        const bool lineMan = role_[j] == ROLE_CB || role_[j] == ROLE_FB;
        for (int c = 0; c < N; c++) {
            float v = 6.f;                                              // unassigned
            if (c < m) {
                const int k = jobs[c];
                const MetricVector2D P = pt[k];
                float t = plusTta(q, P, 7.0f);
                if (j != ci) t += 0.35f;                                // switching delay
                if (k == JOB_LANE) {
                    const float tBall = distance(S.laneFrom, P) / std::max(cfg_.passSpeed, 1.f);
                    t += 0.8f * (1.f - v8Sigmoid((tBall - plusTta(q, P, 7.0f)) / 0.15f));   // interception probability
                    t += 0.02f * segmentDistance(S.laneFrom, S.laneTo, q.pos);              // can he cover the lane
                }
                if (k == JOB_RUNNER) t += 0.02f * distance(q.pos, state_.opp[S.runnerIdx].pos);
                if (k == JOB_PRESS && lineMan && goalDistance(B) > 28.f && !S.lmCover) t += 0.6f;   // line players do not leave the line uncovered
                const int pj = S.jobOf[j];
                if (pj >= 0 && pj != k) t += 0.5f * S.jobThreat[pj];     // abandoning a live assignment
                if (pj == k) t -= 0.25f;                                 // stickiness
                if (!release[k] && S.jobHolder[k] == j) t -= 50.f;       // committed holder
                v = clampf(t, -60.f, 5.f);
            }
            cost[r * N + c] = v;
        }
    }
    hungarianSolve(cost, N, rowToCol);
    int newHolder[JOB_N] = {-1, -1, -1, -1};
    for (int r = 0; r < N; r++) {
        const int c = rowToCol[r];
        if (c >= 0 && c < m) newHolder[jobs[c]] = cand[r];
    }
    for (int k = 0; k < JOB_N; k++) {
        if (newHolder[k] != S.jobHolder[k]) { S.jobSince[k] = now; S.assignCount++; }
        S.jobHolder[k] = newHolder[k];
        S.jobOn[k] = on[k] && newHolder[k] >= 0;
        S.jobPt[k] = pt[k];
        S.jobThreat[k] = thr[k];
    }
    for (int i = 0; i < TEAM_SIZE; i++) S.jobOf[i] = -1;
    for (int k = 0; k < JOB_N; k++) if (S.jobHolder[k] >= 0) S.jobOf[S.jobHolder[k]] = k;
    S.prevOppCarrier = car;
    S.prevRunner = S.runnerIdx;
}

// true while the controlled defender should keep his job instead of following the game's nearest-to-ball switch
bool PitchEngine::v8HoldSwitch(int nearIdx) const {
    if (!cfg_.v8 || !cfg_.v8Assign) return false;
    const int ci = controlledIdx();
    if (ci < 0 || v8_.jobOf[ci] < 0) return false;
    if (state_.timeMs - v8_.jobSince[v8_.jobOf[ci]] >= (double)cfg_.v8CommitMs) return false;
    return v8_.jobHolder[JOB_PRESS] != nearIdx;                    // the nearest player is not the assigned presser: stay on the job
}

// ---------------------------------------------------------------------------------------------------------------------
// 4. Counter-press: recovery probability and cover
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8PressUpdate() {
    V8State& S = v8_;
    const double now = state_.timeMs;
    if (state_.status == ST_NOT_LIVE || !ballUsable()) return;
    if (lostBallMs_ > 0.0 && lostBallMs_ != S.lossAt) {                // a turnover happened
        S.lossAt = lostBallMs_;
        S.pressStart = now;
        S.lossBall = state_.ball;
        S.pressDecisions++;
        S.pressFailAt = -1e9;
    }
    const float s = (float)state_.attackSign;
    const MetricVector2D B = state_.ball;
    int car = -1;
    float cd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, B);
        if (d < cd) { cd = d; car = i; }
    }
    int nOwn = 0, nOpp = 0, cover = 0;
    float dOwn = 30.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& q = state_.team[i];
        if (!q.active || !q.visible || isKeeperIdx(i)) continue;
        const float d = distance(q.pos, B);
        dOwn = std::min(dOwn, d);
        if (d <= 9.f) nOwn++;
        if (goalDistance(q.pos) < goalDistance(B) - 1.f && d < 32.f && d > 3.5f) cover++;
    }
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible || i == car) continue;
        if (distance(o.pos, B) <= 9.f) nOpp++;
    }
    const bool loose = car < 0 || cd > cfg_.looseTouchDist;
    const float carSpeed = car >= 0 ? state_.opp[car].vel5.length() : 0.f;
    const float since = lostBallMs_ > 0.0 ? (float)((now - lostBallMs_) * 0.001) : 9.f;
    const float z = -0.50f + 0.50f * (float)nOwn - 0.42f * (float)nOpp - 0.10f * dOwn + (loose ? 0.80f : 0.f) - 0.12f * carSpeed - 0.35f * std::max(0.f, since - 1.2f);
    S.recProb += 0.4f * (v8Sigmoid(z) - S.recProb);
    S.coverCount = cover;
    S.pressOk = S.recProb >= cfg_.v8PressRecMin && cover >= cfg_.v8PressMinCover;
    (void)s;
}

// the counter-press switch of the pack is vetoed here
bool PitchEngine::v8PressGate() const {
    if (!cfg_.v8 || !cfg_.v8Press) return true;
    if (!v8_.pressOk) return false;
    if (cfg_.v8Fsm && (v8_.st == TS_RECOVER || v8_.st == TS_KICKOFF)) return false;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// 7. Tactical state machine
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8FsmUpdate() {
    V8State& S = v8_;
    const double now = state_.timeMs;
    if (state_.status == ST_NOT_LIVE) return;
    const int stab = plus_.stableStatus;
    bool edge = false;
    if (stab != S.fsmPrev) {
        if (stab == ST_TEAM) S.gainAt = now;
        if (S.fsmPrev == ST_TEAM && stab == ST_OPP) S.lossAt = now;
        if (stab == ST_OPP) S.oppGainAt = now;
        S.fsmPrev = stab;
        edge = true;
    }
    // kick-off: confirm the restart detector's guess with the layout (ball on the spot, both sides in their own half)
    if (v7_.koUntil > S.koSeenAt + 1.0 && v7_.koUntil > now) {
        S.koSeenAt = v7_.koUntil;
        const float s = (float)state_.attackSign;
        int ownN = 0, ownHalf = 0, oppN = 0, oppHalf = 0;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& q = state_.team[i];
            if (q.active && q.visible && !isKeeperIdx(i)) { ownN++; if (s * q.pos.x <= 1.5f) ownHalf++; }
            const PlayerState& o = state_.opp[i];
            if (o.active && o.visible) { oppN++; if (s * o.pos.x >= -1.5f) oppHalf++; }
        }
        const bool ballOk = ballUsable() && state_.ball.length() < 2.5f && state_.ballVel.length() < 2.0f;
        if (ballOk && ownN >= 6 && oppN >= 7 && ownHalf >= ownN - 2 && oppHalf >= oppN - 3) { S.koUntil = now + (double)cfg_.v8KickoffMs; S.koSeen = true; }
        else if (ballOk && ownN >= 6 && ownHalf * 2 < ownN && oppHalf * 2 < oppN) S.dirBad += 30;       // both teams in the wrong halves: the direction is suspect
    }
    const int lead = plus_.goalsFor - plus_.goalsAgainst;
    const float ph = v7Phase();
    const bool protect = (lead >= 1 && ph >= 0.62f) || (lead >= 2 && ph >= 0.45f);
    const bool lossWin = lostBallMs_ > 0.0 && now - lostBallMs_ < (double)cfg_.counterPressMs && stab != ST_TEAM;
    S.emergency = state_.status != ST_TEAM && ((S.preempt && goalDistance(state_.ball) < 45.f) || S.sk != SK_NONE);
    int want;
    if (S.emergency) want = TS_DEFEND;
    else if (now < S.koUntil) want = TS_KICKOFF;
    else if (stab == ST_TEAM) {
        int ahead = 0;
        const float s = (float)state_.attackSign;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (o.active && o.visible && !(o.pos.length() > 40.f && s * o.pos.x > 40.f) && s * o.pos.x > s * state_.ball.x + 1.f) ahead++;
        }
        if (protect) want = TS_PROTECT;
        else if (now - S.gainAt < 4500.0 && ahead <= 5) want = TS_COUNTER;
        else want = TS_CONTROL;
    } else {
        if (lossWin) {
            const bool progressed = goalDistance(state_.ball) < goalDistance(S.lossBall) - 6.f;
            if (S.st == TS_PRESS && now - S.pressStart > 1200.0 && (progressed || S.recProb < 0.8f * cfg_.v8PressRecMin)) { S.pressFailAt = now; want = TS_RECOVER; }
            else if (S.pressOk && S.pressFailAt < S.pressStart) want = TS_PRESS;
            else want = TS_RECOVER;
        } else if (S.st == TS_RECOVER && now - S.stSince < 3500.0 && S.coverCount < 3) want = TS_RECOVER;
        else want = protect ? TS_PROTECT : TS_DEFEND;
    }
    if (want != S.st) {
        const bool force = S.emergency || want == TS_KICKOFF || (want == TS_PRESS && edge);
        if (force || now >= S.stHold) {
            S.trans[S.st][want]++;
            S.dwell[S.st] += now - S.stSince;
            S.prevSt = S.st;
            S.st = want;
            S.stSince = now;
            static const int hold[TS_N] = {500, 600, 800, 700, 600, 1200, 0};
            S.stHold = now + hold[want];
            if (want == TS_PRESS) S.retreats += 0; else if (want == TS_RECOVER) S.retreats++;
        }
    }
    switch (S.st) {
        case TS_DEFEND:  S.riskMul = 1.5f; S.minAdd = 0.04f;  S.shotMul = 0.85f; S.lineAdd = 0.f;  S.pressAllowed = false; break;
        case TS_PRESS:   S.riskMul = 1.2f; S.minAdd = 0.02f;  S.shotMul = 1.0f;  S.lineAdd = 0.f;  S.pressAllowed = true;  break;
        case TS_RECOVER: S.riskMul = 1.6f; S.minAdd = 0.05f;  S.shotMul = 0.85f; S.lineAdd = -2.f; S.pressAllowed = false; break;
        case TS_COUNTER: S.riskMul = 0.7f; S.minAdd = -0.02f; S.shotMul = 1.15f; S.lineAdd = 0.f;  S.pressAllowed = false; break;
        case TS_PROTECT: S.riskMul = 1.9f; S.minAdd = 0.07f;  S.shotMul = 0.80f; S.lineAdd = -3.f; S.pressAllowed = false; break;
        case TS_KICKOFF: S.riskMul = 1.8f; S.minAdd = 0.06f;  S.shotMul = 0.70f; S.lineAdd = -4.f; S.pressAllowed = false; break;
        default:         S.riskMul = 1.0f; S.minAdd = 0.0f;   S.shotMul = 1.0f;  S.lineAdd = 0.f;  S.pressAllowed = false; break;
    }
    if (S.emergency) S.lineAdd = std::min(S.lineAdd, -2.f);
}

// kick-off protection: the five best-suited players (not simply the five nearest) take the central wall slots
bool PitchEngine::v8DefKickoff(GestureOutput* out, double now) {
    V8State& S = v8_;
    if (!cfg_.v8 || !cfg_.v8Kickoff || !out || now >= S.koUntil) return false;
    if (state_.ballVel.length() > 6.f) return false;                   // the ball is already away: landing zone logic takes over
    const int ci = controlledIdx();
    if (ci < 0 || isKeeperIdx(ci)) return false;
    const float own = -(float)state_.attackSign;
    int cand[TEAM_SIZE], n = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& q = state_.team[i];
        if (q.active && q.visible && !isKeeperIdx(i)) cand[n++] = i;
    }
    if (n < 3) return false;
    const int m = std::min(n, 5);
    MetricVector2D slot[5];
    for (int k = 0; k < m; k++) slot[k] = MetricVector2D(own * 7.f, ((float)k - (float)(m - 1) * 0.5f) * 3.2f);
    static float cost[HUNGARIAN_MAX * HUNGARIAN_MAX];
    int rowToCol[HUNGARIAN_MAX];
    for (int r = 0; r < n; r++) {
        const int j = cand[r];
        const int rr = role_[j];
        const float fit = rr == ROLE_CB ? -0.45f : (rr == ROLE_DM ? -0.30f : (rr == ROLE_CM ? -0.10f : (rr == ROLE_FB ? 0.15f : ((rr == ROLE_WG || rr == ROLE_ST) ? 0.7f : 0.2f))));
        const float tired = cfg_.v7Stamina ? (1.f - staminaOf(j)) * 0.4f : 0.f;
        for (int c = 0; c < n; c++)
            cost[r * n + c] = c < m ? plusTta(state_.team[j], slot[c], 7.0f) + fit + tired : 4.f;
    }
    hungarianSolve(cost, n, rowToCol);
    int mine = -1;
    for (int r = 0; r < n; r++) if (cand[r] == ci && rowToCol[r] >= 0 && rowToCol[r] < m) mine = rowToCol[r];
    if (mine < 0) return false;
    const MetricVector2D sp = v8Deconflict(slot[mine], ci);
    const float dd = distance(ctrlPos_, sp);
    *out = v7PressTo(sp, 150, false, dd < 1.0f ? 0.f : 1.f);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// 9. Shot / cutback / central pass prevention
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8ShotUpdate() {
    V8State& S = v8_;
    const double now = state_.timeMs;
    const int prevSk = S.sk;
    S.sk = SK_NONE;
    S.resp = SR_NONE;
    if (!ballUsable() || state_.status == ST_NOT_LIVE || state_.status == ST_TEAM) return;
    const float s = (float)state_.attackSign;
    const float own = -s;
    const MetricVector2D G(own * PITCH_HALF_X, 0.f);
    const MetricVector2D B = state_.ball;
    const MetricVector2D V = state_.ballVel;
    const float sp = V.length();
    const float gdB = goalDistance(B);
    int kind = SK_NONE;
    MetricVector2D from = B, to = G;
    float tGo = 9.f;
    int car = -1;
    float cd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, B);
        if (d < cd) { cd = d; car = i; }
    }
    if (cd > 3.2f) car = -1;
    // a) a shot in flight: fast ball heading at the goal mouth
    if (sp >= 9.f && gdB <= 34.f) {
        const float vTo = own * V.x;
        if (vTo > 0.55f * sp) {
            const float tg = gdB / std::max(vTo, 1.f);
            const float yAt = B.y + V.y * tg;
            if (std::fabs(yAt) <= cfg_.goalHalfWidth + 2.f) { kind = SK_SHOT; to = MetricVector2D(G.x, clampf(yAt, -cfg_.goalHalfWidth, cfg_.goalHalfWidth)); tGo = tg; }
        }
    }
    // b) cutback / central pass in flight
    if (kind == SK_NONE && sp >= 6.f && gdB <= 24.f) {
        const float vTo = own * V.x;
        const MetricVector2D dest = B + V * 0.7f;
        if (vTo < 0.2f * sp && std::fabs(dest.y) < std::fabs(B.y) - 2.5f && goalDistance(dest) > gdB && std::fabs(dest.y) <= 10.f && goalDistance(dest) <= 22.f) { kind = SK_CUTBACK; from = dest; }
        else if (std::fabs(V.y) > 0.5f * sp && std::fabs(dest.y) <= 9.f && goalDistance(dest) <= 20.f) { kind = SK_CENTRAL; from = dest; }
    }
    // c) pending threats while the ball is still with the carrier
    if (kind == SK_NONE && car >= 0) {
        const MetricVector2D c = state_.opp[car].pos;
        const float gd = goalDistance(c);
        const float xg = xTGrid::shotValue(own * c.x, c.y);
        if (gd <= 14.f && std::fabs(c.y) <= 11.f) { kind = SK_CLOSE; from = c; }
        else if (gd <= 16.f && std::fabs(c.y) > 13.f) {
            for (int j = 0; j < TEAM_SIZE; j++) {
                const PlayerState& o = state_.opp[j];
                if (j == car || !o.active || !o.visible) continue;
                if (std::fabs(o.future.y) <= 9.f && goalDistance(o.future) >= 7.f && goalDistance(o.future) <= 20.f) { kind = SK_CUTBACK; from = o.future; break; }
            }
        } else if (gd <= 22.f && xg >= 0.10f) { kind = SK_CLOSE; from = c; }
    }
    if (kind == SK_NONE) return;
    S.sk = kind;
    if (prevSk == SK_NONE) S.skSince = now;
    MetricVector2D T = to;
    if (kind != SK_SHOT) T = MetricVector2D(G.x, clampf(from.y * 0.5f, -2.5f, 2.5f));
    const float shotV = kind == SK_SHOT ? std::max(sp, 12.f) : 20.f;
    const float dLine = std::max(distance(from, T), 0.5f);
    if (kind != SK_SHOT) tGo = dLine / shotV + 0.35f;
    S.shotFrom = from;
    S.shotTo = T;
    S.shotSpeed = shotV;
    S.tReact = tGo;
    // lane cover: a team-mate already stands on the line
    S.laneCover = 0.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& q = state_.team[i];
        if (!q.active || !q.visible || isKeeperIdx(i)) continue;
        const MetricVector2D rel = q.pos - from;
        const MetricVector2D seg = T - from;
        const float t = (rel.x * seg.x + rel.y * seg.y) / std::max(seg.x * seg.x + seg.y * seg.y, 1e-6f);
        if (t > 0.05f && t < 0.95f && segmentDistance(from, T, q.pos) < 1.3f) { S.laneCover = 1.f; break; }
    }
    // the response for the controlled defender
    const int ci = controlledIdx();
    float tProj = 0.35f;
    if (ci >= 0) {
        const MetricVector2D rel = ctrlPos_ - from;
        const MetricVector2D seg = T - from;
        tProj = clampf((rel.x * seg.x + rel.y * seg.y) / std::max(seg.x * seg.x + seg.y * seg.y, 1e-6f), 0.12f, 0.75f);
    }
    S.blockPt = from + (T - from) * tProj;
    if (ci >= 0 && !isKeeperIdx(ci)) {
        const float tta = plusTta(state_.team[ci], S.blockPt, 7.0f);
        const float allowed = tProj * dLine / shotV + (kind == SK_SHOT ? 0.f : 0.3f);
        const bool feasible = tta <= allowed;
        bool mateCloser = false;
        for (int i = 0; i < TEAM_SIZE; i++) {
            if (i == ci) continue;
            const PlayerState& q = state_.team[i];
            if (!q.active || !q.visible || isKeeperIdx(i)) continue;
            if (distance(q.pos, S.blockPt) < 1.2f && plusTta(q, S.blockPt, 7.0f) + 0.2f < tta) { mateCloser = true; break; }
        }
        const float dCar = car >= 0 ? distance(ctrlPos_, state_.opp[car].pos) : 1e9f;
        if (mateCloser || S.laneCover > 0.5f) S.resp = SR_NONE;                       // already covered: do not leave position or crowd a team-mate
        else if (kind == SK_SHOT) S.resp = feasible ? SR_BLOCK : SR_RECOVER;
        else if (kind == SK_CLOSE) S.resp = dCar <= 5.f ? SR_PRESSURE : SR_BLOCK;
        else if (sp >= 6.f) {
            const MetricVector2D bp = B + V * (0.4f);
            S.resp = plusTta(state_.team[ci], bp, 7.0f) <= 0.4f ? SR_INTERCEPT : (feasible ? SR_BLOCK : SR_RECOVER);
        } else S.resp = dCar <= 7.f ? SR_PRESSURE : SR_BLOCK;
    }
    // counters (edge-debounced)
    const float xgFrom = xTGrid::shotValue(own * from.x, from.y);
    if (kind == SK_SHOT && now - S.lastShotAgainst > 3000.0) { S.shotsAgainst++; S.lastShotAgainst = now; v8RecordEvent("shot_against", sp); }
    if ((xgFrom >= 0.12f || kind == SK_SHOT) && now - S.lastDanger > 2500.0) { S.dangerAgainst++; S.lastDanger = now; v8RecordEvent("danger", xgFrom); }
}

bool PitchEngine::gkIntent(float* dirY, bool* rush) const {
    if (!cfg_.v8 || !cfg_.v8GkControl || v8_.sk == SK_NONE) return false;
    if (dirY) *dirY = v8_.shotTo.y >= 0.f ? 1.f : -1.f;
    if (rush) *rush = v8_.sk == SK_CLOSE || v8_.sk == SK_CUTBACK;
    return true;
}

bool PitchEngine::v8DefShot(GestureOutput* out, double now, bool noPress, float dC) {
    V8State& S = v8_;
    (void)dC;
    if (!cfg_.v8 || !cfg_.v8ShotGuard || !out || S.sk == SK_NONE) return false;
    if (S.resp == SR_NONE || S.resp == SR_PRESSURE) return false;       // pressure: the normal close-down logic handles the carrier
    const int ci = controlledIdx();
    if (ci < 0 || isKeeperIdx(ci)) return false;
    const MetricVector2D P = v8Deconflict(S.blockPt, ci);
    const float dd = distance(ctrlPos_, P);
    defNextAt_ = now + 100.0;
    if (dd < 0.8f) {
        GestureOutput h = v7PressTo(P, 120, false, 0.f);
        h.actionType = noPress ? ACT_MATCHUP : ACT_PRESS;
        *out = h;
        return true;
    }
    if (noPress) {
        GestureOutput g = makeGesture(ACT_MATCHUP, ctrlPos_, P, 1.f, false);
        g.status = ST_OPP; g.actionType = ACT_MATCHUP; g.durationMs = 200; g.valid = true; g.r = std::min(g.r, 0.8f);
        *out = g;
        return true;
    }
    *out = v7PressTo(P, 180, S.resp == SR_RECOVER && dd > 4.f, 1.f);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Defensive hooks: assignment, last-man hold, retreat, safe-mode fallback
// ---------------------------------------------------------------------------------------------------------------------

bool PitchEngine::v8DefRetreat(GestureOutput* out, double now, bool noPress) {
    if (!out || !ballUsable()) return false;
    const int ci = controlledIdx();
    if (ci < 0 || isKeeperIdx(ci)) return false;
    if (v8_.jobOf[ci] == JOB_PRESS) return false;
    const float own = -(float)state_.attackSign;
    const MetricVector2D G(own * PITCH_HALF_X, 0.f);
    const MetricVector2D B = state_.ball;
    const MetricVector2D toG = v8Unit(G - B, MetricVector2D(own, 0.f));
    const float gdB = goalDistance(B);
    const MetricVector2D rp = v8Deconflict(B + toG * clampf(0.5f * gdB, 6.f, 18.f), ci);
    const float dd = distance(ctrlPos_, rp);
    if (dd < 2.5f) return false;
    defNextAt_ = now + 150.0;
    if (noPress) {
        GestureOutput g = makeGesture(ACT_MATCHUP, ctrlPos_, rp, 1.f, false);
        g.status = ST_OPP; g.actionType = ACT_MATCHUP; g.durationMs = 220; g.valid = true; g.r = std::min(g.r, 0.8f);
        *out = g;
        return true;
    }
    *out = v7PressTo(rp, 200, dd > 10.f, 1.f);
    return true;
}

bool PitchEngine::v8DefHook(GestureOutput* out, double now, bool noPress, float dC) {
    V8State& S = v8_;
    if (!cfg_.v8 || !out || !(cfg_.v8Assign || cfg_.v8Shape)) return false;
    const int ci = controlledIdx();
    if (ci < 0 || isKeeperIdx(ci)) return false;
    if (cfg_.v8Fsm && S.st == TS_RECOVER && v8DefRetreat(out, now, noPress)) return true;
    // the game's switch picks the player nearest the ball: only worth sending when he is the assigned presser
    if (cfg_.v8Assign && S.jobHolder[JOB_PRESS] >= 0 && S.jobHolder[JOB_PRESS] != ci && now > S.switchLock && now - defSwitchAt_ > 600.0) {
        int nearest = -1;
        float nd = 1e9f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& q = state_.team[i];
            if (!q.active || !q.visible) continue;
            const float d = distance(q.pos, state_.ball);
            if (d < nd) { nd = d; nearest = i; }
        }
        if (nearest == S.jobHolder[JOB_PRESS] && dC - nd > 1.5f) {
            defSwitchAt_ = now;
            defNextAt_ = now + 450.0;
            S.switchLock = now + (double)cfg_.v8CommitMs;
            GestureOutput g;
            g.valid = true; g.status = ST_OPP; g.actionType = ACT_SWITCH; g.durationMs = 70;
            *out = g;
            return true;
        }
    }
    int job = cfg_.v8Assign ? S.jobOf[ci] : -1;
    MetricVector2D P;
    if (job == JOB_LINE || (job < 0 && cfg_.v8Shape && S.holdOk && ci == S.lastMan)) { job = JOB_LINE; P = S.holdPt; }
    else if (job == JOB_LANE || job == JOB_RUNNER) P = S.jobPt[job];
    else return false;                                                // pressing job (or none): the normal logic runs
    if (job == JOB_LINE && !S.holdOk) return false;
    P = v8Deconflict(P, ci);
    const float dd = distance(ctrlPos_, P);
    if (dd < 1.2f) return false;
    defNextAt_ = now + 120.0;
    if (noPress) {
        GestureOutput g = makeGesture(ACT_MATCHUP, ctrlPos_, P, 1.f, false);
        g.status = ST_OPP; g.actionType = ACT_MATCHUP; g.durationMs = 200; g.valid = true; g.r = std::min(g.r, 0.8f);
        *out = g;
        return true;
    }
    *out = v7PressTo(P, 200, dd > 8.f, dd < 3.f ? 0.6f : 1.f);
    return true;
}

// emergency defensive fallback while the radar cannot be trusted: stand goal-side of the last known ball, no dash, no switching
bool PitchEngine::v8DefSafe(GestureOutput* out, double now, bool noPress) {
    (void)noPress;
    if (!cfg_.v8 || !cfg_.v8Guard || !v8_.safe || !out || !ballUsable()) return false;
    const int ci = controlledIdx();
    if (ci < 0 || isKeeperIdx(ci)) return false;
    const float own = -(float)state_.attackSign;
    const MetricVector2D G(own * PITCH_HALF_X, 0.f);
    const MetricVector2D B = state_.ball;
    const MetricVector2D toG = v8Unit(G - B, MetricVector2D(own, 0.f));
    const MetricVector2D post = B + toG * std::min(6.f, std::max(goalDistance(B) - 3.f, 1.f));
    if (distance(ctrlPos_, post) < 1.0f) return false;
    GestureOutput g = makeGesture(ACT_MATCHUP, ctrlPos_, post, 1.f, false);
    g.status = ST_OPP; g.actionType = ACT_MATCHUP; g.durationMs = 200; g.valid = true; g.r = std::min(g.r, 0.6f); g.dash = false;
    defNextAt_ = now + 250.0;
    *out = g;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// 8. Opponent observation: pattern events with evidence weights, line statistics, response learning, biases
// ---------------------------------------------------------------------------------------------------------------------

const V8OppProfile* PitchEngine::v8Profile() const {
    for (int i = 0; i < v8m_.nProf; i++) if (v8m_.prof[i].key == v7m_.curSig) return &v8m_.prof[i];
    return nullptr;
}

int PitchEngine::v8ProfileSlot(bool create) {
    V8Mem& M = v8m_;
    for (int i = 0; i < M.nProf; i++) if (M.prof[i].key == v7m_.curSig) return i;
    if (!create) return -1;
    int slot;
    if (M.nProf < V8_PROF) slot = M.nProf++;
    else { slot = 0; for (int i = 1; i < V8_PROF; i++) if (M.prof[i].n < M.prof[slot].n) slot = i; }
    M.prof[slot] = V8OppProfile();
    M.prof[slot].key = v7m_.curSig;
    return slot;
}

static float v8ProfConf(const V8OppProfile& P) {
    float num = 0.f, den = 0.f;
    for (int p = 0; p < V8_PAT; p++) {
        const float evid = 1.f - std::exp(-P.wsum[p] / 8.f);
        float cons = 0.5f;
        if (P.n >= 2) { const float var = std::max(0.f, P.rate2[p] - P.rate[p] * P.rate[p]); cons = 1.f - clampf(std::sqrt(var) / (P.rate[p] + 0.5f), 0.f, 1.f); }
        num += P.rate[p] * evid * cons;
        den += P.rate[p];
    }
    return den > 1e-4f ? clampf(num / den, 0.f, 1.f) : 0.f;
}

void PitchEngine::v8OppObserve() {
    V8State& S = v8_;
    const double now = state_.timeMs;
    if (state_.status == ST_NOT_LIVE || !ballUsable()) return;
    const float s = (float)state_.attackSign;
    const float own = -s;
    const MetricVector2D B = state_.ball;
    const float bsp = state_.ballVel.length();
    const float w = S.trackConf;
    int car = -1;
    float cd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, B);
        if (d < cd) { cd = d; car = i; }
    }
    if (cd > 3.0f) car = -1;
    if (car >= 0 && state_.status == ST_OPP) { S.prevOppCarPos = state_.opp[car].pos; S.prevOppCarAt = now; }
    auto open = [&](int pat, const MetricVector2D& from, float gd0) {
        if (now - S.lastPatAt[pat] < 1500.0) return;
        S.lastPatAt[pat] = now;
        for (int k = 0; k < V8_REL; k++) if (!S.rel[k].on) {
            S.rel[k].on = true; S.rel[k].pat = pat; S.rel[k].t = now; S.rel[k].from = from; S.rel[k].w = w; S.rel[k].gd0 = gd0; S.rel[k].resp = S.lineMode;
            return;
        }
    };
    // opposition release: the ball leaves the opposition carrier fast
    if (now - S.prevOppCarAt < 400.0 && distance(B, S.prevOppCarPos) < 5.f && bsp >= 9.5f && S.prevBallSp < 7.f && state_.status != ST_TEAM) {
        const MetricVector2D from = S.prevOppCarPos;
        const MetricVector2D u = v8Unit(state_.ballVel, MetricVector2D(own, 0.f));
        const float trav = std::min(40.f, bsp * 1.3f);
        const MetricVector2D dest = B + u * trav;
        const float vTo = own * state_.ballVel.x;
        const float gd0 = goalDistance(from);
        int pat = -1;
        bool recvAhead = false;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (o.active && o.visible && distance(o.pos, dest) < 4.5f && goalDistance(o.pos) < gd0 - 8.f) recvAhead = true;
        }
        if (vTo > 0.6f * bsp && gd0 > 22.f && recvAhead) pat = OP_THROUGH;
        else if (std::fabs(state_.ballVel.y) > 0.8f * bsp && trav * std::fabs(u.y) >= 20.f) pat = OP_SWITCH;
        else if (gd0 < 24.f && std::fabs(from.y) > 16.f && std::fabs(dest.y) < 12.f && vTo > -0.2f * bsp) pat = OP_WINGCUT;
        else if (bsp >= 14.f && trav >= 28.f && gd0 > 45.f) pat = OP_LONG;
        if (pat >= 0) {
            open(pat, from, gd0);
            if (pat == OP_THROUGH && S.oppGainAt > 0.0 && now - S.oppGainAt < 20000.0) { S.throughDelaySum += (float)((now - S.oppGainAt) * 0.001); S.throughDelayN++; }
        }
    }
    // their press on us: three or more of them around our carrier in our own half
    if (state_.status == ST_TEAM && state_.carrier >= 0 && state_.carrier < TEAM_SIZE) {
        const PlayerState& c = state_.team[state_.carrier];
        int near = 0;
        for (int i = 0; i < TEAM_SIZE; i++) { const PlayerState& o = state_.opp[i]; if (o.active && o.visible && distance(o.pos, c.pos) <= 11.f) near++; }
        if (near >= 3 && s * c.pos.x < 5.f) {
            if (now - S.lastPatAt[OP_PRESS] >= 4000.0) { open(OP_PRESS, c.pos, (float)statTurnovers_); }
        }
    }
    // resolve events after 2.8 s
    for (int k = 0; k < V8_REL; k++) {
        V8Rel& R = S.rel[k];
        if (!R.on || now - R.t < 2800.0) continue;
        bool ok;
        if (R.pat == OP_PRESS) ok = statTurnovers_ > (int)R.gd0;                              // they won the ball
        else ok = (plus_.stableStatus == ST_OPP && goalDistance(B) < R.gd0 - 6.f) || S.lastShotAgainst > R.t;
        S.pat[R.pat] += R.w;
        if (ok) S.patOk[R.pat] += R.w;
        S.respSum[R.pat][R.resp] += R.w * (ok ? 0.f : 1.f);
        S.respW[R.pat][R.resp] += R.w;
        R.on = false;
    }
    // routes: where the opposition attacks from, sampled once a second in their attacking third
    static thread_local double lastRoute = -1e9;
    if (state_.status == ST_OPP && goalDistance(B) < 45.f && now - lastRoute > 1000.0) {
        lastRoute = now;
        S.routeCnt[B.y < -11.f ? 0 : (B.y > 11.f ? 2 : 1)] += 1.f;
    }
    // line statistics, once a second
    static thread_local double lastLine = -1e9;
    if (oppLineConf_ > 0.4f && now - lastLine > 1000.0) {
        lastLine = now;
        S.lineN += 1.0;
        const double d = oppLineDepth_ - S.lineMean;
        S.lineMean += d / S.lineN;
        S.lineM2 += d * (oppLineDepth_ - S.lineMean);
    }
    // biases from the stored profile (every 2 s); the response mode is picked per dominant pattern with 15 % exploration
    static thread_local double lastBias = -1e9;
    if (now - lastBias > 2000.0) {
        lastBias = now;
        const V8OppProfile* P = v8Profile();
        S.oppLineBias = 0.f;
        S.oppPressMul = 1.f;
        if (P && P->conf > 0.2f) {
            const float danger = v8Sat(P->rate[OP_THROUGH] / 3.f) * (P->wsum[OP_THROUGH] > 0.f ? P->succ[OP_THROUGH] : 0.f);
            float bias = -0.6f * danger * P->conf;
            int dom = 0;
            for (int p = 1; p < V8_PAT; p++) if (P->rate[p] > P->rate[dom]) dom = p;
            int mode = 1;
            float bq = -1.f;
            for (int r = 0; r < V8_RESP; r++) { const float q = P->resp[dom][r] + (r == 1 ? 0.02f : 0.f); if (q > bq) { bq = q; mode = r; } }
            uint32_t sd = (uint32_t)(frameNo_ / 40) * 2654435761u + 77u;
            if (plusRand(sd) < 0.15f) mode = (int)(plusRand(sd) * 3.f) % 3;
            if (P->respN[dom][mode] < 3.f) mode = 1;
            S.lineMode = mode;
            static const float mb[3] = {-0.4f, 0.f, 0.2f};
            bias += mb[mode] * P->conf;
            S.oppLineBias = clampf(bias, -0.8f, 0.3f);
            S.oppPressMul = 1.f + 0.5f * v8Sat(P->rate[OP_PRESS] / 4.f) * P->succ[OP_PRESS] * P->conf;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 5. Plan validity
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8PlanValidate() {
    V8State& S = v8_;
    V7State& P = v7_;
    const double now = state_.timeMs;
    if (P.nPlan <= 0 || now >= P.planUntil) return;
    const char* why = nullptr;
    if (state_.status != ST_TEAM) { if (now - P.planAt > 300.0 && plus_.stableStatus != ST_TEAM) why = "possession"; }
    else if (S.preempt && S.topScore > cfg_.v8ThreatTrig + 0.1f && S.st == TS_DEFEND) why = "threat";
    else if (state_.carrier >= 0 && state_.carrier < TEAM_SIZE) {
        const V7Plan& p0 = P.plan[0];
        if (p0.type == ACT_PASS || p0.type == ACT_THROUGH || p0.type == ACT_LOFTED) {
            const int ti = p0.targetId - 1;
            if (ti < 0 || ti >= TEAM_SIZE || !state_.team[ti].active || !state_.team[ti].visible) why = "receiver gone";
            else if (ti == state_.carrier) why = nullptr;
            else {
                float cl = 0.f;
                const float xp = passProbability(state_.team[state_.carrier].pos, state_.team[ti].future, cl, p0.type == ACT_THROUGH, false);
                if (xp < 0.45f) why = "lane closed";
            }
        }
    }
    if (why) { P.nPlan = 0; P.planUntil = -1e9; S.planCancels++; v8RecordEvent(why, 0.f); }
}

// ---------------------------------------------------------------------------------------------------------------------
// Experience stream of PitchBot's own decisions (with the best alternative), regret counter
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8NoteDecision() {
    V8State& S = v8_;
    const double now = state_.timeMs;
    // resolve the pending record
    if (S.expPend >= 0) {
        V8Exp& E = S.exp[S.expPend];
        if (now - E.t > 2500.0) {
            const bool lost = statTurnovers_ > E.tov0;
            float r = lost ? -0.8f : 0.15f;
            if (shotsLive_ > E.shots0) r += 0.8f;
            E.r = clampf(r, -1.f, 1.f);
            E.done = true;
            if (E.r < 0.f && E.alt >= 0) {
                bool ok = false;
                const float est = v7RewardModel(E.f, E.alt, &ok);
                if (ok && est > E.r + 0.2f) S.regret++;
            }
            S.expPend = -1;
        }
        return;
    }
    if (human_ || state_.status != ST_TEAM || state_.carrier < 0 || chosen_ < 0 || chosen_ >= nCand_ || now - S.expAt < 300.0) return;
    if (!gesture_.valid) return;
    const PassCandidate& c = cand_[chosen_];
    int alt = -1;
    float bu = -1e9f;
    const int cls = v7ClassOf(c);
    for (int i = 0; i < nCand_; i++) {
        if (i == chosen_) continue;
        const int k = v7ClassOf(cand_[i]);
        if (k != cls && cand_[i].utility > bu) { bu = cand_[i].utility; alt = k; }
    }
    V8Exp& E = S.exp[S.expHead];
    E = V8Exp();
    v7StateFeat(state_.team[state_.carrier], E.f);
    E.cls = cls; E.alt = alt; E.margin = alt >= 0 ? c.utility - bu : 0.f; E.t = now; E.tov0 = statTurnovers_; E.shots0 = shotsLive_;
    S.expPend = S.expHead;
    S.expHead = (S.expHead + 1) % V8_EXP;
    if (S.nExp < V8_EXP) S.nExp++;
    S.expAt = now;
}

// ---------------------------------------------------------------------------------------------------------------------
// 6. Utility layer: counter exposure, state limits, double-team safety, safe-mode restrictions
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8Apply(const PlayerState& car) {
    V8State& S = v8_;
    if (!cfg_.v8 || human_) return;
    (void)car;
    const bool safe = cfg_.v8Guard && S.safe;
    const float riskMul = cfg_.v8Fsm ? S.riskMul : 1.f;
    for (int i = 0; i < nCand_; i++) {
        PassCandidate& c = cand_[i];
        const bool isPass = c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED;
        float add = 0.f;
        if (cfg_.v8Exposure && c.type != ACT_SHOOT) {
            const float loss = isPass ? 1.f - clampf(c.xP, 0.f, 1.f) : (c.type == ACT_CARRY ? clampf(0.12f + 0.45f * clampf(c.pressure, 0.f, 1.f), 0.f, 1.f) : 0.5f);
            int oppNear = 0, ownCover = 0;
            const float gT = goalDistance(c.target);
            for (int k = 0; k < TEAM_SIZE; k++) {
                const PlayerState& o = state_.opp[k];
                if (o.active && distance(o.pos, c.target) < 12.f) oppNear++;
                const PlayerState& q = state_.team[k];
                if (!q.active || isKeeperIdx(k) || k == c.targetId - 1) continue;
                if (goalDistance(q.pos) < gT - 1.f && distance(q.pos, c.target) < 25.f) ownCover++;
            }
            const float expo = clampf((float)(oppNear - ownCover + 1) / 3.f, 0.f, 1.f);
            const float depthF = clampf(1.f - gT / 55.f, 0.15f, 1.f);
            add -= cfg_.wExpo * loss * expo * depthF * riskMul;
        }
        if (cfg_.v8Fsm) {
            if (isPass && S.minAdd > 0.f) {
                const float hi = cfg_.minPassXp + S.minAdd;
                if (c.xP < hi) add -= 0.25f * (hi - c.xP) / std::max(hi, 0.1f);
            }
            if (c.type == ACT_SHOOT) add += 0.20f * (S.shotMul - 1.f) * clampf(c.xP, 0.f, 1.f);
            if ((S.st == TS_PROTECT || S.st == TS_KICKOFF || S.st == TS_RECOVER) && c.type == ACT_PASS && c.xP >= 0.80f) add += 0.10f * (c.xP - 0.7f) * (cfg_.v8OppModel ? S.oppPressMul : 1.f);
            if (S.st == TS_COUNTER && isPass && c.deltaXT > 0.02f && c.xP >= 0.6f) add += 0.08f;
        }
        if (safe) {
            if (c.type == ACT_THROUGH || c.type == ACT_LOFTED) add -= 3.f;
            else if (c.type == ACT_SHOOT) add -= 1.5f;
            else if (c.type == ACT_CARRY) add -= 0.4f;
            else if (isPass && c.xP < 0.8f) add -= 0.6f * (0.8f - c.xP) / 0.8f;
        }
        c.utility += add;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 10. Input latency per kind, bounded compensation
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::noteInputLatency(int kind, float queueMs, float overheadMs, int flags) {
    if (!cfg_.v8 || !cfg_.v8Latency) return;
    if (!(queueMs >= 0.f && queueMs < 5000.f && overheadMs >= 0.f && overheadMs < 5000.f)) return;
    V8LatKind& L = v8_.lat[kind < 0 ? 0 : (kind >= IK_N ? IK_N - 1 : kind)];
    if (flags & 1) L.late++;
    if (flags & 2) { L.missed++; return; }                      // a dropped command has no timing
    if (flags & 4) { L.timeouts++; return; }
    const float x = queueMs + overheadMs;
    if (L.n < 200) L.n++;
    const float a = 1.f / (float)L.n;
    const float d = x - L.mean;
    L.mean += a * d;
    L.m2 = (1.f - a) * (L.m2 + a * d * d);
    L.qMean += a * (queueMs - L.qMean);
    L.oMean += a * (overheadMs - L.oMean);
    L.ema = L.n == 1 ? x : L.ema + 0.2f * (x - L.ema);
    if (L.n >= 30 && std::fabs(L.ema - L.mean) > std::max(25.f, 0.35f * L.mean)) L.drift++; else L.drift = std::max(0, L.drift - 1);
    if (L.drift >= 12) {                                          // the device changed (load, frame rate): recalibrate quickly
        L.mean = L.ema; L.m2 = (0.25f * L.ema) * (0.25f * L.ema); L.n = 20; L.drift = 0; L.recal++;
    }
}

float PitchEngine::latencyLeadMs(int kind) const {
    const V8LatKind& L = v8_.lat[kind < 0 ? 0 : (kind >= IK_N ? IK_N - 1 : kind)];
    if (L.n < 12) return 0.f;
    return clampf(L.mean + 0.5f * L.sd(), 0.f, 400.f);
}

// per-kind residual relative to the average of all kinds with data: the common part is already in effLatencyMs
float PitchEngine::v8LatencyComp(int kind) const {
    if (!cfg_.v8 || !cfg_.v8Latency) return 0.f;
    float sum = 0.f; int k = 0;
    for (int i = 0; i < IK_N; i++) if (v8_.lat[i].n >= 12) { sum += latencyLeadMs(i); k++; }
    if (k < 2 || v8_.lat[kind < 0 ? 0 : (kind >= IK_N ? IK_N - 1 : kind)].n < 12) return 0.f;
    return clampf(latencyLeadMs(kind) - sum / (float)k, -cfg_.v8LatMaxComp, cfg_.v8LatMaxComp);
}

std::string PitchEngine::inputLatencyReport() const {
    static const char* N[IK_N] = {"move", "pass", "shot", "switch", "other"};
    std::string o;
    char b[220];
    for (int k = 0; k < IK_N; k++) {
        const V8LatKind& L = v8_.lat[k];
        std::snprintf(b, sizeof b, "%-6s n=%d mean=%.0fms sd=%.0f queue=%.0f dispatch=%.0f late=%d missed=%d timeouts=%d recal=%d | decision->ball release %.0fms (n=%d, none seen %d)\n",
                      N[k], L.n, (double)L.mean, (double)L.sd(), (double)L.qMean, (double)L.oMean, L.late, L.missed, L.timeouts, L.recal, (double)L.effMean, L.effN, L.effMiss);
        o += b;
    }
    o += "note: dispatch time confirms the gesture reached Android, not that the game acted on it; the last column is the observed effect.\n";
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// 11/12. Command sanitiser, frame recorder
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8RecordEvent(const char* what, float value) {
    V8State& S = v8_;
    if (!cfg_.v8Record || S.recTrunc) return;
    char b[96];
    std::snprintf(b, sizeof b, "O,%lld,%s,%.2f\n", (long long)state_.timeMs, what, (double)value);
    S.rec += b;
}

void PitchEngine::v8Record(const MetricVector2D* home, int nHome, const MetricVector2D* away, int nAway, const MetricVector2D* ball, bool ballSeen) {
    V8State& S = v8_;
    if (!cfg_.v8 || !cfg_.v8Record || S.recTrunc) return;
    if (S.recFrames >= 36000) { S.recTrunc = true; return; }
    if (!S.recHeader) {
        char h[96];
        std::snprintf(h, sizeof h, "H,v6.18,%d,%d\n", state_.attackSign, v8_.evalMatch ? 1 : 0);
        S.rec.reserve(1 << 20);
        S.rec += h;
        S.recHeader = true;
    }
    char b[64];
    std::snprintf(b, sizeof b, "F,%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d", (long long)frameTimeMs_, hudPoss_, ctrlSeen_ ? 1 : 0, (int)std::lround(ctrlPos_.x * 100.f),
                  (int)std::lround(ctrlPos_.y * 100.f), ballSeen && ball ? 1 : 0, ball ? (int)std::lround(ball->x * 100.f) : 0, ball ? (int)std::lround(ball->y * 100.f) : 0, nHome, nAway);
    S.rec += b;
    for (int i = 0; i < nHome; i++) { std::snprintf(b, sizeof b, ",%d,%d", (int)std::lround(home[i].x * 100.f), (int)std::lround(home[i].y * 100.f)); S.rec += b; }
    for (int i = 0; i < nAway; i++) { std::snprintf(b, sizeof b, ",%d,%d", (int)std::lround(away[i].x * 100.f), (int)std::lround(away[i].y * 100.f)); S.rec += b; }
    S.rec += "\n";
    S.recFrames++;
}

std::string PitchEngine::exportReplay() const { return v8_.rec.empty() ? v8m_.lastReplay : v8_.rec; }

// last line of defence before the gesture leaves the engine
GestureOutput PitchEngine::v8Sanitize(const GestureOutput& in) {
    V8State& S = v8_;
    GestureOutput g = in;
    const double now = state_.timeMs;
    if (cfg_.v8Guard && g.valid) {
        const int t = g.actionType;
        const bool attackType = t == ACT_CARRY || t == ACT_PASS || t == ACT_THROUGH || t == ACT_LOFTED || t == ACT_SHOOT || t == ACT_SHOT;
        const bool defType = t == ACT_TACKLE || t == ACT_SWITCH || t == ACT_MATCHUP || t == ACT_PRESS || t == ACT_STEPUP;
        bool drop = false;
        if (!std::isfinite(g.theta) || !std::isfinite(g.r)) drop = true;
        else if (attackType && g.status != ST_TEAM && state_.status != ST_TEAM) drop = true;             // attacking command without the ball
        else if (defType && state_.status == ST_TEAM && g.status == ST_TEAM) drop = true;                // defensive command while we hold it
        else if (S.safe && (t == ACT_THROUGH || t == ACT_LOFTED || t == ACT_SHOT)) drop = true;          // risky automation paused
        if (drop) { g = GestureOutput(); g.status = in.status; S.cmdDrops++; v8RecordEvent("drop", (float)t); }
        else {
            if (g.dash && (t == ACT_SHOT || t == ACT_SHOOT || t == ACT_TACKLE || t == ACT_SWITCH)) { g.dash = false; S.cmdConflicts++; }
            if (g.flick != 0 && t != ACT_SHOT) { g.flick = 0; S.cmdConflicts++; }
            if (g.durationMs < 1) g.durationMs = 1;
            if (g.durationMs > 4095) g.durationMs = 4095;
        }
    }
    // effect-latency bookkeeping and the recorder
    if (cfg_.v8Latency && g.valid) {
        const int t = g.actionType;
        const int kind = (t == ACT_PASS || t == ACT_THROUGH || t == ACT_LOFTED) ? IK_PASS : ((t == ACT_SHOOT || t == ACT_SHOT) ? IK_SHOT : -1);
        if (kind >= 0 && state_.status == ST_TEAM && (S.pendKind != kind || now - S.pendAt > 1500.0)) { S.pendKind = kind; S.pendAt = now; }
    }
    if (cfg_.v8Record && !S.recTrunc && g.valid) {
        char b[160];
        std::snprintf(b, sizeof b, "G,%lld,%d,%d,%.1f,%.2f,%d,%d,%d\n", (long long)now, g.status, g.actionType, (double)g.theta, (double)g.r, g.durationMs, state_.carrier,
                      chosen_ >= 0 && chosen_ < nCand_ ? cand_[chosen_].targetId : -1);
        S.rec += b;
    }
    return g;
}

// ---------------------------------------------------------------------------------------------------------------------
// 12. Per-match metrics, train / evaluation split, regression gate
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v8BeginMatch() {
    v8_.clear();
    v8_.evalMatch = cfg_.v8 && cfg_.v8Bench && (v8m_.seen % 4 == 3);      // every fourth match is measured, never trained on
}

bool PitchEngine::v8Suspect(std::string* why) const {
    if (!cfg_.v8 || !cfg_.v8Guard || !why) return false;
    const V8State& S = v8_;
    bool bad = false;
    auto add = [&](const char* w) { if (!why->empty()) *why += ", "; *why += w; bad = true; };
    if (S.liveFrames > 100) {
        if (S.confSum / (double)S.frames < 0.45) add("low tracking confidence");
        if ((double)S.safeFrames / (double)S.liveFrames > 0.35) add("safe mode too long");
        if (S.dirSuspect) add("attack direction doubtful");
        if (S.scaleBad) add("radar scale doubtful");
    }
    return bad;
}

static void v8AggAdd(V8Agg& A, const V8MatchRec& r) {
    A.n++;
    const float a = 1.f / (float)A.n;
    const float gd = (float)(r.gf - r.ga);
    A.cs += a * ((r.ga == 0 ? 1.f : 0.f) - A.cs);
    A.gd += a * (gd - A.gd);
    A.danger += a * ((float)r.dangerAgainst - A.danger);
    A.shots += a * ((float)r.shotsFor - A.shots);
    A.win += a * ((r.gf > r.ga ? 1.f : (r.gf == r.ga ? 0.5f : 0.f)) - A.win);
    A.perf += a * (r.perf - A.perf);
}

void PitchEngine::v8UpdateProfile(const V8MatchRec& rec) {
    const V8State& S = v8_;
    const int slot = v8ProfileSlot(true);
    V8OppProfile& P = v8m_.prof[slot];
    P.n++;
    const float mins = std::max(rec.liveSec / 60.f, 1.f);
    for (int p = 0; p < V8_PAT; p++) {
        const float r = S.pat[p] / mins;
        P.rate[p] += (r - P.rate[p]) / (float)P.n;
        P.rate2[p] += (r * r - P.rate2[p]) / (float)P.n;
        const float tot = P.wsum[p] + S.pat[p];
        if (tot > 1e-4f) P.succ[p] = (P.succ[p] * P.wsum[p] + S.patOk[p]) / tot;
        P.wsum[p] = std::min(tot, 400.f);
        for (int q = 0; q < V8_RESP; q++) {
            const float t2 = P.respN[p][q] + S.respW[p][q];
            if (t2 > 1e-4f) P.resp[p][q] = (P.resp[p][q] * P.respN[p][q] + S.respSum[p][q]) / t2;
            P.respN[p][q] = std::min(t2, 200.f);
        }
    }
    if (S.lineN > 5.0) {
        const float sd = (float)std::sqrt(S.lineM2 / S.lineN);
        P.lineDepth = P.n == 1 ? (float)S.lineMean : P.lineDepth + 0.3f * ((float)S.lineMean - P.lineDepth);
        P.lineSd = P.n == 1 ? sd : P.lineSd + 0.3f * (sd - P.lineSd);
    }
    if (S.throughDelayN > 0) {
        const float d = S.throughDelaySum / (float)S.throughDelayN;
        P.throughDelay = P.throughN == 0 ? d : P.throughDelay + 0.3f * (d - P.throughDelay);
        P.throughN += S.throughDelayN;
    }
    const float rt = S.routeCnt[0] + S.routeCnt[1] + S.routeCnt[2];
    if (rt > 3.f) for (int k = 0; k < 3; k++) P.route[k] += 0.3f * (S.routeCnt[k] / rt - P.route[k]);
    P.conf = v8ProfConf(P);
}

void PitchEngine::v8EndMatch(std::string& rep, bool valid) {
    V8State& S = v8_;
    V8Mem& M = v8m_;
    if (!cfg_.v8 || !cfg_.v8Bench) return;
    V8MatchRec r;
    r.gen = M.gen;
    r.split = S.evalMatch ? 1 : 0;
    r.valid = valid ? 1 : 0;
    r.gf = plus_.goalsFor; r.ga = plus_.goalsAgainst;
    r.shotsFor = shotsLive_; r.shotsAgainst = S.shotsAgainst; r.dangerAgainst = S.dangerAgainst; r.turnovers = statTurnovers_;
    r.liveSec = (float)(plus_.liveMs / 1000.0);
    r.trackConf = S.frames > 0 ? (float)(S.confSum / (double)S.frames) : 0.f;
    r.safeFrac = S.liveFrames > 0 ? (float)S.safeFrames / (float)S.liveFrames : 0.f;
    r.poss = possShare_;
    const float mins = std::max(r.liveSec / 60.f, 1.f);
    for (int p = 0; p < V8_PAT; p++) { r.patN[p] = S.pat[p] / mins; r.patOk[p] = S.pat[p] > 1e-3f ? S.patOk[p] / S.pat[p] : -1.f; }
    const float gd = (float)(r.gf - r.ga);
    r.perf = clampf(0.35f + 0.20f * clampf(gd / 3.f, -1.f, 1.f) + 0.20f * (r.ga == 0 ? 1.f : 0.f) + 0.15f * (r.gf > r.ga ? 1.f : (r.gf == r.ga ? 0.5f : 0.f)) + 0.10f * (1.f - clampf((float)r.dangerAgainst / 6.f, 0.f, 1.f)), 0.f, 1.f);
    M.seen++;
    const bool train = valid && !S.evalMatch && cfg_.v7Learn;
    if (valid && !S.evalMatch && cfg_.v8OppModel) v8UpdateProfile(r);
    const V8OppProfile* P = v8Profile();
    r.conf = P ? P->conf : 0.f;
    if (P) { float w = 0.f; for (int p = 0; p < V8_PAT; p++) w += P->wsum[p]; r.suff = clampf(w / 30.f, 0.f, 1.f); }
    if (valid && S.evalMatch) {
        v8AggAdd(M.cand, r);
        M.sinceGood++;
        r.held = M.haveGood ? M.cand.perf - M.good.perf : 0.f;
    } else if (M.haveGood) r.held = M.cand.n > 0 ? M.cand.perf - M.good.perf : 0.f;
    if (train) M.gen++;
    M.hist[M.hHead] = r;
    M.hHead = (M.hHead + 1) % V8_HIST;
    if (M.nHist < V8_HIST) M.nHist++;
    char b[200];
    std::snprintf(b, sizeof b, "v6.18: %s match%s, score %d-%d, shots against %d, dangerous chances %d, tracking %.2f; ", S.evalMatch ? "evaluation" : "training", valid ? "" : " (not valid)",
                  r.gf, r.ga, r.shotsAgainst, r.dangerAgainst, (double)r.trackConf);
    rep += b;
    if (valid && S.evalMatch) v8Gate(rep);
    if (cfg_.v8Record) M.lastReplay.swap(S.rec); else M.lastReplay.clear();
}

void PitchEngine::v8Gate(std::string& rep) {
    V8Mem& M = v8m_;
    if (M.cand.n < cfg_.v8GateMinEval) { M.verdict = GV_INSUFFICIENT; M.reason = "collecting evaluation matches"; return; }
    char b[160];
    if (!M.haveGood) {
        M.good = M.cand; M.haveGood = true; M.cand = V8Agg(); M.sinceGood = 0;
        M.verdict = GV_PROMOTE; M.verdictPending = true; M.promotions++; M.reason = "baseline established";
        rep += "gate: baseline established (promote); ";
        return;
    }
    const float csDrop = M.good.cs - M.cand.cs;
    const float gdDrop = M.good.gd - M.cand.gd;
    const float dangerUp = (M.cand.danger - M.good.danger) / std::max(M.good.danger, 0.5f);
    const float attackDrop = (M.good.shots - M.cand.shots) / std::max(M.good.shots, 0.5f);
    const bool severe = csDrop > 0.15f || gdDrop > 0.75f || dangerUp > 0.35f || attackDrop > 0.5f;
    const bool improved = !severe && (M.cand.cs > M.good.cs + 0.05f || M.cand.gd > M.good.gd + 0.25f);
    if (severe) {
        std::snprintf(b, sizeof b, "regression: clean sheets %+.2f, goal difference %+.2f, dangerous chances %+.0f%%, attacking output %+.0f%%", (double)-csDrop, (double)-gdDrop, (double)(dangerUp * 100.f), (double)(-attackDrop * 100.f));
        const bool tradeoff = M.cand.win > M.good.win + 0.1f || M.cand.cs > M.good.cs + 0.05f;       // one metric better, another severely worse: still rejected
        M.reason = std::string(b) + (tradeoff ? " (trade-off rejected)" : "");
        M.verdict = GV_ROLLBACK; M.verdictPending = true; M.rollbacks++;
        M.cand = V8Agg(); M.sinceGood = 0;
        rep += "gate: ROLLBACK - " + M.reason + "; ";
    } else if (improved || M.cand.n >= 12) {
        M.reason = improved ? "better than the known-good version, no severe regression" : "stable after 12 evaluation matches";
        M.good = M.cand; M.cand = V8Agg(); M.sinceGood = 0;
        M.verdict = GV_PROMOTE; M.verdictPending = true; M.promotions++;
        rep += "gate: promote - " + M.reason + "; ";
    } else {
        M.verdict = GV_HOLD; M.reason = "no severe regression, no clear gain yet";
        rep += "gate: hold; ";
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Persistence and dashboard data
// ---------------------------------------------------------------------------------------------------------------------

std::string PitchEngine::v8Export() const {
    const V8Mem& M = v8m_;
    std::string o = "v8 1\n";
    char b[420];
    std::snprintf(b, sizeof b, "v8m %d %d %d %d %d %d %d %d %d %d\n", M.seen, M.gen, M.haveGood ? 1 : 0, M.sinceGood, M.verdict, M.verdictPending ? 1 : 0, M.promotions, M.rollbacks, M.nHist, M.hHead);
    o += b;
    std::string rs = M.reason;
    for (char& c : rs) if (c == '\n' || c == '\r') c = ' ';
    o += "v8r " + rs + "\n";
    std::snprintf(b, sizeof b, "v8g %d %.4f %.4f %.4f %.4f %.4f %.4f\n", M.good.n, (double)M.good.cs, (double)M.good.gd, (double)M.good.danger, (double)M.good.shots, (double)M.good.win, (double)M.good.perf); o += b;
    std::snprintf(b, sizeof b, "v8c %d %.4f %.4f %.4f %.4f %.4f %.4f\n", M.cand.n, (double)M.cand.cs, (double)M.cand.gd, (double)M.cand.danger, (double)M.cand.shots, (double)M.cand.win, (double)M.cand.perf); o += b;
    for (int i = 0; i < M.nHist; i++) {
        const V8MatchRec& r = M.hist[i];
        std::snprintf(b, sizeof b, "v8h %d %d %d %d %d %d %d %d %d %.1f %.3f %.3f %.3f %.3f %.3f %.3f %.3f", r.gen, r.split, r.valid, r.gf, r.ga, r.shotsFor, r.shotsAgainst, r.dangerAgainst, r.turnovers,
                      (double)r.liveSec, (double)r.trackConf, (double)r.safeFrac, (double)r.poss, (double)r.conf, (double)r.suff, (double)r.held, (double)r.perf);
        o += b;
        for (int p = 0; p < V8_PAT; p++) { std::snprintf(b, sizeof b, " %.3f %.3f", (double)r.patN[p], (double)r.patOk[p]); o += b; }
        o += "\n";
    }
    for (int i = 0; i < M.nProf; i++) {
        const V8OppProfile& P = M.prof[i];
        std::snprintf(b, sizeof b, "v8p %u %d %.3f %.3f %.3f %d %.3f %.3f %.3f %.3f", (unsigned)P.key, P.n, (double)P.lineDepth, (double)P.lineSd, (double)P.throughDelay, P.throughN, (double)P.conf,
                      (double)P.route[0], (double)P.route[1], (double)P.route[2]);
        o += b;
        for (int p = 0; p < V8_PAT; p++) { std::snprintf(b, sizeof b, " %.4f %.4f %.4f %.3f", (double)P.rate[p], (double)P.rate2[p], (double)P.succ[p], (double)P.wsum[p]); o += b; }
        for (int p = 0; p < V8_PAT; p++) for (int q = 0; q < V8_RESP; q++) { std::snprintf(b, sizeof b, " %.3f %.2f", (double)P.resp[p][q], (double)P.respN[p][q]); o += b; }
        o += "\n";
    }
    return o;
}

bool PitchEngine::v8Import(const std::string& str) {
    V8Mem T;
    bool any = false, bad = false;
    size_t pos = 0;
    int nHistDecl = 0;
    while (pos < str.size() && !bad) {
        size_t e = str.find('\n', pos);
        if (e == std::string::npos) e = str.size();
        const std::string ln = str.substr(pos, e - pos);
        pos = e + 1;
        const char* c = ln.c_str();
        if (ln.compare(0, 4, "v8m ") == 0) {
            int hg, vp;
            if (std::sscanf(c, "v8m %d %d %d %d %d %d %d %d %d %d", &T.seen, &T.gen, &hg, &T.sinceGood, &T.verdict, &vp, &T.promotions, &T.rollbacks, &nHistDecl, &T.hHead) == 10) {
                T.haveGood = hg != 0; T.verdictPending = vp != 0; any = true;
                T.verdict = std::min(std::max(T.verdict, 0), 3);
            } else bad = true;
        } else if (ln.compare(0, 4, "v8r ") == 0) T.reason = ln.substr(4);
        else if (ln.compare(0, 4, "v8g ") == 0 || ln.compare(0, 4, "v8c ") == 0) {
            V8Agg& A = ln[2] == 'g' ? T.good : T.cand;
            if (std::sscanf(c + 4, "%d %f %f %f %f %f %f", &A.n, &A.cs, &A.gd, &A.danger, &A.shots, &A.win, &A.perf) != 7) bad = true;
        } else if (ln.compare(0, 4, "v8h ") == 0 && T.nHist < V8_HIST) {
            V8MatchRec& r = T.hist[T.nHist];
            int off = 0;
            if (std::sscanf(c, "v8h %d %d %d %d %d %d %d %d %d %f %f %f %f %f %f %f %f%n", &r.gen, &r.split, &r.valid, &r.gf, &r.ga, &r.shotsFor, &r.shotsAgainst, &r.dangerAgainst, &r.turnovers,
                            &r.liveSec, &r.trackConf, &r.safeFrac, &r.poss, &r.conf, &r.suff, &r.held, &r.perf, &off) < 17) { bad = true; continue; }
            const char* q = c + off;
            for (int p = 0; p < V8_PAT; p++) {
                char* en = nullptr;
                const float a = std::strtof(q, &en); if (en == q) { bad = true; break; } q = en;
                const float bq = std::strtof(q, &en); if (en == q) { bad = true; break; } q = en;
                r.patN[p] = std::isfinite(a) ? a : 0.f; r.patOk[p] = std::isfinite(bq) ? bq : -1.f;
            }
            T.nHist++;
        } else if (ln.compare(0, 4, "v8p ") == 0 && T.nProf < V8_PROF) {
            V8OppProfile& P = T.prof[T.nProf];
            unsigned key; int off = 0;
            if (std::sscanf(c, "v8p %u %d %f %f %f %d %f %f %f %f%n", &key, &P.n, &P.lineDepth, &P.lineSd, &P.throughDelay, &P.throughN, &P.conf, &P.route[0], &P.route[1], &P.route[2], &off) < 10) { bad = true; continue; }
            P.key = key;
            const char* q = c + off;
            auto rd = [&](float* v) { char* en = nullptr; const float a = std::strtof(q, &en); if (en == q || !std::isfinite(a)) return false; *v = a; q = en; return true; };
            for (int p = 0; p < V8_PAT && !bad; p++) if (!(rd(&P.rate[p]) && rd(&P.rate2[p]) && rd(&P.succ[p]) && rd(&P.wsum[p]))) bad = true;
            for (int p = 0; p < V8_PAT && !bad; p++) for (int k = 0; k < V8_RESP && !bad; k++) if (!(rd(&P.resp[p][k]) && rd(&P.respN[p][k]))) bad = true;
            if (!bad) T.nProf++;
        }
    }
    if (!any || bad) return false;
    T.nHist = std::min(T.nHist, V8_HIST);
    T.hHead = T.hHead < 0 ? 0 : T.hHead % V8_HIST;
    T.lastReplay = v8m_.lastReplay;
    v8m_ = T;
    return true;
}

// rows of V8_DASH_STRIDE floats, oldest first (at most 30 matches)
int PitchEngine::dashSeries(float* out, int maxFloats) const {
    const V8Mem& M = v8m_;
    const int rows = std::min(std::min(M.nHist, 30), maxFloats / V8_DASH_STRIDE);
    for (int k = 0; k < rows; k++) {
        const int idx = ((M.hHead - rows + k) % V8_HIST + V8_HIST) % V8_HIST;
        const V8MatchRec& r = M.hist[idx];
        float* o = out + k * V8_DASH_STRIDE;
        o[0] = (float)r.valid; o[1] = (float)r.split; o[2] = r.gf > r.ga ? 1.f : (r.gf == r.ga ? 0.5f : 0.f); o[3] = (float)(r.gf - r.ga);
        o[4] = r.ga == 0 ? 1.f : 0.f; o[5] = (float)r.shotsAgainst; o[6] = (float)r.dangerAgainst; o[7] = (float)r.shotsFor;
        for (int p = 0; p < V8_PAT; p++) { o[8 + p] = r.patN[p]; o[13 + p] = r.patOk[p]; }
        o[18] = r.conf; o[19] = r.suff; o[20] = r.held; o[21] = r.perf; o[22] = r.trackConf; o[23] = (float)r.gen;
    }
    return rows;
}

// the running match: updates every call, for the live graphs
int PitchEngine::liveSeries(float* out, int maxFloats) const {
    if (maxFloats < V8_DASH_STRIDE) return 0;
    const V8State& S = v8_;
    const V8OppProfile* P = v8Profile();
    for (int i = 0; i < V8_DASH_STRIDE; i++) out[i] = 0.f;
    out[0] = S.trackConf; out[1] = S.recProb; out[2] = (float)S.st; out[3] = S.topScore; out[4] = S.safe ? 1.f : 0.f;
    out[5] = (float)S.shotsAgainst; out[6] = (float)S.dangerAgainst; out[7] = (float)S.preemptCount;
    for (int p = 0; p < V8_PAT; p++) { out[8 + p] = S.pat[p]; out[13 + p] = S.pat[p] > 1e-3f ? S.patOk[p] / S.pat[p] : -1.f; }
    out[18] = P ? P->conf : 0.f;
    float w = 0.f; if (P) for (int p = 0; p < V8_PAT; p++) w += P->wsum[p];
    out[19] = clampf(w / 30.f, 0.f, 1.f);
    out[20] = v8m_.haveGood && v8m_.cand.n > 0 ? v8m_.cand.perf - v8m_.good.perf : 0.f;
    out[21] = (float)S.regret; out[22] = (float)S.planCancels; out[23] = S.evalMatch ? 1.f : 0.f;
    return V8_DASH_STRIDE;
}
