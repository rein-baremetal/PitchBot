// layer: roll-out and planned-sequence buffer, PolicyNet, opponent intent, shape morph, embeddings, pre-emptive stick
// alignment, sequence value net, meta controller and play styles, learned defence head, risk layer, timing residual, ball
// landing, survival and escape, recurrent opponent predictor, adversarial set pieces, kick-off lock, keeper shadow, stamina,
// tempo lock, dual-press detector, latency compensation, and the end-of-match training (EWC, replay, diversity, league, anomaly
// filter). Included at the end of pitch_engine.cpp after pitch_plan.inc (inside namespace pe).
// Everything here is radar-only and offline. Learned parts start neutral (zero output) and only gain influence with data.

namespace {
inline float v7Sig(float x) { return 1.f / (1.f + std::exp(-clampf(x, -30.f, 30.f))); }
inline uint32_t v7Mix(uint32_t a) { a ^= a >> 16; a *= 0x7FEB352Du; a ^= a >> 15; a *= 0x846CA68Bu; a ^= a >> 16; return a; }
inline float v7Hash01(uint32_t a) { return (float)(v7Mix(a) & 0xFFFFFFu) * (1.f / 16777216.f); }
// fixed pseudo-random recurrent weights (no storage): the readout is the only part that learns
inline float v7GruW(int layer, int i, int j) { return (v7Hash01((uint32_t)(layer * 7919 + i * 104729 + j * 1299709 + 17)) - 0.5f) * 1.3f; }
inline float v7Wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
}

void V7Mem::clear() {
    policy.init(0x7A3C1u); seqNet.init(0x51B7u); defNet.init(0x9E31u); metaNet.init(0x2C4Du);
    static const float escPrior[7] = {0.f, 0.9f, 1.4f, -0.3f, 0.2f, -1.2f, -0.4f};
    for (int i = 0; i < 7; i++) esc[i] = escPrior[i];
    for (int k = 0; k < 3; k++) for (int b = 0; b < 3; b++) for (int a = 0; a < 5; a++) { timingQ[k][b][a] = 0.f; timingN[k][b][a] = 0; }
    for (int k = 0; k < 6; k++) for (int r = 0; r < 3; r++) { spW[k][r] = 1.f; spN[k][r] = 0; }
    for (int i = 0; i < V7_EMB_N; i++) emb[i] = V7Embed();
    nEmb = 0; curSig = 0;
    replay.clear(); seqRep.clear(); escRep.clear(); defRep.clear();
    for (int c = 0; c < V7_CKPT; c++) for (int k = 0; k < V7Policy::NP; k++) ckpt[c][k] = 0.f;
    nCkpt = ckptHead = 0;
    for (int i = 0; i < V7_FPN; i++) for (int k = 0; k < V7_FP; k++) fp[i][k] = 0.f;
    nFp = fpHead = 0;
    matches = matchesRejected = trainedMatches = 0;
    report.clear();
}

// ---------------------------------------------------------------------------------------------------------------------
// Small helpers and features
// ---------------------------------------------------------------------------------------------------------------------

float PitchEngine::v7Phase() const {
    if (plus_.phase >= 0.f) return clampf(plus_.phase, 0.f, 1.f);
    return clampf((float)(plus_.liveMs / 1000.0 / std::max(cfg_.plusMatchSec, 60.f)), 0.f, 1.f);
}

float PitchEngine::v7EmbedVal(int d) const {
    if (!cfg_.v7Embed || !v7_.embLoaded || d < 0 || d >= V7_EMB_D) return 0.f;
    const float live = (plus_.im[d] - 0.5f) * 2.f;
    return clampf(0.7f * v7_.emb[d] + 0.3f * live, -1.f, 1.f);
}

void PitchEngine::v7LoadEmbed() {
    V7State& S = v7_;
    for (int d = 0; d < V7_EMB_D; d++) S.emb[d] = 0.f;
    for (int i = 0; i < v7m_.nEmb; i++)
        if (v7m_.emb[i].key == v7m_.curSig) { for (int d = 0; d < V7_EMB_D; d++) S.emb[d] = v7m_.emb[i].e[d]; break; }
    S.embLoaded = true;
}

void PitchEngine::v7StateFeat(const PlayerState& car, float* f) const {
    const float s = (float)state_.attackSign;
    const float pr = cfg_.plan ? planPressure(car.pos) : lanePressure(car.pos);
    f[0] = clampf(pr / 1.5f, 0.f, 1.f);
    f[1] = clampf(s * car.pos.x / PITCH_HALF_X, -1.f, 1.f);
    f[2] = clampf(std::fabs(car.pos.y) / PITCH_HALF_Y, 0.f, 1.f);
    f[3] = clampf(nearestOppDistance(car.pos) / 15.f, 0.f, 1.f);
    f[4] = clampf(plusLeadEst() / 2.f, -1.f, 1.f);
    f[5] = v7Phase();
    f[6] = clampf(car.vel5.length() / 8.f, 0.f, 1.f);
    f[7] = clampf(oppLineDepth_ / PITCH_HALF_X, 0.f, 1.f);
    for (int d = 0; d < V7_EMB_D; d++) f[8 + d] = v7EmbedVal(d);
    for (int i = 0; i < V7_NIN; i++) if (!std::isfinite(f[i])) f[i] = 0.f;
}

// 0 pass, 1 through or lofted, 2 carry, 3 shoot, 4 recycle (a pass that gains nothing or goes back)
int PitchEngine::v7ClassOf(const PassCandidate& c) {
    if (c.type == ACT_SHOOT) return 3;
    if (c.type == ACT_CARRY) return 2;
    if (c.type == ACT_THROUGH || c.type == ACT_LOFTED) return 1;
    return c.deltaXT <= 0.01f ? 4 : 0;
}

float PitchEngine::effLatencyMs() const {
    if (cfg_.v7 && cfg_.v7Latency && v7LatN_ >= 20 && finite(v7LatMs_)) return clampf(v7LatMs_, 20.f, 400.f);
    return cfg_.kalLatencyMs;
}

float PitchEngine::staminaOf(int i) const {
    if (i < 0 || i >= TEAM_SIZE) return 1.f;
    return clampf(1.f - v7_.hi[i] / std::max(cfg_.v7StaminaCap, 50.f), 0.f, 1.f);
}

void PitchEngine::setPlayStyle(int style, double lockMs) {
    V7State& S = v7_;
    const int st = style < 0 ? 0 : (style > 2 ? 2 : style);
    S.styleCur = st;
    for (int k = 0; k < 3; k++) S.swT[k] = k == st ? 1.f : 0.f;
    S.styleLockUntil = state_.timeMs + std::max(0.0, lockMs);
}

// ---------------------------------------------------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v7Frame(double dtSec) {
    V7State& S = v7_;
    const double now = state_.timeMs;
    S.frames++;
    if (!state_.ballSeen) S.noBall++;
    if (trackJumps_ > 0) S.jumpFrames++;
    if (S.lastFrameMs > 0.0) {
        const double d = now - S.lastFrameMs;
        if (d > 0.0 && d < 2000.0) { S.dtN++; const double dl = d - S.dtMean; S.dtMean += dl / (double)S.dtN; S.dtM2 += dl * (d - S.dtMean); }
    }
    S.lastFrameMs = now;
    if (!S.embLoaded) v7LoadEmbed();
    v7BallHistory();
    v7HistoryUpdate();
    if (cfg_.v7Stamina) v7StaminaUpdate(dtSec);

    // debounced possession edge: a turnover ends the planned sequence
    const int st = plus_.stableStatus;
    if (st != S.prevStable) {
        if (S.prevStable == ST_TEAM && st == ST_OPP) {
            v7SeqEnd(-1);
            if (S.tupPending && S.tupP.cls != 1 && S.tupP.cls != 0 && S.tupP.cls != 4) v7ResolveTup(-0.6f);
            S.nPlan = 0;
            S.spLost = true;
        }
        S.prevStable = st;
    }
    if (S.tupPending && (S.tupP.cls == 2) && now - S.tupT > 1500.0) v7ResolveTup(0.15f);

    v7RestartUpdate();
    if (cfg_.v7OppIntent && ((++S.oiCount) % 7) == 0) v7IntentUpdate();
    if (cfg_.v7Recurrent) v7RecurrentUpdate(dtSec);
    v7TempoUpdate();
    if (cfg_.v7Risk) v7RiskUpdate();
    v7StyleUpdate(dtSec);
    if (cfg_.v7Morph) v7MorphUpdate();
    if (cfg_.v7DualPress) v7DualUpdate();
    v7SurvivalUpdate();
    v7DefenceOutcome();
}

// ---- ball history: last six detections, quadratic fit, measured rolling friction, landing zone ----

static bool v7FitBall(const double* T, const MetricVector2D* P, int n, int head, MetricVector2D* p0, MetricVector2D* v0, MetricVector2D* a0,
                      float* rms, double* span) {
    if (n < 5) return false;
    double tt[6]; MetricVector2D pp[6];
    for (int i = 0; i < n; i++) { const int k = (head - n + i + 12) % 6; tt[i] = T[k]; pp[i] = P[k]; }
    const double tref = tt[n - 1];
    *span = tt[n - 1] - tt[0];
    if (*span < 250.0) return false;
    double A[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, bx[3] = {0, 0, 0}, by[3] = {0, 0, 0};
    for (int i = 0; i < n; i++) {
        const double u = (tt[i] - tref) * 0.001;
        const double ph[3] = {1.0, u, u * u};
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) A[r][c] += ph[r] * ph[c];
            bx[r] += ph[r] * pp[i].x; by[r] += ph[r] * pp[i].y;
        }
    }
    double M[3][7];
    for (int r = 0; r < 3; r++) { for (int c = 0; c < 3; c++) M[r][c] = A[r][c]; M[r][3] = bx[r]; M[r][4] = by[r]; }
    for (int c = 0; c < 3; c++) {
        int pv = c;
        for (int r = c + 1; r < 3; r++) if (std::fabs(M[r][c]) > std::fabs(M[pv][c])) pv = r;
        if (std::fabs(M[pv][c]) < 1e-9) return false;
        if (pv != c) for (int k = 0; k < 5; k++) std::swap(M[pv][k], M[c][k]);
        for (int r = 0; r < 3; r++) {
            if (r == c) continue;
            const double f = M[r][c] / M[c][c];
            for (int k = c; k < 5; k++) M[r][k] -= f * M[c][k];
        }
    }
    double cx[3], cy[3];
    for (int r = 0; r < 3; r++) { cx[r] = M[r][3] / M[r][r]; cy[r] = M[r][4] / M[r][r]; }
    p0->x = (float)cx[0]; p0->y = (float)cy[0];
    v0->x = (float)cx[1]; v0->y = (float)cy[1];
    a0->x = (float)(2.0 * cx[2]); a0->y = (float)(2.0 * cy[2]);
    double ss = 0;
    for (int i = 0; i < n; i++) {
        const double u = (tt[i] - tref) * 0.001;
        const double ex = cx[0] + cx[1] * u + cx[2] * u * u - pp[i].x, ey = cy[0] + cy[1] * u + cy[2] * u * u - pp[i].y;
        ss += ex * ex + ey * ey;
    }
    *rms = (float)std::sqrt(ss / n);
    return std::isfinite(*rms) && std::isfinite(p0->x) && std::isfinite(v0->x) && std::isfinite(a0->x);
}

void PitchEngine::v7BallHistory() {
    V7State& S = v7_;
    if (!state_.ballSeen || ballAssumed_) return;
    const double t = state_.timeMs;
    if (S.bhN > 0) { const int last = (S.bhHead + 5) % 6; if (t - S.bhT[last] < 40.0) return; if (t - S.bhT[last] > 700.0) S.bhN = 0; }
    S.bhT[S.bhHead] = t; S.bhP[S.bhHead] = state_.ball; S.bhHead = (S.bhHead + 1) % 6; if (S.bhN < 6) S.bhN++;
    MetricVector2D p0, v0, a0; float rms; double span;
    if (!v7FitBall(S.bhT, S.bhP, S.bhN, S.bhHead, &p0, &v0, &a0, &rms, &span)) return;
    const float sp = v0.length();
    if (state_.status == ST_TEAM || sp < 4.f || sp > 25.f || rms > 0.8f) return;      // only free, fast, clean flights teach the friction
    const float dec = -a0.dot(v0 * (1.f / sp));
    if (dec < 0.5f || dec > 12.f) return;
    S.fric += (clampf(dec, 1.f, 8.f) - S.fric) * 0.08f;
    S.fricN++;
}

bool PitchEngine::predictBallLanding(MetricVector2D* land, float* tLand) const {
    const V7State& S = v7_;
    if (!cfg_.v7 || !cfg_.v7Landing || S.bhN < 5) return false;
    if (state_.timeMs - S.bhT[(S.bhHead + 5) % 6] > 300.0) return false;
    MetricVector2D p0, v0, a0; float rms; double span;
    if (!v7FitBall(S.bhT, S.bhP, S.bhN, S.bhHead, &p0, &v0, &a0, &rms, &span) || rms > 1.5f) return false;
    const float sp = v0.length();
    if (sp < 2.5f || sp > 40.f) return false;
    const float d = S.fricN >= 5 ? S.fric : 3.0f;
    const float tl = clampf((sp - 1.5f) / d, 0.f, 3.0f);
    const float dist = sp * tl - 0.5f * d * tl * tl;
    MetricVector2D L = p0 + v0 * (dist / sp);
    L.x = clampf(L.x, -PITCH_HALF_X, PITCH_HALF_X); L.y = clampf(L.y, -PITCH_HALF_Y, PITCH_HALF_Y);
    if (land) *land = L;
    if (tLand) *tLand = tl;
    return true;
}

// time for a ball launched at v0 (m/s) to cover len metres with the measured friction
static float v7FlightTime(float len, float v0, float d) {
    v0 = std::max(v0, 1.f); d = std::max(d, 0.5f);
    const float disc = v0 * v0 - 2.f * d * len;
    if (disc <= 0.f) return std::min(3.f, v0 / d);
    return (v0 - std::sqrt(disc)) / d;
}

// ---- opponent position history (about 1.2 s) ----
void PitchEngine::v7HistoryUpdate() {
    V7State& S = v7_;
    const double t = state_.timeMs;
    if (S.hN > 0 && t - S.hT[(S.hHead + V7_HIST - 1) % V7_HIST] < 90.0) return;
    S.hT[S.hHead] = t;
    for (int i = 0; i < TEAM_SIZE; i++) S.hP[S.hHead][i] = state_.opp[i].active ? state_.opp[i].pos : MetricVector2D(1e4f, 1e4f);
    S.hHead = (S.hHead + 1) % V7_HIST;
    if (S.hN < V7_HIST) S.hN++;
}

// ---- stamina from radar movement ----
void PitchEngine::v7StaminaUpdate(double dtSec) {
    V7State& S = v7_;
    const float dt = (float)clampf((float)dtSec, 0.f, 0.3f);
    float sum = 0.f; int n = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& p = state_.team[i];
        if (!p.active || !p.visible) continue;
        const float sp = p.vel5.length();
        if (sp > 5.5f) S.hi[i] += sp * dt * (sp > 7.5f ? 1.4f : 1.f);
        else if (sp < 2.5f) S.hi[i] = std::max(0.f, S.hi[i] - 1.0f * dt);
        if (!isKeeperIdx(i)) { sum += staminaOf(i); n++; }
    }
    if (n > 0) S.stamAvg = sum / (float)n;
}

// ---- restarts: kick-off, corner, goal kick, throw-in, free kick (a stoppage followed by a stationary ball) ----
void PitchEngine::v7RestartUpdate() {
    V7State& S = v7_;
    const double now = state_.timeMs;
    if (restartUntilMs_ > S.lastRestartUntil + 1.0 && restartUntilMs_ > now) {
        S.lastRestartUntil = restartUntilMs_;
        S.restartPend = true; S.restartPendAt = now;
        if (S.nStop < 8) S.stopT[S.nStop++] = now; else { for (int i = 1; i < 8; i++) S.stopT[i - 1] = S.stopT[i]; S.stopT[7] = now; }
    }
    // a finished set-piece routine updates its mixing weights (EXP3)
    if (S.spRoutine >= 0 && (S.spStep >= 4 || now - S.spStart > 7000.0)) {
        const bool ok = S.spShot || (!S.spLost && state_.status == ST_TEAM);
        float& w = v7m_.spW[S.spKind][S.spRoutine];
        const float p = std::max(S.spProb[S.spRoutine], 0.05f);
        w = clampf(w * std::exp(0.25f * ((ok ? 1.f : 0.f) / p) / 3.f - 0.06f), 0.2f, 6.f);
        v7m_.spN[S.spKind][S.spRoutine]++;
        S.spRoutine = -1;
    }
    if (!S.restartPend) return;
    if (now - S.restartPendAt > 1200.0) { S.restartPend = false; return; }
    if (!ballUsable() || (state_.status != ST_TEAM && state_.status != ST_OPP)) return;
    S.restartPend = false;
    const float s = (float)state_.attackSign;
    const MetricVector2D b = state_.ball;
    const bool ours = state_.status == ST_TEAM;
    int kind;
    if (std::fabs(b.x) < 7.f && std::fabs(b.y) < 7.f) kind = 1;
    else if (std::fabs(b.x) > PITCH_HALF_X - 5.f && std::fabs(b.y) > PITCH_HALF_Y - 5.f) kind = 2;
    else if (std::fabs(b.x) > PITCH_HALF_X - 10.f && std::fabs(b.y) < 14.f) kind = 3;
    else if (std::fabs(b.y) > PITCH_HALF_Y - 2.f) kind = 4;
    else kind = 5;
    if (kind == 1 && cfg_.v7Kickoff) { S.koUntil = now + 1500.0; S.koOurs = ours; }
    if (!ours || !cfg_.v7SetPiece) return;
    // routine mix: EXP3 weights, nudged by what the opposition does, with 25 % uniform exploration
    float w[3] = {v7m_.spW[kind][0], v7m_.spW[kind][1], v7m_.spW[kind][2]};
    if (plus_.im[0] > 0.6f) w[2] *= 1.3f;       // a high press: the direct ball finds space behind it
    if (plus_.im[2] > 0.6f) w[1] *= 0.8f;       // a compact block: the far-post overload is crowded
    const float tot = w[0] + w[1] + w[2];
    const float gam = 0.25f;
    uint32_t sd = (uint32_t)(frameNo_ * 2654435761u) ^ 0x51ED270Bu;
    if (!sd) sd = 1u;
    const float u = plusRand(sd);
    int pick = 2;
    for (int r = 0; r < 3; r++) S.spProb[r] = (1.f - gam) * w[r] / std::max(tot, 1e-3f) + gam / 3.f;
    { float a2 = 0.f; for (int r = 0; r < 3; r++) { a2 += S.spProb[r]; if (u < a2) { pick = r; break; } } }
    S.spKind = kind; S.spRoutine = pick; S.spStep = 0; S.spStart = now; S.spOurs = true; S.spShot = false; S.spLost = false;
    (void)s;
}

// ---- opponent intent: build, progress, direct, switch, protect (every 6 to 8 frames) ----
void PitchEngine::v7IntentUpdate() {
    V7State& S = v7_;
    if (state_.status == ST_NOT_LIVE || !ballUsable()) return;
    const float a = -(float)state_.attackSign;                // the opposition attacks toward +a on the x axis
    float cvx = 0.f, bh = 0.f; int n = 0, behind = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        cvx += a * o.vel5.x; n++;
        if (a * o.pos.x < a * state_.ball.x) behind++;
    }
    if (n < 4) return;
    cvx /= (float)n; bh = (float)behind / (float)n;
    float fv = a * state_.ballVel.x, lv = std::fabs(state_.ballVel.y);
    float dp = clampf(a * state_.ball.x / PITCH_HALF_X, -1.f, 1.f);
    const float bs = state_.ballVel.length();
    if (state_.status == ST_OPP && oppCarrier_ >= 0 && oppCarrier_ < TEAM_SIZE) {
        const PlayerState& c = state_.opp[oppCarrier_];
        fv = a * c.vel5.x; lv = std::max(lv * 0.5f, std::fabs(c.vel5.y));
        dp = clampf(a * c.pos.x / PITCH_HALF_X, -1.f, 1.f);
    }
    const float fvn = clampf(fv / 6.f, -1.f, 1.f), bsn = clampf(bs / 18.f, 0.f, 1.f), lvn = clampf(lv / 10.f, 0.f, 1.f), cvn = clampf(cvx / 4.f, -1.f, 1.f);
    const float cp = clampf(state_.oppSpread / 30.f, 0.f, 1.f);
    const bool oppLeading = plus_.goalsAgainst > plus_.goalsFor && v7Phase() > 0.4f;
    float z[5];
    z[0] = 1.4f * (-dp) - 0.9f * fvn - 0.5f * bsn + 0.3f;
    z[1] = 1.3f * fvn + 1.0f * cvn + 0.2f;
    z[2] = 2.4f * bsn + 0.6f * fvn - 0.6f;
    z[3] = 2.2f * lvn + 0.7f * bsn - 0.5f;
    z[4] = 1.8f * bh - 0.8f * fvn - 0.8f * bsn + (oppLeading ? 0.5f : 0.f) + 0.3f * (1.f - cp) - 0.2f;
    float mx = z[0]; for (int k = 1; k < 5; k++) mx = std::max(mx, z[k]);
    float sum = 0.f, p[5];
    for (int k = 0; k < 5; k++) { p[k] = std::exp(z[k] - mx); sum += p[k]; }
    for (int k = 0; k < 5; k++) S.oi[k] = 0.6f * S.oi[k] + 0.4f * (p[k] / sum);
    int top = 0; for (int k = 1; k < 5; k++) if (S.oi[k] > S.oi[top]) top = k;
    if (top != S.oiTop) {
        S.oiTop = top;
        if (S.nFlip < 10) S.flipT[S.nFlip++] = state_.timeMs; else { for (int i = 1; i < 10; i++) S.flipT[i - 1] = S.flipT[i]; S.flipT[9] = state_.timeMs; }
    }
}

// ---- tempo lock: erratic opposition (many stoppages or a jittery intent) forces patient recycling for a fixed window ----
void PitchEngine::v7TempoUpdate() {
    V7State& S = v7_;
    if (!cfg_.v7TempoLock) { S.tlUntil = -1e9; return; }
    const double now = state_.timeMs;
    if (now < S.tlUntil + 10000.0) return;                      // active, or inside the cool-down
    int stops = 0, flips = 0;
    for (int i = 0; i < S.nStop; i++) if (now - S.stopT[i] < 150000.0) stops++;
    for (int i = 0; i < S.nFlip; i++) if (now - S.flipT[i] < 60000.0) flips++;
    if (stops >= 4 || flips >= 7) S.tlUntil = now + 20000.0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Risk layer (score and clock), play styles, meta controller
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v7RiskUpdate() {
    V7State& S = v7_;
    const int diff = plus_.goalsFor - plus_.goalsAgainst;
    const float ph = v7Phase();
    const int minute = (int)(ph * 90.f);
    if (diff == S.lastDiff && minute == S.lastMin) return;
    S.lastDiff = diff; S.lastMin = minute;
    const float late = clampf((ph - 0.55f) / 0.45f, 0.f, 1.f);
    const float lead = (float)std::min(std::abs(diff), 2);
    if (diff > 0) {                      // protect: stricter pass floor, fewer through balls and long shots
        S.rMinAdd = std::min(0.08f, 0.03f * lead * (0.5f + late));
        S.rThrMul = 1.f - 0.25f * lead * (0.5f + late) * 0.5f;
        S.rShotMul = 1.f - 0.20f * lead * (0.5f + late) * 0.5f;
    } else if (diff < 0) {               // chase: lower floor, more through balls and shots
        S.rMinAdd = -std::min(0.08f, 0.04f * lead * (0.4f + late));
        S.rThrMul = 1.f + 0.30f * lead * (0.4f + late) * 0.5f;
        S.rShotMul = 1.f + 0.25f * lead * (0.4f + late) * 0.5f;
    } else {
        S.rMinAdd = 0.01f * late; S.rThrMul = 1.f; S.rShotMul = 1.f;
    }
}

void PitchEngine::v7MetaWindow() {
    V7State& S = v7_;
    const double now = state_.timeMs;
    const float lead = clampf(plusLeadEst() / 2.f, -1.f, 1.f);
    float x[8];
    x[0] = lead; x[1] = v7Phase(); x[2] = possShare_; x[3] = clampf(onlineKept_, 0.f, 1.f);
    x[4] = clampf((float)turnoverCount_ / std::max(1.f, (float)(plus_.liveMs / 60000.0)) / 6.f, 0.f, 1.f);
    x[5] = plus_.im[0]; x[6] = plus_.im[3];
    x[7] = clampf(planPressure(state_.carrier >= 0 && state_.carrier < TEAM_SIZE ? state_.team[state_.carrier].pos : state_.ball) / 1.5f, 0.f, 1.f);
    // resolve the previous window
    if (S.metaStyle >= 0 && cfg_.v7MetaNet && cfg_.v7Learn) {
        const int dk = S.passKept - S.metaKept0, dl = S.passLost - S.metaLost0;
        const float poss = S.metaPossN > 0 ? (float)(S.metaPossSum / S.metaPossN) : 0.5f;
        const int dg = (plus_.goalsFor - plus_.goalsAgainst) - S.metaGoals0;
        float R = 0.5f * (float)(dk - dl) / (float)std::max(4, dk + dl) + 1.2f * (float)dg + 0.6f * (poss - 0.5f);
        R = clampf(R, -2.f, 2.f);
        V7Mlp<8, 8, 5>& N = v7m_.metaNet;
        float out[5];
        N.forward(S.metaX, out);
        const float base = out[S.metaStyle];
        N.step(S.metaX, S.metaStyle, R, 0.08f, 0.f, nullptr);
        const float adv = R - base;
        for (int k = 0; k < 2; k++) N.step(S.metaX, 3 + k, out[3 + k] + 0.5f * adv * S.metaRisk[k], 0.05f, 0.f, nullptr);
    }
    for (int i = 0; i < 8; i++) S.metaX[i] = x[i];
    // choose the next style and risk offsets
    float out[5];
    v7m_.metaNet.forward(x, out);
    const float conf = clampf((float)v7m_.metaNet.trained / 300.f, 0.f, 1.f);
    uint32_t sd = (uint32_t)(frameNo_ * 2246822519u) ^ 0xA5F152B3u;
    if (!sd) sd = 1u;
    int best = 0; float bv = -1e9f;
    for (int k = 0; k < 3; k++) {
        const float v = out[k] + (0.25f * (1.f - conf) + 0.04f) * (plusRand(sd) - 0.5f) * 2.f + (k == S.styleCur ? 0.03f : 0.f);
        if (v > bv) { bv = v; best = k; }
    }
    for (int k = 0; k < 2; k++) S.metaRisk[k] = (plusRand(sd) - 0.5f) * 0.6f * (1.f - 0.5f * conf);
    S.metaMinAdd = 0.05f * std::tanh(out[3] + S.metaRisk[0]);
    S.metaShotMul = 1.f + 0.25f * std::tanh(out[4] + S.metaRisk[1]);
    if (now >= S.styleLockUntil) {
        S.styleCur = best;
        for (int k = 0; k < 3; k++) S.swT[k] = k == best ? 1.f : 0.f;
    }
    S.metaStyle = best;
    S.metaWinStart = now; S.metaKept0 = S.passKept; S.metaLost0 = S.passLost;
    S.metaGoals0 = plus_.goalsFor - plus_.goalsAgainst; S.metaPossSum = 0; S.metaPossN = 0;
}

void PitchEngine::v7StyleUpdate(double dtSec) {
    V7State& S = v7_;
    if (!cfg_.v7Style) { S.sw[0] = 1.f; S.sw[1] = S.sw[2] = 0.f; return; }
    const double now = state_.timeMs;
    S.metaPossSum += possShare_; S.metaPossN++;
    const int diff = plus_.goalsFor - plus_.goalsAgainst;
    const bool scoreChanged = S.lastDiff != -99 && diff != S.metaGoals0 && S.metaStyle >= 0;
    if (S.metaWinStart < 0.0) { S.metaWinStart = now; S.styleNextAt = now + 30000.0; if (cfg_.v7MetaNet) v7MetaWindow(); }
    if (now >= S.styleNextAt || (scoreChanged && now - S.metaWinStart > 8000.0)) {
        uint32_t sd = (uint32_t)(frameNo_ * 2654435761u) ^ 0x6C8E9CF5u;
        if (!sd) sd = 1u;
        S.styleNextAt = now + 30000.0 + 15000.0 * plusRand(sd);
        if (cfg_.v7MetaNet) v7MetaWindow();
        else if (now >= S.styleLockUntil) {          // rule fallback: protect with Control, chase with Vertical
            const int st = diff > 0 && v7Phase() > 0.5f ? PS_CONTROL : (diff < 0 ? PS_VERTICAL : PS_CONTROL);
            S.styleCur = st;
            for (int k = 0; k < 3; k++) S.swT[k] = k == st ? 1.f : 0.f;
        }
    }
    const float r = clampf((float)(dtSec / 25.0), 0.f, 1.f);     // blend over about 25 s
    for (int k = 0; k < 3; k++) S.sw[k] += (S.swT[k] - S.sw[k]) * r;
}

float PitchEngine::v7TempScale() const {
    if (!cfg_.v7 || !cfg_.v7Style) return 1.f;
    const V7State& S = v7_;
    return clampf(1.f + 0.35f * S.sw[2] + 0.05f * S.sw[1], 0.8f, 1.4f);   // Control is the unchanged baseline
}

// ---------------------------------------------------------------------------------------------------------------------
// Shape morph, dual press
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v7MorphUpdate() {
    V7State& S = v7_;
    const double now = state_.timeMs;
    if (state_.status == ST_NOT_LIVE || !ballUsable()) { S.morphStr = 0.f; return; }
    if (now < S.morphUntil - 1000.0) return;                     // refresh every 500 ms
    const float s = (float)state_.attackSign;
    const MetricVector2D B = state_.ball;
    int nT = 0, nO = 0, tIdx[TEAM_SIZE], nFar = 0;
    float farD[TEAM_SIZE];
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& p = state_.team[i];
        if (!p.active || !p.visible || isKeeperIdx(i)) continue;
        const float d = distance(p.pos, B);
        if (d < 16.f) nT++; else if (d < 42.f) { tIdx[nFar] = i; farD[nFar] = d; nFar++; }
    }
    for (int i = 0; i < TEAM_SIZE; i++) { const PlayerState& o = state_.opp[i]; if (o.active && o.visible && distance(o.pos, B) < 16.f) nO++; }
    const int diff = nT - nO;
    S.morphStr = 0.f; S.morphIdx[0] = S.morphIdx[1] = -1;
    if (state_.status == ST_TEAM && diff >= 2 && nFar > 0) {
        // overload: two runners beyond the ball line, into the side with fewer opponents
        int up = 0, down = 0;
        for (int i = 0; i < TEAM_SIZE; i++) { const PlayerState& o = state_.opp[i]; if (!o.active) continue; if (o.pos.y > B.y && distance(o.pos, B) < 24.f) up++; else if (o.pos.y <= B.y && distance(o.pos, B) < 24.f) down++; }
        const float side = up <= down ? 1.f : -1.f;
        for (int k = 0; k < 2 && nFar > 0; k++) {
            int bi = 0;
            for (int a = 1; a < nFar; a++) if (farD[a] < farD[bi]) bi = a;
            S.morphIdx[k] = tIdx[bi];
            S.morphTo[k] = MetricVector2D(clampf(B.x + s * (k == 0 ? 10.f : 14.f), -PITCH_HALF_X + 3.f, PITCH_HALF_X - 3.f),
                                          clampf(B.y + side * (k == 0 ? 8.f : -6.f), -PITCH_HALF_Y + 2.f, PITCH_HALF_Y - 2.f));
            tIdx[bi] = tIdx[nFar - 1]; farD[bi] = farD[nFar - 1]; nFar--;
        }
        S.morphStr = 1.f; S.morphUntil = now + 1500.0;
    } else if (diff <= -2 && nFar > 0) {
        // underload: two players drop to the ball side, behind the ball, so there are passing options and cover
        for (int k = 0; k < 2 && nFar > 0; k++) {
            int bi = 0;
            for (int a = 1; a < nFar; a++) if (farD[a] < farD[bi]) bi = a;
            S.morphIdx[k] = tIdx[bi];
            S.morphTo[k] = MetricVector2D(clampf(B.x - s * 7.f, -PITCH_HALF_X + 3.f, PITCH_HALF_X - 3.f),
                                          clampf(B.y + (k == 0 ? 5.f : -5.f), -PITCH_HALF_Y + 2.f, PITCH_HALF_Y - 2.f));
            tIdx[bi] = tIdx[nFar - 1]; farD[bi] = farD[nFar - 1]; nFar--;
        }
        S.morphStr = 1.f; S.morphUntil = now + 1500.0;
    }
}

void PitchEngine::v7DualUpdate() {
    V7State& S = v7_;
    const double now = state_.timeMs;
    if (state_.status != ST_TEAM || state_.carrier < 0 || state_.carrier >= TEAM_SIZE || S.hN < 4) { if (now - S.dualAt > 800.0) S.dual = false; return; }
    const PlayerState& car = state_.team[state_.carrier];
    int ids[TEAM_SIZE], n = 0;
    MetricVector2D bear[TEAM_SIZE];
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) continue;
        const MetricVector2D v = o.pos - car.pos;
        const float d = v.length();
        if (d > 8.5f || d < 0.3f) continue;
        const MetricVector2D u = v * (1.f / d);
        const float closing = -o.vel5.dot(u);                    // positive when he runs toward the carrier
        if (closing < 0.8f) continue;
        ids[n] = i; bear[n] = u; n++;
        if (n >= TEAM_SIZE) break;
    }
    bool found = false;
    if (n >= 2) {
        // snapshot about 0.8 s ago
        int hi = -1; double bestDt = 1e9;
        for (int k = 0; k < S.hN; k++) {
            const int idx = (S.hHead - 1 - k + 2 * V7_HIST) % V7_HIST;
            const double dt = std::fabs((now - S.hT[idx]) - 800.0);
            if (dt < bestDt) { bestDt = dt; hi = idx; }
        }
        if (hi >= 0 && bestDt < 250.0) {
            float bestMove = 2.0f; int bj = -1;
            for (int a = 0; a < n && !found; a++)
                for (int b = a + 1; b < n; b++)
                    if (bear[a].dot(bear[b]) < 0.82f) {                  // the two approach from more than about 35 degrees apart
                        for (int q = 0; q < 2; q++) {
                            const int oi = q == 0 ? ids[a] : ids[b];
                            const MetricVector2D old = S.hP[hi][oi];
                            if (old.x > 1e3f) continue;
                            const float moved = distance(old, car.pos) - distance(state_.opp[oi].pos, car.pos);
                            if (moved > bestMove) { bestMove = moved; bj = oi; }
                        }
                    }
            if (bj >= 0) { S.vacated = S.hP[hi][bj]; found = true; }
        }
    }
    if (found) { S.dual = true; S.dualAt = now; }
    else if (now - S.dualAt > 800.0) S.dual = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Survival and escape
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v7SurvivalUpdate() {
    V7State& S = v7_;
    const double now = state_.timeMs;
    if (!cfg_.v7Survival || human_) { S.surv = false; return; }
    if (S.surv && now > S.survUntil) { S.surv = false; S.survLock = now + 1500.0; }
    // escape outcome: three seconds later, did we keep the ball and is the pressure lower?
    if (S.escPend) {
        const bool lost = plus_.stableStatus == ST_OPP;
        if (lost || now - S.escT >= 3000.0) {
            float y = 0.f;
            if (!lost && state_.status == ST_TEAM && state_.carrier >= 0 && state_.carrier < TEAM_SIZE) {
                const float pr = cfg_.plan ? planPressure(state_.team[state_.carrier].pos) : lanePressure(state_.team[state_.carrier].pos);
                if (pr < S.escP0 - 0.3f) y = 1.f;
            }
            if (S.escS.size() < 400) { V7EscSample e; for (int k = 0; k < 7; k++) e.f[k] = S.escF[k]; e.y = y; S.escS.push_back(e); }
            S.escPend = false;
        }
    }
    if (state_.status != ST_TEAM || state_.carrier < 0 || state_.carrier >= TEAM_SIZE) { if (S.surv && state_.status == ST_OPP) S.surv = false; return; }
    const PlayerState& car = state_.team[state_.carrier];
    const float pr = cfg_.plan ? planPressure(car.pos) : 0.5f * localPressure(car.pos);
    int n6 = 0;
    for (int i = 0; i < TEAM_SIZE; i++) { const PlayerState& o = state_.opp[i]; if (o.active && distance(o.future, car.pos) < 6.f) n6++; }
    const bool trigger = pr > cfg_.v7SurvPress || (n6 >= 3 && pr > 1.0f);
    if (!S.surv && trigger && now > S.survLock && !inOwnBox(car.pos)) {
        uint32_t sd = (uint32_t)(frameNo_ * 2654435761u) ^ 0x2F6B9D31u;
        if (!sd) sd = 1u;
        S.surv = true;
        S.survUntil = now + 4000.0 + 2000.0 * plusRand(sd);
    }
}

bool PitchEngine::v7SurvivalPick(int* pick, const PlayerState& car) {
    V7State& S = v7_;
    if (!cfg_.v7Survival || !S.surv || human_ || !pick) return false;
    MetricVector2D C;
    float wsum = 0.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        const float d = distance(o.future, car.pos);
        if (d < 10.f) { const float w = 1.f / (d + 0.5f); C = C + o.future * w; wsum += w; }
    }
    if (wsum <= 0.f) return false;
    C = C * (1.f / wsum);
    MetricVector2D away = car.pos - C;
    const float al = away.length();
    if (al < 0.3f) return false;
    away = away * (1.f / al);
    MetricVector2D T; int nt = 0;
    for (int i = 0; i < TEAM_SIZE; i++) if (state_.team[i].active) { T = T + state_.team[i].pos; nt++; }
    if (nt > 0) T = T * (1.f / (float)nt);
    const float s = (float)state_.attackSign;
    int best = -1; float bs = -1e9f; float bf[7] = {0};
    for (int i = 0; i < nCand_; i++) {
        const PassCandidate& c = cand_[i];
        if (c.type == ACT_SHOOT) continue;                      // shots keep their own rules
        const MetricVector2D dir = c.target - car.pos;
        const float dl = dir.length();
        if (dl < 1e-3f) continue;
        const float esc = dir.dot(away) / dl;
        const bool isPass = plusIsPass(c.type);
        if (isPass && (c.xP < 0.5f || c.offside > 0.5f)) continue;
        const float tgtPr = clampf((cfg_.plan ? planPressure(c.target) : lanePressure(c.target)) / 1.5f, 0.f, 1.f);
        float f[7] = {1.f, esc, isPass ? c.xP : 0.5f, dl / 40.f, clampf(s * (c.target.x - car.pos.x) / 30.f, -1.f, 1.f), tgtPr, isPass ? 0.f : 1.f};
        float head = 0.f;
        for (int k = 0; k < 7; k++) head += v7m_.esc[k] * f[k];
        const float h = v7Sig(head);
        float score;
        if (isPass) {
            const float shape = clampf(1.f - distance(c.target, T) / 45.f, 0.f, 1.f);
            score = 0.55f * c.xP + 0.35f * esc + 0.10f * shape - 0.25f * tgtPr + (cfg_.v7Escape ? 0.30f * (h - 0.5f) * 2.f : 0.f);
        } else {
            if (esc < 0.2f) continue;                          // a carry that does not lead away is no escape
            score = 0.40f * esc + 0.05f - 0.30f * clampf((cfg_.plan ? planPressure(car.pos + away * 3.f) : 0.5f) / 1.5f, 0.f, 1.f);
        }
        if (score > bs) { bs = score; best = i; for (int k = 0; k < 7; k++) bf[k] = f[k]; }
    }
    if (best < 0) return false;
    *pick = best;
    if (!S.escPend) {
        S.escPend = true; S.escT = state_.timeMs; S.escP0 = cfg_.plan ? planPressure(car.pos) : lanePressure(car.pos);
        for (int k = 0; k < 7; k++) S.escF[k] = bf[k];
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Learned defence head: choose tight / normal / passive for the current defensive episode, learn from the result
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v7DefModeChoose(float dC) {
    V7State& S = v7_;
    const float s = (float)state_.attackSign;
    int nT = 0, nO = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (state_.team[i].active && distance(state_.team[i].pos, state_.ball) < 14.f) nT++;
        if (state_.opp[i].active && distance(state_.opp[i].pos, state_.ball) < 14.f) nO++;
    }
    float csp = 0.f;
    if (oppCarrier_ >= 0 && oppCarrier_ < TEAM_SIZE) csp = state_.opp[oppCarrier_].vel5.length();
    (void)s;
    S.defX[0] = clampf(dC / 20.f, 0.f, 1.f);
    S.defX[1] = clampf(csp / 8.f, 0.f, 1.f);
    S.defX[2] = clampf(goalDistance(state_.ball) / PITCH_HALF_X, 0.f, 2.f) * 0.5f;
    S.defX[3] = clampf((float)(nT - nO) / 4.f, -1.f, 1.f);
    S.defX[4] = clampf(plusLeadEst() / 2.f, -1.f, 1.f);
    S.defX[5] = S.stamAvg;
    float out[3];
    v7m_.defNet.forward(S.defX, out);
    out[0] += 0.02f;                                             // ties go to the normal mode
    uint32_t sd = (uint32_t)(frameNo_ * 2654435761u) ^ 0x1B873593u;
    if (!sd) sd = 1u;
    int m = 0;
    for (int k = 1; k < 3; k++) if (out[k] > out[m]) m = k;
    if (plusRand(sd) < 0.10f) m = (int)(plusRand(sd) * 3.f) % 3;
    S.defMode = m;
}

float PitchEngine::v7DefBufMul() const {
    if (!cfg_.v7 || !cfg_.v7DefHead) return 1.f;
    return v7_.defMode == 1 ? 0.75f : (v7_.defMode == 2 ? 1.3f : 1.f);
}

void PitchEngine::v7DefenceOutcome() {
    V7State& S = v7_;
    if (!cfg_.v7DefHead || human_) return;
    const double now = state_.timeMs;
    auto resolve = [&](float r) {
        if (!S.defPend) return;
        if (cfg_.v7Learn) {
            v7m_.defNet.step(S.defX, S.defMode, r, 0.05f, 0.f, nullptr);
            V7Tup t; for (int k = 0; k < 6; k++) t.f[k] = S.defX[k];
            t.cls = S.defMode; t.r = r;
            if (S.defS.size() < 300) S.defS.push_back(t);
        }
        S.defPend = false;
    };
    const int st = plus_.stableStatus;
    if (st == ST_OPP && state_.status != ST_NOT_LIVE && ballUsable()) {
        float dC = 20.f;
        for (int i = 0; i < TEAM_SIZE; i++) if (state_.team[i].active) dC = std::min(dC, distance(state_.team[i].pos, state_.ball));
        if (controlledSeen()) dC = distance(ctrlPos_, state_.ball);
        if (S.defStart < 0.0) { S.defStart = now; S.defAt = now; S.defGa0 = plus_.goalsAgainst; v7DefModeChoose(dC); S.defPend = true; }
        else if (now - S.defAt > 6000.0) { resolve(-0.05f); S.defAt = now; S.defGa0 = plus_.goalsAgainst; v7DefModeChoose(dC); S.defPend = true; }
        if (S.defPend && plus_.goalsAgainst > S.defGa0) resolve(-2.f);
        else if (S.defPend && goalDistance(state_.ball) < 20.f && std::fabs(state_.ball.y) < 24.f) resolve(-0.6f);
    } else {
        if (S.defPend && st == ST_TEAM) resolve(now - S.defAt < 2500.0 ? 1.0f : 0.5f);
        else if (S.defPend && st != ST_OPP) resolve(0.f);
        S.defStart = -1e9;
        if (st != ST_OPP) S.defMode = 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Recurrent opponent predictor: a GRU with fixed random gates; only the linear readout is trained (online, normalised LMS).
// Predicts the velocity change over the next 0.35 s and the probability of a stop-fake or sharp cut.
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v7RecurrentUpdate(double dtSec) {
    V7State& S = v7_;
    const double now = state_.timeMs;
    const float dt = (float)clampf((float)dtSec, 0.02f, 0.3f);
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || !o.visible) { S.gprevOk[i] = false; continue; }
        const MetricVector2D v = o.vel5;
        const float sp = v.length();
        const float ang = std::atan2(v.y, v.x);
        float u[4];
        u[0] = clampf(v.x / 8.f, -1.f, 1.f); u[1] = clampf(v.y / 8.f, -1.f, 1.f);
        u[2] = S.gprevOk[i] ? clampf((sp - S.gprevSp[i]) / dt / 6.f, -1.f, 1.f) : 0.f;
        u[3] = (S.gprevOk[i] && sp > 1.5f && S.gprevSp[i] > 1.5f) ? clampf(v7Wrap(ang - S.gprevAng[i]) / 3.14159f, -1.f, 1.f) : 0.f;
        S.gprevSp[i] = sp; S.gprevAng[i] = ang; S.gprevOk[i] = true;
        float* h = S.gh[i];
        float z[6], r[6], nn[6], hn[6];
        for (int j = 0; j < 6; j++) {
            float az = 0.f, ar = 0.f;
            for (int k = 0; k < 4; k++) { az += v7GruW(0, j, k) * u[k]; ar += v7GruW(1, j, k) * u[k]; }
            for (int k = 0; k < 6; k++) { az += v7GruW(0, j, 4 + k) * h[k]; ar += v7GruW(1, j, 4 + k) * h[k]; }
            z[j] = v7Sig(az); r[j] = v7Sig(ar);
        }
        for (int j = 0; j < 6; j++) {
            float a = 0.f;
            for (int k = 0; k < 4; k++) a += v7GruW(2, j, k) * u[k];
            for (int k = 0; k < 6; k++) a += v7GruW(2, j, 4 + k) * r[k] * h[k];
            nn[j] = std::tanh(a);
            hn[j] = (1.f - z[j]) * h[j] + z[j] * nn[j];
        }
        for (int j = 0; j < 6; j++) h[j] = hn[j];
        float phi[7];
        for (int j = 0; j < 6; j++) phi[j] = h[j];
        phi[6] = 1.f;
        float o3[3] = {0.f, 0.f, 0.f};
        for (int q = 0; q < 3; q++) for (int k = 0; k < 7; k++) o3[q] += S.gro[q][k] * phi[k];
        S.predDv[i] = MetricVector2D(clampf(o3[0], -1.f, 1.f) * 8.f, clampf(o3[1], -1.f, 1.f) * 8.f);
        S.pSharp[i] = v7Sig(o3[2]);
        // delayed supervision: velocity 0.3 s after the stored feature vector
        int slot = S.gphiN[i] % 5;
        S.gphiN[i]++;
        for (int k = 0; k < 7; k++) S.gphi[i][slot][k] = phi[k];
        S.gphiT[i][slot] = now; S.gphiV[i][slot] = v;
        for (int a = 0; a < 5; a++) {
            if (a == slot || S.gphiT[i][a] <= 0.0) continue;
            const double age = now - S.gphiT[i][a];
            if (age < 300.0 || age > 700.0) continue;
            const MetricVector2D dv = v - S.gphiV[i][a];
            const float tx = clampf(dv.x / 8.f, -1.f, 1.f), ty = clampf(dv.y / 8.f, -1.f, 1.f);
            const float ys = dv.length() > 3.0f ? 1.f : 0.f;
            float nrm = 1.f;
            for (int k = 0; k < 7; k++) nrm += S.gphi[i][a][k] * S.gphi[i][a][k];
            float p0 = 0.f, p1 = 0.f, p2 = 0.f;
            for (int k = 0; k < 7; k++) { p0 += S.gro[0][k] * S.gphi[i][a][k]; p1 += S.gro[1][k] * S.gphi[i][a][k]; p2 += S.gro[2][k] * S.gphi[i][a][k]; }
            const float e0 = tx - p0, e1 = ty - p1, e2 = ys - v7Sig(p2);
            for (int k = 0; k < 7; k++) {
                const float ph = S.gphi[i][a][k] / nrm;
                S.gro[0][k] += 0.6f * e0 * ph; S.gro[1][k] += 0.6f * e1 * ph; S.gro[2][k] += 1.0f * e2 * ph;
            }
            S.gphiT[i][a] = 0.0;                                 // used once
            S.gSeen++;
        }
    }
}

MetricVector2D PitchEngine::v7PredCarrier(int oppIdx) const {
    if (oppIdx < 0 || oppIdx >= TEAM_SIZE) return MetricVector2D();
    const PlayerState& o = state_.opp[oppIdx];
    return o.pos + (o.vel5 + v7_.predDv[oppIdx]) * 0.35f;
}

// ---------------------------------------------------------------------------------------------------------------------
// Experience bookkeeping
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v7ResolveTup(float r) {
    V7State& S = v7_;
    if (!S.tupPending) return;
    S.tupPending = false;
    if (!cfg_.v7Learn) return;
    S.tupP.r = r;
    if ((int)S.tup.size() < V7_TUP_CAP) S.tup.push_back(S.tupP);
    else { uint32_t sd = (uint32_t)(frameNo_ * 2654435761u) | 1u; S.tup[(size_t)(plusRand(sd) * (float)V7_TUP_CAP) % S.tup.size()] = S.tupP; }
}

void PitchEngine::v7SeqFeat(const PlayerState& car, const PassCandidate* c, float* f) const {
    const V7State& S = v7_;
    const float s = (float)state_.attackSign;
    V7Hop h[4]; int n = 0;
    for (int i = 0; i < S.nHops && i < 3; i++) h[n++] = S.hops[i];
    if (c) {
        V7Hop x; const MetricVector2D d = c->target - car.pos; const float l = d.length();
        x.len = c->length; x.fwd = s * d.x; x.cosA = l > 1e-3f ? s * d.x / l : 0.f;
        h[n++] = x;
    }
    f[0] = clampf((float)n / 3.f, 0.f, 1.f);
    for (int k = 0; k < 2; k++) {
        const int idx = n - 2 + k;
        const bool ok = idx >= 0 && idx < n;
        f[1 + k * 3] = ok ? clampf(h[idx].len / 40.f, 0.f, 1.f) : 0.f;
        f[2 + k * 3] = ok ? clampf(h[idx].fwd / 30.f, -1.f, 1.f) : 0.f;
        f[3 + k * 3] = ok ? clampf(h[idx].cosA, -1.f, 1.f) : 0.f;
    }
    f[7] = c ? clampf(s * c->target.x / PITCH_HALF_X, -1.f, 1.f) : clampf(s * car.pos.x / PITCH_HALF_X, -1.f, 1.f);
}

void PitchEngine::v7SeqEnd(int outcome) {
    V7State& S = v7_;
    if (S.nHops >= 1 && outcome != 0 && cfg_.v7Learn && state_.carrier >= -1) {
        // one sample per prefix of the sequence, all labelled with how it ended
        V7Hop keep[3]; const int n = S.nHops;
        for (int i = 0; i < n; i++) keep[i] = S.hops[i];
        for (int k = 1; k <= n && S.seqS.size() < (size_t)V7_SEQ_CAP; k++) {
            V7SeqSample q;
            q.f[0] = clampf((float)k / 3.f, 0.f, 1.f);
            for (int m = 0; m < 2; m++) {
                const int idx = k - 2 + m;
                const bool ok = idx >= 0;
                q.f[1 + m * 3] = ok ? clampf(keep[idx].len / 40.f, 0.f, 1.f) : 0.f;
                q.f[2 + m * 3] = ok ? clampf(keep[idx].fwd / 30.f, -1.f, 1.f) : 0.f;
                q.f[3 + m * 3] = ok ? clampf(keep[idx].cosA, -1.f, 1.f) : 0.f;
            }
            q.f[7] = 0.f;
            q.y = (float)outcome;
            S.seqS.push_back(q);
        }
    }
    S.nHops = 0;
}

void PitchEngine::v7NoteAction(const PassCandidate& c, const GestureOutput& g, const PlayerState& car) {
    V7State& S = v7_;
    if (!cfg_.v7 || !g.valid || human_) return;
    const double now = state_.timeMs;
    const float s = (float)state_.attackSign;
    const int cls = v7ClassOf(c);
    const bool isPass = plusIsPass(c.type);
    // tuple (state features, class, later reward)
    bool take = true;
    if (c.type == ACT_CARRY) take = !S.tupPending && now - S.tupT > 1200.0;
    else if (S.tupPending && S.tupP.cls != 2) take = true;
    if (take) {
        if (S.tupPending) v7ResolveTup(0.f);
        v7StateFeat(car, S.tupP.f);
        S.tupP.cls = cls; S.tupP.r = 0.f; S.tupT = now; S.tupPending = true;
        S.clsCount[cls]++;
        if (c.type == ACT_SHOOT) v7ResolveTup(0.4f + 0.6f * clampf(c.xP, 0.f, 1.f));
    }
    if (isPass) {
        S.lenSum += c.length; S.lenN++;
        const MetricVector2D d = c.target - car.pos;
        if (s * d.x > 0.f) S.fwdN++;
        V7Hop x; x.len = c.length; x.fwd = s * d.x; x.cosA = d.length() > 1e-3f ? s * d.x / d.length() : 0.f;
        if (S.nHops < 3) S.hops[S.nHops++] = x; else { S.hops[0] = S.hops[1]; S.hops[1] = S.hops[2]; S.hops[2] = x; }
        if (S.tKind >= 0) S.tPend = true;
    }
    if (c.type != ACT_CARRY) { S.axSum += s * car.pos.x; S.axN++; }
    if (c.type == ACT_SHOOT) { v7SeqEnd(1); S.spShot = true; S.nPlan = 0; }
    if (S.spRoutine >= 0 && c.type != ACT_CARRY) S.spStep++;
    // planned sequence: consume the step that was played, drop the plan when the play leaves it
    if (S.nPlan > 0 && now < S.planUntil && isPass) {
        if (S.plan[0].type == c.type && S.plan[0].targetId == c.targetId) {
            for (int i = 1; i < S.nPlan; i++) S.plan[i - 1] = S.plan[i];
            S.nPlan--;
            S.planUntil = std::max(S.planUntil, now + 1200.0);
        } else S.nPlan = 0;
    }
}

void PitchEngine::v7PassResult(bool kept) {
    V7State& S = v7_;
    if (!cfg_.v7) return;
    if (kept) S.passKept++; else { S.passLost++; S.spLost = true; }
    if (S.tupPending && S.tupP.cls != 2 && S.tupP.cls != 3) v7ResolveTup(kept ? 0.8f : -1.0f);
    if (S.tPend && S.tKind >= 0 && S.tPr >= 0 && S.tArm >= 0 && cfg_.v7Learn) {
        float& q = v7m_.timingQ[S.tKind][S.tPr][S.tArm];
        int& n = v7m_.timingN[S.tKind][S.tPr][S.tArm];
        n++;
        q += (std::max(0.02f, 1.f / (float)n)) * ((kept ? 1.f : -1.f) - q);
    }
    S.tPend = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Roll-out (depth 3) and the planned-sequence buffer
// ---------------------------------------------------------------------------------------------------------------------

float PitchEngine::v7RollVal(int type, int targetId) const {
    const V7State& S = v7_;
    for (int i = 0; i < S.nRoll; i++) if (S.rollType[i] == type && S.rollTgt[i] == targetId) return S.rollV[i];
    return 0.f;
}

// depth-limited expansion used by the roll-out: the best next pass (or shot) from a holder, with the argmax for the plan.
// A plain struct with a recursive member (no std::function: the engine must not allocate inside a frame).
struct PitchEngine::V7Roll {
    PitchEngine* e; float s;
    float xt(const MetricVector2D& p) const { return e->grid_.valueAt(s * p.x, p.y); }
    float sh(const MetricVector2D& p) const { return xTGrid::shotValue(s * p.x, p.y); }
    float rec(const MetricVector2D& from, int holder, float tEl, int depth, int* argId, MetricVector2D* argPos) const {
        float best = 0.5f * sh(from);
        if (argId) *argId = -1;
        if (depth <= 0) return best;
        int pj[4] = {-1, -1, -1, -1}; float pg[4] = {-1e9f, -1e9f, -1e9f, -1e9f};
        for (int j = 0; j < TEAM_SIZE; j++) {
            const PlayerState& m = e->state_.team[j];
            if (j == holder || !m.active || !m.visible || e->isKeeperIdx(j)) continue;
            MetricVector2D P = m.pos + m.vel * (tEl + 0.3f);
            P.x = clampf(P.x, -PITCH_HALF_X, PITCH_HALF_X); P.y = clampf(P.y, -PITCH_HALF_Y, PITCH_HALF_Y);
            const float g = xt(P) - xt(from);
            for (int k = 0; k < 4; k++) if (g > pg[k]) { for (int q = 3; q > k; q--) { pg[q] = pg[q - 1]; pj[q] = pj[q - 1]; } pg[k] = g; pj[k] = j; break; }
        }
        for (int k = 0; k < 4; k++) {
            if (pj[k] < 0) continue;
            const PlayerState& m = e->state_.team[pj[k]];
            MetricVector2D P = m.pos + m.vel * (tEl + 0.3f);
            P.x = clampf(P.x, -PITCH_HALF_X, PITCH_HALF_X); P.y = clampf(P.y, -PITCH_HALF_Y, PITCH_HALF_Y);
            float cl = 0.f;
            const float px = e->passProbability(from, P, cl, false, false) * (depth == 2 ? 0.92f : 0.85f);
            const float len = distance(from, P);
            const float val = px * (pg[k] + 0.7f * rec(P, pj[k], tEl + len / std::max(e->cfg_.passSpeed, 1.f), depth - 1, nullptr, nullptr)) - (1.f - px) * 0.04f;
            if (val > best) { best = val; if (argId) { *argId = pj[k]; if (argPos) *argPos = P; } }
        }
        return best;
    }
};

void PitchEngine::v7Rollout(const PlayerState& car) {
    V7State& S = v7_;
    const double now = state_.timeMs;
    if (S.nRoll > 0 && now - S.rollAt < 100.0) return;
    S.rollAt = now; S.nRoll = 0;
    if (state_.carrier < 0 || nCand_ < 2) return;
    const float s = (float)state_.attackSign;
    V7Roll R{this, s};
    auto xtv = [&](const MetricVector2D& p) { return R.xt(p); };
    auto shv = [&](const MetricVector2D& p) { return R.sh(p); };
    int ord[MAX_CAND], n = 0;
    for (int i = 0; i < nCand_; i++) ord[n++] = i;
    const int K = std::min(n, 8);
    for (int a = 0; a < K; a++) for (int b = a + 1; b < n; b++) if (cand_[ord[b]].utility > cand_[ord[a]].utility) std::swap(ord[a], ord[b]);
    const int ci = state_.carrier;
    float vals[8]; float sum = 0.f;
    for (int a = 0; a < K; a++) {
        const PassCandidate& c = cand_[ord[a]];
        float v;
        if (plusIsPass(c.type)) {
            const int ti = c.targetId - 1;
            const float t = c.length / std::max(cfg_.passSpeed, 1.f);
            v = c.xP * (c.deltaXT + 0.7f * R.rec(c.target, ti, t, 2, nullptr, nullptr)) - (1.f - c.xP) * 0.04f;
        } else if (c.type == ACT_SHOOT) {
            v = c.xP * shv(car.pos);
        } else {
            const MetricVector2D d = c.target - car.pos; const float dl = d.length();
            const MetricVector2D np = dl > 1e-3f ? car.pos + d * (3.f / dl) : car.pos;
            const float pr = clampf((cfg_.plan ? planPressure(np) : lanePressure(np)) / 1.5f, 0.f, 1.f);
            v = (1.f - 0.4f * pr) * (xtv(np) - xtv(car.pos) + 0.7f * R.rec(np, ci, 0.7f, 2, nullptr, nullptr)) - 0.02f * pr;
        }
        vals[a] = safe(v, 0.f); sum += vals[a];
        S.rollType[a] = c.type; S.rollTgt[a] = c.targetId;
    }
    const float mean = sum / (float)std::max(K, 1);
    for (int a = 0; a < K; a++) S.rollV[a] = clampf(vals[a] - mean, -0.3f, 0.3f);
    S.nRoll = K;
    // plan the best sequence when it clearly beats the rest and the first pass is safe
    if (now < S.planUntil && S.nPlan > 0) return;
    int bi = -1, si = -1;
    for (int a = 0; a < K; a++) { if (bi < 0 || vals[a] > vals[bi]) bi = a; }
    for (int a = 0; a < K; a++) { if (a != bi && (si < 0 || vals[a] > vals[si])) si = a; }
    if (bi < 0) return;
    const PassCandidate& b = cand_[ord[bi]];
    if (!plusIsPass(b.type) || b.xP < cfg_.minPassXp || b.offside > 0.5f || vals[bi] < 0.05f) return;
    if (si >= 0 && vals[bi] - vals[si] < 0.02f) return;
    int j2 = -1, j3 = -1; MetricVector2D p2, p3;
    const float t1 = b.length / std::max(cfg_.passSpeed, 1.f);
    R.rec(b.target, b.targetId - 1, t1, 2, &j2, &p2);
    if (j2 < 0) return;
    R.rec(p2, j2, t1 + 0.6f, 1, &j3, &p3);
    S.plan[0].type = b.type; S.plan[0].targetId = b.targetId;
    S.plan[1].type = ACT_PASS; S.plan[1].targetId = j2 + 1;
    S.nPlan = 2;
    if (j3 >= 0) { S.plan[2].type = ACT_PASS; S.plan[2].targetId = j3 + 1; S.nPlan = 3; }
    uint32_t sd = (uint32_t)(frameNo_ * 2654435761u) | 1u;
    S.planUntil = now + 1500.0 + 1000.0 * plusRand(sd);
    S.planAt = now;
}

// ---------------------------------------------------------------------------------------------------------------------
// Utility layer: everything above that changes a candidate's utility
// ---------------------------------------------------------------------------------------------------------------------

void PitchEngine::v7Apply(const PlayerState& car) {
    V7State& S = v7_;
    if (!cfg_.v7 || human_) return;
    const double now = state_.timeMs;
    const float s = (float)state_.attackSign;
    if (cfg_.v7Rollout) v7Rollout(car);
    // policy network (state-only features: one forward pass per call)
    float Q[V7_NCLS] = {0, 0, 0, 0, 0}, meanQ = 0.f, polConf = 0.f;
    if (cfg_.v7Policy) {
        float f[V7_NIN];
        v7StateFeat(car, f);
        v7m_.policy.forward(f, Q);
        for (int k = 0; k < V7_NCLS; k++) meanQ += Q[k];
        meanQ /= (float)V7_NCLS;
        polConf = clampf((float)v7m_.policy.trained / 600.f, 0.f, 1.f);
    }
    const float seqConf = clampf((float)v7m_.seqNet.trained / 200.f, 0.f, 1.f);
    const float minXp0 = cfg_.minPassXp;
    float minAdd = S.rMinAdd * (cfg_.v7Risk ? 1.f : 0.f) + (cfg_.v7MetaNet ? S.metaMinAdd : 0.f);
    if (cfg_.v7Stamina) {
        minAdd += 0.05f * clampf((0.6f - S.stamAvg) / 0.6f, 0.f, 1.f);
        if (state_.carrier >= 0 && staminaOf(state_.carrier) < 0.4f) minAdd += 0.03f;
    }
    const bool tl = v7TempoLockedNow();
    if (tl) minAdd = std::max(minAdd, 0.02f);
    const float thrMul = cfg_.v7Risk ? S.rThrMul : 1.f;
    const float shotMul = (cfg_.v7Risk ? S.rShotMul : 1.f) * (cfg_.v7MetaNet ? S.metaShotMul : 1.f);
    const bool ko = cfg_.v7Kickoff && now < S.koUntil && S.koOurs && state_.status == ST_TEAM;
    const bool spOn = cfg_.v7SetPiece && S.spRoutine >= 0 && S.spStep < 4 && now - S.spStart < 7000.0 && S.spOurs;
    const bool planOn = cfg_.v7Rollout && S.nPlan > 0 && now < S.planUntil;
    const MetricVector2D B = state_.ball;
    for (int i = 0; i < nCand_; i++) {
        PassCandidate& c = cand_[i];
        const bool isPass = plusIsPass(c.type);
        const MetricVector2D d = c.target - car.pos;
        const float dl = d.length();
        const float fwd = s * d.x;
        const int cls = v7ClassOf(c);
        float add = 0.f;
        if (cfg_.v7Policy) add += cfg_.wPolicy * polConf * clampf(Q[cls] - meanQ, -1.f, 1.f);
        if (cfg_.v7Rollout) add += cfg_.wRoll * v7RollVal(c.type, c.targetId);
        if (cfg_.v7SeqNet && isPass && seqConf > 0.f) {
            float sf[8], o1[1];
            v7SeqFeat(car, &c, sf);
            v7m_.seqNet.forward(sf, o1);
            add += cfg_.wSeqNet * seqConf * clampf(o1[0], -1.f, 1.f);
        }
        if (cfg_.v7OppIntent && isPass) {
            const float* p = S.oi;
            float m = 0.f;
            m += p[4] * (c.type == ACT_THROUGH ? -1.f : (c.type == ACT_LOFTED ? 0.5f : (std::fabs(d.y) > 15.f ? 0.6f : 0.f)));
            m += p[1] * (c.type == ACT_THROUGH ? 1.f : (c.type == ACT_LOFTED ? 0.5f : (dl < 18.f ? -0.2f : 0.f)));
            m += p[0] * (c.type == ACT_PASS && dl < 18.f ? 0.5f : (c.type == ACT_THROUGH ? -0.3f : 0.f));
            m += p[2] * (c.type == ACT_PASS && fwd / 30.f > 0.3f ? 0.4f : 0.2f);
            m += p[3] * ((d.y * (-B.y) > 0.f && std::fabs(d.y) > 12.f) ? 0.6f : 0.f);
            add += clampf(cfg_.wIntent7 * m, -0.12f, 0.12f);
        }
        if (cfg_.v7Morph && S.morphStr > 0.f && now < S.morphUntil)
            for (int k = 0; k < 2; k++) {
                if (S.morphIdx[k] < 0) continue;
                const float dm = distance(c.target, S.morphTo[k]);
                if (dm < 7.f && (c.type == ACT_CARRY || (isPass && c.targetId - 1 == S.morphIdx[k] && c.xP >= 0.5f)))
                    add += cfg_.wMorph * (1.f - dm / 7.f) * S.morphStr * (isPass ? c.xP : 0.6f);
            }
        if (cfg_.v7DualPress && S.dual && now - S.dualAt < 1200.0 && (isPass || c.type == ACT_CARRY)) {
            const float dv = distance(c.target, S.vacated);
            if (dv < 8.f && (c.type == ACT_CARRY || (c.xP >= 0.45f && c.offside < 0.5f)))
                add += cfg_.wVacated * (1.f - dv / 8.f) * (isPass ? c.xP : 0.6f * (1.f - clampf(c.pressure, 0.f, 1.f)));
        }
        if (cfg_.v7Risk || cfg_.v7MetaNet || cfg_.v7Stamina || tl) {
            if (isPass) {
                const float lo = minXp0, hi = minXp0 + minAdd;
                if (minAdd > 0.f && c.xP >= lo && c.xP < hi) add -= 0.20f * (hi - c.xP) / std::max(hi, 0.1f) + 0.03f;
                else if (minAdd < 0.f && c.xP >= hi && c.xP < lo && c.offside < 0.5f) add += 0.15f * (lo - c.xP) / std::max(lo, 0.1f);
                if (c.type == ACT_THROUGH && !spOn) add += (thrMul - 1.f) * 0.15f;
            } else if (c.type == ACT_SHOOT) add += (shotMul - 1.f) * 0.20f * c.xP;
        }
        if (cfg_.v7Style) {
            const float wv = S.sw[1], wh = S.sw[2];
            if (isPass) {
                add += wv * (0.30f * std::max(0.f, c.deltaXT) * c.xP + (c.type == ACT_THROUGH ? 0.05f : 0.f));
                add += wh * ((v7Hash01((uint32_t)(frameNo_ / 6) * 977u + (uint32_t)c.targetId * 131u + (uint32_t)c.type) - 0.5f) * 0.10f + (c.type == ACT_LOFTED ? 0.05f : 0.f));
            } else if (c.type == ACT_CARRY) {
                add += 0.02f * wv * (fwd > 0.f ? 1.f : 0.f) + 0.03f * wh * (1.f - clampf(c.pressure, 0.f, 1.f));
            }
        }
        if (tl) {
            if (isPass && (dl > 26.f || c.type == ACT_THROUGH)) add -= 0.15f;
            else if (c.type == ACT_PASS && dl <= 24.f && c.xP >= 0.8f) add += 0.08f;
        }
        if (ko) {
            if (c.type == ACT_THROUGH || c.type == ACT_LOFTED || c.type == ACT_SHOOT) add -= 0.8f;
            else if (c.type == ACT_CARRY) add -= 0.3f;
            else if (c.type == ACT_PASS && c.xP >= 0.8f && fwd < 8.f) add += 0.25f;
        }
        if (spOn && c.offside < 0.5f) {
            const int r = S.spRoutine, st = S.spStep, kind = S.spKind;
            const bool nearBox = s * c.target.x > PITCH_HALF_X - 22.f;
            float b = 0.f;
            if (kind == 1) {                                                    // kick-off
                if (r == 0 && c.type == ACT_PASS && fwd < 6.f && c.xP >= 0.8f) b = 0.7f;
                else if (r == 1 && c.type == ACT_PASS && std::fabs(d.y) > 10.f && c.xP >= 0.7f) b = 0.6f;
                else if (r == 2 && isPass && fwd > 10.f && fwd < 28.f && c.xP >= 0.55f) b = 0.5f;
            } else {
                if (r == 0) {                                                   // short disguised routine, then the third man
                    if (st < 2 && c.type == ACT_PASS && c.length <= 16.f && c.xP >= 0.7f) b = 0.6f;
                    else if (st < 2 && (c.type == ACT_THROUGH || c.type == ACT_LOFTED || c.type == ACT_SHOOT)) b = -0.5f;
                    else if (st >= 2 && isPass && fwd > 8.f && c.xP >= 0.6f) b = 0.4f;
                } else if (r == 1) {                                            // far-post overload
                    const bool farSide = d.y * B.y < 0.f || std::fabs(c.target.y) > 8.f;
                    if ((c.type == ACT_LOFTED || c.type == ACT_PASS) && nearBox && farSide && std::fabs(c.target.y) > 6.f && c.xP >= 0.35f) b = 0.65f;
                    else if (c.type == ACT_LOFTED && std::fabs(d.y) > 20.f && c.xP >= 0.4f) b = 0.4f;
                } else {                                                        // direct exploit
                    if (c.type == ACT_SHOOT && c.xP >= 0.15f) b = 0.7f;
                    else if ((c.type == ACT_THROUGH || c.type == ACT_LOFTED) && fwd > 15.f && c.xP >= 0.45f) b = 0.55f;
                }
            }
            add += b;
        }
        if (planOn && isPass && S.plan[0].type == c.type && S.plan[0].targetId == c.targetId && c.xP >= 0.9f * minXp0 && c.offside < 0.5f)
            add += cfg_.wBuf;
        if (!std::isfinite(add)) add = 0.f;
        c.utility += clampf(add, -0.9f, 0.9f);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Pre-emptive stick alignment, timing residual
// ---------------------------------------------------------------------------------------------------------------------

bool PitchEngine::v7PreAlignStep(const PassCandidate& c, const PlayerState& car, bool forced, GestureOutput* out) {
    V7State& S = v7_;
    if (!cfg_.v7 || !cfg_.v7PreAlign || human_ || forced || !out) return false;
    if (c.type != ACT_PASS && c.type != ACT_THROUGH) return false;
    const double now = state_.timeMs;
    if (S.paType == c.type && S.paTarget == c.targetId && now - S.paDoneAt < 800.0) return false;   // already aligned for this pick
    const float sp = car.vel5.length();
    if (sp < 1.0f || nearestOppDistance(car.pos) < 3.0f) return false;
    const int ti = c.targetId - 1;
    MetricVector2D recv = c.target;
    if (ti >= 0 && ti < TEAM_SIZE) {
        const float tt = c.length / std::max(cfg_.passSpeed, 1.f);
        recv = c.target + state_.team[ti].vel * (0.5f * tt);       // where the receiver will be as the ball arrives
    }
    const MetricVector2D dv = recv - car.pos;
    const float dl = dv.length();
    if (dl < 1e-3f) return false;
    const float cosA = clampf(car.vel5.dot(dv) / (sp * dl), -1.f, 1.f);
    const float deg = std::acos(cosA) * 57.2957795f;
    if (deg < 10.f || deg > 100.f) return false;
    GestureOutput g = makeGesture(ACT_CARRY, car.pos, recv, 0.f, false);
    if (!g.valid) return false;
    const int ms = (int)clampf(100.f + 80.f * (deg / 90.f), 100.f, 180.f);
    g.durationMs = ms;
    S.paType = c.type; S.paTarget = c.targetId; S.paDoneAt = now + (double)ms;
    nextDecisionAt_ = now;                                         // the real pass follows right after the alignment
    *out = g;
    return true;
}

void PitchEngine::v7TimingLead(PassCandidate& c) const {
    V7State& S = const_cast<V7State&>(v7_);
    S.tKind = -1; S.tArm = -1; S.tPr = -1;
    if (!cfg_.v7 || !cfg_.v7Timing || human_ || !plusIsPass(c.type)) return;
    static const float OFF[5] = {-120.f, -60.f, 0.f, 60.f, 120.f};
    const int kind = c.type == ACT_PASS ? 0 : 1;
    const float pr = cfg_.plan && state_.carrier >= 0 ? planPressure(state_.team[state_.carrier].pos) : 0.f;
    const int pb = pr < 0.35f ? 0 : (pr < 0.8f ? 1 : 2);
    int arm = 2;
    float bq = -1e9f;
    uint32_t sd = (uint32_t)((frameNo_ / 10) * 2654435761u) ^ 0x3C6EF372u;
    if (!sd) sd = 1u;
    for (int a = 0; a < 5; a++) {
        const float q = v7m_.timingQ[kind][pb][a] + (a == 2 ? 0.01f : 0.f);
        if (q > bq) { bq = q; arm = a; }
    }
    if (plusRand(sd) < 0.12f) arm = (int)(plusRand(sd) * 5.f) % 5;
    S.tKind = kind; S.tPr = pb; S.tArm = arm;
    const int ti = c.targetId - 1;
    if (ti < 0 || ti >= TEAM_SIZE) return;
    const float k = c.type == ACT_PASS ? 0.5f : 1.f;
    // measured friction changes the flight time the lead is built for
    float dt = OFF[arm] * 0.001f;
    if (cfg_.v7Landing && v7_.fricN >= 5) {
        const float v0 = std::max(cfg_.passSpeed, 4.f);
        dt += v7FlightTime(c.length, v0, v7_.fric) - c.length / std::max(cfg_.passSpeed, 1.f);
        dt = clampf(dt, -0.25f, 0.25f);
    }
    c.target = c.target + state_.team[ti].vel * (dt * k);
}

// positive learned offset: hold the release a moment (only when not pressed)
bool PitchEngine::v7TimingHold(const PassCandidate& c, const PlayerState& car, bool forced) {
    V7State& S = v7_;
    if (!cfg_.v7 || !cfg_.v7Timing || human_ || forced || S.tArm < 3 || !plusIsPass(c.type)) { S.tHoldUntil = -1e9; return false; }
    const double now = state_.timeMs;
    if (nearestOppDistance(car.pos) < 4.5f) { S.tHoldUntil = -1e9; return false; }
    if (S.tHoldType != c.type || S.tHoldTarget != c.targetId || now > S.tHoldUntil + 400.0) {
        S.tHoldType = c.type; S.tHoldTarget = c.targetId;
        S.tHoldUntil = now + (S.tArm == 3 ? 60.0 : 120.0);
        return true;
    }
    return now < S.tHoldUntil;
}

// ---------------------------------------------------------------------------------------------------------------------
// Defence helpers
// ---------------------------------------------------------------------------------------------------------------------

GestureOutput PitchEngine::v7PressTo(const MetricVector2D& to, int ms, bool dash, float r) {
    GestureOutput g = makeGesture(ACT_PRESS, ctrlPos_, to, 1.f, false);
    g.status = ST_OPP; g.actionType = ACT_PRESS; g.durationMs = ms; g.dash = dash; g.r = r;
    return g;
}

bool PitchEngine::v7GkShadowPoint(MetricVector2D* pt) {
    if (!cfg_.v7 || !cfg_.v7GkShadow || !pt || !ballUsable() || state_.status != ST_OPP) return false;
    const float own = -(float)state_.attackSign;
    const MetricVector2D B = state_.ball;
    if (goalDistance(B) > 26.f || std::fabs(B.y) > 24.f) return false;
    const float dB = std::fabs(B.x - own * PITCH_HALF_X);
    if (dB < 4.f) return false;
    MetricVector2D K(own * (PITCH_HALF_X - 2.f), 0.f);
    for (int i = 0; i < TEAM_SIZE; i++) if (state_.team[i].active && isKeeperIdx(i)) { K = state_.team[i].pos; break; }
    const float kd = std::fabs(K.x - own * PITCH_HALF_X);
    const float mirrorY = B.y * (kd / dB);                          // on the bisector of the shot
    const float yEff = K.y + (mirrorY - K.y) * 0.65f;               // he commits late: only part of the way
    const float wK = clampf(2.3f - 0.015f * (dB - 6.f), 1.6f, 2.3f);
    const float gapHi = 3.66f - (yEff + wK), gapLo = (yEff - wK) + 3.66f;
    if (gapHi <= 0.f && gapLo <= 0.f) return false;
    v7_.gkGap = std::max(gapHi, gapLo);
    const float postY = gapHi >= gapLo ? 3.2f : -3.2f;
    const MetricVector2D P(own * PITCH_HALF_X, postY);
    MetricVector2D u = P - B;
    const float ul = u.length();
    if (ul < 1e-3f) return false;
    *pt = B + u * (2.0f / ul);
    return true;
}

MetricVector2D PitchEngine::v7PredCarrierNear(const MetricVector2D& B, int* idx) const {
    int ci = -1; float cd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) { const PlayerState& o = state_.opp[i]; if (!o.active || !o.visible) continue; const float d = distance(o.pos, B); if (d < cd) { cd = d; ci = i; } }
    if (idx) *idx = ci;
    return ci >= 0 ? v7PredCarrier(ci) : B;
}

// early defensive rules: kick-off wall, ball landing zone
bool PitchEngine::v7DefEarly(GestureOutput* out, double now) {
    V7State& S = v7_;
    if (!cfg_.v7 || !out) return false;
    const float own = -(float)state_.attackSign;
    if (cfg_.v7Kickoff && now < S.koUntil) {
        // the five outfield players nearest the centre spot form a tight central wall; the controlled one takes his slot
        int ord[TEAM_SIZE], n = 0;
        for (int i = 0; i < TEAM_SIZE; i++) if (state_.team[i].active && !isKeeperIdx(i)) ord[n++] = i;
        for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++)
            if (state_.team[ord[b]].pos.length() < state_.team[ord[a]].pos.length()) std::swap(ord[a], ord[b]);
        const int m = std::min(n, 5);
        const int ci = controlledIdx();
        int rank = -1;
        int five[5];
        for (int a = 0; a < m; a++) five[a] = ord[a];
        for (int a = 0; a < m; a++) for (int b = a + 1; b < m; b++) if (state_.team[five[b]].pos.y < state_.team[five[a]].pos.y) std::swap(five[a], five[b]);
        for (int a = 0; a < m; a++) if (five[a] == ci) rank = a;
        if (rank >= 0) {
            const MetricVector2D slot(own * 7.f, ((float)rank - (float)(m - 1) * 0.5f) * 3.0f);
            if (distance(ctrlPos_, slot) < 1.0f) { GestureOutput h = v7PressTo(slot, 150, false, 0.f); *out = h; return true; }
            *out = v7PressTo(slot, 150, false, 1.f);
            return true;
        }
    }
    return false;
}

// landing-zone interception for a pass or clearance in flight
bool PitchEngine::v7DefLanding(GestureOutput* out, double now, bool noPress) {
    if (!cfg_.v7 || !cfg_.v7Landing || noPress || !out) return false;
    MetricVector2D L; float tl;
    if (state_.ballVel.length() < 6.f || !predictBallLanding(&L, &tl)) return false;
    const int ci = controlledIdx();
    if (ci < 0) return false;
    float tc = plusTta(state_.team[ci], L, 7.5f), tm = 1e9f; int bj = -1;
    for (int j = 0; j < TEAM_SIZE; j++) {
        if (j == ci || !state_.team[j].active || isKeeperIdx(j)) continue;
        const float t = plusTta(state_.team[j], L, 7.5f);
        if (t < tm) { tm = t; bj = j; }
    }
    if (bj >= 0 && tm + 0.35f < tc && now - defSwitchAt_ > 600.0) {
        defSwitchAt_ = now; defNextAt_ = now + 450.0;
        GestureOutput g; g.valid = true; g.status = ST_OPP; g.actionType = ACT_SWITCH; g.durationMs = 70;
        *out = g;
        return true;
    }
    if (tc <= tm - 0.05f && tc < tl + 0.8f && distance(ctrlPos_, L) > 1.2f) {
        *out = v7PressTo(L, 200, distance(ctrlPos_, L) > 3.5f, 1.f);
        defNextAt_ = now + 120.0;
        return true;
    }
    return false;
}

// shape morph for the controlled defender when we are outnumbered around the ball
bool PitchEngine::v7DefMid(GestureOutput* out, double now, bool noPress, float dC) {
    V7State& S = v7_;
    if (!cfg_.v7 || !cfg_.v7Morph || noPress || !out || S.morphStr <= 0.f || now > S.morphUntil || dC < 6.f) return false;
    const int ci = controlledIdx();
    for (int k = 0; k < 2; k++) {
        if (S.morphIdx[k] < 0 || S.morphIdx[k] != ci) continue;
        const float dd = distance(ctrlPos_, S.morphTo[k]);
        if (dd < 1.5f) return false;
        *out = v7PressTo(S.morphTo[k], 200, dd > 9.f, dd < 3.f ? 0.6f : 1.f);
        defNextAt_ = now + 150.0;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// End-of-match learning: anomaly filter, replay, PolicyNet + EWC, diversity, sequence / escape / defence heads, league step
// ---------------------------------------------------------------------------------------------------------------------

// 1 = a clean match worth learning from, 0 = lag, a broken radar, a tiny sample or an absurd score (the reason is returned)
float PitchEngine::v7QualityScore(std::string& why) const {
    const V7State& S = v7_;
    why.clear();
    auto add = [&](const char* w) { if (!why.empty()) why += ", "; why += w; };
    const double liveSec = plus_.liveMs / 1000.0;
    if (liveSec < 150.0) add("too short");
    if (S.dtN > 50 && S.dtMean > 1.0 && std::sqrt(S.dtM2 / (double)S.dtN) / S.dtMean > 0.8) add("frame timing jitter");
    if (S.frames > 100 && (double)S.noBall / (double)S.frames > 0.55) add("ball missing too often");
    if (S.frames > 100 && (double)S.jumpFrames / (double)S.frames > 0.08) add("radar jumps");
    if (plus_.goalsFor + plus_.goalsAgainst > 12 || std::abs(plus_.goalsFor - plus_.goalsAgainst) > 7) add("score outlier");
    const int att = S.passKept + S.passLost;
    if (att >= 25 && (float)S.passKept / (float)att < 0.2f) add("pass error-rate spike");
    return why.empty() ? 1.f : 0.f;
}

float PitchEngine::v7RewardModel(const float* f, int cls, bool* ok) const {
    float bd[6]; float br[6]; int nb = 0;
    for (const V7MatchRec& m : v7m_.replay)
        for (const V7Tup& t : m.tup) {
            if (t.cls != cls) continue;
            float d2 = 0.f;
            for (int k = 0; k < 8; k++) { const float e = f[k] - t.f[k]; d2 += e * e; }
            if (d2 > 0.6f) continue;
            if (nb < 6) { bd[nb] = d2; br[nb] = t.r; nb++; }
            else { int w = 0; for (int k = 1; k < 6; k++) if (bd[k] > bd[w]) w = k; if (d2 < bd[w]) { bd[w] = d2; br[w] = t.r; } }
        }
    if (ok) *ok = nb >= 3;
    if (nb < 3) return 0.f;
    float s = 0.f; for (int k = 0; k < nb; k++) s += br[k];
    return s / (float)nb;
}

void PitchEngine::v7TrainPolicy(const std::vector<V7Tup>& cur, float divLambda, std::string& rep) {
    V7Mem& M = v7m_;
    std::vector<const V7Tup*> batch;
    for (const V7Tup& t : cur) batch.push_back(&t);
    std::vector<const V7Tup*> rp;
    for (const V7MatchRec& m : M.replay) for (const V7Tup& t : m.tup) rp.push_back(&t);
    const size_t take = std::min<size_t>(rp.size(), 480);
    if (take > 0) { const size_t stride = std::max<size_t>(1, rp.size() / take); for (size_t i = 0; i < rp.size() && batch.size() < cur.size() + take; i += stride) batch.push_back(rp[i]); }
    if (batch.empty()) return;
    float fbar[V7_NCLS] = {0.2f, 0.2f, 0.2f, 0.2f, 0.2f};
    if (M.nFp > 0) {
        for (int k = 0; k < V7_NCLS; k++) { float s = 0.f; for (int i = 0; i < M.nFp; i++) s += M.fp[i][k]; fbar[k] = s / (float)M.nFp; }
    }
    const bool ewc = cfg_.v7Ewc && M.policy.trained > 0;
    const float lam = ewc ? 6.0f : 0.f;
    float g2[V7Policy::NP];
    for (int k = 0; k < V7Policy::NP; k++) g2[k] = 0.f;
    uint32_t sd = 0x9E3779B9u + (uint32_t)M.matches * 7919u;
    float loss = 0.f; int nl = 0;
    const int epochs = 5;
    for (int ep = 0; ep < epochs; ep++) {
        const float lr = 0.02f * (1.f - 0.15f * (float)ep);
        const bool last = ep == epochs - 1;
        for (size_t i = 0; i < batch.size(); i++) {
            sd ^= sd << 13; sd ^= sd >> 17; sd ^= sd << 5;
            const V7Tup& t = *batch[(size_t)(sd % (uint32_t)batch.size())];
            float target = t.r;
            if (cfg_.v7Diversity && M.nFp > 0) target -= divLambda * (fbar[t.cls] - 0.2f);   // over-used classes score a little lower
            const float e = M.policy.step(t.f, t.cls, clampf(target, -1.5f, 1.5f), lr, lam, nullptr);
            if (last) { loss += e; nl++; M.policy.sens(t.f, t.cls, g2); }
        }
    }
    if (cfg_.v7Ewc && nl > 0) {
        for (int k = 0; k < V7Policy::NP; k++) {
            const float F = clampf(g2[k] / (float)nl, 0.f, 4.f);
            M.policy.fisher[k] = clampf(0.5f * M.policy.fisher[k] + 0.5f * F, 0.f, 4.f);
            M.policy.anchor[k] = M.policy.p[k];
        }
    }
    char b[96];
    std::snprintf(b, sizeof b, "policy trained on %d samples (loss %.3f); ", (int)batch.size(), nl ? loss / (float)nl : 0.f);
    rep += b;
}

void PitchEngine::v7League(std::string& rep) {
    V7Mem& M = v7m_;
    if (M.nCkpt < 2) return;
    std::vector<const V7Tup*> pool;
    for (const V7MatchRec& m : M.replay) for (const V7Tup& t : m.tup) pool.push_back(&t);
    if (pool.size() < 60) return;
    V7Policy mem[3];
    const int nm = std::min(M.nCkpt, 3);
    for (int i = 0; i < nm; i++) { const int idx = ((M.ckptHead - 1 - i) % V7_CKPT + V7_CKPT) % V7_CKPT; for (int k = 0; k < V7Policy::NP; k++) mem[i].p[k] = M.ckpt[idx][k]; }
    const size_t stride = std::max<size_t>(1, pool.size() / 160);
    int upd = 0;
    for (size_t i = 0; i < pool.size(); i += stride) {
        const V7Tup& t = *pool[i];
        float oc[V7_NCLS]; M.policy.forward(t.f, oc);
        int ac = 0; for (int k = 1; k < V7_NCLS; k++) if (oc[k] > oc[ac]) ac = k;
        bool ok1; const float rc = v7RewardModel(t.f, ac, &ok1);
        int bestA = -1; float bestR = rc + 0.15f;
        for (int m = 0; m < nm; m++) {
            float o[V7_NCLS]; mem[m].forward(t.f, o);
            int a = 0; for (int k = 1; k < V7_NCLS; k++) if (o[k] > o[a]) a = k;
            if (a == ac) continue;
            bool ok2; const float r = v7RewardModel(t.f, a, &ok2);
            if (ok2 && r > bestR) { bestR = r; bestA = a; }
        }
        if (bestA >= 0) { M.policy.step(t.f, bestA, clampf(bestR, -1.5f, 1.5f), 0.008f, cfg_.v7Ewc ? 6.f : 0.f, nullptr); upd++; }
    }
    char b[96];
    std::snprintf(b, sizeof b, "league step: %d updates from %d earlier policies; ", upd, nm);
    rep += b;
}

std::string PitchEngine::v7EndMatch() {
    V7State& S = v7_;
    V7Mem& M = v7m_;
    std::string rep = "v6.17: ";
    if (!cfg_.v7 || S.frames < 30) { rep += "no live play, nothing learned"; M.report = rep; S.clear(); if (cfg_.v8) { v8EndMatch(rep, false); M.report = rep; v8BeginMatch(); } return rep; }
    M.matches++;
    if (S.tupPending) v7ResolveTup(0.f);
    v7SeqEnd(0);
    std::string why;
    const float q = v7QualityScore(why);
    bool v8Valid = true;
    { std::string w8; if (v8Suspect(&w8)) { v8Valid = false; why += (why.empty() ? "" : ", ") + w8; } }
    if (cfg_.v7Anomaly && (q < 0.5f || !v8Valid)) {
        M.matchesRejected++;
        v8Valid = false;
        rep += "match not used for learning (" + why + ")";
    } else if (cfg_.v8 && cfg_.v8Bench && v8_.evalMatch) {
        rep += "evaluation match: measured, not trained on";
    } else if (!cfg_.v7Learn) {
        rep += "learning switched off";
    } else {
        // opponent embedding
        if (cfg_.v7Embed) {
            int slot = -1;
            for (int i = 0; i < M.nEmb; i++) if (M.emb[i].key == M.curSig) slot = i;
            if (slot < 0) {
                if (M.nEmb < V7_EMB_N) slot = M.nEmb++;
                else { slot = 0; for (int i = 1; i < V7_EMB_N; i++) if (M.emb[i].n < M.emb[slot].n) slot = i; }
                M.emb[slot] = V7Embed(); M.emb[slot].key = M.curSig;
            }
            for (int d = 0; d < V7_EMB_D; d++) M.emb[slot].e[d] = clampf(0.7f * M.emb[slot].e[d] + 0.3f * (plus_.im[d] - 0.5f) * 2.f, -1.f, 1.f);
            M.emb[slot].n++;
        }
        // behaviour fingerprint of this match and its similarity to the recent past
        float fpNow[V7_FP];
        int ncls = 0; for (int k = 0; k < V7_NCLS; k++) ncls += S.clsCount[k];
        for (int k = 0; k < V7_NCLS; k++) fpNow[k] = ncls > 0 ? (float)S.clsCount[k] / (float)ncls : 0.2f;
        fpNow[5] = S.lenN > 0 ? clampf((float)(S.lenSum / S.lenN) / 45.f, 0.f, 1.f) : 0.3f;
        fpNow[6] = S.lenN > 0 ? (float)S.fwdN / (float)S.lenN : 0.5f;
        fpNow[7] = S.axN > 0 ? clampf(((float)(S.axSum / S.axN) / PITCH_HALF_X + 1.f) * 0.5f, 0.f, 1.f) : 0.5f;
        float divLambda = 0.03f;
        if (M.nFp > 0) {
            float mean[V7_FP] = {0};
            for (int i = 0; i < M.nFp; i++) for (int k = 0; k < V7_FP; k++) mean[k] += M.fp[i][k] / (float)M.nFp;
            float dot = 0.f, a2 = 0.f, b2 = 0.f;
            for (int k = 0; k < V7_FP; k++) { dot += fpNow[k] * mean[k]; a2 += fpNow[k] * fpNow[k]; b2 += mean[k] * mean[k]; }
            const float sim = dot / std::max(std::sqrt(a2 * b2), 1e-6f);
            divLambda = sim > 0.97f ? 0.10f : 0.03f;
            char b[64]; std::snprintf(b, sizeof b, "behaviour similarity %.2f; ", sim); rep += b;
        }
        v7TrainPolicy(S.tup, divLambda, rep);
        // sequence value net
        {
            std::vector<V7SeqSample> all = S.seqS;
            for (const V7SeqSample& q2 : M.seqRep) all.push_back(q2);
            float g2[V7Mlp<8, 6, 1>::NP]; for (int k = 0; k < V7Mlp<8, 6, 1>::NP; k++) g2[k] = 0.f;
            uint32_t sd = 0x85EBCA6Bu + (uint32_t)M.matches;
            for (int ep = 0; ep < 4 && !all.empty(); ep++)
                for (size_t i = 0; i < all.size(); i++) {
                    sd ^= sd << 13; sd ^= sd >> 17; sd ^= sd << 5;
                    const V7SeqSample& z = all[sd % (uint32_t)all.size()];
                    M.seqNet.step(z.f, 0, 0.8f * z.y, 0.03f, cfg_.v7Ewc ? 6.f : 0.f, nullptr);
                    if (ep == 3) M.seqNet.sens(z.f, 0, g2);
                }
            if (cfg_.v7Ewc && !all.empty()) for (int k = 0; k < V7Mlp<8, 6, 1>::NP; k++) { M.seqNet.fisher[k] = clampf(0.5f * M.seqNet.fisher[k] + 0.5f * g2[k] / (float)all.size(), 0.f, 4.f); M.seqNet.anchor[k] = M.seqNet.p[k]; }
            for (const V7SeqSample& z : S.seqS) M.seqRep.push_back(z);
            if (M.seqRep.size() > (size_t)V7_SEQ_CAP) M.seqRep.erase(M.seqRep.begin(), M.seqRep.begin() + (M.seqRep.size() - V7_SEQ_CAP));
            char b[64]; std::snprintf(b, sizeof b, "%d sequences; ", (int)S.seqS.size()); rep += b;
        }
        // escape head: logistic regression with a pull toward its prior
        {
            std::vector<V7EscSample> all = S.escS;
            for (const V7EscSample& e : M.escRep) all.push_back(e);
            for (int ep = 0; ep < 8; ep++)
                for (const V7EscSample& e : all) {
                    float z = 0.f; for (int k = 0; k < 7; k++) z += M.esc[k] * e.f[k];
                    const float err = e.y - v7Sig(z);
                    for (int k = 0; k < 7; k++) M.esc[k] = clampf(M.esc[k] + 0.05f * err * e.f[k] - 0.002f * M.esc[k], -4.f, 4.f);
                }
            for (const V7EscSample& e : S.escS) M.escRep.push_back(e);
            if (M.escRep.size() > (size_t)V7_ESC_CAP) M.escRep.erase(M.escRep.begin(), M.escRep.begin() + (M.escRep.size() - V7_ESC_CAP));
            char b[64]; std::snprintf(b, sizeof b, "%d escape episodes; ", (int)S.escS.size()); rep += b;
        }
        // defence head: consolidate this match's episodes with the stored ones
        {
            for (const V7Tup& t : S.defS) M.defRep.push_back(t);
            if (M.defRep.size() > 600) M.defRep.erase(M.defRep.begin(), M.defRep.begin() + (M.defRep.size() - 600));
            for (const V7Tup& t : M.defRep) M.defNet.step(t.f, t.cls, t.r, 0.02f, 0.f, nullptr);
        }
        // replay buffer: an evenly thinned copy of this match; the last 20 matches are kept
        {
            V7MatchRec mr;
            const size_t n = S.tup.size();
            const size_t keep = std::min<size_t>(n, V7_TUP_MATCH);
            for (size_t i = 0; i < keep; i++) mr.tup.push_back(S.tup[i * n / std::max<size_t>(keep, 1)]);
            M.replay.push_back(mr);
            if (M.replay.size() > (size_t)V7_REPLAY) M.replay.erase(M.replay.begin());
        }
        M.trainedMatches++;
        if (cfg_.v7League && M.trainedMatches % 3 == 0) v7League(rep);
        if (M.trainedMatches % 2 == 1) {
            for (int k = 0; k < V7Policy::NP; k++) M.ckpt[M.ckptHead][k] = M.policy.p[k];
            M.ckptHead = (M.ckptHead + 1) % V7_CKPT;
            if (M.nCkpt < V7_CKPT) M.nCkpt++;
        }
        for (int k = 0; k < V7_FP; k++) M.fp[M.fpHead][k] = fpNow[k];
        M.fpHead = (M.fpHead + 1) % V7_FPN;
        if (M.nFp < V7_FPN) M.nFp++;
    }
    if (cfg_.v8) { v8EndMatch(rep, v8Valid); v8BeginMatch(); }
    M.report = rep;
    S.clear();
    return rep;
}

// ---------------------------------------------------------------------------------------------------------------------
// Persistence (text, versioned, every number validated on the way in)
// ---------------------------------------------------------------------------------------------------------------------

namespace {
void v7PutF(std::string& o, const float* v, int n, const char* fmt = " %.4f") {
    char b[24];
    for (int i = 0; i < n; i++) { std::snprintf(b, sizeof b, fmt, (double)v[i]); o += b; }
    o += "\n";
}
bool v7GetF(const char* c, float* out, int n, float lim = 20.f) {
    for (int i = 0; i < n; i++) {
        char* e = nullptr;
        const float v = std::strtof(c, &e);
        if (e == c || !std::isfinite(v)) return false;
        out[i] = v < -lim ? -lim : (v > lim ? lim : v);
        c = e;
    }
    return true;
}
template <int I, int H, int O>
void v7PutNet(std::string& o, const char* nm, const V7Mlp<I, H, O>& n) {
    char b[48];
    std::snprintf(b, sizeof b, "v7p %s %d", nm, n.trained); o += b; v7PutF(o, n.p, V7Mlp<I, H, O>::NP);
    std::snprintf(b, sizeof b, "v7a %s", nm); o += b; v7PutF(o, n.anchor, V7Mlp<I, H, O>::NP);
    std::snprintf(b, sizeof b, "v7f %s", nm); o += b; v7PutF(o, n.fisher, V7Mlp<I, H, O>::NP);
}
}

std::string PitchEngine::v7Export() const {
    const V7Mem& M = v7m_;
    std::string o = "v7 1\n";
    char b[96];
    std::snprintf(b, sizeof b, "v7m %d %d %d\n", M.matches, M.matchesRejected, M.trainedMatches); o += b;
    v7PutNet(o, "pol", M.policy); v7PutNet(o, "seq", M.seqNet); v7PutNet(o, "def", M.defNet); v7PutNet(o, "meta", M.metaNet);
    o += "v7es"; v7PutF(o, M.esc, 7);
    for (int k = 0; k < 3; k++) for (int p = 0; p < 3; p++) for (int a = 0; a < 5; a++)
        if (M.timingN[k][p][a] > 0) { std::snprintf(b, sizeof b, "v7tq %d %d %d %.4f %d\n", k, p, a, (double)M.timingQ[k][p][a], M.timingN[k][p][a]); o += b; }
    for (int k = 0; k < 6; k++) for (int r = 0; r < 3; r++)
        if (M.spN[k][r] > 0 || M.spW[k][r] != 1.f) { std::snprintf(b, sizeof b, "v7sp %d %d %.4f %d\n", k, r, (double)M.spW[k][r], M.spN[k][r]); o += b; }
    for (int i = 0; i < M.nEmb; i++) {
        std::snprintf(b, sizeof b, "v7em %u %d", (unsigned)M.emb[i].key, M.emb[i].n); o += b; v7PutF(o, M.emb[i].e, V7_EMB_D);
    }
    std::snprintf(b, sizeof b, "v7ckn %d %d\n", M.nCkpt, M.ckptHead); o += b;
    for (int i = 0; i < M.nCkpt; i++) { std::snprintf(b, sizeof b, "v7ck %d", i); o += b; v7PutF(o, M.ckpt[i], V7Policy::NP); }
    std::snprintf(b, sizeof b, "v7fpn %d %d\n", M.nFp, M.fpHead); o += b;
    for (int i = 0; i < M.nFp; i++) { std::snprintf(b, sizeof b, "v7fp %d", i); o += b; v7PutF(o, M.fp[i], V7_FP); }
    for (const V7MatchRec& m : M.replay) {
        o += "v7rm\n";
        for (const V7Tup& t : m.tup) {
            std::snprintf(b, sizeof b, "v7t %d %.2f", t.cls, (double)t.r); o += b; v7PutF(o, t.f, V7_NIN, " %.3f");
        }
    }
    for (const V7SeqSample& z : M.seqRep) { std::snprintf(b, sizeof b, "v7s %.2f", (double)z.y); o += b; v7PutF(o, z.f, 8, " %.3f"); }
    for (const V7EscSample& z : M.escRep) { std::snprintf(b, sizeof b, "v7e %.2f", (double)z.y); o += b; v7PutF(o, z.f, 7, " %.3f"); }
    for (const V7Tup& t : M.defRep) { std::snprintf(b, sizeof b, "v7d %d %.2f", t.cls, (double)t.r); o += b; v7PutF(o, t.f, 6, " %.3f"); }
    if (cfg_.v8) o += v8Export();
    return o;
}

bool PitchEngine::v7Import(const std::string& str) {
    if (str.compare(0, 5, "v7 1\n") != 0) return false;
    V7Mem T;
    T.clear();
    bool bad = false;
    size_t pos = 0;
    auto netOf = [&](const std::string& nm, int* np, float** p, float** a, float** f, int** tr) -> bool {
        if (nm == "pol") { *np = V7Policy::NP; *p = T.policy.p; *a = T.policy.anchor; *f = T.policy.fisher; *tr = &T.policy.trained; return true; }
        if (nm == "seq") { *np = V7Mlp<8, 6, 1>::NP; *p = T.seqNet.p; *a = T.seqNet.anchor; *f = T.seqNet.fisher; *tr = &T.seqNet.trained; return true; }
        if (nm == "def") { *np = V7Mlp<6, 6, 3>::NP; *p = T.defNet.p; *a = T.defNet.anchor; *f = T.defNet.fisher; *tr = &T.defNet.trained; return true; }
        if (nm == "meta") { *np = V7Mlp<8, 8, 5>::NP; *p = T.metaNet.p; *a = T.metaNet.anchor; *f = T.metaNet.fisher; *tr = &T.metaNet.trained; return true; }
        return false;
    };
    while (pos < str.size() && !bad) {
        size_t e = str.find('\n', pos);
        if (e == std::string::npos) e = str.size();
        const std::string ln = str.substr(pos, e - pos);
        pos = e + 1;
        const char* c = ln.c_str();
        char nm[16];
        if (ln.compare(0, 4, "v7m ") == 0) {
            std::sscanf(c, "v7m %d %d %d", &T.matches, &T.matchesRejected, &T.trainedMatches);
        } else if (ln.compare(0, 4, "v7p ") == 0 || ln.compare(0, 4, "v7a ") == 0 || ln.compare(0, 4, "v7f ") == 0) {
            int tr = 0, off = 0;
            if (ln[2] == 'p') { if (std::sscanf(c + 4, "%15s %d%n", nm, &tr, &off) < 2) { bad = true; continue; } }
            else { if (std::sscanf(c + 4, "%15s%n", nm, &off) < 1) { bad = true; continue; } }
            int np; float *p, *a, *f; int* trp;
            if (!netOf(nm, &np, &p, &a, &f, &trp)) { bad = true; continue; }
            float* dst = ln[2] == 'p' ? p : (ln[2] == 'a' ? a : f);
            if (!v7GetF(c + 4 + off, dst, np)) { bad = true; continue; }
            if (ln[2] == 'p') *trp = std::max(0, tr);
        } else if (ln.compare(0, 5, "v7es ") == 0) {
            if (!v7GetF(c + 5, T.esc, 7, 4.f)) bad = true;
        } else if (ln.compare(0, 5, "v7tq ") == 0) {
            int k, p, a, n; float q;
            if (std::sscanf(c, "v7tq %d %d %d %f %d", &k, &p, &a, &q, &n) == 5 && k >= 0 && k < 3 && p >= 0 && p < 3 && a >= 0 && a < 5 && std::isfinite(q)) { T.timingQ[k][p][a] = clampf(q, -1.f, 1.f); T.timingN[k][p][a] = std::max(0, n); }
        } else if (ln.compare(0, 5, "v7sp ") == 0) {
            int k, r, n; float w;
            if (std::sscanf(c, "v7sp %d %d %f %d", &k, &r, &w, &n) == 4 && k >= 0 && k < 6 && r >= 0 && r < 3 && std::isfinite(w)) { T.spW[k][r] = clampf(w, 0.2f, 6.f); T.spN[k][r] = std::max(0, n); }
        } else if (ln.compare(0, 5, "v7em ") == 0 && T.nEmb < V7_EMB_N) {
            unsigned key; int n, off = 0;
            if (std::sscanf(c, "v7em %u %d%n", &key, &n, &off) >= 2 && v7GetF(c + off, T.emb[T.nEmb].e, V7_EMB_D, 1.f)) { T.emb[T.nEmb].key = key; T.emb[T.nEmb].n = std::max(0, n); T.nEmb++; }
        } else if (ln.compare(0, 6, "v7ckn ") == 0) {
            std::sscanf(c, "v7ckn %d %d", &T.nCkpt, &T.ckptHead);
            T.nCkpt = std::min(std::max(T.nCkpt, 0), V7_CKPT); T.ckptHead = ((T.ckptHead % V7_CKPT) + V7_CKPT) % V7_CKPT;
        } else if (ln.compare(0, 5, "v7ck ") == 0) {
            int i, off = 0;
            if (std::sscanf(c, "v7ck %d%n", &i, &off) >= 1 && i >= 0 && i < V7_CKPT) { if (!v7GetF(c + off, T.ckpt[i], V7Policy::NP)) bad = true; }
        } else if (ln.compare(0, 6, "v7fpn ") == 0) {
            std::sscanf(c, "v7fpn %d %d", &T.nFp, &T.fpHead);
            T.nFp = std::min(std::max(T.nFp, 0), V7_FPN); T.fpHead = ((T.fpHead % V7_FPN) + V7_FPN) % V7_FPN;
        } else if (ln.compare(0, 5, "v7fp ") == 0) {
            int i, off = 0;
            if (std::sscanf(c, "v7fp %d%n", &i, &off) >= 1 && i >= 0 && i < V7_FPN) { if (!v7GetF(c + off, T.fp[i], V7_FP, 2.f)) bad = true; }
        } else if (ln == "v7rm") {
            if (T.replay.size() < (size_t)V7_REPLAY) T.replay.push_back(V7MatchRec());
        } else if (ln.compare(0, 4, "v7t ") == 0 && !T.replay.empty()) {
            V7Tup t; int off = 0;
            if (std::sscanf(c, "v7t %d %f%n", &t.cls, &t.r, &off) >= 2 && t.cls >= 0 && t.cls < V7_NCLS && std::isfinite(t.r) && v7GetF(c + off, t.f, V7_NIN, 2.f)) {
                if (T.replay.back().tup.size() < (size_t)V7_TUP_MATCH) T.replay.back().tup.push_back(t);
            }
        } else if (ln.compare(0, 4, "v7s ") == 0) {
            V7SeqSample z; int off = 0;
            if (std::sscanf(c, "v7s %f%n", &z.y, &off) >= 1 && std::isfinite(z.y) && v7GetF(c + off, z.f, 8, 2.f) && T.seqRep.size() < (size_t)V7_SEQ_CAP) T.seqRep.push_back(z);
        } else if (ln.compare(0, 4, "v7e ") == 0) {
            V7EscSample z; int off = 0;
            if (std::sscanf(c, "v7e %f%n", &z.y, &off) >= 1 && std::isfinite(z.y) && v7GetF(c + off, z.f, 7, 2.f) && T.escRep.size() < (size_t)V7_ESC_CAP) T.escRep.push_back(z);
        } else if (ln.compare(0, 4, "v7d ") == 0) {
            V7Tup t; int off = 0;
            if (std::sscanf(c, "v7d %d %f%n", &t.cls, &t.r, &off) >= 2 && t.cls >= 0 && t.cls < 3 && std::isfinite(t.r) && v7GetF(c + off, t.f, 6, 2.f) && T.defRep.size() < 600) T.defRep.push_back(t);
        }
    }
    if (bad) return false;
    T.curSig = v7m_.curSig;
    v7m_ = T;
    if (cfg_.v8) v8Import(str);
    return true;
}
