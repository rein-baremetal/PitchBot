// Meta layer: opposition reaction model, deception (anti-pattern residuals, fakes, commit gate), 1v1 and shape defence,
// restarts, security breaker, four-mode machine, late-game tempo. Included at the end of pitch_engine.cpp after
// pitch_plus.inc (inside namespace pe). Everything is per match; the state is cleared at every new match.
//
// The radar delivers about a dozen frames a second, so nothing here pretends to run faster than that: "micro" terms are
// small per-frame residuals on the stick direction, and windows are rounded to whole frames by the callers.

void MetaState::clear() {
    nActs = actHead = 0;
    for (int i = 0; i < META_ACT; i++) acts[i] = MetaAct();
    for (int k = 0; k < 2; k++) { reMean[k] = reDev[k] = 0.f; reN[k] = 0; watch[k] = MetaWatch(); }
    for (int i = 0; i < TEAM_SIZE; i++) {
        prevPos[0][i] = prevPos[1][i] = MetricVector2D();
        prevOk[0][i] = prevOk[1][i] = false;
        prevVel[i] = MetricVector2D();
        animOk[i] = false;
        lastBig[i] = -1e9;
        dirVar[i] = 0.f;
        anim[i] = 0;
    }
    freezeUntil = -1e9;
    // classifier prior: features = [1, dist/10, closing/6, (closing/6)^2, accel/10, speed/8]
    static const float prior[5][6] = {
        {0.5f, -1.0f, 0.0f, -2.5f, -0.5f, -0.5f},     // jockey
        {-1.5f, -1.2f, 2.5f, 1.0f, 2.5f, 1.0f},       // lunge
        {0.2f, 0.8f, 0.0f, -2.0f, -0.5f, -0.8f},      // contain
        {-0.8f, -0.3f, 1.8f, 0.8f, 0.8f, 0.3f},       // step-in
        {-1.0f, 0.2f, -3.0f, 0.5f, -1.0f, 0.0f}};     // drop-off
    for (int c = 0; c < 5; c++) { prob[c] = 0.2f; for (int f = 0; f < 6; f++) W[c][f] = prior[c][f]; }
    for (int f = 0; f < 6; f++) feat[f] = 0.f;
    iT0 = -1;
    iD0 = 0.f;
    iOpp = -1;
    iPrevD = 0.f;
    iPrevT = -1e9;
    iPrevClose = 0.f;
    for (int d = 0; d < 4; d++) for (int a = 0; a < 4; a++) burst[d][a] = 0.f;
    burstTot = 0.f;
    sideCnt[0] = sideCnt[1] = 0.f;
    contain = 0.f;
    prevCarSpeed = 0.f;
    prevCarAt = lastBurstAt = -1e9;
    oppProg = 0.f;
    prevBallX = 0.f;
    prevBallOk = false;
    for (int k = 0; k < 3; k++) { oppPassOk[k] = 1.f; oppPassLost[k] = 1.f; }
    oppPassKind = -1;
    oppPassAt = -1e9;
    baitUntil = -1e9;
    baitKind = -1;
    prevStable = 0;
    for (int i = 0; i < XT_COLS * XT_ROWS; i++) field[i] = 0.5f;
    fieldAt = -1e9;
    safeUntil = -1e9;
    tZone[0] = tZone[1] = tZone[2] = -1;
    overSide = 0;
    overUntil = overAt = -1e9;
    oppCenY = 0.f;
    for (int z = 0; z < META_ZONES; z++) for (int p = 0; p < 3; p++) for (int k = 0; k < 3; k++) { spAtt[z][p][k] = 0.f; spLost[z][p][k] = 0.f; }
    curZone = -1; curPress = 0; curKind = 0;
    feintUntil = feintLastAt = -1e9;
    feintDir = MetricVector2D();
    feintKind = 0;
    oppJump = 0.f;
    jumpAt = -1e9;
    commitStart = -1.0;
    commitType = commitTarget = -1;
    commitDoneAt = -1e9;
    for (int i = 0; i < 4; i++) { smU[i] = 0.f; smType[i] = -1; smTarget[i] = -1; }
    for (int i = 0; i < META_SYS; i++) { sysSucc[i] = 0.5f; sysN[i] = 0; }
    lastSys = -1;
    lastSysAt = -1e9;
    recentInter = 0;
    lastInterAt = -1e9;
    thrRate = 0.f;
    thrN = 0;
    thrOffsetMs = 0.f;
    mode = 0;
    modeSince = -1e9;
    possEma = 0.5f;
    modeAt = -1e9;
    threatEma = 0.f;
    restartKind = 0;
    stillSince = -1e9;
    readEma = 0.f;
    recAt = secAt = baitAt = -1e9;
}

// ---------------------------------------------------------------------------------------------------------------------
// Reaction model (decision latency and execution latency of the opposition, 80th percentile) and the anti-bait scales
// ---------------------------------------------------------------------------------------------------------------------

float PitchEngine::metaReactP80() const {
    const MetaState& M = meta_;
    if (M.reN[0] < 4) return cfg_.defReact;
    const float dec = M.reMean[0] + 1.05f * M.reDev[0] * 1.25f;          // mean + 0.84 sigma, sigma ~ 1.25 * mean abs deviation
    const float exe = M.reN[1] >= 4 ? M.reMean[1] + 1.05f * M.reDev[1] * 1.25f : 0.1f;
    // a radar cannot tell a reaction to our pass from ordinary running, so the measurement may only move the plan a little:
    // never riskier than 10 % under the configured reaction, at most 1.8 times it when the opposition is clearly slow
    return clampf(dec + 0.5f * exe, 0.9f * cfg_.defReact, 1.8f * cfg_.defReact);
}

// The configured defender reaction is a credit: extra running time the defender is allowed on top of the ball's flight
// (he starts moving as the pass is struck). A quicker opposition than assumed earns a larger credit, a slower one a smaller
// credit. Code that needs the arrival delay itself (races, lane cuts) reads metaReactP80 directly.
float PitchEngine::defReactEff() const {
    if (!cfg_.meta || !cfg_.metaReact) return cfg_.defReact;
    const float credit = cfg_.defReact + 0.8f * (cfg_.defReact - metaReactP80());
    return clampf(credit, 0.6f * cfg_.defReact, 1.4f * cfg_.defReact);
}

float PitchEngine::metaSysScale(int sys) const {
    if (!cfg_.meta || !cfg_.metaAntiBait || sys < 0 || sys >= META_SYS) return 1.f;
    const MetaState& M = meta_;
    if (M.sysN[sys] < 4) return 1.f;
    return clampf(0.25f + 1.5f * M.sysSucc[sys], 0.2f, 1.3f);
}

int PitchEngine::metaModeNow() const { return cfg_.meta && cfg_.metaFsm ? meta_.mode : 0; }

static inline float metaModeScale(int mode, int what) {   // what: 0 noise, 1 feint, 2 micro
    static const float t[4][3] = {{1.0f, 0.8f, 1.0f}, {0.6f, 0.2f, 0.7f}, {1.5f, 1.6f, 1.4f}, {1.0f, 0.6f, 1.0f}};
    return t[mode < 0 ? 0 : (mode > 3 ? 3 : mode)][what];
}

int PitchEngine::metaZone(const MetricVector2D& p) const {
    const float s = (float)state_.attackSign;
    const int col = (int)clampf((s * p.x + PITCH_HALF_X) / (2.f * PITCH_HALF_X) * 6.f, 0.f, 5.f);
    return col * 2 + (p.y > 0.f ? 1 : 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::metaIntentUpdate(double now) {
    MetaState& M = meta_;
    const int ci = state_.carrier;
    if (state_.status != ST_TEAM || ci < 0 || ci >= TEAM_SIZE) { M.iT0 = -1; M.iPrevT = -1e9; return; }
    const PlayerState& car = state_.team[ci];
    int bi = -1;
    float bd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, car.pos);
        if (d < bd) { bd = d; bi = i; }
    }
    if (bi < 0 || bd > 10.f) { M.iT0 = -1; M.iPrevT = -1e9; return; }
    const PlayerState& o = state_.opp[bi];
    const float dtp = (float)((now - M.iPrevT) / 1000.0);
    float close = M.iPrevClose;
    float acc = 0.f;
    if (bi == M.iOpp && dtp > 0.01f && dtp < 0.4f) {
        const float c = (M.iPrevD - bd) / dtp;
        close = 0.5f * M.iPrevClose + 0.5f * c;
        acc = (close - M.iPrevClose) / dtp;
    } else {
        close = 0.f;
    }
    M.iPrevD = bd;
    M.iPrevT = now;
    M.iPrevClose = close;
    const float c6 = clampf(close / 6.f, -1.f, 1.f);
    M.feat[0] = 1.f;
    M.feat[1] = clampf(bd / 10.f, 0.f, 1.5f);
    M.feat[2] = c6;
    M.feat[3] = c6 * c6;
    M.feat[4] = clampf(acc / 10.f, -1.f, 1.f);
    M.feat[5] = clampf(o.vel.length() / 8.f, 0.f, 1.2f);
    float z[5], mx = -1e9f;
    for (int c = 0; c < 5; c++) {
        z[c] = 0.f;
        for (int f = 0; f < 6; f++) z[c] += M.W[c][f] * M.feat[f];
        mx = std::max(mx, z[c]);
    }
    float sum = 0.f;
    for (int c = 0; c < 5; c++) { z[c] = std::exp(z[c] - mx); sum += z[c]; }
    for (int c = 0; c < 5; c++) M.prob[c] = z[c] / sum;
    // self-supervised sample: what the opponent actually did over the next 0.6 s labels the features seen at the start
    if (bi != M.iOpp) { M.iOpp = bi; M.iT0 = -1; }
    if (M.iT0 < 0) {
        M.iT0 = now;
        M.iD0 = bd;
        for (int f = 0; f < 6; f++) M.featStart[f] = M.feat[f];
        for (int c = 0; c < 5; c++) M.probStart[c] = M.prob[c];
    } else if (now - M.iT0 >= 600.0) {
        const float dd = bd - M.iD0;
        const float avgClose = -dd / 0.6f;
        int label;
        if (dd <= -3.5f && avgClose >= 4.5f) label = 1;
        else if (dd <= -1.5f) label = 3;
        else if (dd >= 2.0f) label = 4;
        else if (bd < 3.5f) label = 0;
        else label = 2;
        for (int c = 0; c < 5; c++) {
            const float y = c == label ? 1.f : 0.f;
            for (int f = 0; f < 6; f++) M.W[c][f] = clampf(M.W[c][f] + 0.05f * (y - M.probStart[c]) * M.featStart[f], -4.f, 4.f);
        }
        M.iT0 = -1;
    }
}

void PitchEngine::metaFrame(double dtSec) {
    MetaState& M = meta_;
    const double now = state_.timeMs;
    const float s = (float)state_.attackSign;
    const float dt = (float)std::max(dtSec, 1e-3);

    // radar jump: one player displaced further than a human can run in one frame (identity swap or bad read)
    {
        int nJump = 0;
        const float lim = cfg_.metaJumpM * std::max(1.f, dt / 0.08f);
        for (int side = 0; side < 2; side++)
            for (int i = 0; i < TEAM_SIZE; i++) {
                const PlayerState& p = side ? state_.opp[i] : state_.team[i];
                if (!p.active || !p.visible) { M.prevOk[side][i] = false; continue; }
                if (M.prevOk[side][i] && distance(p.pos, M.prevPos[side][i]) > lim) nJump++;
                M.prevPos[side][i] = p.pos;
                M.prevOk[side][i] = true;
            }
        if ((nJump >= 1 || trackJumps_ >= 1) && cfg_.metaTrackFreeze) M.freezeUntil = now + 450.0;
    }

    // animation proxy: a player who just changed speed or direction sharply is locked for about 340 ms
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) { M.animOk[i] = false; M.anim[i] = 0; continue; }
        const float sp = o.vel.length();
        if (M.animOk[i]) {
            const MetricVector2D dv = o.vel - M.prevVel[i];
            const float psp = M.prevVel[i].length();
            float ang = 0.f;
            if (sp > 1.5f && psp > 1.5f) ang = std::acos(clampf(o.vel.dot(M.prevVel[i]) / (sp * psp), -1.f, 1.f));
            if (dv.length() > 1.8f || ang > 0.5f) M.lastBig[i] = now;
            M.dirVar[i] += 0.2f * (ang - M.dirVar[i]);
        }
        M.prevVel[i] = o.vel;
        M.animOk[i] = true;
        M.anim[i] = (now - M.lastBig[i] < 340.0 && sp < 5.0f) ? 1 : ((sp > 5.5f && M.dirVar[i] < 0.12f) ? 2 : 0);
    }

    // reaction watches started when a pass left the carrier: first sharp velocity change = decision latency
    for (int k = 0; k < 2; k++) {
        MetaWatch& w = M.watch[k];
        if (!w.active) continue;
        if (now - w.t0 > 1100.0 || w.opp < 0) { w.active = false; continue; }
        const PlayerState& o = state_.opp[w.opp];
        if (!o.active || !o.visible) { w.active = false; continue; }
        const float sp = o.vel.length();
        const float dvl = (o.vel - w.v0).length();
        const float toward = (o.vel - w.v0).dot(w.lane);
        if (w.tDec < 0.0) {
            if (dvl > 1.8f && toward > 1.2f && now - w.t0 > 60.0) {
                const float x = (float)((now - w.t0) / 1000.0) - clampf(cfg_.kalLatencyMs * 0.001f, 0.f, 0.4f);
                if (x > 0.04f && x < 0.9f) {
                    M.reMean[0] += (M.reN[0] ? 0.12f : 1.f) * (x - M.reMean[0]);
                    M.reDev[0] += (M.reN[0] ? 0.12f : 1.f) * (std::fabs(x - M.reMean[0]) - M.reDev[0]);
                    M.reN[0]++;
                }
                w.tDec = now;
            }
        } else if (sp > w.sp0 + 2.0f || now - w.tDec > 700.0) {
            if (sp > w.sp0 + 2.0f) {
                const float x = (float)((now - w.tDec) / 1000.0);
                if (x > 0.02f && x < 0.8f) {
                    M.reMean[1] += (M.reN[1] ? 0.12f : 1.f) * (x - M.reMean[1]);
                    M.reDev[1] += (M.reN[1] ? 0.12f : 1.f) * (std::fabs(x - M.reMean[1]) - M.reDev[1]);
                    M.reN[1]++;
                }
            }
            w.active = false;
        }
    }

    // opposition progression speed and carrier bursts (distance x angle histogram) and engagement side
    if (ballUsable()) {
        if (M.prevBallOk && state_.status == ST_OPP) {
            const float prog = clampf(-s * (state_.ball.x - M.prevBallX) / dt, -3.f, 12.f);
            M.oppProg += 0.05f * (prog - M.oppProg);
        }
        M.prevBallX = state_.ball.x;
        M.prevBallOk = true;
    }
    if (state_.status == ST_OPP) {
        int ci = -1;
        float cd = 1e9f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (!o.active || !o.visible) continue;
            const float d = distance(o.pos, state_.ball);
            if (d < cd) { cd = d; ci = i; }
        }
        if (ci >= 0 && cd < 2.5f) {
            const PlayerState& c = state_.opp[ci];
            const float sp = c.vel.length();
            // our nearest defender
            int di = -1;
            float dd = 1e9f;
            for (int i = 0; i < TEAM_SIZE; i++) {
                const PlayerState& q = state_.team[i];
                if (!q.active || !q.visible || isKeeperIdx(i)) continue;
                const float d = distance(q.pos, c.pos);
                if (d < dd) { dd = d; di = i; }
            }
            if (di >= 0 && dd < 6.f && now - M.prevCarAt < 400.0 && sp - M.prevCarSpeed > 2.0f && now - M.lastBurstAt > 800.0) {
                const MetricVector2D line = state_.team[di].pos - c.pos;
                const float ll = line.length();
                float ang = 0.f;
                if (ll > 1e-3f && sp > 1.f) ang = std::acos(clampf(c.vel.dot(line) / (sp * ll), -1.f, 1.f));
                const int db = dd < 2.f ? 0 : (dd < 3.f ? 1 : (dd < 4.f ? 2 : 3));
                const int ab = ang < 0.52f ? 0 : (ang < 1.05f ? 1 : (ang < 1.57f ? 2 : 3));
                M.burst[db][ab] += 1.f;
                M.burstTot += 1.f;
                M.lastBurstAt = now;
            }
            if (di >= 0 && dd < 3.2f && sp > 2.f) {
                const MetricVector2D line = state_.team[di].pos - c.pos;
                const float lateral = c.vel.x * line.y - c.vel.y * line.x;        // which side of the defender he is going
                if (std::fabs(lateral) > 1.0f && now - M.lastBurstAt > 300.0) M.sideCnt[lateral > 0.f ? 1 : 0] += 0.25f;
            }
            M.prevCarSpeed = sp;
            M.prevCarAt = now;
        }
    }

    if (cfg_.metaIntent1v1) metaIntentUpdate(now);

    // while we carry: how often the nearest opponent jumps at us early (drives the fake probability)
    if (state_.status == ST_TEAM && state_.carrier >= 0 && now - M.jumpAt > 700.0) {
        const float nd = nearestOppDistance(state_.team[state_.carrier].pos);
        if (nd > 3.f && nd < 8.f) {
            const float j = M.prob[1] + M.prob[3] > 0.55f ? 1.f : 0.f;
            M.oppJump += 0.15f * (j - M.oppJump);
            M.jumpAt = now;
        }
    }
    if (now - M.lastInterAt > 20000.0 && M.recentInter > 0) { M.recentInter--; M.lastInterAt = now; }

    // interception-probability field every 200 ms: where does one of our players beat every opponent to the ball
    if (cfg_.metaField && now - M.fieldAt >= 200.0) {
        M.fieldAt = now;
        for (int r = 0; r < XT_ROWS; r++)
            for (int c = 0; c < XT_COLS; c++) {
                const float ax = -PITCH_HALF_X + ((float)c + 0.5f) * (2.f * PITCH_HALF_X / (float)XT_COLS);
                const float y = -PITCH_HALF_Y + ((float)r + 0.5f) * (2.f * PITCH_HALF_Y / (float)XT_ROWS);
                const MetricVector2D p(s * ax, y);
                float tt = 1e3f, to = 1e3f;
                for (int i = 0; i < TEAM_SIZE; i++) {
                    const PlayerState& q = state_.team[i];
                    if (q.active && q.visible) tt = std::min(tt, plusTta(q, p, 7.0f));
                    const PlayerState& o = state_.opp[i];
                    if (o.active && o.visible) to = std::min(to, plusTta(o, p, 7.0f));
                }
                M.field[r * XT_COLS + c] = logistic((to - tt) / 0.25f);
            }
    }

    // half-space overload: counts in the attacking half, kept for 6-10 s
    if (cfg_.metaOverload && now - M.overAt >= 200.0) {
        M.overAt = now;
        int ours[2] = {0, 0}, theirs[2] = {0, 0};
        float cy = 0.f;
        int nc = 0;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& q = state_.team[i];
            if (q.active && q.visible && !isKeeperIdx(i) && s * q.pos.x > 5.f && std::fabs(q.pos.y) >= 6.f && std::fabs(q.pos.y) <= 22.f) ours[q.pos.y > 0.f ? 1 : 0]++;
            const PlayerState& o = state_.opp[i];
            if (o.active && o.visible && s * o.pos.x > 5.f) {
                if (std::fabs(o.pos.y) >= 6.f && std::fabs(o.pos.y) <= 22.f) theirs[o.pos.y > 0.f ? 1 : 0]++;
                cy += o.pos.y;
                nc++;
            }
        }
        if (nc > 0) M.oppCenY += 0.1f * (cy / (float)nc - M.oppCenY);
        for (int sd = 0; sd < 2; sd++) {
            if (ours[sd] >= 3 && ours[sd] - theirs[sd] >= 1) {
                const int side = sd ? 1 : -1;
                if (M.overSide != side || now > M.overUntil) M.overUntil = now + 8000.0;
                M.overSide = side;
                if (M.overSide == side) M.overUntil = std::max(M.overUntil, now + 1500.0);
            }
        }
        if (now > M.overUntil) M.overSide = 0;
    }

    // restart detection: only in the edge zones where a stationary ball means a restart
    if (cfg_.metaSetPiece && ballUsable()) {
        if (state_.ballVel.length() < 0.45f) {
            if (M.stillSince < 0.0) M.stillSince = now;
        } else {
            M.stillSince = -1e9;
            M.restartKind = 0;
        }
        if (M.stillSince > 0.0 && now - M.stillSince > 500.0) {
            const MetricVector2D b = state_.ball;
            const bool corner = std::fabs(b.x) > PITCH_HALF_X - 3.5f && std::fabs(b.y) > PITCH_HALF_Y - 3.5f;
            const bool goalKick = std::fabs(b.x) > PITCH_HALF_X - 9.f && std::fabs(b.y) < 12.f && !corner;
            const bool throwIn = std::fabs(b.y) > PITCH_HALF_Y - 1.2f && !corner;
            int kind = 0;
            if (state_.status == ST_TEAM) kind = corner ? 1 : (goalKick ? 2 : (throwIn ? 3 : 0));
            else if (state_.status == ST_OPP) {
                const float own = -s;
                if ((corner || goalKick || throwIn) && own * b.x > 10.f) kind = 5;
            }
            M.restartKind = kind;
        }
    } else {
        M.restartKind = 0;
    }

    // possession and threat trend, four-mode machine every 500 ms
    M.possEma += 0.01f * (possShare_ - M.possEma);
    {
        const float inThird = (state_.status == ST_OPP && ballUsable() && -s * state_.ball.x > PITCH_HALF_X - 24.f) ? 1.f : 0.f;
        M.threatEma += 0.004f * (inThird - M.threatEma);
    }
    if (cfg_.metaFsm && now - M.modeAt >= 500.0) {
        M.modeAt = now;
        const float ph = plus_.phase >= 0.f ? clampf(plus_.phase, 0.f, 1.f)
                                            : clampf((float)(plus_.liveMs / 1000.0 / std::max(cfg_.plusMatchSec, 60.f)), 0.f, 1.f);
        const int lead = plus_.goalsFor - plus_.goalsAgainst;
        int want = 0;
        if ((lead <= -1 && ph >= 0.75f) || (lead <= -2 && ph >= 0.4f)) want = 3;
        else if ((lead >= 1 && ph >= 0.5f) || (lead >= 2 && ph >= 0.3f)) want = 1;
        else if ((ph < 0.5f && lead <= 0) || (lead == 0 && ph >= 0.7f && M.possEma < 0.42f) || (lead <= 0 && M.threatEma > 0.35f)) want = 2;
        const bool strong = want == 3 || want == 1;
        if (want != M.mode && (strong || now - M.modeSince > 8000.0)) { M.mode = want; M.modeSince = now; }
    }
}

// stable possession edges from plusFrame: opposition pass bookkeeping for the bait lane
void PitchEngine::metaEdge(int prev, int cur) {
    MetaState& M = meta_;
    const double now = state_.timeMs;
    const float s = (float)state_.attackSign;
    if (prev == ST_OPP && cur == ST_LOOSE && ballUsable()) {
        const float sp = state_.ballVel.length();
        if (sp > 4.f) {
            const float fwd = -s * state_.ballVel.x / sp;
            M.oppPassKind = fwd > 0.5f ? 0 : (fwd > -0.3f ? 1 : 2);
            M.oppPassAt = now;
        }
    } else if (prev == ST_LOOSE && M.oppPassKind >= 0 && now - M.oppPassAt < 3500.0) {
        if (cur == ST_OPP) {
            M.oppPassOk[M.oppPassKind] += 1.f;
            if (M.baitUntil > now - 1500.0 && M.baitKind == M.oppPassKind) {
                M.sysSucc[3] += 0.2f * (0.f - M.sysSucc[3]);
                M.sysN[3]++;
            }
        } else if (cur == ST_TEAM) {
            M.oppPassLost[M.oppPassKind] += 1.f;
            if (M.baitUntil > now - 1500.0 && M.baitKind == M.oppPassKind) {
                M.sysSucc[3] += 0.2f * (1.f - M.sysSucc[3]);
                M.sysN[3]++;
            }
        }
        M.oppPassKind = -1;
    }
    M.prevStable = cur;
}

void PitchEngine::metaTurnover() {
    MetaState& M = meta_;
    const double now = state_.timeMs;
    if (M.lastSys >= 0 && now - M.lastSysAt < 3000.0) {
        M.sysSucc[M.lastSys] += 0.2f * (0.f - M.sysSucc[M.lastSys]);
        M.sysN[M.lastSys]++;
        M.lastSys = -1;
    }
    if (!cfg_.metaSecurity) return;
    const int z = ballUsable() ? metaZone(state_.ball) : -1;
    M.tZone[0] = M.tZone[1];
    M.tZone[1] = M.tZone[2];
    M.tZone[2] = z;
    if (z >= 0 && M.tZone[0] == z && M.tZone[1] == z) M.safeUntil = now + 20000.0;
}

void PitchEngine::metaPassResult(bool kept) {
    MetaState& M = meta_;
    const double now = state_.timeMs;
    if (M.curZone >= 0) {
        M.spAtt[M.curZone][M.curPress][M.curKind] += 1.f;
        if (!kept) M.spLost[M.curZone][M.curPress][M.curKind] += 1.f;
        if (M.curKind == 1) {
            M.thrRate += 0.15f * ((kept ? 0.f : 1.f) - M.thrRate);
            M.thrN++;
        }
        M.curZone = -1;
    }
    if (!kept) {
        M.recentInter++;
        M.lastInterAt = now;
    }
    if (M.lastSys >= 0 && now - M.lastSysAt < 3500.0) {
        M.sysSucc[M.lastSys] += 0.2f * ((kept ? 1.f : 0.f) - M.sysSucc[M.lastSys]);
        M.sysN[M.lastSys]++;
        M.lastSys = -1;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Candidate preparation: no-look through-ball timing
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::metaPrepare(const PlayerState& car) {
    if (!cfg_.meta || !cfg_.metaThroughTiming) return;
    MetaState& M = meta_;
    static const float offs[7] = {-140.f, -100.f, -60.f, 0.f, 60.f, 100.f, 140.f};
    for (int i = 0; i < nCand_; i++) {
        PassCandidate& c = cand_[i];
        if (c.type != ACT_THROUGH) continue;
        const int ti = c.targetId - 1;
        if (ti < 0 || ti >= TEAM_SIZE || !state_.team[ti].active) continue;
        uint32_t sd = 0xB5297A4Du ^ (uint32_t)(frameNo_ * 2654435761u) ^ (uint32_t)(i * 40503u);
        if (!sd) sd = 1u;
        float w[7], tot = 0.f;
        const float vary = 1.f + std::max(0.f, M.thrRate - 0.3f) * 3.f;       // intercepted a lot: vary the release more
        for (int k = 0; k < 7; k++) {
            w[k] = std::exp(-std::fabs(offs[k]) / 90.f) * (offs[k] == 0.f ? 1.f : vary);
            tot += w[k];
        }
        float u = plusRand(sd) * tot, off = 0.f;
        for (int k = 0; k < 7; k++) { u -= w[k]; if (u <= 0.f) { off = offs[k]; break; } }
        if (off == 0.f) continue;
        const PlayerState& r = state_.team[ti];
        MetricVector2D t = c.target + r.vel * (off * 0.001f);
        t.x = clampf(t.x, -PITCH_HALF_X, PITCH_HALF_X);
        t.y = clampf(t.y, -PITCH_HALF_Y, PITCH_HALF_Y);
        const float len = distance(car.pos, t);
        if (len < cfg_.minPassLen || len > cfg_.safePassLen || offsideAmount(t) > 0.5f) continue;
        float clr = 0.f;
        const float xp = passProbability(car.pos, t, clr, true, false) * receiverReliability(r);
        if (xp < c.xP - 0.04f) continue;
        c.target = t;
        c.length = len;
        c.clearance = clr;
        c.xP = xp;
        c.xPRaw = xp;
        c.offside = offsideAmount(t);
        c.deltaXT = std::max(computeExpectedThreat(car, t), cfg_.minDeltaXT);
        c.pressure = lanePressure(t);
        M.thrOffsetMs = off;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Utility terms
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::metaApply(const PlayerState& car) {
    if (!cfg_.meta || nCand_ <= 0) return;
    MetaState& M = meta_;
    const double now = state_.timeMs;
    const float s = (float)state_.attackSign;
    const int mode = metaModeNow();
    const int ci = state_.carrier;
    const float ph = plus_.phase >= 0.f ? clampf(plus_.phase, 0.f, 1.f)
                                        : clampf((float)(plus_.liveMs / 1000.0 / std::max(cfg_.plusMatchSec, 60.f)), 0.f, 1.f);
    const int lead = plus_.goalsFor - plus_.goalsAgainst;
    const bool late = cfg_.metaLate && ph >= cfg_.metaLateStart;
    const bool safeMode = cfg_.metaSecurity && now < M.safeUntil;
    const bool frozen = cfg_.metaTrackFreeze && now < M.freezeUntil;
    const float here = (plus_.epvInit && ci >= 0)
        ? epvValue(epvRoleBin(ci), s * car.pos.x, car.pos.y, plusDensBin(oppDensityAt(car.pos, 6.f)), 0) : grid_.valueAt(s * car.pos.x, car.pos.y);
    const float nearOpp = nearestOppDistance(car.pos);
    const float lp = lanePressure(car.pos);
    if (cfg_.metaSecurity && plus_.liveMs > 60000.0 && M.possEma < 0.36f && now >= M.safeUntil) M.safeUntil = now + 15000.0;
    if (safeMode && M.possEma > 0.55f && now > M.safeUntil - 12000.0) M.safeUntil = now;      // recovered

    // nearest opponent intent classes
    const float pLunge = M.prob[1] + 0.6f * M.prob[3];
    const float pDrop = M.prob[4];
    const bool intentLive = cfg_.metaIntent1v1 && nearOpp < 8.f && M.iOpp >= 0;

    // third-man setup passes: pass to B, B finds a runner C
    const bool third = cfg_.metaThirdMan;
    const float over = (float)M.overSide;
    const bool overload = cfg_.metaOverload && over != 0.f && now < M.overUntil;
    const bool switchNow = overload && std::fabs(M.oppCenY) > 3.5f && M.oppCenY * over > 0.f;      // they have shifted toward the overload

    for (int i = 0; i < nCand_; i++) {
        PassCandidate& c = cand_[i];
        const bool pass = plusIsPass(c.type);
        float b = 0.f;

        if (pass) {
            // animation lock: locked defenders near the lane cannot change direction for a moment
            if (cfg_.metaLock) {
                int nl = 0;
                for (int k = 0; k < TEAM_SIZE; k++) {
                    const PlayerState& o = state_.opp[k];
                    if (!o.active || M.anim[k] != 1) continue;
                    if (segmentDistance(car.pos, c.target, o.pos) < 4.f) nl++;
                }
                b += cfg_.wLock * (float)std::min(nl, 2) * c.xP;
            }
            // interception-probability field: passes into zones where we win the second ball
            if (cfg_.metaField && now - M.fieldAt < 2000.0) {
                const int col = xTGrid::colOf(s * c.target.x), row = xTGrid::rowOf(c.target.y);
                const float pw = M.field[row * XT_COLS + col];
                b += cfg_.wField * (pw - 0.5f) * c.xP * (c.type == ACT_PASS ? 0.6f : 1.0f);
            }
            // 3-second territory: where the receiver is going to be
            if (cfg_.metaMultiScale && c.targetId >= 1 && c.targetId <= TEAM_SIZE && plus_.epvInit) {
                const PlayerState& r = state_.team[c.targetId - 1];
                MetricVector2D p3 = c.target + r.vel * 1.5f;
                p3.x = clampf(p3.x, -PITCH_HALF_X, PITCH_HALF_X);
                p3.y = clampf(p3.y, -PITCH_HALF_Y, PITCH_HALF_Y);
                const float d3 = epvValue(epvRoleBin(c.targetId - 1), s * p3.x, p3.y, plusDensBin(oppDensityAt(p3, 6.f)), 1) - here;
                b += cfg_.wMS * c.xP * (d3 - c.deltaXT) * 0.5f;
            }
            // third man
            if (third && c.targetId >= 1 && c.xP >= 0.55f && c.deltaXT <= 0.05f && plus_.epvInit) {
                const int bi = c.targetId - 1;
                float best = 0.f;
                for (int k = 0; k < TEAM_SIZE; k++) {
                    if (k == bi || k == ci || isKeeperIdx(k)) continue;
                    const PlayerState& q = state_.team[k];
                    if (!q.active || s * q.vel.x < 2.f || s * (q.future.x - c.target.x) < 5.f) continue;
                    if (distance(c.target, q.future) > cfg_.maxPassLen) continue;
                    if (opennessOf(q.future) < 0.4f) continue;
                    float clr = 0.f;
                    const float pbc = passProbability(c.target, q.future, clr, false, false) * receiverReliability(q);
                    const float gain = epvValue(epvRoleBin(k), s * q.future.x, q.future.y, plusDensBin(oppDensityAt(q.future, 6.f)), 1) -
                                       epvValue(epvRoleBin(bi), s * c.target.x, c.target.y, 1, 0);
                    best = std::max(best, pbc * gain);
                }
                b += cfg_.wThird * c.xP * best * 3.f;
            }
            // overload and the sudden switch
            if (overload) {
                if (std::fabs(c.target.y) >= 6.f && std::fabs(c.target.y) <= 22.f && c.target.y * over > 0.f && s * c.target.x > 0.f) b += cfg_.wOver * c.xP;
                if (switchNow && c.target.y * over < 0.f && std::fabs(c.target.y - car.pos.y) >= 18.f && c.xP >= 0.5f) b += 0.35f * c.xP;
            }
            // 1v1 intent: when a lunge is coming a quick release is worth more
            if (intentLive && pLunge > 0.5f && c.xP >= 0.7f) b += 0.12f * (pLunge - 0.4f);
            // possession security: short, supported, away from pressure; no through balls
            if (safeMode) {
                if (c.length > 12.f) b -= 0.4f * (c.length - 12.f) / 6.f;
                if (c.type == ACT_THROUGH) b -= 2.0f;
                if (s * (c.target.x - car.pos.x) < 0.f && c.xP >= 0.8f) b += 0.25f;
                const float dp = lp - lanePressure(c.target);
                if (dp > 0.f) b += 0.2f * dp;
            }
            // radar jump: nearest safe team-mate or nothing risky
            if (frozen) {
                if (c.type == ACT_THROUGH || c.type == ACT_LOFTED) b -= 1.5f;
                if (c.xP >= 0.8f && c.length <= 14.f) b += 0.4f;
            }
            // four-mode machine: the chaotic mode accepts more risk
            if (mode == 2 && c.xP < 0.7f && c.deltaXT > 0.04f) b += 0.10f;
            // late game
            if (late) {
                if (lead >= 1) {
                    if (c.deltaXT <= 0.01f && c.xP >= 0.8f) b += 0.22f;
                    if (c.xP < 0.8f) b -= 0.25f * (0.8f - c.xP);
                } else if (lead <= -1) {
                    if (c.deltaXT > 0.f) b += 0.45f * c.deltaXT * c.xP * 3.f;
                    if (c.type == ACT_LOFTED || c.type == ACT_THROUGH) b += 0.10f;
                }
            }
        } else if (c.type == ACT_CARRY) {
            if (intentLive) {
                if (pLunge > 0.5f && nearOpp < 4.f) b -= 0.15f * (pLunge - 0.3f);
                if (pDrop > 0.5f) b += 0.12f * (pDrop - 0.4f);
            }
            if (safeMode && nearOpp < 6.f) b -= 0.2f;
            if (frozen && nearOpp < 5.f) b -= 0.6f;
            if (late && lead >= 1 && nearOpp > 6.f) b += 0.10f;
        } else if (c.type == ACT_SHOOT) {
            if (frozen) b -= 1.5f;
            if (late && lead <= -1) b += 0.10f;
        }

        // restarts
        if (cfg_.metaSetPiece && M.restartKind > 0 && M.restartKind < 5) {
            const float ax = s * c.target.x;
            if (M.restartKind == 1) {                                  // our corner
                if (pass && ax > PITCH_HALF_X - 18.f && std::fabs(c.target.y) < 14.f) b += cfg_.wSet * (0.6f + 0.4f * c.xP);
                if (c.type == ACT_CARRY) b -= 0.6f;
            } else if (M.restartKind == 2) {                           // our goal kick
                if (pass && c.type == ACT_LOFTED && ax > -10.f && c.xP >= 0.55f) b += 0.5f * cfg_.wSet;
                if (pass && c.xP >= 0.85f) b += 0.35f * cfg_.wSet;
                if (c.type == ACT_CARRY) b -= 0.3f;
            } else if (M.restartKind == 3) {                           // our throw-in
                if (pass && c.xP >= 0.75f) b += cfg_.wSet * c.xP;
                if (c.type == ACT_CARRY) b -= 0.6f;
                if (c.type == ACT_THROUGH) b -= 0.4f;
            }
        }
        c.utility += clampf(b, -0.5f, 0.5f);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Decision-time deception: anti-pattern residuals
// ---------------------------------------------------------------------------------------------------------------------

static inline float metaActSim(const MetaAct& a, const MetaAct& b) {
    const float dl = (a.len - b.len) / 3.f;
    float da = a.ang - b.ang;
    while (da > 3.14159265f) da -= 6.2831853f;
    while (da < -3.14159265f) da += 6.2831853f;
    da /= 0.35f;
    return std::exp(-(dl * dl + da * da));
}

void PitchEngine::metaDecorrelate(int pick, const PlayerState& car) {
    if (!cfg_.meta || !cfg_.metaNoise || pick < 0 || pick >= nCand_ || human_) return;
    MetaState& M = meta_;
    PassCandidate& c = cand_[pick];
    if (!plusIsPass(c.type) || M.nActs < 6) return;
    const float scale = metaSysScale(0) * metaModeScale(metaModeNow(), 0);
    if (scale < 0.25f) return;
    MetaAct cur;
    const MetricVector2D d = c.target - car.pos;
    cur.len = d.length();
    cur.ang = std::atan2(d.y, d.x);
    float worst = 0.f;
    for (int i = 0; i < M.nActs; i++) worst = std::max(worst, metaActSim(cur, M.acts[i]));
    if (worst < 0.82f) return;
    static const float R[8][2] = {{1.2f, 0.f}, {-1.2f, 0.f}, {0.f, 1.4f}, {0.f, -1.4f}, {0.9f, 0.9f}, {0.9f, -0.9f}, {-0.9f, 0.9f}, {-0.9f, -0.9f}};
    uint32_t sd = 0x2545F491u ^ (uint32_t)(frameNo_ * 2654435761u);
    if (!sd) sd = 1u;
    const int start = (int)(plusRand(sd) * 8.f) % 8;
    const float ul = d.length() > 1e-3f ? 1.f / d.length() : 0.f;
    const MetricVector2D ux(d.x * ul, d.y * ul), uy(-ux.y, ux.x);        // residual table expressed in the pass frame
    for (int k = 0; k < 8; k++) {
        const float* r = R[(start + k) % 8];
        MetricVector2D t = c.target + ux * r[0] + uy * r[1];
        t.x = clampf(t.x, -PITCH_HALF_X, PITCH_HALF_X);
        t.y = clampf(t.y, -PITCH_HALF_Y, PITCH_HALF_Y);
        const float len = distance(car.pos, t);
        if (len < cfg_.minPassLen || len > cfg_.safePassLen || offsideAmount(t) > 0.5f) continue;
        float clr = 0.f;
        const bool th = c.type == ACT_THROUGH, lo = c.type == ACT_LOFTED;
        const int ti = c.targetId - 1;
        if (ti < 0 || ti >= TEAM_SIZE) return;
        const float xp = passProbability(car.pos, t, clr, th, lo) * receiverReliability(state_.team[ti]);
        if (xp < c.xP - 0.04f) continue;
        MetaAct n;
        const MetricVector2D dn = t - car.pos;
        n.len = dn.length();
        n.ang = std::atan2(dn.y, dn.x);
        float w2 = 0.f;
        for (int i = 0; i < M.nActs; i++) w2 = std::max(w2, metaActSim(n, M.acts[i]));
        if (w2 >= worst - 0.05f) continue;
        c.target = t;
        c.length = len;
        c.clearance = clr;
        c.xP = xp;
        c.xPRaw = xp;
        c.offside = offsideAmount(t);
        M.lastSys = 0;
        M.lastSysAt = state_.timeMs;
        return;
    }
}

void PitchEngine::metaNoteAction(const PassCandidate& c, const GestureOutput& g, const PlayerState& car) {
    if (!cfg_.meta || !g.valid || !plusIsPass(c.type)) return;
    MetaState& M = meta_;
    MetaAct a;
    const MetricVector2D d = c.target - car.pos;
    a.len = d.length();
    a.ang = std::atan2(d.y, d.x);
    a.hold = (float)g.durationMs;
    a.sx = g.r * std::cos(g.theta);
    a.sy = g.r * std::sin(g.theta);
    float sim = 0.f;
    for (int i = 0; i < M.nActs; i++) sim = std::max(sim, metaActSim(a, M.acts[i]));
    M.readEma += 0.15f * (sim - M.readEma);
    M.acts[M.actHead] = a;
    M.actHead = (M.actHead + 1) % META_ACT;
    if (M.nActs < META_ACT) M.nActs++;
    // zone bookkeeping for the odd-play table
    M.curZone = metaZone(car.pos);
    const float lp = lanePressure(car.pos);
    M.curPress = lp < 0.3f ? 0 : (lp < 0.6f ? 1 : 2);
    M.curKind = c.type == ACT_THROUGH ? 1 : (c.type == ACT_LOFTED ? 2 : 0);
    // start a reaction watch on the two opponents nearest to the lane
    if (cfg_.metaReact) {
        int idx[2] = {-1, -1};
        float dd[2] = {1e9f, 1e9f};
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (!o.active || !o.visible) continue;
            const float dl = segmentDistance(car.pos, c.target, o.pos);
            if (dl > 7.f) continue;
            if (dl < dd[0]) { dd[1] = dd[0]; idx[1] = idx[0]; dd[0] = dl; idx[0] = i; }
            else if (dl < dd[1]) { dd[1] = dl; idx[1] = i; }
        }
        for (int k = 0; k < 2; k++) {
            M.watch[k] = MetaWatch();
            if (idx[k] < 0) continue;
            M.watch[k].active = true;
            M.watch[k].opp = idx[k];
            M.watch[k].t0 = state_.timeMs;
            M.watch[k].v0 = state_.opp[idx[k]].vel;
            M.watch[k].sp0 = state_.opp[idx[k]].vel.length();
            {                                                      // direction from the opponent to the nearest point of the lane
                const MetricVector2D ab = c.target - car.pos;
                const float l2 = std::max(ab.x * ab.x + ab.y * ab.y, 1e-6f);
                const float t = clampf((state_.opp[idx[k]].pos - car.pos).dot(ab) / l2, 0.f, 1.f);
                MetricVector2D d = (car.pos + ab * t) - state_.opp[idx[k]].pos;
                const float dl = d.length();
                M.watch[k].lane = dl > 1e-3f ? d * (1.f / dl) : MetricVector2D();
            }
        }
    }
    if (M.thrOffsetMs != 0.f && c.type == ACT_THROUGH) { M.lastSys = 5; M.lastSysAt = state_.timeMs; }
    M.thrOffsetMs = 0.f;
}

// ---------------------------------------------------------------------------------------------------------------------
// Feints, odd plays and the commit gate. Returns true when *out holds the gesture to play now.
// ---------------------------------------------------------------------------------------------------------------------

bool PitchEngine::metaPreCommit(int* pick, const PlayerState& car, bool forced, GestureOutput* out) {
    if (!cfg_.meta || human_ || !pick || *pick < 0 || *pick >= nCand_) return false;
    MetaState& M = meta_;
    const double now = state_.timeMs;
    const float s = (float)state_.attackSign;

    // a feint in progress keeps the stick on the decoy until it ends, then the real decision is made fresh
    if (now < M.feintUntil) {
        GestureOutput tg = makeGesture(ACT_CARRY, car.pos, M.feintDir, 0.f, false);
        if (tg.valid) {
            tg.durationMs = (int)std::max(60.0, M.feintUntil - now);
            nextDecisionAt_ = M.feintUntil;
            *out = tg;
            return true;
        }
    }
    const PassCandidate& pc = cand_[*pick];
    if (pc.type == ACT_CARRY || forced || state_.crowded) { M.commitStart = -1.0; return false; }
    if (now < M.freezeUntil) { M.commitStart = -1.0; return false; }
    const float nearOpp = nearestOppDistance(car.pos);
    const bool ownThird = s * car.pos.x < -PITCH_HALF_X + 24.f;
    const int mode = metaModeNow();

    // odd plays: a 3rd or 4th ranked legal pass in a zone where the opposition seldom intercepts
    if (cfg_.metaStupid && pc.type != ACT_SHOOT && nearOpp > 3.5f && !ownThird && plus_.liveMs > 90000.0) {
        const float p = 0.03f * metaSysScale(4) * metaModeScale(mode, 0);
        uint32_t sd = 0x68E31DA4u ^ (uint32_t)(frameNo_ * 2654435761u);
        if (!sd) sd = 1u;
        if (plusRand(sd) < p) {
            int order[MAX_CAND], n = 0;
            for (int i = 0; i < nCand_; i++) if (plusIsPass(cand_[i].type) && cand_[i].xP >= 0.65f && cand_[i].offside < 0.5f) order[n++] = i;
            for (int a = 0; a < n; a++)
                for (int b = a + 1; b < n; b++)
                    if (cand_[order[b]].utility > cand_[order[a]].utility) std::swap(order[a], order[b]);
            const int z = metaZone(car.pos);
            const float lp = lanePressure(car.pos);
            const int pr = lp < 0.3f ? 0 : (lp < 0.6f ? 1 : 2);
            for (int a = 2; a < std::min(n, 4); a++) {
                const PassCandidate& q = cand_[order[a]];
                const int kind = q.type == ACT_THROUGH ? 1 : (q.type == ACT_LOFTED ? 2 : 0);
                const float rate = (M.spLost[z][pr][kind] + 0.3f) / (M.spAtt[z][pr][kind] + 1.f);
                if (rate < 0.25f) { *pick = order[a]; M.lastSys = 4; M.lastSysAt = now; break; }
            }
        }
    }
    const PassCandidate& pk = cand_[*pick];

    // feints: begin the opposite action, then snap to the real one
    if (cfg_.metaFeint && nearOpp > 4.5f && !ownThird && now - M.feintLastAt > 6000.0 && now > M.feintUntil &&
        (pk.type != ACT_SHOOT || nearOpp > 6.f)) {
        float p = 0.04f + 0.05f * (float)std::min(M.recentInter, 3) + 0.08f * M.oppJump;
        p *= metaSysScale(pk.type == ACT_SHOOT ? 2 : 1) * metaModeScale(mode, 1);
        p = clampf(p, 0.f, 0.22f);
        uint32_t sd = 0x9E3779B1u ^ (uint32_t)(frameNo_ * 2246822519u);
        if (!sd) sd = 1u;
        if (plusRand(sd) < p) {
            MetricVector2D decoy;
            int kind;
            if (pk.type == ACT_SHOOT) {
                // fake a pass: toward the best open team-mate
                int bi = -1;
                float bu = -1e9f;
                for (int i = 0; i < nCand_; i++)
                    if (plusIsPass(cand_[i].type) && cand_[i].xP >= 0.6f && cand_[i].utility > bu) { bu = cand_[i].utility; bi = i; }
                if (bi < 0) goto commit_gate;
                decoy = cand_[bi].target;
                kind = 2;
            } else {
                // fake a shot: toward goal when there is room, otherwise a step the other way
                if (s * car.pos.x > 8.f) decoy = MetricVector2D(s * PITCH_HALF_X, clampf(car.pos.y * 0.4f, -6.f, 6.f));
                else {
                    const MetricVector2D d = pk.target - car.pos;
                    decoy = car.pos + MetricVector2D(d.x * 0.6f, -d.y * 0.6f);
                }
                kind = 1;
            }
            GestureOutput tg = makeGesture(ACT_CARRY, car.pos, decoy, 0.f, false);
            if (tg.valid) {
                const double dur = 120.0 + 40.0 * (double)(sd % 3u) * 0.5;
                M.feintUntil = now + dur;
                M.feintLastAt = now;
                M.feintDir = decoy;
                M.feintKind = kind;
                M.lastSys = kind;
                M.lastSysAt = now;
                nextDecisionAt_ = M.feintUntil;
                tg.durationMs = (int)dur;
                *out = tg;
                return true;
            }
        }
    }
commit_gate:
    // delayed decision window with late commitment among the top three options (safe situations only)
    if (cfg_.metaCommit && nearOpp > 4.5f && !ownThird && now - M.commitDoneAt > 700.0) {
        int top[3] = {-1, -1, -1};
        for (int i = 0; i < nCand_; i++) {
            if (cand_[i].type == ACT_CARRY) continue;
            for (int k = 0; k < 3; k++) {
                if (top[k] < 0 || cand_[i].utility > cand_[top[k]].utility) {
                    for (int m = 2; m > k; m--) top[m] = top[m - 1];
                    top[k] = i;
                    break;
                }
            }
        }
        // smooth the utilities of the beam across frames
        float nu[4] = {0, 0, 0, 0};
        int nt[4] = {-1, -1, -1, -1}, ng[4] = {-1, -1, -1, -1};
        int nn = 0;
        for (int k = 0; k < 3 && nn < 4; k++) {
            if (top[k] < 0) continue;
            const PassCandidate& q = cand_[top[k]];
            float u = q.utility;
            for (int j = 0; j < 4; j++)
                if (M.smType[j] == q.type && M.smTarget[j] == q.targetId) { u = 0.5f * u + 0.5f * M.smU[j]; break; }
            nu[nn] = u; nt[nn] = q.type; ng[nn] = q.targetId; nn++;
        }
        for (int j = 0; j < 4; j++) { M.smU[j] = nu[j]; M.smType[j] = nt[j]; M.smTarget[j] = ng[j]; }
        const double W = clampf(0.5f * metaReactP80(), 0.07f, 0.20f) * 1000.0 * (double)metaSysScale(6);
        if (M.commitStart < 0.0) {
            M.commitStart = now;
            M.commitType = pk.type;
            M.commitTarget = pk.targetId;
        }
        const double el = now - M.commitStart;
        const float gap = nn >= 2 ? nu[0] - nu[1] : 1.f;
        const bool collapse = el >= W || gap >= 0.12f || nearOpp < 4.0f || pk.xP < 0.5f || pk.type == ACT_SHOOT;
        if (!collapse) {
            GestureOutput tg = makeGesture(ACT_CARRY, car.pos, pk.target, 0.f, false);
            if (tg.valid) {
                tg.durationMs = 70;
                nextDecisionAt_ = now;
                *out = tg;
                return true;
            }
        }
        // collapse: soft-max over the smoothed beam
        if (nn >= 2) {
            uint32_t sd = 0xC2B2AE35u ^ (uint32_t)(frameNo_ * 2654435761u);
            if (!sd) sd = 1u;
            float w[4], tot = 0.f, mx = nu[0];
            for (int j = 1; j < nn; j++) mx = std::max(mx, nu[j]);
            for (int j = 0; j < nn; j++) { w[j] = std::exp((nu[j] - mx) / 0.04f); tot += w[j]; }
            float u = plusRand(sd) * tot;
            int sel = 0;
            for (int j = 0; j < nn; j++) { u -= w[j]; if (u <= 0.f) { sel = j; break; } }
            for (int i = 0; i < nCand_; i++)
                if (cand_[i].type == nt[sel] && cand_[i].targetId == ng[sel] && cand_[i].xP >= 0.5f) { *pick = i; break; }
        }
        M.commitStart = -1.0;
        M.commitDoneAt = now;
        M.lastSys = M.lastSys < 0 ? 6 : M.lastSys;
        M.lastSysAt = now;
    } else {
        M.commitStart = -1.0;
    }
    return false;
}

float PitchEngine::metaMicroAngle(float pressure) const {
    if (!cfg_.meta || !cfg_.metaMicro) return 0.f;
    const MetaState& M = meta_;
    const float amp = (0.05f + 0.07f * clampf(pressure, 0.f, 1.f) + 0.04f * M.readEma) * metaModeScale(metaModeNow(), 2);
    return amp * std::sin(6.2831853f * 10.f * (float)(state_.timeMs / 1000.0) + 1.3f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Defence: adaptive jockey distance, weak side, second man, bait lane, recovery runs, shape
// ---------------------------------------------------------------------------------------------------------------------

float PitchEngine::metaContainBuf() const {
    const float base = 0.5f * (cfg_.containMin + cfg_.containMax);
    const MetaState& M = meta_;
    if (!cfg_.meta || !cfg_.metaContain || M.burstTot < 3.f) return base;
    static const float centre[4] = {1.5f, 2.5f, 3.5f, 5.0f};
    float sum = 0.f;
    for (int d = 0; d < 4; d++) for (int a = 0; a < 4; a++) sum += M.burst[d][a] * centre[d];
    const float mb = sum / M.burstTot;                       // where he usually bursts: stay just outside it
    const float conf = std::min(1.f, M.burstTot / 6.f);
    float adj = clampf((mb + 1.0f - base) * conf, -0.6f, 1.4f);
    if (-(float)state_.attackSign * state_.ball.x > PITCH_HALF_X - 26.f) adj -= 0.4f;        // tight on space in our third
    return clampf(base + adj, 1.4f, 5.0f);
}

float PitchEngine::metaShapeScale() const {
    if (!cfg_.meta) return 1.f;
    return clampf(1.f + (2.5f - meta_.oppProg) * 0.08f, 0.6f, 1.3f);
}

bool PitchEngine::metaSecondMan(MetricVector2D* pt) {
    const int ci = controlledIdx();
    if (ci < 0 || isKeeperIdx(ci) || !ballUsable()) return false;
    int carr = -1;
    float cd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, state_.ball);
        if (d < cd) { cd = d; carr = i; }
    }
    if (carr < 0 || cd > 2.5f) return false;
    const MetricVector2D cp = state_.opp[carr].pos;
    // is a first man (another of ours) already on him?
    int first = -1;
    float fd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (i == ci) continue;
        const PlayerState& q = state_.team[i];
        if (!q.active || !q.visible || isKeeperIdx(i)) continue;
        const float d = distance(q.pos, cp);
        if (d < fd) { fd = d; first = i; }
    }
    if (first < 0 || fd > 3.5f) return false;
    const float own = -(float)state_.attackSign;
    // the escape lane: forward pass to a team-mate of his that the first man does not already cover
    float bestScore = 0.f;
    MetricVector2D bestRecv;
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (i == carr) continue;
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        if (own * (o.pos.x - cp.x) < 2.f || distance(o.pos, cp) > 34.f || distance(o.pos, cp) < 6.f) continue;
        const MetricVector2D ln = o.pos - cp;
        const float lnl = ln.length();
        const MetricVector2D from = lnl > 8.f ? cp + ln * (4.f / lnl) : cp;           // the first man always stands on the first metres
        const float cover = segmentDistance(from, o.pos, state_.team[first].pos);
        if (cover < 2.5f) continue;
        const float score = (own * (o.pos.x - cp.x)) / 30.f + 0.1f * std::min(cover, 8.f) / 8.f;
        if (score > bestScore) { bestScore = score; bestRecv = o.pos; }
    }
    if (bestScore <= 0.f) return false;
    *pt = cp + (bestRecv - cp) * 0.28f;
    return true;
}

bool PitchEngine::metaBaitPoint(MetricVector2D* pt) {
    MetaState& M = meta_;
    const double now = state_.timeMs;
    const int ci = controlledIdx();
    if (ci < 0 || isKeeperIdx(ci) || !ballUsable()) return false;
    const float own = -(float)state_.attackSign;
    if (M.oppPassLost[0] / (M.oppPassOk[0] + M.oppPassLost[0]) < 0.40f) return false;       // that lane is not a bad bet for him
    if (metaSysScale(3) < 0.35f) return false;
    int carr = -1;
    float cd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, state_.ball);
        if (d < cd) { cd = d; carr = i; }
    }
    if (carr < 0 || cd > 2.5f) return false;
    const MetricVector2D cp = state_.opp[carr].pos;
    float best = 0.f;
    MetricVector2D recv;
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (i == carr) continue;
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float prog = own * (o.pos.x - cp.x);
        const float d = distance(o.pos, cp);
        if (prog < 8.f || d < 10.f || d > 34.f) continue;
        if (prog > best) { best = prog; recv = o.pos; }
    }
    if (best <= 0.f) return false;
    const MetricVector2D lane = recv - cp;
    const float ll = lane.length();
    if (ll < 1e-3f) return false;
    const MetricVector2D u(lane.x / ll, lane.y / ll), nrm(-u.y, u.x);
    // stand beside the lane (not on it) so it looks open, close enough to run it down
    const MetricVector2D on = cp + lane * 0.4f;
    const float side = (ctrlPos_.x - on.x) * nrm.x + (ctrlPos_.y - on.y) * nrm.y >= 0.f ? 1.f : -1.f;
    *pt = on + nrm * (side * 2.0f);
    M.baitUntil = now + 1500.0;
    M.baitKind = 0;
    return true;
}

// Recovery runs after a turnover: dangerous lanes (carrier to goal, carrier to his forward runners) are assigned greedily
// to our players by time-to-cut; returns the controlled player's cut point.
bool PitchEngine::metaRecoveryPoint(MetricVector2D* pt) {
    const int ci = controlledIdx();
    if (ci < 0 || isKeeperIdx(ci) || !ballUsable()) return false;
    const float own = -(float)state_.attackSign;
    const MetricVector2D ownGoal(own * PITCH_HALF_X, 0.f);
    int carr = -1;
    float cd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, state_.ball);
        if (d < cd) { cd = d; carr = i; }
    }
    if (carr < 0) return false;
    const MetricVector2D cp = state_.opp[carr].pos;
    struct Lane { MetricVector2D to; float danger; };
    Lane lanes[5];
    int nl = 0;
    lanes[nl++] = {ownGoal, 1.0f};
    float used[TEAM_SIZE];
    for (int i = 0; i < TEAM_SIZE; i++) used[i] = -1.f;
    for (int k = 0; k < 4; k++) {
        int bi = -1;
        float bs = 0.f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            if (i == carr || used[i] > 0.f) continue;
            const PlayerState& o = state_.opp[i];
            if (!o.active || !o.visible) continue;
            const float prog = own * (o.future.x - cp.x);
            if (prog < 3.f || distance(o.future, cp) > 36.f) continue;
            const float sc = std::exp(-distance(o.future, ownGoal) / 26.f);
            if (sc > bs) { bs = sc; bi = i; }
        }
        if (bi < 0) break;
        used[bi] = 1.f;
        lanes[nl++] = {state_.opp[bi].future, 0.5f + 0.5f * bs};
    }
    // players: our outfielders that are not the keeper
    int pl[TEAM_SIZE], np = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& q = state_.team[i];
        if (q.active && q.visible && !isKeeperIdx(i)) pl[np++] = i;
    }
    bool taken[TEAM_SIZE];
    for (int i = 0; i < TEAM_SIZE; i++) taken[i] = false;
    for (int k = 0; k < nl; k++) {
        int bp = -1;
        float bt = 1e9f;
        MetricVector2D bc;
        for (int a = 0; a < np; a++) {
            if (taken[pl[a]]) continue;
            const PlayerState& q = state_.team[pl[a]];
            // cut point: the point of the lane nearest to the player, 3..14 m from the carrier
            const MetricVector2D seg = lanes[k].to - cp;
            const float l2 = std::max(seg.x * seg.x + seg.y * seg.y, 1e-6f);
            const float t = clampf(((q.pos.x - cp.x) * seg.x + (q.pos.y - cp.y) * seg.y) / l2, 3.f / std::sqrt(l2), std::min(1.f, 14.f / std::sqrt(l2)));
            const MetricVector2D c = cp + seg * t;
            const float cost = plusTta(q, c, 7.0f) / lanes[k].danger;
            if (cost < bt) { bt = cost; bp = pl[a]; bc = c; }
        }
        if (bp < 0) continue;
        taken[bp] = true;
        if (bp == ci) { *pt = bc; return true; }
    }
    return false;
}

// Which side of our defender he usually goes (+1 = toward +y): stand on that side so the space is left on his weak side.
float PitchEngine::metaSideBias() const {
    const MetaState& M = meta_;
    if (!cfg_.meta || !cfg_.metaContain) return 0.f;
    const float tot = M.sideCnt[0] + M.sideCnt[1];
    if (tot < 2.f) return 0.f;
    return clampf((M.sideCnt[1] - M.sideCnt[0]) / (tot + 2.f), -1.f, 1.f) * 0.9f;
}
