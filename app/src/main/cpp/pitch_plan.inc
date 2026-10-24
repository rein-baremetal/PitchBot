// Planning layer: lane-cut model, offside trap, combination streak, keeper model, role shape and cover shadow, conditional
// counter-press, tempo phases, ball hypotheses, free-man ranking, directional clears, local overloads, per-channel
// learning, restart build-up, Gaussian pressure field, late-game risk and adaptive cadence. Included at the end of
// pitch_engine.cpp after pitch_meta.inc (inside namespace pe). Everything is per match.

void PlanState::clear() {
    cacheAt = -1e9; cacheType = cacheTarget = -1;
    linePrev = lineRate = 0.f; linePrevOk = false; trapUntil = -1e9;
    streak = 0; streakAt = comboForceAt = -1e9;
    keeperBias = 0.f; keeperN = 0; keeperWatch = false; keeperWatchAt = -1e9; keeperY0 = 0.f;
    cpOk = true; cpUsed = false; cpCheckAt = cpBlockedUntil = midBlockUntil = -1e9;
    cpFailAt[0] = cpFailAt[1] = -1e9;
    pressTrig = 6.f; pressTrigN = 0; prevClose = 0.f;
    lineLevel = lineBehind = lineLead = 0.f; haveLevel = haveLead = false; lineSampleAt = -1e9;
    winAt = -1e9;
    seenEma = 1.f; ballHyp = 0; attachSide = attachIdx = -1; attachOff = MetricVector2D();
    for (int i = 0; i < TEAM_SIZE; i++) { danger[i] = 0.f; freeR[i] = 0.5f; }
    topFree[0] = topFree[1] = -1; rankAt = -1e9;
    sideUntil = sideLast = -1e9; sideTo = MetricVector2D();
    for (int c = 0; c < PLAN_CH; c++) for (int k = 0; k < 3; k++) { chOk[c][k] = 0.f; chN[c][k] = 0.f; }
    curChan = 1;
    buildUntil = -1e9; buildCount = 0; lastRestart = 0;
    shadowAt = -1e9;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pressure field: Gaussian weights of the opponents around a point, boosted by how fast they are closing on it
// ---------------------------------------------------------------------------------------------------------------------

float PitchEngine::planPressure(const MetricVector2D& p) const {
    float sum = 0.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        const MetricVector2D d = p - o.future;
        const float dl = d.length();
        if (dl > 12.f) continue;
        float w = std::exp(-dl * dl / (2.f * 3.2f * 3.2f));
        if (dl > 1e-3f) {
            const float closing = o.vel.dot(d) / dl;                     // speed toward the point
            w *= 1.f + clampf(closing / 6.f, 0.f, 1.f);
        }
        sum += w;
    }
    return sum;
}

double PitchEngine::ballHoldMs() const {
    // a dirty radar (the ring is lost often) is allowed to coast longer
    if (!cfg_.plan || !cfg_.planBall) return 700.0;
    const float dirty = clampf((0.6f - plan_.seenEma) / 0.6f, 0.f, 1.f);
    return 700.0 + 500.0 * (double)dirty;
}

// ---------------------------------------------------------------------------------------------------------------------
// Per-frame bookkeeping
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::planOnTurnover() {
    PlanState& P = plan_;
    const double now = frameTimeMs_;
    int near = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& q = state_.team[i];
        if (q.active && q.visible && !isKeeperIdx(i) && distance(q.pos, state_.ball) <= 12.f) near++;
    }
    P.cpOk = near >= 3;
    P.cpUsed = false;
    P.cpCheckAt = now + 2500.0;
    if (!P.cpOk) P.midBlockUntil = now + 3000.0;                      // not enough players: drop into a compact block
}

void PitchEngine::planOnWin() { plan_.winAt = frameTimeMs_; }

bool PitchEngine::planCounterPressOk() const {
    if (!cfg_.plan || !cfg_.planCounterPress) return true;
    return plan_.cpOk && state_.timeMs >= plan_.cpBlockedUntil;
}

void PitchEngine::planOnShot(const PlayerState& car) {
    (void)car;
    if (!cfg_.plan || !cfg_.planKeeper) return;
    MetricVector2D k;
    if (!findKeeper(*this, (float)state_.attackSign, &k)) return;
    plan_.keeperWatch = true;
    plan_.keeperWatchAt = state_.timeMs;
    plan_.keeperY0 = k.y;
}

void PitchEngine::planFrame(double dtSec) {
    PlanState& P = plan_;
    const double now = state_.timeMs;
    const float dt = (float)std::max(dtSec, 1e-3);
    const float s = (float)state_.attackSign;

    if (state_.status != ST_NOT_LIVE) P.seenEma += 0.03f * ((state_.ballSeen ? 1.f : 0.f) - P.seenEma);

    // offside trap: their back line stepping up quickly
    if (cfg_.planTrap && oppLineConf_ > 0.3f) {
        if (P.linePrevOk) {
            const float r = (oppLineDepth_ - P.linePrev) / dt;
            P.lineRate += 0.2f * (clampf(r, -6.f, 6.f) - P.lineRate);
            if (P.lineRate > 1.0f) P.trapUntil = now + 220.0;
        }
        P.linePrev = oppLineDepth_;
        P.linePrevOk = true;
    } else {
        P.linePrevOk = false;
    }

    // conditional counter-press: did the window win the ball back? two failures within a minute cool the press off
    if (P.cpCheckAt > 0.0 && now >= P.cpCheckAt) {
        if (P.cpUsed && state_.status == ST_OPP) {
            const double prev = P.cpFailAt[1];                          // the failure before this one
            P.cpFailAt[0] = prev;
            P.cpFailAt[1] = now;
            if (prev > now - 60000.0) P.cpBlockedUntil = now + 20000.0;
        }
        P.cpCheckAt = -1e9;
    }

    // keeper model: where did he go after our shot
    if (P.keeperWatch && now - P.keeperWatchAt >= 700.0) {
        MetricVector2D k;
        if (findKeeper(*this, s, &k)) {
            const float dy = clampf((k.y - P.keeperY0) / 2.5f, -1.f, 1.f);
            P.keeperBias = P.keeperN == 0 ? dy : 0.6f * P.keeperBias + 0.4f * dy;
            P.keeperN++;
        }
        P.keeperWatch = false;
    }

    // press trigger distance: where the nearest opponent starts closing on our carrier
    if (state_.status == ST_TEAM && meta_.iOpp >= 0) {
        if (meta_.iPrevClose > 3.f && P.prevClose <= 1.5f && meta_.iPrevD > 1.5f && meta_.iPrevD < 10.f) {
            P.pressTrig += (P.pressTrigN ? 0.2f : 1.f) * (meta_.iPrevD - P.pressTrig);
            P.pressTrigN++;
        }
        P.prevClose = meta_.iPrevClose;
    }
    // line depth by score state, sampled every 10 s: how deep do they drop when they are losing
    if (cfg_.planOppModel && oppLineConf_ > 0.3f && now - P.lineSampleAt >= 10000.0) {
        P.lineSampleAt = now;
        const int lead = plus_.goalsFor - plus_.goalsAgainst;
        float& slot = lead > 0 ? P.lineLead : (lead < 0 ? P.lineBehind : P.lineLevel);
        bool& have = lead > 0 ? P.haveLead : P.haveLevel;
        if (lead >= 0) { slot = have ? 0.6f * slot + 0.4f * oppLineDepth_ : oppLineDepth_; have = true; }
    }

    // danger of every opponent and freedom of every team-mate, every 100 ms
    if (cfg_.planFreeMan && now - P.rankAt >= 100.0 && ballUsable()) {
        P.rankAt = now;
        const MetricVector2D ownGoal(-s * PITCH_HALF_X, 0.f);
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            P.danger[i] = 0.f;
            if (!o.active || !o.visible) continue;
            const float gd = distance(o.future, ownGoal);
            float support = 0.f;
            for (int j = 0; j < TEAM_SIZE; j++)
                if (j != i && state_.opp[j].active && distance(state_.opp[j].future, o.future) < 12.f) support += 1.f;
            const float lane = clampf(segmentDistance(state_.ball, o.future, state_.team[std::max(controlledIdx(), 0)].pos) / 4.f, 0.f, 1.f);
            P.danger[i] = std::exp(-gd / 24.f) * (0.5f + 0.5f * lane) * (0.6f + 0.4f * std::min(support, 3.f) / 3.f);
        }
        float best[2] = {-1.f, -1.f};
        P.topFree[0] = P.topFree[1] = -1;
        for (int j = 0; j < TEAM_SIZE; j++) {
            const PlayerState& t = state_.team[j];
            P.freeR[j] = 0.5f;
            if (!t.active || !t.visible || isKeeperIdx(j)) continue;
            const float open = clampf(opennessOf(t.future), 0.f, 1.f);
            const MetricVector2D toG(s * PITCH_HALF_X - t.future.x, -t.future.y);
            const float gl = toG.length();
            const float prog = gl > 1e-3f ? clampf((s * (t.future.x - state_.ball.x)) / 20.f, -1.f, 1.f) * 0.5f + 0.5f : 0.5f;
            float support = 0.f;
            for (int k = 0; k < TEAM_SIZE; k++)
                if (k != j && state_.team[k].active && distance(state_.team[k].future, t.future) < 15.f) support += 1.f;
            P.freeR[j] = clampf(0.5f * open + 0.3f * prog + 0.2f * std::min(support, 3.f) / 3.f - 0.15f * planPressure(t.future), 0.f, 1.f);
            if (j == state_.carrier) continue;
            if (P.freeR[j] > best[0]) { best[1] = best[0]; P.topFree[1] = P.topFree[0]; best[0] = P.freeR[j]; P.topFree[0] = j; }
            else if (P.freeR[j] > best[1]) { best[1] = P.freeR[j]; P.topFree[1] = j; }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Ball hypotheses while the ring is lost: coast, or hidden under the nearest player
// ---------------------------------------------------------------------------------------------------------------------

bool PitchEngine::planBallHypothesis(double dtSec) {
    (void)dtSec;
    if (!cfg_.plan || !cfg_.planBall) return false;
    PlanState& P = plan_;
    const float sp = state_.ballVel.length();
    if (P.ballHyp == 2 && P.attachIdx >= 0) {            // already attached: follow that player while he is tracked
        const PlayerState& a = P.attachSide ? state_.opp[P.attachIdx] : state_.team[P.attachIdx];
        if (a.active) {
            state_.ball = a.pos + P.attachOff;
            state_.ballVel = a.vel;
            return true;
        }
        P.ballHyp = 1;
    }
    int bs = -1, bi = -1;
    float bd = 1e9f;
    for (int side = 0; side < 2; side++)
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& p = side ? state_.opp[i] : state_.team[i];
            if (!p.active || !p.visible) continue;
            const float d = distance(p.pos, state_.ball);
            if (d < bd) { bd = d; bs = side; bi = i; }
        }
    const float sCoast = 0.1f + 0.9f * clampf(sp / 6.f, 0.f, 1.f);
    const float sAttach = sp < 3.f && bd < 2.2f ? (1.f - sp / 3.f) * std::exp(-bd / 2.2f) + 0.05f : 0.f;
    if (sAttach > sCoast && bi >= 0) {
        P.ballHyp = 2;
        P.attachSide = bs;
        P.attachIdx = bi;
        const PlayerState& a = bs ? state_.opp[bi] : state_.team[bi];
        P.attachOff = (state_.ball - a.pos) * 0.5f;
        state_.ball = a.pos + P.attachOff;
        state_.ballVel = a.vel;
        return true;
    }
    P.ballHyp = 1;
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Keeper aim, directional clears, shape
// ---------------------------------------------------------------------------------------------------------------------

float PitchEngine::planKeeperAim(float aim) const {
    if (!cfg_.plan || !cfg_.planKeeper || plan_.keeperN < 1 || std::fabs(plan_.keeperBias) < 0.35f) return aim;
    // he tends to go to this side: take the other post, a little inside it
    const float inner = cfg_.goalHalfWidth - cfg_.stunPostInset;
    if (aim * plan_.keeperBias > 0.f) return aim > 0.f ? -inner : inner;
    return aim;
}

float PitchEngine::planClearSide(const PlayerState& car, float side) const {
    const float s = (float)state_.attackSign;
    const float a = cfg_.panicAngleDeg * (PI_F / 180.f);
    float bestScore = -1e9f, best = side;
    float pressY = 0.f;
    int np = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (o.active && distance(o.future, car.pos) < 14.f) { pressY += o.future.y - car.pos.y; np++; }
    }
    for (int sd = -1; sd <= 1; sd += 2) {
        const MetricVector2D t = car.pos + MetricVector2D(s * std::cos(a), (float)sd * std::sin(a)) * cfg_.panicReach;
        if (std::fabs(t.y) > PITCH_HALF_Y - 1.5f) continue;
        float score = 0.f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (o.active && distance(o.future, t) < 8.f) score -= 1.f;
            const PlayerState& q = state_.team[i];
            if (q.active && q.visible && i != state_.carrier && distance(q.future, t) < 12.f) score += 0.8f;
        }
        if (np > 0 && (pressY / (float)np) * (float)sd < 0.f) score += 0.6f;          // opposite to the press
        if (sd == (side > 0.f ? 1 : -1)) score += 0.3f;                              // keep the default side on a tie
        if (score > bestScore) { bestScore = score; best = (float)sd; }
    }
    return best;
}

// Cover shadow: a midfielder who is not pressing stands in the lane from the carrier to the most dangerous attacker.
bool PitchEngine::planShadowPoint(MetricVector2D* pt) {
    if (!cfg_.plan || !cfg_.planShape || !cfg_.planFreeMan) return false;
    const int ci = controlledIdx();
    if (ci < 0 || isKeeperIdx(ci) || !ballUsable()) return false;
    const int r = role_[ci];
    if (r != ROLE_DM && r != ROLE_CM && r != ROLE_AM) return false;
    int carr = -1;
    float cd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, state_.ball);
        if (d < cd) { cd = d; carr = i; }
    }
    if (carr < 0 || cd > 2.5f) return false;
    int di = -1;
    float bd = 0.12f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (i == carr || !state_.opp[i].active) continue;
        if (plan_.danger[i] > bd) { bd = plan_.danger[i]; di = i; }
    }
    if (di < 0) return false;
    const MetricVector2D cp = state_.opp[carr].pos;
    const MetricVector2D p = cp + (state_.opp[di].future - cp) * 0.4f;
    // only the nearest of our midfielders to that point takes it
    float mine = distance(state_.team[ci].pos, p);
    for (int j = 0; j < TEAM_SIZE; j++) {
        if (j == ci || !state_.team[j].active || isKeeperIdx(j)) continue;
        const int rj = role_[j];
        if ((rj == ROLE_DM || rj == ROLE_CM || rj == ROLE_AM) && distance(state_.team[j].pos, p) < mine - 0.5f) return false;
    }
    *pt = p;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lane cut: where can an opponent be before the ball passes, with acceleration and a lean toward the lane
// ---------------------------------------------------------------------------------------------------------------------

float PitchEngine::planLaneCut(const PlayerState& car, const PassCandidate& c) const {
    const MetricVector2D a = car.pos, ab = c.target - a;
    const float L = ab.length();
    if (L < 1.f) return 0.f;
    const float vb = std::max(cfg_.passSpeed, 6.f);
    float worst = 0.f;
    const float react = metaReactP80();
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float t = clampf((o.pos - a).dot(ab) / (L * L), 0.f, 1.f);
        const MetricVector2D P = a + ab * t;
        const float tb = clampf(t * L / vb, 0.05f, 2.0f);                // when the ball gets to P
        const MetricVector2D toLane = P - o.pos;
        const float dl = toLane.length();
        float closed = 0.f;
        if (dl > 1e-3f) {
            const MetricVector2D u = toLane * (1.f / dl);
            const float vAlong = o.vel.dot(u);
            const float aAlong = clampf(meta_.acc[i].dot(u), -6.f, 6.f);
            closed = vAlong * tb * 0.6f + 0.5f * aAlong * tb * tb * 0.5f + 0.9f * tb;     // measured motion + a lean toward the lane
        }
        const float need = std::max(0.f, dl - std::max(closed, 0.f) - cfg_.defBody) / 6.5f + react;
        worst = std::max(worst, logistic((tb + 0.08f - need) / 0.12f));
    }
    return worst;
}

// ---------------------------------------------------------------------------------------------------------------------
// Candidate preparation and utility terms
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::planPrepare(const PlayerState& car) {
    if (!cfg_.plan) return;
    // adaptive through lead: lean the aim away from the nearest defender's side by up to 0.9 m while xP holds
    for (int i = 0; i < nCand_; i++) {
        PassCandidate& c = cand_[i];
        if (c.type != ACT_THROUGH) continue;
        int bi = -1;
        float bd = 1e9f;
        for (int k = 0; k < TEAM_SIZE; k++) {
            const PlayerState& o = state_.opp[k];
            if (!o.active) continue;
            const float d = distance(o.future, c.target);
            if (d < bd) { bd = d; bi = k; }
        }
        if (bi < 0 || bd > 6.f) continue;
        const float side = state_.opp[bi].future.y > c.target.y ? -1.f : 1.f;
        MetricVector2D t = c.target;
        t.y = clampf(t.y + side * 0.9f, -PITCH_HALF_Y, PITCH_HALF_Y);
        const float len = distance(car.pos, t);
        if (len < cfg_.minPassLen || len > cfg_.safePassLen || offsideAmount(t) > 0.5f) continue;
        const int ti = c.targetId - 1;
        if (ti < 0 || ti >= TEAM_SIZE) continue;
        float clr = 0.f;
        const float xp = passProbability(car.pos, t, clr, true, false) * receiverReliability(state_.team[ti]);
        if (xp < c.xP - 0.02f) continue;
        c.target = t;
        c.length = len;
        c.clearance = clr;
        c.xP = xp;
        c.xPRaw = xp;
        c.offside = offsideAmount(t);
    }
}

void PitchEngine::planApply(const PlayerState& car) {
    if (!cfg_.plan || nCand_ <= 0) return;
    PlanState& P = plan_;
    const double now = state_.timeMs;
    const float s = (float)state_.attackSign;
    const int mode = plusModeNow();
    const float ph = plus_.phase >= 0.f ? clampf(plus_.phase, 0.f, 1.f)
                                        : clampf((float)(plus_.liveMs / 1000.0 / std::max(cfg_.plusMatchSec, 60.f)), 0.f, 1.f);
    const int lead = plus_.goalsFor - plus_.goalsAgainst;
    const float ax = s * car.pos.x;
    const float press = cfg_.planPressure ? planPressure(car.pos) : 0.f;
    const bool highPress = press > cfg_.planPressHigh;
    const int tempoZone = ax < -12.f ? 0 : (ax > cfg_.plusFinalAx ? 2 : 1);
    const bool late8 = ph >= 1.f - 8.f / 90.f, late3 = ph >= 1.f - 3.f / 90.f;     // minutes of a 90-minute match

    // numbers around the ball in the attacking half
    int ours = 0, theirs = 0, oppL = 0, oppR = 0;
    MetricVector2D cen(car.pos.x, car.pos.y);
    float pressY = 0.f;
    int np = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& q = state_.team[i];
        if (q.active && q.visible && i != state_.carrier && !isKeeperIdx(i) && distance(q.pos, car.pos) <= 14.f) { ours++; cen = cen + q.pos; }
        const PlayerState& o = state_.opp[i];
        if (o.active && o.visible) {
            if (distance(o.pos, car.pos) <= 14.f) {
                theirs++;
                pressY += o.pos.y - car.pos.y;
                np++;
            }
            if (distance(o.pos, car.pos) <= 22.f) { if (o.pos.y > car.pos.y) oppR++; else oppL++; }
        }
    }
    cen = cen * (1.f / (float)(ours + 1));
    const bool finalThird = ax > cfg_.plusFinalAx;
    const bool overload = cfg_.planOverload && finalThird && ours >= 2 && theirs >= 1 && ours + 1 > theirs;      // 2v1, 3v2
    const bool outnumbered = cfg_.planOverload && finalThird && theirs > ours + 1;
    const float weakSide = oppR > oppL ? -1.f : 1.f;
    const float awayFromPress = np > 0 && pressY > 0.f ? -1.f : 1.f;

    for (int i = 0; i < nCand_; i++) {
        PassCandidate& c = cand_[i];
        const bool pass = plusIsPass(c.type);
        float b = 0.f;
        if (pass) {
            // lane cut
            if (cfg_.planLane && c.type != ACT_LOFTED) {
                const float cut = planLaneCut(car, c);
                if (cut > 0.85f) b -= 1.2f; else b -= cfg_.wLane * cut * (0.4f + c.xP);
            }
            // offside trap: delay the through ball while the line steps up, prefer the lay-off
            if (cfg_.planTrap && now < P.trapUntil) {
                if (c.type == ACT_THROUGH) b -= 0.40f;
                else if (c.deltaXT <= 0.03f && c.xP >= 0.8f) b += 0.15f;
            }
            // combination streak: short high-xP passes while it lasts
            if (cfg_.planCombo && P.streak > 0 && now - P.streakAt < 3500.0 && c.length <= 14.f && c.xP >= 0.7f)
                b += 0.05f * (float)std::min(P.streak, 4);
            // free man
            if (cfg_.planFreeMan && c.targetId >= 1 && c.targetId <= TEAM_SIZE) {
                b += cfg_.wFree * (P.freeR[c.targetId - 1] - 0.5f) * c.xP;
                if (highPress && (c.targetId - 1 == P.topFree[0] || c.targetId - 1 == P.topFree[1]) && c.xP >= 0.55f) b += 0.15f;
            }
            // tempo phases
            if (cfg_.planTempo) {
                if (mode == 2) { if (c.type == ACT_THROUGH) b -= 1.5f; }
                else {
                    if (tempoZone == 0 && c.xP >= 0.8f) b += 0.06f;                              // build: the safe pass is rewarded
                    if (tempoZone == 1 && c.deltaXT > 0.01f && c.xP >= 0.6f) b += 0.05f * c.xP;   // progress: forward gain is rewarded
                    if (tempoZone == 2 && c.type == ACT_THROUGH) b += 0.08f * c.xP;
                    if (mode == 3 && c.deltaXT >= 0.01f && c.xP >= 0.55f) b += c.xP < 0.7f ? 0.10f : 0.06f;   // chase: forward gain wins, with a lower safety floor
                }
            }
            // local overloads
            if (overload) {
                if (distance(c.target, cen) <= 8.f) b += 0.14f * c.xP;
                if (c.type == ACT_THROUGH) b += 0.10f * c.xP;
            } else if (outnumbered) {
                if (c.xP >= 0.8f && c.deltaXT <= 0.02f) b += 0.15f;
                if ((c.type == ACT_LOFTED || c.type == ACT_PASS) && c.target.y * weakSide > 8.f && std::fabs(c.target.y - car.pos.y) > 14.f && c.xP >= 0.55f) b += 0.20f;
            }
            // channel learning against this opponent
            if (cfg_.planChannel) {
                const int ch = c.target.y < -11.f ? 0 : (c.target.y > 11.f ? 2 : 1);
                const int kind = c.type == ACT_THROUGH ? 1 : (c.type == ACT_LOFTED ? 2 : 0);
                if (P.chN[ch][kind] >= 3.f) {
                    const float rate = (P.chOk[ch][kind] + 1.f) / (P.chN[ch][kind] + 2.f);
                    b += clampf((rate - 0.75f) * 0.5f, -0.25f, 0.08f);
                }
            }
            // build-up after a restart: away from the press side first, no risk until three passes are made
            if (cfg_.planRestart && now < P.buildUntil && P.buildCount < 3) {
                if (c.type == ACT_THROUGH || c.type == ACT_LOFTED) b -= 0.30f;
                if ((c.target.y - car.pos.y) * awayFromPress > 3.f && c.xP >= 0.8f) b += 0.12f;
            }
            // first pass after winning the ball: direct against a slow recovery, safe against a quick one
            if (cfg_.planOppModel && now - P.winAt < 2500.0) {
                if (plus_.im[4] < 0.4f && c.deltaXT > 0.f) b += 0.20f * c.deltaXT * c.xP * 3.f;
                else if (plus_.im[4] > 0.6f && c.xP >= 0.85f) b += 0.10f;
            }
            // cut-back after a second ball is won near their box
            if (cfg_.planSecondBall && now - shotFiredMs_ < 4000.0 && s * car.pos.x > PITCH_HALF_X - 16.f &&
                s * c.target.x > PITCH_HALF_X - 14.f && std::fabs(c.target.y) < 8.f && c.xP >= 0.5f) b += 0.20f;
            // late-game risk
            if (cfg_.planLateRisk && late8) {
                if (lead >= 1) {
                    if (c.length > 14.f) b -= 0.3f * std::min(1.f, (c.length - 14.f) / 6.f);
                    if (c.xP < 0.85f) b -= 0.3f * (0.85f - c.xP);
                } else if (lead <= -1) {
                    if (c.deltaXT > 0.01f && c.xP >= 0.5f && c.xP < 0.8f) b += 0.10f;
                    if (c.type == ACT_THROUGH) b += 0.10f;
                }
            }
        } else if (c.type == ACT_SHOOT) {
            if (cfg_.planLateRisk && late8 && lead <= -1 && c.length > 18.f) b += 0.10f;
            if (cfg_.planTempo && tempoZone == 2 && c.xP > 0.3f && mode != 2) b += 0.05f;
        } else if (c.type == ACT_CARRY) {
            if (outnumbered) b -= 0.10f;
        }
        if (cfg_.planLateRisk && late3 && lead == 0) {                     // a draw in the closing minutes: do not be predictable
            uint32_t h = (uint32_t)(frameNo_ * 2654435761u) ^ (uint32_t)((i + 1) * 40503u);
            h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            b += ((float)(h & 0xFFFFu) / 65535.f - 0.5f) * 0.10f;
        }
        c.utility += clampf(b, -1.5f, 0.5f);
    }

    // plan cache: the option that led a moment ago keeps a small edge for a few frames
    if (cfg_.planCache) {
        if (now - P.cacheAt < 300.0 && P.cacheType >= 0) {
            for (int i = 0; i < nCand_; i++)
                if (cand_[i].type == P.cacheType && cand_[i].targetId == P.cacheTarget && cand_[i].type != ACT_CARRY) cand_[i].utility += cfg_.wCache;
        }
        int bi = -1;
        for (int i = 0; i < nCand_; i++)
            if (cand_[i].type != ACT_CARRY && (bi < 0 || cand_[i].utility > cand_[bi].utility)) bi = i;
        if (bi >= 0) {
            if (P.cacheType != cand_[bi].type || P.cacheTarget != cand_[bi].targetId) P.cacheAt = now;
            else if (now - P.cacheAt >= 300.0) P.cacheAt = now;
            P.cacheType = cand_[bi].type;
            P.cacheTarget = cand_[bi].targetId;
        }
    }
}

void PitchEngine::planNoteAction(const PassCandidate& c, const PlayerState& car) {
    if (!cfg_.plan) return;
    PlanState& P = plan_;
    const double now = state_.timeMs;
    if (c.type == ACT_LOFTED) P.streak = 0;                                  // a long ball or a clear ends the combination
    else if (plusIsPass(c.type) && c.length <= 14.f && c.xP >= 0.65f) {
        P.streak = now - P.streakAt < 3500.0 ? P.streak + 1 : 1;
        P.streakAt = now;
    }
    P.curChan = c.target.y < -11.f ? 0 : (c.target.y > 11.f ? 2 : 1);
    if (now < P.buildUntil && plusIsPass(c.type)) P.buildCount++;
    (void)car;
}

void PitchEngine::planPassResult(bool kept) {
    if (!cfg_.plan || !cfg_.planChannel) return;
    PlanState& P = plan_;
    const int kind = meta_.curKind >= 0 && meta_.curKind < 3 ? meta_.curKind : 0;
    const int ch = P.curChan;
    for (int c = 0; c < PLAN_CH; c++) for (int k = 0; k < 3; k++) { P.chOk[c][k] *= 0.98f; P.chN[c][k] *= 0.98f; }     // about the last 50 passes
    P.chN[ch][kind] += 1.f;
    if (kept) P.chOk[ch][kind] += 1.f;
}

// ---------------------------------------------------------------------------------------------------------------------
// Decision-time: forced one-two return, sidestep away from a closing opponent, cadence
// ---------------------------------------------------------------------------------------------------------------------

bool PitchEngine::planForcePick(int* pick, const PlayerState& car) {
    if (!cfg_.plan || !cfg_.planCombo || !pick || *pick < 0 || cand_[*pick].type == ACT_SHOOT) return false;
    PlanState& P = plan_;
    const double now = state_.timeMs;
    const float s = (float)state_.attackSign;
    if (plus_.lastPasser < 0 || now - plus_.lastPassMs > 1500.0 || state_.carrier == plus_.lastPasser || now - P.comboForceAt < 1500.0) return false;
    const PlayerState& pr = state_.team[plus_.lastPasser];
    if (!pr.active || s * (pr.future.x - plus_.lastPassFrom.x) < 3.f || s * pr.vel.x < 1.5f) return false;
    if (nearestOppDistance(car.pos) > 7.f) return false;                       // free carriers can take their time
    int bi = -1;
    for (int i = 0; i < nCand_; i++) {
        const PassCandidate& c = cand_[i];
        if (c.targetId == plus_.lastPasser + 1 && (c.type == ACT_PASS || c.type == ACT_THROUGH) && c.xP >= 0.7f && c.offside < 0.5f)
            if (bi < 0 || c.utility > cand_[bi].utility) bi = i;
    }
    if (bi < 0 || cand_[bi].utility < cand_[*pick].utility - 0.12f) return false;
    *pick = bi;
    P.comboForceAt = now;
    return true;
}

bool PitchEngine::planSidestep(int pick, const PlayerState& car, bool forced, GestureOutput* out) {
    if (!cfg_.plan || !cfg_.planSidestep || human_ || forced || pick < 0 || cand_[pick].type != ACT_CARRY) return false;
    PlanState& P = plan_;
    const double now = state_.timeMs;
    const float s = (float)state_.attackSign;
    if (now < P.sideUntil) {                                                  // a sidestep in progress keeps its direction
        GestureOutput tg = makeGesture(ACT_CARRY, car.pos, P.sideTo, 0.f, false);
        if (!tg.valid) return false;
        tg.durationMs = (int)std::max(60.0, P.sideUntil - now);
        nextDecisionAt_ = P.sideUntil;
        *out = tg;
        return true;
    }
    if (now - P.sideLast < 1200.0) return false;
    int bi = -1;
    float bd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, car.pos);
        if (d < bd) { bd = d; bi = i; }
    }
    if (bi < 0 || bd > 5.f || bd < 2.0f) return false;
    const PlayerState& o = state_.opp[bi];
    const MetricVector2D toMe = car.pos - o.pos;
    if (o.vel.dot(toMe) / std::max(toMe.length(), 1e-3f) < 2.5f) return false;       // not actually closing
    MetricVector2D u = toMe * (1.f / std::max(toMe.length(), 1e-3f));
    MetricVector2D perp(-u.y, u.x);
    if (s * perp.x < 0.f && std::fabs(perp.x) > 0.3f) perp = perp * -1.f;           // never step back toward our own goal
    if (car.pos.y * perp.y > 0.f && std::fabs(car.pos.y) > 24.f) perp = perp * -1.f; // and not toward the touchline
    MetricVector2D d = perp * 0.7f + u * 0.5f;
    const float dl = d.length();
    if (dl < 1e-3f) return false;
    d = d * (1.f / dl);
    const MetricVector2D to = car.pos + d * 4.f;
    GestureOutput tg = makeGesture(ACT_CARRY, car.pos, to, 0.f, false);
    if (!tg.valid) return false;
    const double dur = 90.0 + 10.0 * (double)(frameNo_ % 3);
    P.sideUntil = now + dur;
    P.sideLast = now;
    P.sideTo = to;
    nextDecisionAt_ = P.sideUntil;
    tg.durationMs = (int)dur;
    *out = tg;
    return true;
}

int PitchEngine::planCadenceMs(int base) const {
    if (!cfg_.plan || !cfg_.planCadence) return base;
    const int ci = state_.carrier;
    if (state_.status != ST_TEAM || ci < 0 || ci >= TEAM_SIZE) return base;
    const MetricVector2D pos = state_.team[ci].pos;
    const float ax = (float)state_.attackSign * pos.x;
    const float p = planPressure(pos);
    if (p > cfg_.planPressHigh || ax > cfg_.plusFinalAx) return std::min(base, 100);
    if (cfg_.planCalmSlow && p < cfg_.planPressCalm && ax < cfg_.plusFinalAx && !state_.crowded) return std::max(base, 200);
    return base;
}
