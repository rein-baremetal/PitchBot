// Match intelligence pack. Included at the end of pitch_engine.cpp (inside namespace pe).
//
// Everything in here is per match: the state is cleared whenever the engine is reset for a new match and nothing is
// written to disk. The pack only adds bounded terms to the existing candidate utilities, steers the carry direction,
// and adds defensive guidance; the outputs stay stick direction + the mapped buttons.

namespace {

inline uint32_t plusXs(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
inline float plusRand(uint32_t& s) { return (float)(plusXs(s) & 0xFFFFFFu) * (1.f / 16777216.f); }

inline int plusDensBin(int n) { return n <= 0 ? 0 : (n <= 2 ? 1 : 2); }
inline bool plusIsPass(int t) { return t == ACT_PASS || t == ACT_THROUGH || t == ACT_LOFTED; }
inline int plusActBin(int t) { return t == ACT_THROUGH ? 1 : (t == ACT_LOFTED ? 2 : 0); }

}  // namespace

void PlusState::clear() {
    liveMs = 0;
    phase = -1.f;
    goalsFor = goalsAgainst = 0;
    lastGoalMs = -1e9;
    lastBallAx = lastBallY = 0.f;
    lastBallMs = -1e9;
    lastBallValid = false;
    stableStatus = candStatus = candRun = 0;
    lastTeamMs = -1e9;
    const float im0[5] = {0.5f, 0.5f, 0.5f, 0.3f, 0.5f};
    for (int i = 0; i < 5; i++) { im[i] = im0[i]; ip[i] = 0.25f; }
    longFlag = false;
    longEvents = 0;
    oppPossMs = 0;
    epvInit = false;
    for (int i = 0; i < PLUS_EPV_N; i++) epv[i] = 0.f;
    nTrace = 0;
    for (int i = 0; i < PLUS_TRACE; i++) trace[i] = 0;
    nSeq = seqHead = 0;
    cur = PlusSeq();
    for (int i = 0; i < PLUS_SEQ; i++) seq[i] = PlusSeq();
    for (int i = 0; i < PLUS_KEYS; i++) keyLift[i] = keyN[i] = 0.f;
    for (int i = 0; i < PLUS_PAIRS; i++) { pairKey[i] = 0; pairLift[i] = 0.f; pairUsed[i] = 0; }
    for (int i = 0; i < 4; i++) { theta[i] = 0.5f; eps[i] = 0.f; }
    winStartMs = -1;
    winValueSum = 0.f;
    winValueN = 0;
    winTurnovers = 0;
    baseline = 0.f;
    lastPasser = -1;
    lastPassFrom = MetricVector2D();
    lastPassMs = -1e9;
    for (int i = 0; i < MAX_CAND; i++) mh[i] = 0.5f;
    coverAt = -1e9;
    clutchOverride = -1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Clock, estimated score and match phase
// ---------------------------------------------------------------------------------------------------------------------

int PitchEngine::plusModeNow() const {
    if (!cfg_.plus || !cfg_.plusPhase) return 0;
    const float ph = plus_.phase >= 0.f ? clampf(plus_.phase, 0.f, 1.f)
                                        : clampf((float)(plus_.liveMs / 1000.0 / std::max(cfg_.plusMatchSec, 60.f)), 0.f, 1.f);
    const int lead = plus_.goalsFor - plus_.goalsAgainst;
    if (lead >= 2 && ph >= 0.3f) return 2;
    if (lead >= 1 && ph >= 0.5f) return 2;
    if (lead <= -2 && ph >= 0.3f) return 3;
    if (lead <= -1 && ph >= 0.5f) return 3;
    if (ph < 0.5f && lead <= 0) return 1;
    return 0;
}

int PitchEngine::effClutch() const {
    const int m = plusModeNow();
    if (m == 2) return 1;
    if (m == 3) return 2;
    return clutch_;
}

float PitchEngine::plusLeadEst() const { return (float)(plus_.goalsFor - plus_.goalsAgainst); }

// A goal shows up on the radar as a stoppage with the ball last seen in a goal mouth.
void PitchEngine::plusStoppage() {
    PlusState& P = plus_;
    plusEndSequence(0, state_.ball);
    // A stoppage near the penalty area is not enough evidence of a goal: corners, saves,
    // clearances and noisy radar dots all create similar patterns. Keep this estimate
    // conservative until a reliable scoreboard/OCR signal is available.
    if (!P.lastBallValid || state_.timeMs - P.lastBallMs > 2500.0) return;
    if (state_.timeMs - P.lastGoalMs < 20000.0) return;          // one replay is one goal
    if (std::fabs(P.lastBallAx) < PITCH_HALF_X - 2.8f || std::fabs(P.lastBallY) > 5.5f) return;
    if (P.lastBallAx > 0.f) P.goalsFor++; else P.goalsAgainst++;
    P.lastGoalMs = state_.timeMs;
}

void PitchEngine::plusFrame(double dtSec) {
    PlusState& P = plus_;
    P.liveMs += dtSec * 1000.0;
    const float s = (float)state_.attackSign;
    if (ballUsable()) {
        P.lastBallAx = s * state_.ball.x;
        P.lastBallY = state_.ball.y;
        P.lastBallMs = state_.timeMs;
        P.lastBallValid = true;
    }
    if (!P.epvInit) epvInit();

    // debounced possession edge: a turnover ends the running sequence
    const int st = state_.status;
    if (st == P.candStatus) { if (P.candRun < 100000) P.candRun++; } else { P.candStatus = st; P.candRun = 1; }
    if (st == ST_TEAM) P.lastTeamMs = state_.timeMs;
    if (P.candRun >= 5 && P.stableStatus != st) {
        const int prev = P.stableStatus;
        P.stableStatus = st;
        if (cfg_.meta) metaEdge(prev, st);
        if (st == ST_OPP && prev != ST_OPP && state_.timeMs - P.lastTeamMs < 2500.0) {
            plusEndSequence(-1, state_.ball);
            P.winTurnovers++;
            if (cfg_.meta) metaTurnover();
        }
    }

    if (cfg_.plusIntent) plusIntentUpdate(dtSec);
    if (cfg_.plusStyle) plusStyleUpdate(dtSec);
}

// ---------------------------------------------------------------------------------------------------------------------
// Opponent intent filter (scalar Kalman filters over five latent traits)
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::plusIntentUpdate(double dtSec) {
    PlusState& P = plus_;
    const float s = (float)state_.attackSign;
    float obs[5] = {0, 0, 0, 0, 0};
    bool has[5] = {false, false, false, false, false};

    float ax[TEAM_SIZE];
    int idx[TEAM_SIZE];
    int n = 0;
    int deepest = -1;
    float deepestAx = -1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float a = s * o.pos.x;
        if (a > deepestAx) { deepestAx = a; deepest = i; }
        ax[n] = a;
        idx[n++] = i;
    }
    if (n >= 7) {
        float sum = 0.f;
        int m = 0, left = 0, right = 0;
        float retreat = 0.f;
        for (int k = 0; k < n; k++) {
            if (idx[k] == deepest) continue;
            const PlayerState& o = state_.opp[idx[k]];
            sum += ax[k];
            m++;
            if (o.pos.y > 0.f) right++; else left++;
            retreat += s * o.vel.x;
        }
        if (m > 0) {
            obs[0] = clampf(0.5f - (sum / (float)m) / 40.f, 0.f, 1.f);       // 1 = pressing high
            has[0] = true;
            obs[1] = clampf(0.5f + 0.5f * (float)(right - left) / (float)std::max(right + left, 1), 0.f, 1.f);
            has[1] = true;
            obs[2] = clampf(1.f - state_.oppSpread / 30.f, 0.f, 1.f);        // 1 = compact
            has[2] = true;
            if (state_.status == ST_TEAM && state_.timeMs - ownerSinceMs_ < 3000.0) {
                obs[4] = clampf((retreat / (float)m) / 4.f, 0.f, 1.f);       // how fast they drop back after losing the ball
                has[4] = true;
            }
        }
    }
    if (state_.status == ST_OPP) {
        P.oppPossMs += dtSec * 1000.0;
        const float bs = state_.ballVel.length();
        if (bs >= 11.f && s * state_.ballVel.x < -8.f) {
            if (!P.longFlag) { P.longFlag = true; P.longEvents++; }
        } else if (bs < 6.f) {
            P.longFlag = false;
        }
        if (P.oppPossMs > 4000.0) {
            const float perMin = (float)P.longEvents / (float)(P.oppPossMs / 60000.0);
            obs[3] = clampf(perMin / 8.f, 0.f, 1.f);
            has[3] = true;
        }
    }
    const float R = 0.05f, q = 0.0006f;
    for (int k = 0; k < 5; k++) {
        P.ip[k] += q;
        if (!has[k]) continue;
        const float K = P.ip[k] / (P.ip[k] + R);
        P.im[k] += K * (obs[k] - P.im[k]);
        P.ip[k] *= (1.f - K);
    }
}

float PitchEngine::plusIntentBonus(const PlayerState& car, const PassCandidate& c) const {
    (void)car;
    if (!plusIsPass(c.type)) return 0.f;
    const PlusState& P = plus_;
    const float conf = clampf(1.f - P.ip[0] * 4.f, 0.f, 1.f);
    if (conf < 0.2f) return 0.f;
    float b = 0.f;
    // a high press leaves room behind the line
    b += (P.im[0] - 0.5f) * 2.f * (c.type == ACT_THROUGH ? 1.f : (c.type == ACT_LOFTED ? 0.6f : 0.2f)) * c.xP * 0.5f;
    // attack the flank they have fewer players on
    const float weak = P.im[1] > 0.5f ? -1.f : 1.f;
    const float strength = std::fabs(P.im[1] - 0.5f) * 2.f;
    if (c.deltaXT > 0.f) {
        const float side = c.target.y * weak;
        b += strength * (side > 6.f ? 1.f : (side < -6.f ? -0.4f : 0.f)) * c.xP * 0.5f;
    }
    // compact and quick to recover: go wide; stretched out: go through the middle
    if (std::fabs(c.target.y) > 16.f) b += 0.5f * P.im[2] * P.im[4] * c.xP;
    if (std::fabs(c.target.y) < 10.f && c.deltaXT > 0.02f) b += 0.4f * (1.f - P.im[2]) * c.xP;
    return b * conf;
}

// ---------------------------------------------------------------------------------------------------------------------
// Role-aware expected possession value
// ---------------------------------------------------------------------------------------------------------------------

int PitchEngine::epvRoleBin(int teamIdx) const {
    if (teamIdx < 0 || teamIdx >= TEAM_SIZE) return 2;
    switch (role_[teamIdx]) {
        case ROLE_GK: case ROLE_CB: return 0;
        case ROLE_FB: case ROLE_WG: return 1;
        case ROLE_DM: case ROLE_CM: return 2;
        case ROLE_AM: return 3;
        case ROLE_ST: return 4;
        default: return 2;
    }
}

int PitchEngine::epvCell(int role, float ax, float y, int dens, int vbin) const {
    const int col = xTGrid::colOf(ax), row = xTGrid::rowOf(y);
    return (((role * XT_COLS + col) * XT_ROWS + row) * PLUS_EPV_DENS + dens) * PLUS_EPV_VEL + vbin;
}

void PitchEngine::epvInit() {
    static const float densF[PLUS_EPV_DENS] = {1.00f, 0.86f, 0.66f};
    static const float roleF[PLUS_EPV_ROLES] = {0.97f, 1.00f, 1.00f, 1.02f, 1.03f};
    for (int r = 0; r < PLUS_EPV_ROLES; r++)
        for (int c = 0; c < XT_COLS; c++)
            for (int w = 0; w < XT_ROWS; w++)
                for (int d = 0; d < PLUS_EPV_DENS; d++)
                    for (int v = 0; v < PLUS_EPV_VEL; v++)
                        plus_.epv[(((r * XT_COLS + c) * XT_ROWS + w) * PLUS_EPV_DENS + d) * PLUS_EPV_VEL + v] =
                            grid_.xt[w][c] * densF[d] * roleF[r] * (v ? 1.04f : 1.0f);
    plus_.epvInit = true;
}

float PitchEngine::epvValue(int role, float ax, float y, int dens, int vbin) const {
    if (!plus_.epvInit) return grid_.valueAt(ax, y);
    return plus_.epv[epvCell(role, ax, y, dens, vbin)];
}

int PitchEngine::oppDensityAt(const MetricVector2D& p, float r) const {
    int n = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (o.active && distance(o.future, p) < r) n++;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sequences: recording, TD(lambda) value update, motif mining
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::plusNoteAction(const PassCandidate& c, const PlayerState& car) {
    if (!cfg_.plus) return;
    PlusState& P = plus_;
    if (!plusIsPass(c.type)) return;
    const float s = (float)state_.attackSign;
    const int ci = state_.carrier;
    const int ti = c.targetId - 1;
    if (ci < 0 || ti < 0 || ti >= TEAM_SIZE) return;
    if (P.nTrace == 0) {
        P.trace[P.nTrace++] = epvCell(epvRoleBin(ci), s * car.pos.x, car.pos.y, plusDensBin(oppDensityAt(car.pos, 6.f)), s * car.vel.x > 1.5f ? 1 : 0);
    }
    if (P.nTrace < PLUS_TRACE) {
        const PlayerState& r = state_.team[ti];
        P.trace[P.nTrace++] = epvCell(epvRoleBin(ti), s * c.target.x, c.target.y, plusDensBin(oppDensityAt(c.target, 6.f)), s * r.vel.x > 1.5f ? 1 : 0);
    }
    if (P.cur.n < PLUS_STEPS) {
        const int key = (role_[ci] * 3 + plusActBin(c.type)) * 9 + role_[ti];
        P.cur.key[P.cur.n++] = (unsigned short)std::min(std::max(key, 0), PLUS_KEYS - 1);
    }
    P.lastPasser = ci;
    P.lastPassFrom = car.pos;
    P.lastPassMs = state_.timeMs;
}

void PitchEngine::plusEndSequence(int outcome, const MetricVector2D& at) {
    PlusState& P = plus_;
    if (P.cur.n > 0 || P.nTrace > 0) {
        if (outcome != 0 && P.epvInit && P.nTrace > 0) {
            const float s = (float)state_.attackSign;
            const float R = outcome > 0 ? xTGrid::shotValue(s * at.x, at.y) : -0.03f;
            float G = R;
            const float lam = clampf(cfg_.plusLambda, 0.f, 1.f), al = clampf(cfg_.plusAlpha, 0.f, 0.5f);
            for (int t = P.nTrace - 1; t >= 0; t--) {
                float& V = P.epv[P.trace[t]];
                const float Vt = V;
                V = clampf(Vt + al * (G - Vt), 0.f, 0.9f);
                G = (1.f - lam) * Vt + lam * G;
            }
        }
        if (P.cur.n > 0) {
            P.cur.outcome = outcome;
            P.seq[P.seqHead] = P.cur;
            P.seqHead = (P.seqHead + 1) % PLUS_SEQ;
            if (P.nSeq < PLUS_SEQ) P.nSeq++;
            plusMineSequences();
        }
    }
    P.cur = PlusSeq();
    P.nTrace = 0;
}

void PitchEngine::plusMineSequences() {
    PlusState& P = plus_;
    float sum[PLUS_KEYS];
    float cnt[PLUS_KEYS];
    for (int i = 0; i < PLUS_KEYS; i++) sum[i] = cnt[i] = 0.f;
    float gsum = 0.f, gn = 0.f;
    for (int i = 0; i < PLUS_PAIRS; i++) { P.pairUsed[i] = 0; P.pairLift[i] = 0.f; P.pairKey[i] = 0; }
    float psum[PLUS_PAIRS], pcnt[PLUS_PAIRS];
    for (int i = 0; i < PLUS_PAIRS; i++) psum[i] = pcnt[i] = 0.f;
    for (int i = 0; i < P.nSeq; i++) {
        const PlusSeq& q = P.seq[i];
        if (q.outcome == 0 || q.n == 0) continue;
        gsum += (float)q.outcome;
        gn += 1.f;
    }
    const float gm = gn > 0.f ? gsum / gn : 0.f;
    for (int i = 0; i < P.nSeq; i++) {
        const PlusSeq& q = P.seq[i];
        if (q.outcome == 0 || q.n == 0) continue;
        for (int t = 0; t < q.n; t++) {
            sum[q.key[t]] += (float)q.outcome;
            cnt[q.key[t]] += 1.f;
            if (t > 0) {
                const unsigned short pk = (unsigned short)(q.key[t - 1] * PLUS_KEYS + q.key[t]);
                int h = (int)((q.key[t - 1] * 31u + q.key[t] * 17u) % PLUS_PAIRS);
                for (int probe = 0; probe < PLUS_PAIRS; probe++) {
                    const int j = (h + probe) % PLUS_PAIRS;
                    if (!P.pairUsed[j]) { P.pairUsed[j] = 1; P.pairKey[j] = pk; psum[j] = 0.f; pcnt[j] = 0.f; }
                    if (P.pairKey[j] == pk) { psum[j] += (float)q.outcome; pcnt[j] += 1.f; break; }
                }
            }
        }
    }
    for (int k = 0; k < PLUS_KEYS; k++) {
        P.keyN[k] = cnt[k];
        P.keyLift[k] = cnt[k] > 0.f ? (sum[k] / cnt[k] - gm) * cnt[k] / (cnt[k] + 4.f) : 0.f;
    }
    for (int j = 0; j < PLUS_PAIRS; j++)
        if (P.pairUsed[j]) P.pairLift[j] = pcnt[j] > 0.f ? (psum[j] / pcnt[j] - gm) * pcnt[j] / (pcnt[j] + 3.f) : 0.f;
}

float PitchEngine::plusMotifBonus(const PlayerState& car, const PassCandidate& c) const {
    (void)car;
    const PlusState& P = plus_;
    if (!plusIsPass(c.type) || P.nSeq < 6) return 0.f;
    const int ci = state_.carrier, ti = c.targetId - 1;
    if (ci < 0 || ti < 0 || ti >= TEAM_SIZE) return 0.f;
    const int key = std::min(std::max((role_[ci] * 3 + plusActBin(c.type)) * 9 + role_[ti], 0), PLUS_KEYS - 1);
    float lift = P.keyLift[key];
    if (P.cur.n > 0) {
        const unsigned short pk = (unsigned short)(P.cur.key[P.cur.n - 1] * PLUS_KEYS + key);
        const int h = (int)((P.cur.key[P.cur.n - 1] * 31u + (unsigned)key * 17u) % PLUS_PAIRS);
        for (int probe = 0; probe < PLUS_PAIRS; probe++) {
            const int j = (h + probe) % PLUS_PAIRS;
            if (!P.pairUsed[j]) break;
            if (P.pairKey[j] == pk) { lift += 0.6f * P.pairLift[j]; break; }
        }
    }
    return clampf(lift, -0.5f, 0.5f);
}

// ---------------------------------------------------------------------------------------------------------------------
// TD style controller: patience, directness, width, risk (a perturbation search driven by the possession-value TD error)
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::plusStyleUpdate(double dtSec) {
    (void)dtSec;
    PlusState& P = plus_;
    const double now = state_.timeMs;
    const float s = (float)state_.attackSign;
    if (state_.status == ST_TEAM && ballUsable()) {
        P.winValueSum += grid_.valueAt(s * state_.ball.x, state_.ball.y);
        P.winValueN++;
    }
    if (P.winStartMs < 0) {
        P.winStartMs = now;
        P.winTurnovers = 0;
        uint32_t sd = 0x9E3779B9u ^ (uint32_t)(frameNo_ * 2654435761u);
        for (int i = 0; i < 4; i++) P.eps[i] = (plusRand(sd) - 0.5f) * 0.24f;
        return;
    }
    if (now - P.winStartMs < 3000.0) return;
    const float v = P.winValueN > 0 ? P.winValueSum / (float)P.winValueN : 0.f;
    const float r = v - 0.04f * (float)P.winTurnovers;
    const float delta = r - P.baseline;
    P.baseline += 0.2f * (r - P.baseline);
    for (int i = 0; i < 4; i++) {
        P.theta[i] = clampf(P.theta[i] + (delta > 0.f ? 0.5f * P.eps[i] : -0.25f * P.eps[i]), 0.15f, 0.85f);
    }
    uint32_t sd = 0xA511E9B3u ^ (uint32_t)(frameNo_ * 2246822519u);
    for (int i = 0; i < 4; i++) P.eps[i] = (plusRand(sd) - 0.5f) * 0.24f;
    P.winStartMs = now;
    P.winValueSum = 0.f;
    P.winValueN = 0;
    P.winTurnovers = 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Potential-field carry direction
// ---------------------------------------------------------------------------------------------------------------------

MetricVector2D PitchEngine::plusFieldDir(const MetricVector2D& from) const {
    const float s = (float)state_.attackSign;
    MetricVector2D F(s * 0.6f, 0.f);
    const int ci = state_.carrier;
    const int role = epvRoleBin(ci);
    const float base = epvValue(role, s * from.x, from.y, plusDensBin(oppDensityAt(from, 6.f)), 0);
    for (int k = 0; k < 8; k++) {
        const float a = (float)k * 0.78539816f;
        const MetricVector2D d(std::cos(a), std::sin(a));
        MetricVector2D p = from + d * 6.f;
        p.x = clampf(p.x, -PITCH_HALF_X, PITCH_HALF_X);
        p.y = clampf(p.y, -PITCH_HALF_Y, PITCH_HALF_Y);
        const float gain = epvValue(role, s * p.x, p.y, plusDensBin(oppDensityAt(p, 5.f)), 1) - base;
        F = F + d * (gain * 14.f);
    }
    for (int j = 0; j < TEAM_SIZE; j++) {
        if (j == ci) continue;
        const PlayerState& t = state_.team[j];
        if (!t.active || isKeeperIdx(j)) continue;
        const MetricVector2D dp = t.future - from;
        const float d = dp.length();
        if (d < 4.f || d > 18.f || s * dp.x < -2.f) continue;
        F = F + dp * (0.25f * opennessOf(t.future) / d);
    }
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        const MetricVector2D dp = from - o.future;
        const float d = dp.length();
        if (d >= 12.f || d < 0.1f) continue;
        const float k = (12.f - d) / 12.f;
        const float ahead = s * (o.future.x - from.x) > -1.f ? 1.3f : 0.8f;
        F = F + dp * (1.5f * k * k * ahead / d);
    }
    if (std::fabs(from.y) > 26.f) F.y -= (from.y > 0.f ? 1.f : -1.f) * (std::fabs(from.y) - 26.f) / 8.f * 1.2f;
    if (s * from.x > 40.f) F.x -= s * 0.4f;
    if (s * from.x < -20.f && s * F.x < 0.f) F.x = 0.f;
    const float l = F.length();
    if (!(l > 1e-4f)) return MetricVector2D(s, 0.f);
    return F * (1.f / l);
}

// ---------------------------------------------------------------------------------------------------------------------
// Candidate preparation (through-ball hypotheses) and utility bonuses
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::plusPrepare(const PlayerState& car) {
    PlusState& P = plus_;
    for (int i = 0; i < MAX_CAND; i++) P.mh[i] = 0.5f;
    if (!cfg_.plusThrough) { if (cfg_.meta) metaPrepare(car);
    planPrepare(car); return; }
    const float s = (float)state_.attackSign;
    const MetricVector2D from = car.pos;
    for (int i = 0; i < nCand_; i++) {
        PassCandidate& c = cand_[i];
        if (c.type != ACT_THROUGH && c.type != ACT_LOFTED) continue;
        const int ti = c.targetId - 1;
        if (ti < 0 || ti >= TEAM_SIZE) continue;
        const PlayerState& r = state_.team[ti];
        if (!r.active) continue;
        const float T = clampf(c.length / std::max(cfg_.passSpeed, 1.f), 0.3f, 1.6f);
        const MetricVector2D fwd(s, 0.f), lat(0.f, 1.f);
        const float am = cfg_.accMax;
        // acceleration profiles: hold speed, push on, ease off, run across (both ways), stop
        const MetricVector2D acc[7] = {MetricVector2D(), fwd * am, fwd * (0.5f * am), fwd * (-0.5f * am), lat * (0.6f * am), lat * (-0.6f * am),
                                       r.vel * (-1.f / std::max(T, 0.3f))};
        MetricVector2D pt[7];
        float sc[7];
        float sumSc = 0.f, wSum = 0.f;
        MetricVector2D aim;
        const float rel = receiverReliability(r);
        for (int k = 0; k < 7; k++) {
            MetricVector2D v = r.vel + acc[k] * T;
            const float vl = v.length();
            if (vl > cfg_.maxSpeed) v = v * (cfg_.maxSpeed / vl);
            MetricVector2D p = r.pos + (r.vel + v) * (0.5f * T);
            p.x = clampf(p.x, -PITCH_HALF_X, PITCH_HALF_X);
            p.y = clampf(p.y, -PITCH_HALF_Y, PITCH_HALF_Y);
            pt[k] = p;
            float clr = 0.f;
            const float xp = passProbability(from, p, clr, c.type == ACT_THROUGH, c.type == ACT_LOFTED) * rel;
            const bool off = offsideAmount(p) > 0.5f;
            const float shot = clampf(xTGrid::shotValue(s * p.x, p.y) / 0.25f, 0.f, 1.f);
            sc[k] = off ? 0.f : xp * (0.55f + 0.45f * shot);
            sumSc += sc[k];
            const float w = sc[k] * sc[k];
            aim = aim + p * w;
            wSum += w;
        }
        P.mh[i] = sumSc / 7.f;
        if (wSum > 1e-4f) {
            aim = aim * (1.f / wSum);
            const float len = distance(from, aim);
            if (len >= cfg_.minPassLen && len <= cfg_.safePassLen) {
                float clr = 0.f;
                const float xp = passProbability(from, aim, clr, c.type == ACT_THROUGH, c.type == ACT_LOFTED) * rel;
                if (xp >= c.xP - 0.05f && offsideAmount(aim) <= 0.5f) {
                    c.target = aim;
                    c.length = len;
                    c.clearance = clr;
                    c.xP = xp;
                    c.xPRaw = xp;
                    c.offside = offsideAmount(aim);
                    c.deltaXT = std::max(computeExpectedThreat(car, aim), cfg_.minDeltaXT);
                    c.pressure = lanePressure(aim);
                }
            }
        }
    }
    if (cfg_.meta) metaPrepare(car);
    planPrepare(car);
}

// Three-touch pass-sequence dynamic program over the team-mates: value of the best chain pass -> pass -> pass from
// every first pass. q[i] receives the chain value of candidate i (pass kinds only).
float PitchEngine::plusSeqValue(const PlayerState& car, float* q, int n) const {
    const float s = (float)state_.attackSign;
    float V0[TEAM_SIZE];
    MetricVector2D fp[TEAM_SIZE];
    bool on[TEAM_SIZE];
    for (int k = 0; k < TEAM_SIZE; k++) {
        const PlayerState& t = state_.team[k];
        on[k] = t.active && !isKeeperIdx(k) && k != state_.carrier;
        fp[k] = t.future;
        V0[k] = 0.f;
        if (!on[k]) continue;
        float v = epvValue(epvRoleBin(k), s * t.future.x, t.future.y, plusDensBin(oppDensityAt(t.future, 6.f)), s * t.vel.x > 1.5f ? 1 : 0);
        if (cfg_.oppLine && oppLineConf_ > 0.25f && s * t.future.x > (PITCH_HALF_X - oppLineDepth_) + 1.f) v += 0.03f * oppLineConf_;
        V0[k] = v;
    }
    float P2[TEAM_SIZE][TEAM_SIZE];
    for (int a = 0; a < TEAM_SIZE; a++)
        for (int b = 0; b < TEAM_SIZE; b++) {
            P2[a][b] = 0.f;
            if (!on[a] || !on[b] || a == b) continue;
            if (distance(fp[a], fp[b]) > cfg_.maxPassLen * 1.2f || distance(fp[a], fp[b]) < cfg_.minPassLen) continue;
            float clr = 0.f;
            P2[a][b] = passProbability(fp[a], fp[b], clr, false, false) * receiverReliability(state_.team[b]);
        }
    // Dynamic program over up to three further passes after the candidate root pass.
    // Each edge is multiplied by its estimated completion probability, so a longer chain
    // is rewarded only when the accumulated risk is justified by its eventual position.
    float V1[TEAM_SIZE], V2[TEAM_SIZE], V3[TEAM_SIZE];
    for (int a = 0; a < TEAM_SIZE; a++) {
        V1[a] = V0[a];
        for (int b = 0; b < TEAM_SIZE; b++) V1[a] = std::max(V1[a], P2[a][b] * V0[b]);
    }
    for (int a = 0; a < TEAM_SIZE; a++) {
        V2[a] = V0[a];
        for (int b = 0; b < TEAM_SIZE; b++) V2[a] = std::max(V2[a], P2[a][b] * V1[b]);
    }
    for (int a = 0; a < TEAM_SIZE; a++) {
        V3[a] = V0[a];
        for (int b = 0; b < TEAM_SIZE; b++) V3[a] = std::max(V3[a], P2[a][b] * V2[b]);
    }
    const int ci = state_.carrier;
    float here = epvValue(epvRoleBin(ci), s * car.pos.x, car.pos.y, plusDensBin(oppDensityAt(car.pos, 6.f)), s * car.vel.x > 1.5f ? 1 : 0);
    for (int i = 0; i < n; i++) {
        const PassCandidate& c = cand_[i];
        q[i] = -1.f;
        if (!plusIsPass(c.type)) continue;
        const int ti = c.targetId - 1;
        if (ti < 0 || ti >= TEAM_SIZE || !on[ti]) continue;
        // Root pass plus at most three follow-up passes = a four-pass plan.
        q[i] = c.xP * V3[ti];
    }
    return here;
}

// Depth-limited Monte-Carlo tree over the discrete actions (pass, carry in eight directions, shoot). Opponents are
// advanced with their measured velocity plus pursuit of the ball, team-mates keep running; outcomes are sampled from
// lane clearance. q[i] = mean return of root action i, have[i] tells whether it was evaluated within the time budget.
void PitchEngine::plusTreeScores(const PlayerState& car, float* q, bool* have) const {
    const auto t0 = std::chrono::steady_clock::now();
    const float s = (float)state_.attackSign;
    const int n = nCand_;
    for (int i = 0; i < n; i++) { q[i] = 0.f; have[i] = false; }
    struct Sim {
        MetricVector2D me, tm[TEAM_SIZE], tv[TEAM_SIZE], op[TEAM_SIZE], ov[TEAM_SIZE];
        bool tOn[TEAM_SIZE], oOn[TEAM_SIZE];
    };
    Sim base;
    base.me = car.pos;
    for (int k = 0; k < TEAM_SIZE; k++) {
        const PlayerState& t = state_.team[k];
        base.tm[k] = t.future;
        base.tv[k] = t.vel;
        base.tOn[k] = t.active && !isKeeperIdx(k) && k != state_.carrier;
        const PlayerState& o = state_.opp[k];
        base.op[k] = o.future;
        base.ov[k] = o.vel;
        base.oOn[k] = o.active;
    }
    const MetricVector2D goal(s * PITCH_HALF_X, 0.f);
    auto clampP = [](MetricVector2D p) {
        p.x = clampf(p.x, -PITCH_HALF_X, PITCH_HALF_X);
        p.y = clampf(p.y, -PITCH_HALF_Y, PITCH_HALF_Y);
        return p;
    };
    auto nearestOpp = [&](const Sim& S, const MetricVector2D& p) {
        float m = 1e3f;
        for (int i = 0; i < TEAM_SIZE; i++) if (S.oOn[i]) m = std::min(m, distance(S.op[i], p));
        return m;
    };
    auto value = [&](const Sim& S, const MetricVector2D& p) {
        static const float densF[3] = {1.f, 0.86f, 0.66f};
        int cnt = 0;
        for (int i = 0; i < TEAM_SIZE; i++) if (S.oOn[i] && distance(S.op[i], p) < 6.f) cnt++;
        return grid_.valueAt(s * p.x, p.y) * densF[plusDensBin(cnt)];
    };
    auto lane = [&](const Sim& S, const MetricVector2D& a, const MetricVector2D& b) {
        float m = 1e3f;
        for (int i = 0; i < TEAM_SIZE; i++) if (S.oOn[i]) m = std::min(m, segmentDistance(a, b, S.op[i]));
        return logistic(2.2f * (m - 1.5f));
    };
    auto advance = [&](Sim& S, float dt) {
        for (int i = 0; i < TEAM_SIZE; i++) {
            if (S.oOn[i]) {
                MetricVector2D d = S.me - S.op[i];
                const float l = d.length();
                if (l > 1e-3f) S.op[i] = S.op[i] + d * (std::min(l, 5.0f * dt * 0.55f) / l);
                S.op[i] = clampP(S.op[i] + S.ov[i] * (0.25f * dt));
            }
            if (S.tOn[i]) S.tm[i] = clampP(S.tm[i] + S.tv[i] * (0.6f * dt));
        }
    };
    const float DT = 0.8f;
    const int DEPTH = 4;
    const float FAIL = -0.03f;
    uint32_t seed = 0x7F4A7C15u ^ (uint32_t)(frameNo_ * 2654435761u);
    if (!seed) seed = 1u;

    // one action; returns true when the rollout ended, with its return in *ret
    auto act = [&](Sim& S, int kind, int arg, const MetricVector2D& aim, float* ret) -> bool {
        if (kind == 0) {                                   // pass to team-mate arg (aim = where the ball is played to)
            const float p = lane(S, S.me, aim) * 0.97f;
            if (plusRand(seed) >= p) { *ret = FAIL; return true; }
            S.me = aim;
            S.tm[arg] = aim;
            return false;
        }
        if (kind == 1) {                                   // carry toward aim
            const MetricVector2D to = clampP(aim);
            const float p = logistic(1.6f * (nearestOpp(S, to) - 2.0f));
            if (plusRand(seed) >= p) { *ret = FAIL; return true; }
            S.me = to;
            return false;
        }
        const float dg = distance(S.me, goal);              // shoot
        *ret = xTGrid::shotValue(s * S.me.x, S.me.y) * lane(S, S.me, goal) * (dg < 32.f ? 1.f : 0.3f);
        return true;
    };

    for (int i = 0; i < n; i++) {
        const PassCandidate& c = cand_[i];
        if (std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count() > cfg_.plusTreeMs) break;
        int kind;
        int arg = 0;
        if (plusIsPass(c.type)) {
            arg = c.targetId - 1;
            if (arg < 0 || arg >= TEAM_SIZE || !base.tOn[arg]) continue;
            kind = 0;
        } else if (c.type == ACT_CARRY) kind = 1;
        else if (c.type == ACT_SHOOT) kind = 2;
        else continue;
        float total = 0.f;
        const int N = std::max(4, cfg_.plusTreeRollouts);
        for (int r = 0; r < N; r++) {
            Sim S = base;
            float ret = 0.f;
            bool done = act(S, kind, arg, c.target, &ret);
            int depth = 1;
            while (!done && depth < DEPTH) {
                advance(S, DT);
                const float u = plusRand(seed);
                const float dg = distance(S.me, goal);
                if (u < 0.45f) {                            // best of three random team-mates
                    int bk = -1;
                    float bv = -1e9f;
                    for (int tries = 0; tries < 3; tries++) {
                        const int k = (int)(plusRand(seed) * (float)TEAM_SIZE) % TEAM_SIZE;
                        if (!S.tOn[k]) continue;
                        const float d = distance(S.me, S.tm[k]);
                        if (d < cfg_.minPassLen || d > cfg_.maxPassLen) continue;
                        const float v = value(S, S.tm[k]);
                        if (v > bv) { bv = v; bk = k; }
                    }
                    done = bk >= 0 ? act(S, 0, bk, S.tm[bk], &ret) : act(S, 1, 0, S.me + MetricVector2D(s, 0.f) * 5.5f, &ret);
                } else if (u < 0.80f || dg > 26.f) {        // carry in one of eight directions, biased forward
                    const float a = (plusRand(seed) * 2.f - 1.f) * 1.57f + (s > 0.f ? 0.f : 3.14159f);
                    const MetricVector2D to = S.me + MetricVector2D(std::cos(a), std::sin(a)) * 5.5f;
                    done = act(S, 1, 0, to, &ret);
                } else {
                    done = act(S, 2, 0, goal, &ret);
                }
                depth++;
            }
            total += done ? ret : value(S, S.me);
        }
        q[i] = total / (float)N;
        have[i] = true;
    }
}

void PitchEngine::plusApply(const PlayerState& car) {
    PlusState& P = plus_;
    if (!cfg_.plus || nCand_ <= 0) return;
    const float s = (float)state_.attackSign;
    const int n = nCand_;
    const double now = state_.timeMs;
    const int mode = plusModeNow();

    float seqQ[MAX_CAND];
    float seqHere = 0.f;
    const bool useSeq = cfg_.plusSeq;
    if (useSeq) seqHere = plusSeqValue(car, seqQ, n);
    float treeQ[MAX_CAND];
    bool treeHave[MAX_CAND];
    float treeMean = 0.f;
    int treeN = 0;
    const bool useTree = cfg_.plusTree;
    if (useTree) {
        plusTreeScores(car, treeQ, treeHave);
        for (int i = 0; i < n; i++) if (treeHave[i]) { treeMean += treeQ[i]; treeN++; }
        if (treeN > 0) treeMean /= (float)treeN;
    }

    const int ci = state_.carrier;
    const int crole = epvRoleBin(ci);
    const int cdens = plusDensBin(oppDensityAt(car.pos, 6.f));
    const int cvel = s * car.vel.x > 1.5f ? 1 : 0;
    const float hereEpv = epvValue(crole, s * car.pos.x, car.pos.y, cdens, cvel);

    // style parameters, nudged by the phase
    float th[4] = {P.theta[0], P.theta[1], P.theta[2], P.theta[3]};      // patience, directness, width, risk
    if (mode == 2) { th[0] += 0.25f; th[3] -= 0.20f; th[1] -= 0.10f; }
    else if (mode == 3) { th[1] += 0.25f; th[3] += 0.20f; th[0] -= 0.15f; }
    else if (mode == 1) { th[1] += 0.15f; th[3] += 0.10f; }
    for (int i = 0; i < 4; i++) th[i] = clampf(th[i], 0.f, 1.f);

    // alternatives for the final-third rule
    bool altExists = false;
    for (int i = 0; i < n; i++) {
        const PassCandidate& c = cand_[i];
        if ((plusIsPass(c.type) && c.xP >= 0.5f && c.offside < 0.5f) || (c.type == ACT_SHOOT && c.xP >= 0.4f)) altExists = true;
    }
    const bool finalThird = cfg_.plusFinalThird && s * car.pos.x > cfg_.plusFinalAx;
    const bool oneTwoOn = cfg_.plusOneTwo && P.lastPasser >= 0 && now - P.lastPassMs < 2200.0 && ci != P.lastPasser;
    const float poss = possShare_;

    for (int i = 0; i < n; i++) {
        PassCandidate& c = cand_[i];
        const bool pass = plusIsPass(c.type);
        float b = 0.f;

        if (cfg_.plusEpv && P.epvInit && (pass || c.type == ACT_CARRY)) {
            const int ti = pass ? c.targetId - 1 : ci;
            if (ti >= 0 && ti < TEAM_SIZE) {
                const PlayerState& r = state_.team[ti];
                const float tv = epvValue(epvRoleBin(ti), s * c.target.x, c.target.y, plusDensBin(oppDensityAt(c.target, 6.f)), s * r.vel.x > 1.5f ? 1 : 0);
                b += cfg_.wEpv * c.xP * ((tv - hereEpv) - c.deltaXT);
            }
        }
        if (pass && useSeq && seqQ[i] >= 0.f) b += cfg_.wSeq * (seqQ[i] - c.xP * seqHere);
        if (useTree && treeHave[i] && treeN > 1) b += cfg_.wTree * (treeQ[i] - treeMean);
        if (pass && cfg_.plusThrough && (c.type == ACT_THROUGH || c.type == ACT_LOFTED)) b += cfg_.wThrough * (P.mh[i] - 0.45f);
        if (pass && cfg_.plusMotif) b += cfg_.wMotif * plusMotifBonus(car, c);
        if (cfg_.plusIntent) b += cfg_.wIntent * plusIntentBonus(car, c);

        if (cfg_.plusStyle && pass) {
            if (c.deltaXT <= 0.02f && c.xP >= 0.75f) b += cfg_.wStyle * (th[0] - 0.5f) * 0.5f * c.xP;
            b += cfg_.wStyle * (th[1] - 0.5f) * 2.f * std::max(0.f, c.deltaXT) * c.xP * 3.f;
            b += cfg_.wStyle * (th[2] - 0.5f) * (std::fabs(c.target.y) - std::fabs(car.pos.y)) / 34.f * c.xP * 0.5f;
            if (c.xP < 0.7f) b += cfg_.wStyle * (th[3] - 0.5f) * (0.7f - c.xP);
        }

        // match phase
        if (mode == 1) {                                                    // early press: more shots, more forward play
            if (c.type == ACT_SHOOT) b += 0.10f * std::max(c.xP, 0.3f);
            else if (pass && c.deltaXT > 0.03f) b += 0.06f * c.xP;
        } else if (mode == 2) {                                             // protect the lead: keep the ball
            const float need = clampf((0.65f - poss) * 2.f, 0.f, 1.f);
            if (pass && c.xP >= 0.78f && c.deltaXT <= 0.02f) b += cfg_.wLoop * c.xP * (1.f + need);
            if (pass && c.xP < 0.65f) b -= cfg_.wLoop * (0.65f - c.xP) * 2.f;
            if (c.type == ACT_CARRY && state_.crowded) b -= 0.15f;
            if (c.type == ACT_SHOOT && c.length > 20.f) b -= 0.12f;
        } else if (mode == 3) {                                             // chase: go forward
            if (pass && c.deltaXT > 0.f) b += cfg_.wLoop * 0.8f * c.deltaXT * c.xP * 3.f;
            if (c.type == ACT_SHOOT) b += 0.08f;
        }

        // give-and-go: the passer has run on, give it straight back
        if (oneTwoOn && pass && c.targetId == P.lastPasser + 1 && c.offside < 0.5f && c.xP >= 0.55f) {
            const PlayerState& pr = state_.team[P.lastPasser];
            const float ahead = s * (pr.future.x - P.lastPassFrom.x);
            if (ahead >= 3.0f && s * pr.vel.x >= 1.5f) b += cfg_.wOneTwo * c.xP * (1.f + clampf(c.deltaXT * 4.f, 0.f, 1.f));
        }

        // final third: pass into space or shoot, no dribbling unless the way is clear
        if (finalThird && c.type == ACT_CARRY && altExists && nearestOppDistance(car.future) <= 7.f) b -= cfg_.wNoDribble;

        c.utility += clampf(b, -0.35f, 0.35f);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Interception race, defensive cover, rebound hunting
// ---------------------------------------------------------------------------------------------------------------------

// Time for a player to reach a point under an acceleration limit, starting from his current velocity.
float PitchEngine::plusTta(const PlayerState& p, const MetricVector2D& to, float vmax) const {
    const float a = 7.0f;
    const MetricVector2D d = to - p.pos;
    const float dl = d.length();
    const float D = std::max(0.f, dl - cfg_.defBody);
    if (D <= 0.f) return 0.f;
    const float v0 = dl > 1e-3f ? clampf(p.vel.dot(d) / dl, 0.f, vmax) : 0.f;
    const float tv = (vmax - v0) / a;
    const float da = 0.5f * (v0 + vmax) * tv;
    if (D <= da) return (-v0 + std::sqrt(v0 * v0 + 2.f * a * D)) / a;
    return tv + (D - da) / vmax;
}

// Along the predicted ball path, finds the earliest point one of our players reaches before every opponent.
// Returns false when no such race exists. *ctrlTta is the controlled player's time to the same point.
bool PitchEngine::plusRaceAssign(int* bestIdx, MetricVector2D* where, float* lead, float* ctrlTta) const {
    const int ci = controlledIdx();
    if (ci < 0 || !ballUsable()) return false;
    for (int step = 1; step <= 10; step++) {
        const float t = 0.2f * (float)step;
        MetricVector2D bp;
        bool out = false;
        if (!predictBall(t, &bp, nullptr, &out) || out) break;
        float tOpp = 1e3f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (o.active) tOpp = std::min(tOpp, plusTta(o, bp, 7.5f) + metaReactP80());
        }
        int bi = -1;
        float bt = 1e3f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& q = state_.team[i];
            if (!q.active || !q.visible || isKeeperIdx(i)) continue;
            const float tt = plusTta(q, bp, 7.5f);
            if (tt < bt) { bt = tt; bi = i; }
        }
        if (bi >= 0 && bt <= t - 0.05f && tOpp > bt + 0.15f) {
            *bestIdx = bi;
            *where = bp;
            *lead = tOpp - bt;
            *ctrlTta = plusTta(state_.team[ci], bp, 7.5f);
            return true;
        }
    }
    return false;
}

// Min-conflicts assignment of our outfield players to the dangerous opponents. Returns the point the controlled
// defender should take: the goal side of his assigned opponent, or his slot on the back line when nobody needs marking.
bool PitchEngine::plusCoverPoint(MetricVector2D* pt, bool* needsLine) {
    const int ci = controlledIdx();
    if (ci < 0 || isKeeperIdx(ci)) return false;
    const float own = -(float)state_.attackSign;
    const MetricVector2D ownGoal(own * PITCH_HALF_X, 0.f);

    // their corner: the nearest centre-back takes the near post of the six-yard box, a midfielder the far post, the rest the edge
    if (cfg_.plan && cfg_.planRestart && cfg_.meta && meta_.restartKind == 5 && ballUsable() &&
        std::fabs(state_.ball.x) > PITCH_HALF_X - 6.f && std::fabs(state_.ball.y) > PITCH_HALF_Y - 6.f) {
        const float by = state_.ball.y > 0.f ? 1.f : -1.f;
        const int rr = role_[ci];
        if (rr == ROLE_CB) { *pt = MetricVector2D(own * (PITCH_HALF_X - 3.5f), 3.f * by); *needsLine = false; return true; }
        if (rr == ROLE_DM || rr == ROLE_CM) { *pt = MetricVector2D(own * (PITCH_HALF_X - 4.5f), -3.f * by); *needsLine = false; return true; }
        *pt = MetricVector2D(own * (PITCH_HALF_X - 16.f), clampf(state_.team[ci].pos.y, -10.f, 10.f));
        *needsLine = false;
        return true;
    }

    int carr = -1;
    float cd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const float d = distance(o.pos, state_.ball);
        if (d < cd) { cd = d; carr = i; }
    }
    struct Dg { int idx; float threat; MetricVector2D cover; };
    Dg dg[4];
    int nd = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible || i == carr) continue;
        const float gd = distance(o.future, ownGoal);
        float th = std::exp(-gd / 24.f);
        if (cfg_.plan && cfg_.planFreeMan) th = std::max(th, 0.9f * plan_.danger[i]);
        if (th < 0.18f) continue;
        MetricVector2D toG = ownGoal - o.future;
        const float l = toG.length();
        if (l > 1e-3f) toG = toG * (1.f / l);
        Dg d{i, th, o.future + toG * 1.8f};
        int pos = nd < 4 ? nd : 3;
        if (nd == 4 && th <= dg[3].threat) continue;
        dg[pos] = d;
        if (nd < 4) nd++;
        for (int k = pos; k > 0 && dg[k].threat > dg[k - 1].threat; k--) std::swap(dg[k], dg[k - 1]);
    }
    int def[10];
    float dgl[10];
    int nDef = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& q = state_.team[i];
        if (!q.active || !q.visible || isKeeperIdx(i)) continue;
        def[nDef] = i;
        dgl[nDef++] = goalDistance(q.pos);
    }
    for (int a = 0; a < nDef; a++)
        for (int b = a + 1; b < nDef; b++)
            if (dgl[b] < dgl[a]) { std::swap(dgl[a], dgl[b]); std::swap(def[a], def[b]); }
    nDef = std::min(nDef, 7);
    int me = -1;
    for (int a = 0; a < nDef; a++) if (def[a] == ci) me = a;
    if (me < 0) return false;                       // the controlled player is not one of the defenders nearest our goal

    int asg[10];
    for (int a = 0; a < nDef; a++) asg[a] = -1;
    // objective: uncovered threats + distances + doubled cover
    auto cost = [&]() {
        float J = 0.f;
        for (int u = 0; u < nd; u++) {
            bool cov = false;
            for (int a = 0; a < nDef; a++) if (asg[a] == u) cov = true;
            if (!cov) J += 3.f * dg[u].threat;
        }
        for (int a = 0; a < nDef; a++) {
            if (asg[a] < 0) continue;
            const float d = distance(state_.team[def[a]].pos, dg[asg[a]].cover);
            J += d / 20.f + (d > 16.f ? 1.f : 0.f);
            for (int b = a + 1; b < nDef; b++) if (asg[b] == asg[a]) J += 0.8f * (1.f - 0.5f * dg[asg[a]].threat);
        }
        return J;
    };
    for (int u = 0; u < nd; u++) {                  // greedy start: nearest free defender takes each threat
        int bestA = -1;
        float bd = 1e9f;
        for (int a = 0; a < nDef; a++) {
            if (asg[a] >= 0) continue;
            const float d = distance(state_.team[def[a]].pos, dg[u].cover);
            if (d < bd) { bd = d; bestA = a; }
        }
        if (bestA >= 0) asg[bestA] = u;
    }
    uint32_t seed = 0xC0FFEEu ^ (uint32_t)(frameNo_ * 2654435761u);
    if (!seed) seed = 1u;
    float J = cost();
    for (int it = 0; it < 40; it++) {
        // pick a conflicted variable: a defender on a far or doubled assignment, else a random one
        const int a = (int)(plusRand(seed) * (float)nDef) % nDef;
        int bestV = asg[a];
        float bestJ = J;
        for (int v = -1; v < nd; v++) {
            if (v == asg[a]) continue;
            const int keep = asg[a];
            asg[a] = v;
            const float Jv = cost();
            asg[a] = keep;
            if (Jv < bestJ - 1e-4f) { bestJ = Jv; bestV = v; }
        }
        asg[me == a ? a : a] = bestV;
        J = bestJ;
    }
    // back-line slot for a defender with no mark: stay level with the line and keep the line connected
    float lineDepth = 0.f;
    int nl = 0;
    for (int a = 0; a < std::min(nDef, 4); a++) { lineDepth += dgl[a]; nl++; }
    lineDepth = nl > 0 ? lineDepth / (float)nl : 20.f;
    if (asg[me] >= 0) {
        *pt = dg[asg[me]].cover;
        *needsLine = false;
    } else {
        const float y = state_.team[ci].pos.y;
        float py = clampf(y, -PITCH_HALF_Y + 4.f, PITCH_HALF_Y - 4.f);
        float ld = lineDepth;
        if (cfg_.plan && cfg_.planShape) {
            if (role_[ci] == ROLE_FB && ballUsable() && std::fabs(state_.ball.y - y) > 25.f) py *= 0.75f;   // full-back tucks in when the ball is far side
            if (state_.timeMs < plan_.midBlockUntil) ld = std::max(8.f, ld - 4.f);                          // compact block after a failed press
        }
        *pt = MetricVector2D(own * (PITCH_HALF_X - ld), py);
        *needsLine = true;
    }
    return true;
}

// After our own shot the ball is often loose in the box: the controlled player goes for the rebound.
GestureOutput PitchEngine::plusReboundGesture() {
    GestureOutput none;
    none.status = ST_LOOSE;
    if (!cfg_.plus || !cfg_.plusRebound || !ballUsable() || !controlledSeen()) return none;
    const double now = state_.timeMs;
    const bool second = cfg_.plan && cfg_.planSecondBall;
    if (now - shotFiredMs_ > (second ? 1800.0 : 2600.0) || now < looseNextAt_) return none;
    const float s = (float)state_.attackSign;
    const bool nearGoal = s * state_.ball.x >= 28.f;
    if (!nearGoal && !second) return none;
    MetricVector2D tgt;
    int bi = -1;
    float lead = 0.f, cT = 0.f;
    const int ci = controlledIdx();
    if (plusRaceAssign(&bi, &tgt, &lead, &cT) && bi == ci) {
        // he wins the race to the second ball
    } else if (nearGoal) {
        tgt = MetricVector2D(s * (PITCH_HALF_X - 7.f), clampf(state_.ball.y * 0.5f, -5.f, 5.f));
    } else {                                                    // a clearance: where the ball will come down
        bool out = false;
        MetricVector2D bp;
        if (!predictBall(1.0f, &bp, nullptr, &out) || out) return none;
        tgt = bp;
        if (distance(ctrlPos_, tgt) > 22.f) return none;
    }
    if (distance(ctrlPos_, tgt) < 1.5f) return none;
    GestureOutput g = makeGesture(ACT_PRESS, ctrlPos_, tgt, 1.f, false);
    if (!g.valid) return none;
    g.status = ST_LOOSE;
    g.actionType = ACT_PRESS;
    g.durationMs = 200;
    g.dash = distance(ctrlPos_, tgt) > 3.f;
    looseNextAt_ = now + 200.0;
    return g;
}
