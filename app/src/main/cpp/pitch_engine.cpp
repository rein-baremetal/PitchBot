#include "pitch_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef PITCHBOT_WITH_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>
#endif

namespace pe {

namespace {

constexpr float PI_F = 3.14159265358979f;

inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline bool finite(float v) { return std::isfinite(v); }
inline float safe(float v, float fallback) { return finite(v) ? v : fallback; }
inline float logistic(float z) {
    z = clampf(z, -30.f, 30.f);
    return 1.f / (1.f + std::exp(-z));
}

inline void rgb2hsv(int r, int g, int b, int& h, int& s, int& v) {
    const int mx = std::max(r, std::max(g, b));
    const int mn = std::min(r, std::min(g, b));
    const int d = mx - mn;
    v = mx;
    s = mx > 0 ? (d * 255) / mx : 0;
    if (d == 0) {
        h = 0;
        return;
    }
    int hh;
    if (mx == r) hh = (60 * (g - b)) / d;
    else if (mx == g) hh = 120 + (60 * (b - r)) / d;
    else hh = 240 + (60 * (r - g)) / d;
    if (hh < 0) hh += 360;
    if (hh >= 360) hh -= 360;
    h = hh;
}

}

float MetricVector2D::length() const { return std::sqrt(x * x + y * y); }

float distance(const MetricVector2D& a, const MetricVector2D& b) { return (a - b).length(); }

float segmentDistance(const MetricVector2D& a, const MetricVector2D& b, const MetricVector2D& p) {
    const MetricVector2D ab = b - a;
    const float l2 = ab.dot(ab);
    if (!(l2 > 1e-9f)) return distance(a, p);
    const float t = clampf((p - a).dot(ab) / l2, 0.f, 1.f);
    return distance(a + ab * t, p);
}

double hungarianSolve(const float* cost, int n, int* rowToCol) {
    if (n <= 0) return 0.0;
    if (n > HUNGARIAN_MAX) n = HUNGARIAN_MAX;
    constexpr double INF = 1e18;
    double u[HUNGARIAN_MAX + 1], v[HUNGARIAN_MAX + 1], minv[HUNGARIAN_MAX + 1];
    int p[HUNGARIAN_MAX + 1], way[HUNGARIAN_MAX + 1];
    bool used[HUNGARIAN_MAX + 1];
    for (int i = 0; i <= n; i++) {
        u[i] = v[i] = 0.0;
        p[i] = way[i] = 0;
    }
    for (int i = 1; i <= n; i++) {
        p[0] = i;
        int j0 = 0;
        for (int j = 0; j <= n; j++) {
            minv[j] = INF;
            used[j] = false;
        }
        do {
            used[j0] = true;
            const int i0 = p[j0];
            double delta = INF;
            int j1 = 0;
            for (int j = 1; j <= n; j++) {
                if (used[j]) continue;
                double c = cost[(i0 - 1) * n + (j - 1)];
                if (!std::isfinite(c)) c = 1e6;
                const double cur = c - u[i0] - v[j];
                if (cur < minv[j]) {
                    minv[j] = cur;
                    way[j] = j0;
                }
                if (minv[j] < delta) {
                    delta = minv[j];
                    j1 = j;
                }
            }
            for (int j = 0; j <= n; j++) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }
    double total = 0.0;
    for (int j = 1; j <= n; j++) {
        rowToCol[p[j] - 1] = j - 1;
        double c = cost[(p[j] - 1) * n + (j - 1)];
        if (std::isfinite(c)) total += c;
    }
    return total;
}

bool Homography::fromQuad(const float* s) {
    const double dst[8] = {-PITCH_HALF_X, -PITCH_HALF_Y, PITCH_HALF_X, -PITCH_HALF_Y,
                           PITCH_HALF_X,  PITCH_HALF_Y,  -PITCH_HALF_X, PITCH_HALF_Y};
    double a[8][9];
    for (int i = 0; i < 4; i++) {
        const double x = s[i * 2], y = s[i * 2 + 1];
        const double X = dst[i * 2], Y = dst[i * 2 + 1];
        double* r0 = a[i * 2];
        double* r1 = a[i * 2 + 1];
        r0[0] = x; r0[1] = y; r0[2] = 1; r0[3] = 0; r0[4] = 0; r0[5] = 0; r0[6] = -X * x; r0[7] = -X * y; r0[8] = X;
        r1[0] = 0; r1[1] = 0; r1[2] = 0; r1[3] = x; r1[4] = y; r1[5] = 1; r1[6] = -Y * x; r1[7] = -Y * y; r1[8] = Y;
    }
    for (int c = 0; c < 8; c++) {
        int piv = c;
        for (int r = c + 1; r < 8; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-9) return false;
        if (piv != c)
            for (int k = 0; k < 9; k++) std::swap(a[piv][k], a[c][k]);
        for (int r = 0; r < 8; r++) {
            if (r == c) continue;
            const double f = a[r][c] / a[c][c];
            for (int k = c; k < 9; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int i = 0; i < 8; i++) h[i] = a[i][8] / a[i][i];
    h[8] = 1.0;
    return true;
}

MetricVector2D Homography::apply(float px, float py) const {
    double den = h[6] * px + h[7] * py + h[8];
    if (std::fabs(den) < 1e-12) den = den < 0 ? -1e-12 : 1e-12;
    return {(float)((h[0] * px + h[1] * py + h[2]) / den), (float)((h[3] * px + h[4] * py + h[5]) / den)};
}

xTGrid::xTGrid() { build(); }

float xTGrid::shotValue(float ax, float y) {
    if (!finite(ax) || !finite(y)) return 0.f;
    const float dxg = std::max(PITCH_HALF_X - ax, 0.5f);
    const float dist = std::sqrt(dxg * dxg + y * y);
    float ang = std::atan2(7.32f * dxg, dxg * dxg + y * y - 3.66f * 3.66f);
    if (ang < 0) ang += PI_F;
    const float angF = std::min(ang / 1.2f, 1.0f);
    return clampf(0.45f * std::exp(-dist / 11.0f) * angF, 0.f, 0.6f);
}

void xTGrid::build() {
    const float cw = 2.f * PITCH_HALF_X / XT_COLS;
    const float ch = 2.f * PITCH_HALF_Y / XT_ROWS;
    for (int r = 0; r < XT_ROWS; r++) {
        for (int c = 0; c < XT_COLS; c++) {
            const float ax = -PITCH_HALF_X + (c + 0.5f) * cw;
            const float y = -PITCH_HALF_Y + (r + 0.5f) * ch;
            const float g = shotValue(ax, y);
            const float prog = (ax + PITCH_HALF_X) / (2.f * PITCH_HALF_X);
            const float central = 1.f - 0.35f * std::fabs(y) / PITCH_HALF_Y;
            xg[r][c] = g;
            xt[r][c] = g + (1.f - g) * 0.08f * std::pow(prog, 2.5f) * central;
        }
    }
}

void xTGrid::setValues(const float* v) {
    if (!v) return;
    for (int r = 0; r < XT_ROWS; r++)
        for (int c = 0; c < XT_COLS; c++) xt[r][c] = clampf(safe(v[r * XT_COLS + c], 0.f), 0.f, 1.f);
}

int xTGrid::colOf(float ax) {
    if (!finite(ax)) return 0;
    const int c = (int)std::floor((ax + PITCH_HALF_X) / (2.f * PITCH_HALF_X / XT_COLS));
    return c < 0 ? 0 : (c >= XT_COLS ? XT_COLS - 1 : c);
}

int xTGrid::rowOf(float y) {
    if (!finite(y)) return 0;
    const int r = (int)std::floor((y + PITCH_HALF_Y) / (2.f * PITCH_HALF_Y / XT_ROWS));
    return r < 0 ? 0 : (r >= XT_ROWS ? XT_ROWS - 1 : r);
}

float xTGrid::valueAt(float ax, float y) const { return xt[rowOf(y)][colOf(ax)]; }
float xTGrid::xgAt(float ax, float y) const { return xg[rowOf(y)][colOf(ax)]; }

int64_t GestureOutput::pack() const {
    const int64_t act = actionType & 0xF;
    const int64_t dur = (int64_t)clampf((float)durationMs, 0.f, 4095.f) & 0xFFF;
    const int64_t rr = (int64_t)std::lround(clampf(safe(r, 0.f), 0.f, 1.f) * 255.f) & 0xFF;
    float th = safe(theta, 0.f);
    th = std::fmod(th, 360.f);
    if (th < 0) th += 360.f;
    const int64_t t10 = (int64_t)clampf(std::round(th * 10.f), 0.f, 3599.f) & 0xFFF;
    return act | (dur << 4) | (rr << 16) | (t10 << 24) | ((int64_t)(valid ? 1 : 0) << 36) |
           ((int64_t)(status & 3) << 37) | ((int64_t)(dash ? 1 : 0) << 39) | ((int64_t)(flick & 3) << 40);
}

GestureOutput GestureOutput::unpack(int64_t v) {
    GestureOutput g;
    g.actionType = (int)(v & 0xF);
    g.durationMs = (int)((v >> 4) & 0xFFF);
    g.r = (float)((v >> 16) & 0xFF) / 255.f;
    g.theta = (float)((v >> 24) & 0xFFF) / 10.f;
    g.valid = ((v >> 36) & 1) != 0;
    g.status = (int)((v >> 37) & 3);
    g.dash = ((v >> 39) & 1) != 0;
    g.flick = (int)((v >> 40) & 3);
    return g;
}

PitchEngine::PitchEngine() {
    cls_.assign(MAX_PIXELS, 0);
    ero_.assign(MAX_PIXELS, 0);
    ym_.assign(MAX_PIXELS, 0);
    ym2_.assign(MAX_PIXELS, 0);
    tint_.assign(MAX_PIXELS, 0);
    stamp_.assign(MAX_PIXELS, 0);
    ymu_.assign(MAX_PIXELS, 0);
    stack_.assign(MAX_PIXELS, 0);
    std::memset(hCost_, 0, sizeof(hCost_));
    std::memset(hAssign_, 0, sizeof(hAssign_));
    for (int i = 0; i < MAX_DET; i++) detToTrack_[i] = -1;
    attLog_.reserve(ATT_LOG_MAX);
    reset();
}

void PitchEngine::setConfig(const TacticalConfig& c) {
    cfg_ = c;
    cfg_.tempMin = std::max(cfg_.tempMin, 0.01f);
    rng_ = cfg_.seed ? cfg_.seed : 1u;
}

void PitchEngine::reset() {
    state_ = MatchState();
    hudPoss_ = 0;
    hudPossMs_ = -1e9;
    ctrlSeen_ = false;
    ctrlSeenMs_ = -1e9;
    defNextAt_ = 0;
    defSwitchAt_ = defTackleAt_ = defMatchAt_ = -1e9;
    oppSinceMs_ = -1;
    for (int i = 0; i < TEAM_SIZE; i++) pend_[i] = Pending();
    for (int i = 0; i < TEAM_SIZE; i++) {
        state_.team[i].id = i + 1;
        state_.team[i].team = TEAM_HOME;
        state_.opp[i].id = 101 + i;
        state_.opp[i].team = TEAM_AWAY;
    }
    state_.temperature = cfg_.tempHold;
    signInit_ = false;
    dirEma_ = 0.0;
    signCand_ = 1;
    signRun_ = 0;
    signLockMs_ = -1e9;
    ballAssumed_ = false;
    owner_ = candOwner_ = candRun_ = 0;
    ownerSinceMs_ = 0;
    lastBallSeenMs_ = -1e9;
    counterUntilMs_ = -1e9;
    ballWasKnown_ = false;
    lastOwnerForCounter_ = 0;
    nextDecisionAt_ = cooldownUntil_ = 0;
    nSnap_ = 0; snapOk_ = false; snapShotWindow_ = false; snapCarrier_ = -1; snapT_ = flightT_ = -1e9; flight_ = false; lastCarrierIdx_ = -1;
    liveFrames_ = notLiveFrames_ = 0;
    pendingCue_ = 0;
    cueArmed_ = true;
    shotArmed_ = true; shotRun_ = 0; shotPending_ = false;
    cueRun_ = cueLevelNow_ = 0;
    lastCueMs_ = -1e9;
    cueHoldUntil_ = 0;
    lostSinceMs_ = -1;
    cueAimY_ = 0.f;
    holding_ = false;
    ballHoldStartMs_ = -1;
    crowdSinceMs_ = -1;
    possShare_ = 0.5f;
    possOwnedSec_ = 0.f;
    opp_ = OppModel();
    nCommit_ = 0;
    prevFrameMs_ = 0;
    nCand_ = 0;
    chosen_ = -1;
    gesture_ = GestureOutput();
    turnStartMs_ = -1;
    turnTargetId_ = -1;
    restartUntilMs_ = -1e9;
    notLiveSinceMs_ = -1;
    oppCarrier_ = prevOppCarrier_ = -1;
    oppPassMs_ = lineStepUntil_ = shotFiredMs_ = lastAntMs_ = lastActMs_ = -1e9;
    secondBallUntil_ = postLossUntil_ = pressCloseSince_ = -1e9;
    shotSeq_ = 0;
    defConeAt_ = defStepAt_ = -1e9;
    rollAtMs_ = -1e9;
    aimDirOn_ = false;
    antLastRecv_ = -1;
    switchIdx_ = -1;
    lastBlkReason_ = 0;
    lastBlkMs_ = -1e9;
    lostBallMs_ = -1e9;
    cpCounted_ = false;
    cpSwitchAt_ = -1e9;
    looseNextAt_ = 0;
    haveCentroid_ = false;
    prevRawN_ = 0;
    jumpRun_ = 0;
    stuckSinceMs_ = -1;
    stuckAction_ = -1;
    plus_.clear();
    meta_.clear();
    plan_.clear();
    v7_.clear();
    v8_.clear();
    rng_ = cfg_.seed ? cfg_.seed : 1u;
}

void PitchEngine::setBufferLayout(int rowStride, int pixelStride, bool bgr) {
    rowStride_ = rowStride;
    pixelStride_ = pixelStride < 3 ? 4 : pixelStride;
    bgr_ = bgr;
}

bool PitchEngine::setHomographyQuad(const float* quad8) {
    Homography t;
    if (!quad8 || !t.fromQuad(quad8)) return false;
    std::memcpy(quad_, quad8, sizeof(quad_));
    customQuad_ = true;
    hom_ = t;
    homW_ = homH_ = 0;
    return true;
}

void PitchEngine::clearHomographyQuad() {
    customQuad_ = false;
    homW_ = homH_ = 0;
}

void PitchEngine::rebuildHomography(int w, int h) {
    if (customQuad_) {
        hom_.fromQuad(quad_);
        homW_ = w;
        homH_ = h;
        return;
    }
    if (w == homW_ && h == homH_) return;
    const float q[8] = {0.f, 0.f, (float)w, 0.f, (float)w, (float)h, 0.f, (float)h};
    hom_.fromQuad(q);
    homW_ = w;
    homH_ = h;
}

namespace {
constexpr int kChromaSatMin = 50;
constexpr int kChromaValMin = 50;
constexpr int kNoiseFloorS = 30;
constexpr int kNoiseFloorV = 30;

inline int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
inline int hueDiffCv(int a, int b) {
    int d = std::abs(a - b);
    return d > 90 ? 180 - d : d;
}
}

bool HsvRange::contains(int h, int s, int v) const {
    if (!set) return false;
    if (s < (int)lower.v[1] || s > (int)upper.v[1] || v < (int)lower.v[2] || v > (int)upper.v[2]) return false;
    if (!chroma) return true;
    const int lo = (int)lower.v[0], hi = (int)upper.v[0];
    return wrapHue ? (h >= lo || h <= hi) : (h >= lo && h <= hi);
}

void HsvRange::pack(int* o) const {
    if (!o) return;
    o[0] = set ? 1 : 0;
    o[1] = chroma ? 1 : 0;
    o[2] = h0;
    o[3] = s0;
    o[4] = v0;
    o[5] = (int)lower.v[0];
    o[6] = (int)lower.v[1];
    o[7] = (int)lower.v[2];
    o[8] = (int)upper.v[0];
    o[9] = (int)upper.v[1];
    o[10] = (int)upper.v[2];
}

bool HsvRange::unpack(const int* in) {
    if (!in) return false;
    if (in[0] == 0) {
        *this = HsvRange();
        return true;
    }
    if (in[0] != 1 || (in[1] != 0 && in[1] != 1)) return false;
    const bool hueOk = in[2] >= 0 && in[2] <= 179 && in[5] >= 0 && in[5] <= 179 && in[8] >= 0 && in[8] <= 179;
    bool svOk = true;
    for (int i : {3, 4, 6, 7, 9, 10}) svOk = svOk && in[i] >= 0 && in[i] <= 255;
    if (!hueOk || !svOk || in[6] > in[9] || in[7] > in[10]) return false;
    HsvRange r;
    r.set = true;
    r.chroma = in[1] == 1;
    r.h0 = in[2];
    r.s0 = in[3];
    r.v0 = in[4];
    r.lower = HsvScalar(in[5], in[6], in[7]);
    r.upper = HsvScalar(in[8], in[9], in[10]);
    r.wrapHue = r.chroma && in[5] > in[8];
    *this = r;
    return true;
}

HsvRange PitchEngine::hsvRangeFromRgb(int rgb, int dH, int dS, int dV) {
    dH = clampi(dH, 0, 90);
    dS = clampi(dS, 0, 255);
    dV = clampi(dV, 0, 255);
    int h, s, v;
    rgb2hsv((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, h, s, v);
    h = clampi(h / 2, 0, 179);

    HsvRange o;
    o.set = true;
    o.h0 = h;
    o.s0 = s;
    o.v0 = v;
    o.chroma = s >= kChromaSatMin && v >= kChromaValMin;

    int sLo = clampi(s - dS, 0, 255), sHi = clampi(s + dS, 0, 255);
    int vLo = clampi(v - dV, 0, 255), vHi = clampi(v + dV, 0, 255);
    int hLo = 0, hHi = 179;
    if (o.chroma) {
        sLo = std::max(sLo, kNoiseFloorS);
        vLo = std::max(vLo, kNoiseFloorV);
        hLo = h - dH;
        hHi = h + dH;
        if (hLo < 0) {
            hLo += 180;
            o.wrapHue = true;
        }
        if (hHi > 179) {
            hHi -= 180;
            o.wrapHue = true;
        }
    }
    o.lower = HsvScalar(hLo, sLo, vLo);
    o.upper = HsvScalar(hHi, sHi, vHi);
    return o;
}

HsvRange PitchEngine::defaultBallRange() {
    HsvRange r;
    r.set = true;
    r.chroma = true;
    r.wrapHue = false;
    r.h0 = 28;
    r.s0 = 200;
    r.v0 = 230;
    r.lower = HsvScalar(20, 100, 150);
    r.upper = HsvScalar(35, 255, 255);
    return r;
}

namespace {
inline int colorScore(const HsvRange& sp, int hh, int ss, int vv) {
    return sp.chroma ? hueDiffCv(hh, sp.h0) * 4 + (std::abs(ss - sp.s0) + std::abs(vv - sp.v0)) / 2
                     : std::abs(vv - sp.v0) + ss;
}
}

float PitchEngine::windowWorstScore(const HsvRange& r) const {
    if (!r.set) return 0.f;
    const int sSpan = std::max(r.s0 - (int)r.lower.v[1], (int)r.upper.v[1] - r.s0);
    const int vSpan = std::max(r.v0 - (int)r.lower.v[2], (int)r.upper.v[2] - r.v0);
    if (r.chroma) {
        const int hSpan = std::max(hueDiffCv(r.h0, (int)r.lower.v[0]), hueDiffCv(r.h0, (int)r.upper.v[0]));
        return (float)(hSpan * 4 + (std::max(sSpan, 0) + std::max(vSpan, 0)) / 2);
    }
    return (float)(std::max(vSpan, 0) + (int)r.upper.v[1]);
}

void PitchEngine::updateKitOverlap() {
    yellowKit_ = false;
    const HsvRange& b = spec_[TEAM_BALL];
    if (!b.set) return;
    for (int k = 0; k < 2; k++) {
        const HsvRange& t = spec_[k];
        if (!t.set) continue;
        if (b.contains(t.h0, t.s0, t.v0) || t.contains(b.h0, b.s0, b.v0)) yellowKit_ = true;
    }
}

bool PitchEngine::setTeamColors(int homeRGB, int awayRGB) {
    const HsvRange home = hsvRangeFromRgb(homeRGB & 0xFFFFFF);
    const HsvRange away = hsvRangeFromRgb(awayRGB & 0xFFFFFF);
    if (home.contains(away.h0, away.s0, away.v0) && away.contains(home.h0, home.s0, home.v0)) return false;
    spec_[TEAM_HOME] = home;
    spec_[TEAM_AWAY] = away;
    if (!ballCustom_) spec_[TEAM_BALL] = defaultBallRange();
    haveColors_ = true;
    {   // opponent signature: the away kit colour in 3 bits per channel
        const unsigned c = (unsigned)awayRGB;
        v7m_.curSig = (((c >> 21) & 7u) << 6) | (((c >> 13) & 7u) << 3) | ((c >> 5) & 7u);
        v7_.embLoaded = false;
    }
    updateKitOverlap();
    return true;
}

void PitchEngine::setBallColor(int ballRGB) {
    spec_[TEAM_BALL] = hsvRangeFromRgb(ballRGB & 0xFFFFFF);
    ballCustom_ = true;
    updateKitOverlap();
}

void PitchEngine::clearBallColor() {
    spec_[TEAM_BALL] = defaultBallRange();
    ballCustom_ = false;
    updateKitOverlap();
}

void PitchEngine::setColors(int teamRGB, int oppRGB, int ballRGB) {
    if (setTeamColors(teamRGB, oppRGB)) setBallColor(ballRGB);
}

void PitchEngine::kitBounds(int* out) const {
    if (!out) return;
    for (int k = 0; k < 3; k++) spec_[k].pack(out + k * HSV_PACK_INTS);
}

bool PitchEngine::setKitBounds(const int* in) {
    if (!in) return false;
    HsvRange t[3];
    for (int k = 0; k < 3; k++)
        if (!t[k].unpack(in + k * HSV_PACK_INTS)) return false;
    if (!t[TEAM_HOME].set || !t[TEAM_AWAY].set) return false;
    for (int k = 0; k < 3; k++) spec_[k] = t[k];
    if (!spec_[TEAM_BALL].set) spec_[TEAM_BALL] = defaultBallRange();
    ballCustom_ = true;
    haveColors_ = true;
    updateKitOverlap();
    return true;
}

void PitchEngine::segment(const unsigned char* buf, int w, int h) {
    const int n = w * h;
    const int stride = rowStride_ > 0 ? rowStride_ : w * pixelStride_;
    const int ri = bgr_ ? 2 : 0, bi = bgr_ ? 0 : 2;
    uint8_t* cls = cls_.data();
    uint8_t* ero = ero_.data();
    const bool ballSet = spec_[TEAM_BALL].set;

    for (int y = 0; y < h; y++) {
        const unsigned char* row = buf + (size_t)y * stride;
        uint8_t* out = cls + y * w;
        for (int x = 0; x < w; x++) {
            const unsigned char* px = row + (size_t)x * pixelStride_;
            int hh, ss, vv;
            rgb2hsv(px[ri], px[1], px[bi], hh, ss, vv);
            hh >>= 1;
            int best = 0;
            int bestScore = 1 << 30;
            ym_[(size_t)y * w + x] = ballSet && spec_[TEAM_BALL].contains(hh, ss, vv) ? 1 : 0;
            tint_[(size_t)y * w + x] = (ballSet && hh >= 28 && hh < 80 && ss >= 77 && vv >= 184) ? 1 : 0;
            for (int k = 0; k < 2; k++) {
                const HsvRange& sp = spec_[k];
                if (!sp.contains(hh, ss, vv)) continue;
                const int score = colorScore(sp, hh, ss, vv);
                if (score < bestScore) {
                    bestScore = score;
                    best = k + 1;
                }
            }
            out[x] = (uint8_t)best;
        }
    }

    if (cfg_.erode) {
        std::memset(ero, 0, (size_t)n);
        for (int y = 1; y < h - 1; y++) {
            for (int x = 1; x < w - 1; x++) {
                const int i = y * w + x;
                const uint8_t c = cls[i];
                if (!c) continue;
                const uint8_t* ymk = ym_.data();
                auto sup = [&](int j) { return cls[j] == c || ymk[j] != 0; };
                if (sup(i - 1) && sup(i + 1) && sup(i - w) && sup(i + w)) ero[i] = c;
            }
        }
    } else {
        std::memcpy(ero, cls, (size_t)n);
    }

    nDets_[0] = nDets_[1] = nDets_[2] = 0;
    int32_t* st = stack_.data();
    const float worst[2] = {windowWorstScore(spec_[TEAM_HOME]), windowWorstScore(spec_[TEAM_AWAY])};
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int start = y * w + x;
            const uint8_t c = ero[start];
            if (!c) continue;
            int sp = 0;
            st[sp++] = start;
            ero[start] = 0;
            double sx = 0, sy = 0, sumScore = 0, sxx = 0, syy = 0, sxy = 0;
            int area = 0;
            int bx0 = w, bx1 = -1, by0 = h, by1 = -1;
            while (sp > 0) {
                const int i = st[--sp];
                const int ix = i % w, iy = i / w;
                sx += ix + 0.5;
                sy += iy + 0.5;
                sxx += (ix + 0.5) * (ix + 0.5);
                syy += (iy + 0.5) * (iy + 0.5);
                sxy += (ix + 0.5) * (iy + 0.5);
                area++;
                if (cfg_.colorOnly) {
                    bx0 = std::min(bx0, ix);
                    bx1 = std::max(bx1, ix);
                    by0 = std::min(by0, iy);
                    by1 = std::max(by1, iy);
                    const unsigned char* qp = buf + (size_t)iy * stride + (size_t)ix * pixelStride_;
                    int qh, qs, qv;
                    rgb2hsv(qp[ri], qp[1], qp[bi], qh, qs, qv);
                    sumScore += colorScore(spec_[c - 1], qh >> 1, qs, qv);
                }
                const int nb[4] = {ix > 0 ? i - 1 : -1, ix < w - 1 ? i + 1 : -1, iy > 0 ? i - w : -1,
                                   iy < h - 1 ? i + w : -1};
                for (int k = 0; k < 4; k++) {
                    const int j = nb[k];
                    if (j >= 0 && ero[j] == c) {
                        ero[j] = 0;
                        st[sp++] = j;
                    }
                }
            }
            const int minA = cfg_.minBlobPx;
            if (area < minA || area > cfg_.maxBlobPx) continue;
            const int k = c - 1;
            float meanScore = 0.f;
            if (cfg_.colorOnly) {
                const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
                const float fill = (float)area / (float)std::max(1, bw * bh);
                const float asp = (float)std::max(bw, bh) / (float)std::max(1, std::min(bw, bh));
                if (fill < cfg_.minDotFill || asp > cfg_.maxDotAspect) continue;
                meanScore = (float)(sumScore / area);
                if (worst[k] > 0.f && meanScore > cfg_.maxMeanScore * worst[k]) continue;
            }
            if (nDets_[k] < MAX_DET) {
                Det& d = dets_[k][nDets_[k]++];
                d.x = (float)(sx / area);
                d.y = (float)(sy / area);
                d.area = (float)area;
                d.score = worst[k] > 0.f ? meanScore / worst[k] : 0.f;
                const double mx = sx / area, my = sy / area;
                const double cxx = sxx / area - mx * mx, cyy = syy / area - my * my, cxy = sxy / area - mx * my;
                const double tr = cxx + cyy, det = cxx * cyy - cxy * cxy;
                const double disc = std::sqrt(std::max(0.0, tr * tr / 4.0 - det));
                const double l1 = tr / 2.0 + disc;
                const double ang = 0.5 * std::atan2(2.0 * cxy, cxx - cyy);
                d.ax = (float)std::cos(ang);
                d.ay = (float)std::sin(ang);
                d.len = (float)(4.0 * std::sqrt(std::max(0.0, l1)));
                d.elong = d.len / std::max(0.5f, 2.f * std::sqrt((float)area / PI_F));
            }
        }
    }
    rescueHiddenDots(buf, w, h);
    rejectNonDots(buf, w, h);
    detectBall(w, h);
}

void PitchEngine::rescueHiddenDots(const unsigned char* buf, int w, int h) {
    if (!cfg_.colorOnly || !cfg_.erode || !buf) return;
    const int stride = rowStride_ > 0 ? rowStride_ : w * pixelStride_;
    const int ri = bgr_ ? 2 : 0, bi = bgr_ ? 0 : 2;
    const float worst[2] = {windowWorstScore(spec_[TEAM_HOME]), windowWorstScore(spec_[TEAM_AWAY])};
    float areas[2 * MAX_DET];
    int na = 0;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < nDets_[k] && na < 2 * MAX_DET; i++) areas[na++] = dets_[k][i].area;
    for (int i = 1; i < na; i++) {
        const float a = areas[i];
        int j = i - 1;
        while (j >= 0 && areas[j] > a) { areas[j + 1] = areas[j]; j--; }
        areas[j + 1] = a;
    }
    const float med = na >= 1 ? std::max(1.f, areas[na / 2]) : 4.f;
    const int n0[2] = {nDets_[0], nDets_[1]};

    const int n = w * h;
    const uint8_t* cls = cls_.data();
    const uint8_t* ym = ym_.data();
    uint8_t* vis = ym2_.data();
    std::memset(vis, 0, (size_t)n);
    int32_t* st = stack_.data();
    struct Cand { int k; float x, y, ax, ay, len, elong; float area; bool whole; };
    Cand cands[2 * MAX_DET];
    int nc = 0;
    struct Big { int k, x0, y0, x1, y1; float x, y; float area; };
    Big bigs[8];
    int nbig = 0;
    int addedPerTeam[2] = {0, 0};
    for (int start = 0; start < n; start++) {
        const uint8_t c = cls[start];
        if (!c || vis[start]) continue;
        const int k = c - 1;
        int sp = 0;
        st[sp++] = start;
        vis[start] = 1;
        double sx = 0, sy = 0, sumScore = 0, sxx = 0, syy = 0, sxy = 0;
        int area = 0, edge = 0;
        int x0 = w, x1 = -1, y0 = h, y1 = -1;
        while (sp > 0) {
            const int i = st[--sp];
            const int ix = i % w, iy = i / w;
            sx += ix + 0.5;
            sy += iy + 0.5;
            sxx += (ix + 0.5) * (ix + 0.5);
            syy += (iy + 0.5) * (iy + 0.5);
            sxy += (ix + 0.5) * (iy + 0.5);
            area++;
            {
                const unsigned char* qp = buf + (size_t)iy * stride + (size_t)ix * pixelStride_;
                int qh, qs, qv;
                rgb2hsv(qp[ri], qp[1], qp[bi], qh, qs, qv);
                sumScore += colorScore(spec_[k], qh >> 1, qs, qv);
            }
            x0 = std::min(x0, ix);
            x1 = std::max(x1, ix);
            y0 = std::min(y0, iy);
            y1 = std::max(y1, iy);
            const int nb[4] = {ix > 0 ? i - 1 : -1, ix < w - 1 ? i + 1 : -1, iy > 0 ? i - w : -1, iy < h - 1 ? i + w : -1};
            bool touches = false;
            for (int q = 0; q < 4; q++) {
                const int j = nb[q];
                if (j < 0) continue;
                if (cls[j] == c) {
                    if (!vis[j]) {
                        vis[j] = 1;
                        st[sp++] = j;
                    }
                } else if (cls[j] != 0 || ym[j] != 0) {
                    touches = true;
                }
            }
            if (touches) edge++;
        }
        if (nbig < 8 && area >= 24 && area <= cfg_.maxBlobPx && !(worst[k] > 0.f && (float)(sumScore / area) > cfg_.maxMeanScore * worst[k])) {
            const int gw = x1 - x0 + 1, gh = y1 - y0 + 1;
            const float gfill = (float)area / (float)std::max(1, gw * gh);
            const float gasp = (float)std::max(gw, gh) / (float)std::max(1, std::min(gw, gh));
            if (gasp <= cfg_.glowMaxAspect && gfill >= cfg_.glowMinFill) {
                Big& bg = bigs[nbig++];
                bg.k = k; bg.x0 = x0; bg.y0 = y0; bg.x1 = x1; bg.y1 = y1;
                bg.x = (float)(sx / area); bg.y = (float)(sy / area); bg.area = (float)area;
            }
        }
        if (nDets_[k] + addedPerTeam[k] >= MAX_DET || nDets_[k] + addedPerTeam[k] >= cfg_.maxPerTeam) continue;
        if (area > cfg_.maxBlobPx || nc >= 2 * MAX_DET) continue;
        if (worst[k] > 0.f && (float)(sumScore / area) > cfg_.maxMeanScore * worst[k]) continue;
        const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        const float fill = (float)area / (float)std::max(1, bw * bh);
        const float asp = (float)std::max(bw, bh) / (float)std::max(1, std::min(bw, bh));
        const bool fragment = edge >= cfg_.rescueMinEdge && (float)area <= 1.4f * med &&
                              area >= std::max(2, cfg_.minBlobPx - 1) && fill >= cfg_.minDotFill && asp <= cfg_.maxDotAspect;
        const bool loose = n0[k] < cfg_.looseBelow && area >= cfg_.looseMinPx && (float)area <= 6.f * std::max(med, 4.f) &&
                           fill >= cfg_.looseMinFill && asp <= cfg_.looseMaxAspect;
        if (!fragment && !loose) continue;
        bool covered = false;
        for (int i = 0; i < nDets_[k] && !covered; i++) {
            const Det& d = dets_[k][i];
            covered = d.x >= x0 - 1 && d.x <= x1 + 2 && d.y >= y0 - 1 && d.y <= y1 + 2;
        }
        if (covered) continue;
        Cand& cd = cands[nc++];
        cd.k = k;
        cd.x = (float)(sx / area);
        cd.y = (float)(sy / area);
        cd.area = (float)area;
        cd.whole = fragment;
        {
            const double mx = sx / area, my = sy / area;
            const double cxx = sxx / area - mx * mx, cyy = syy / area - my * my, cxy = sxy / area - mx * my;
            const double tr = cxx + cyy, det = cxx * cyy - cxy * cxy;
            const double disc = std::sqrt(std::max(0.0, tr * tr / 4.0 - det));
            const double ang = 0.5 * std::atan2(2.0 * cxy, cxx - cyy);
            cd.ax = (float)std::cos(ang);
            cd.ay = (float)std::sin(ang);
            cd.len = (float)(4.0 * std::sqrt(std::max(0.0, tr / 2.0 + disc)));
            cd.elong = cd.len / std::max(0.5f, 2.f * std::sqrt((float)area / PI_F));
        }
        addedPerTeam[k]++;
    }
    float uArea[2 * MAX_DET];
    int nu = 0;
    for (int i = 0; i < nc; i++) if (!cands[i].whole) uArea[nu++] = cands[i].area;
    for (int i = 1; i < nu; i++) {
        const float a = uArea[i];
        int j = i - 1;
        while (j >= 0 && uArea[j] > a) { uArea[j + 1] = uArea[j]; j--; }
        uArea[j + 1] = a;
    }
    const float uMed = nu > 0 ? std::max(2.f, uArea[nu / 2]) : 0.f;
    for (int b = 0; b < nbig; b++) {
        const Big& bg = bigs[b];
        const float coreR = std::sqrt(std::max(1.f, med) / PI_F);
        const float ref = uMed > 0.f ? uMed : PI_F * (coreR + 1.f) * (coreR + 1.f);
        if (bg.area < cfg_.glowMinTimes * ref) continue;
        int w2 = 0;
        for (int i = 0; i < nDets_[bg.k]; i++) {
            const Det& d = dets_[bg.k][i];
            if (d.x >= bg.x0 - 2 && d.x <= bg.x1 + 3 && d.y >= bg.y0 - 2 && d.y <= bg.y1 + 3) continue;
            dets_[bg.k][w2++] = d;
        }
        nDets_[bg.k] = w2;
        int w3 = 0;
        for (int i = 0; i < nc; i++) {
            const Cand& cd = cands[i];
            if (cd.k == bg.k && cd.x >= bg.x0 - 2 && cd.x <= bg.x1 + 3 && cd.y >= bg.y0 - 2 && cd.y <= bg.y1 + 3) continue;
            cands[w3++] = cd;
        }
        nc = w3;
        if (nc < 2 * MAX_DET) {
            Cand& g = cands[nc++];
            g.k = bg.k; g.x = bg.x; g.y = bg.y; g.ax = 1.f; g.ay = 0.f; g.len = 0.f; g.elong = 1.f; g.area = bg.area; g.whole = true;
        }
    }
    for (int i = 0; i < nc; i++) {
        const Cand& cd = cands[i];
        if (nDets_[cd.k] >= MAX_DET) continue;
        Det& d = dets_[cd.k][nDets_[cd.k]++];
        d.x = cd.x;
        d.y = cd.y;
        d.area = (cd.whole || uMed <= 0.f) ? med : med * std::max(1.f, cd.area / uMed);
        d.ax = cd.ax; d.ay = cd.ay; d.len = cd.len; d.elong = cd.elong;
        d.score = 0.5f;
    }
}

namespace {
void holeStats(const uint8_t* m, int w, int x0, int y0, int bw, int bh, int& maskPx, int& holes) {
    int rowFirst[64], rowLast[64], colFirst[64], colLast[64];
    for (int i = 0; i < 64; i++) rowFirst[i] = colFirst[i] = 1 << 20, rowLast[i] = colLast[i] = -1;
    maskPx = 0;
    holes = 0;
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            if (!m[(size_t)(y0 + y) * w + (x0 + x)]) continue;
            maskPx++;
            if (x < rowFirst[y]) rowFirst[y] = x;
            if (x > rowLast[y]) rowLast[y] = x;
            if (y < colFirst[x]) colFirst[x] = y;
            if (y > colLast[x]) colLast[x] = y;
        }
    }
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            if (m[(size_t)(y0 + y) * w + (x0 + x)]) continue;
            if (x > rowFirst[y] && x < rowLast[y] && y > colFirst[x] && y < colLast[x]) holes++;
        }
    }
}
}

void PitchEngine::rejectNonDots(const unsigned char* buf, int w, int h) {
    if (!cfg_.rejectNonDots || !cfg_.colorOnly) return;
    const int stride = rowStride_ > 0 ? rowStride_ : w * pixelStride_;
    const int ri = bgr_ ? 2 : 0, bi = bgr_ ? 0 : 2;
    auto radarLike = [&](int hh, int ss, int vv) {
        if (hh >= 60 && hh <= 160 && ss >= 64 && vv <= 191) return true;
        if (ss < 77 && vv >= 115) return true;
        if (vv < 77) return true;
        if (hh >= 170 && hh <= 205 && ss >= 115 && vv >= 140) return true;
        if (hh >= 56 && hh <= 160 && ss >= 77 && vv >= 184) return true;
        const int h2 = hh >> 1;
        for (int t = 0; t < 3; t++) if (spec_[t].set && spec_[t].contains(h2, ss, vv)) return true;
        return false;
    };
    int32_t* stk = stack_.data();
    auto discOk = [&](const Det& d, int k) -> bool {
        const HsvRange& kit = spec_[k];
        if (cfg_.dotMinFill <= 0.f || !kit.set || !kit.chroma) return true;
        {
            int ballPx = 0;
            for (int yy = (int)d.y - 6; yy <= (int)d.y + 6; yy++) {
                for (int xx = (int)d.x - 6; xx <= (int)d.x + 6; xx++) {
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    const float ex = xx + 0.5f - d.x, ey = yy + 0.5f - d.y;
                    if (ex * ex + ey * ey <= 36.f && (ym_[(size_t)yy * w + xx] || tint_[(size_t)yy * w + xx])) ballPx++;
                }
            }
            if (ballPx >= 8) return true;
        }
        auto fam = [&](int x, int y) -> bool {
            const unsigned char* px = buf + (size_t)y * stride + (size_t)x * pixelStride_;
            int hh, ss, vv;
            rgb2hsv(px[ri], px[1], px[bi], hh, ss, vv);
            int dh = std::abs((hh >> 1) - kit.h0);
            dh = std::min(dh, 180 - dh);
            return dh <= 9 && ss * 100 >= 55 * kit.s0 && vv * 100 >= 50 * kit.v0;
        };
        int sx = -1, sy = -1;
        float bd = 1e9f;
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                const int x = (int)d.x + dx, y = (int)d.y + dy;
                if (x < 0 || x >= w || y < 0 || y >= h || !fam(x, y)) continue;
                const float ex = x + 0.5f - d.x, ey = y + 0.5f - d.y;
                if (ex * ex + ey * ey < bd) { bd = ex * ex + ey * ey; sx = x; sy = y; }
            }
        }
        if (sx < 0) return true;
        if (++stampId_ == 0) { std::fill(stamp_.begin(), stamp_.end(), (uint16_t)0); stampId_ = 1; }
        int sp = 0, n = 0, minx = sx, maxx = sx, miny = sy, maxy = sy;
        double sX = 0, sY = 0, sXX = 0, sYY = 0, sXY = 0;
        stk[sp++] = sy * w + sx;
        stamp_[(size_t)sy * w + sx] = stampId_;
        while (sp > 0) {
            const int i = stk[--sp];
            const int ix = i % w, iy = i / w;
            n++;
            sX += ix; sY += iy; sXX += (double)ix * ix; sYY += (double)iy * iy; sXY += (double)ix * iy;
            minx = std::min(minx, ix); maxx = std::max(maxx, ix); miny = std::min(miny, iy); maxy = std::max(maxy, iy);
            if (n > 600) return true;
            static const int dxs[4] = {1, -1, 0, 0}, dys[4] = {0, 0, 1, -1};
            for (int q = 0; q < 4; q++) {
                const int xx = ix + dxs[q], yy = iy + dys[q];
                if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
                const size_t j = (size_t)yy * w + xx;
                if (stamp_[j] == stampId_ || !fam(xx, yy)) continue;
                stamp_[j] = stampId_;
                stk[sp++] = (int32_t)j;
            }
        }
        if (n < 8) return true;
        const float fill = (float)n / (float)((maxx - minx + 1) * (maxy - miny + 1));
        if (fill < cfg_.dotMinFill) return false;
        const double mx = sX / n, my = sY / n;
        const double vxx = sXX / n - mx * mx, vyy = sYY / n - my * my, vxy = sXY / n - mx * my;
        const double tr = vxx + vyy, det = vxx * vyy - vxy * vxy;
        const double disc = std::sqrt(std::max(0.0, tr * tr / 4.0 - det));
        const double l1 = tr / 2.0 + disc, l2 = std::max(0.05, tr / 2.0 - disc);
        const double elong = std::sqrt(std::max(l1, 0.0) / l2);
        if (elong > 2.6 && n >= 14) return false;
        return true;
    };
    for (int k = 0; k < 2; k++) {
        int keep = 0;
        for (int i = 0; i < nDets_[k]; i++) {
            const Det d = dets_[k][i];
            if (!discOk(d, k)) continue;
            const float rd = clampf(std::sqrt(std::max(d.area, 1.f) / PI_F) + 1.f, 2.f, 6.f);
            const float r0 = rd + 1.f, r1 = rd + 3.5f;
            const int y0 = std::max(0, (int)(d.y - r1) - 1), y1 = std::min(h - 1, (int)(d.y + r1) + 1);
            const int x0 = std::max(0, (int)(d.x - r1) - 1), x1 = std::min(w - 1, (int)(d.x + r1) + 1);
            int tot = 0, good = 0;
            for (int y = y0; y <= y1; y++) {
                const unsigned char* row = buf + (size_t)y * stride;
                for (int x = x0; x <= x1; x++) {
                    const float dx = x + 0.5f - d.x, dy = y + 0.5f - d.y;
                    const float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist < r0 || dist > r1) continue;
                    const unsigned char* px = row + (size_t)x * pixelStride_;
                    int hh, ss, vv;
                    rgb2hsv(px[ri], px[1], px[bi], hh, ss, vv);
                    tot++;
                    if (radarLike(hh, ss, vv)) good++;
                }
            }
            if (tot < 10 || (float)good >= cfg_.dotSurroundMin * (float)tot) dets_[k][keep++] = d;
        }
        nDets_[k] = keep;
    }
}

void PitchEngine::detectBall(int w, int h) {
    nDets_[2] = 0;
    ballRing_ = false;
    ballGlow_ = false;
    ballScore_ = 0.f;
    if (!spec_[TEAM_BALL].set || w < 16 || h < 16) return;
#ifdef PITCHBOT_WITH_OPENCV
    detectBallCv(w, h);
    return;
#endif
    const int n = w * h;
    uint8_t* dm = ym2_.data();

    const int maxBox = std::min(63, std::max(cfg_.ballMinPx + 2, (int)(cfg_.ballMaxFrac * (float)std::min(w, h))));
    int32_t* st = stack_.data();
    float bestScore = -1.f;
    float bx = 0.f, by = 0.f;
    bool bestRing = false, bestGlow = false;

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            if (bestRing || !cfg_.colorOnly || !cfg_.ballGlowTint) break;
            for (int i = 0; i < n; i++) ymu_[i] = (uint8_t)(ym_[i] | tint_[i]);
        }
        const uint8_t* ym = pass == 0 ? ym_.data() : ymu_.data();
        int evaluated = 0;

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint8_t v = 0;
                for (int dy = -1; dy <= 1 && !v; dy++) {
                    const int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    for (int dx = -1; dx <= 1; dx++) {
                        const int xx = x + dx;
                        if (xx >= 0 && xx < w && ym[(size_t)yy * w + xx]) { v = 1; break; }
                    }
                }
                dm[(size_t)y * w + x] = v;
            }
        }

        for (int start = 0; start < n; start++) {
            if (dm[start] != 1) continue;
            int sp = 0;
            st[sp++] = start;
            dm[start] = 2;
            int minx = w, maxx = -1, miny = h, maxy = -1;
            while (sp > 0) {
                const int i = st[--sp];
                const int ix = i % w, iy = i / w;
                minx = std::min(minx, ix);
                maxx = std::max(maxx, ix);
                miny = std::min(miny, iy);
                maxy = std::max(maxy, iy);
                for (int dy = -1; dy <= 1; dy++) {
                    const int yy = iy + dy;
                    if (yy < 0 || yy >= h) continue;
                    for (int dx = -1; dx <= 1; dx++) {
                        const int xx = ix + dx;
                        if (xx < 0 || xx >= w) continue;
                        const int j = yy * w + xx;
                        if (dm[j] == 1) {
                            dm[j] = 2;
                            st[sp++] = j;
                        }
                    }
                }
            }
            const int x0 = std::min(w - 1, minx + 1), y0 = std::min(h - 1, miny + 1);
            const int bw = (maxx - 1) - x0 + 1, bh = (maxy - 1) - y0 + 1;
            if (bw < cfg_.ballMinPx || bh < cfg_.ballMinPx || bw > maxBox || bh > maxBox) continue;
            const float aspect = (float)std::min(bw, bh) / (float)std::max(bw, bh);
            if (aspect < 0.6f) continue;
            if (++evaluated > 12) break;

            int px = 0, holes = 0;
            holeStats(ym, w, x0, y0, bw, bh, px, holes);
            float ratio = (px + holes) > 0 ? (float)holes / (float)(px + holes) : 0.f;
            {
                int dpx = 0, dh = 0;
                holeStats(dm, w, x0, y0, bw, bh, dpx, dh);
                const float r2 = (dpx + dh) > 0 ? (float)dh / (float)(dpx + dh) : 0.f;
                ratio = std::max(ratio, r2);
            }
            if (px < 3) continue;

            const float ccx = x0 + 0.5f * bw, ccy = y0 + 0.5f * bh;
            const float r0 = 0.5f * (float)std::max(bw, bh);
            float coverage = 0.f, innerFill = 1.f;
            for (int oy = -1; oy <= 1; oy++) {
                for (int ox = -1; ox <= 1; ox++) {
                    for (int ri = 0; ri < 4; ri++) {
                        const float rr = std::max(1.5f, r0 - 1.5f + 0.5f * ri);
                        const float cx = ccx + 0.5f * ox, cy = ccy + 0.5f * oy;
                        int votes = 0, inner = 0;
                        for (int y = y0 - 1; y <= y0 + bh; y++) {
                            if (y < 0 || y >= h) continue;
                            for (int x = x0 - 1; x <= x0 + bw; x++) {
                                if (x < 0 || x >= w || !ym[(size_t)y * w + x]) continue;
                                const float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
                                if (std::fabs(d - rr) <= 1.0f) votes++;
                                if (d < cfg_.ballInnerFrac * rr) inner++;
                            }
                        }
                        const float cov = clampf((float)votes / (2.f * PI_F * rr), 0.f, 1.f);
                        const float innerArea = std::max(1.f, PI_F * (cfg_.ballInnerFrac * rr) * (cfg_.ballInnerFrac * rr));
                        const float fill = clampf((float)inner / innerArea, 0.f, 1.f);
                        if (cov > coverage) {
                            coverage = cov;
                            innerFill = fill;
                        }
                    }
                }
            }

            const bool cleanRing = ratio >= cfg_.ballMinHoleRatio && coverage >= cfg_.ballMinRingCoverage &&
                                   innerFill <= cfg_.ballMaxInnerFill;
            const bool glowRing = cfg_.colorOnly && ratio >= cfg_.ballGlowHoleRatio && coverage >= cfg_.ballGlowCoverage;
            const bool discRing = cfg_.colorOnly && !yellowKit_ && px >= cfg_.ballDiscMinPx && coverage >= cfg_.ballDiscCoverage &&
                                  innerFill <= cfg_.ballDiscMaxInner && (float)px / (float)(bw * bh) <= cfg_.ballDiscMaxFill;
            const bool ring = cleanRing || glowRing || discRing;
            if (pass == 1 && !ring) continue;
            float score = -1.f;
            if (ring) {
                score = 0.5f + 0.25f * clampf(ratio / 0.3f, 0.f, 1.f) + 0.25f * coverage;
            } else if (cfg_.ballAllowSolidFallback && !yellowKit_ && ratio < cfg_.ballMinHoleRatio) {
                bool continues = true;
                if (cfg_.colorOnly && ringSeen_)
                    continues = ballWasKnown_ && frameTimeMs_ - lastBallSeenMs_ < ballHoldMs() &&
                                distance(hom_.apply(ccx, ccy), state_.ball) < 8.0f;
                const float fill = (float)px / (float)(bw * bh);
                if (continues && fill >= 0.5f) score = 0.2f + 0.1f * fill;
            }
            if (score < 0.f) continue;

            if (ballWasKnown_ && frameTimeMs_ - lastBallSeenMs_ < ballHoldMs()) {
                const float d = distance(hom_.apply(ccx, ccy), state_.ball);
                score += 0.15f * std::exp(-d / 8.f);
            }
            if (score > bestScore) {
                bestScore = score;
                bx = ccx;
                by = ccy;
                bestRing = ring;
                bestGlow = glowRing && !cleanRing && !discRing;
            }
        }
    }

    if (bestScore >= 0.f) {
        Det& d = dets_[2][0];
        d.x = bx;
        d.y = by;
        d.area = 1.f;
        nDets_[2] = 1;
        ballRing_ = bestRing;
        ballGlow_ = bestGlow;
        if (bestRing) ringSeen_ = true;
        ballScore_ = bestScore;
    }
}

#ifdef PITCHBOT_WITH_OPENCV
void PitchEngine::detectBallCv(int w, int h) {
    thread_local cv::Mat dil, scaled;
    thread_local std::vector<std::vector<cv::Point>> contours;
    thread_local std::vector<cv::Vec4i> hier;
    thread_local std::vector<cv::Vec3f> circles;
    static const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));

    cv::Mat mask(h, w, CV_8UC1, ym_.data());
    const int maxBox = std::min(63, std::max(cfg_.ballMinPx + 2, (int)(cfg_.ballMaxFrac * (float)std::min(w, h))));
    float bestScore = -1.f, bx = 0.f, by = 0.f;
    bool bestRing = false;
    int evaluated = 0;

    auto consider = [&](const cv::Mat& m, bool dilated) {
        contours.clear();
        hier.clear();
        cv::findContours(m, contours, hier, cv::RETR_TREE, cv::CHAIN_APPROX_SIMPLE);
        for (size_t i = 0; i < contours.size(); i++) {
            if (hier[i][3] != -1) continue;
            const cv::Rect r = cv::boundingRect(contours[i]);
            int x0 = r.x, y0 = r.y, bw = r.width, bh = r.height;
            if (dilated) { x0 = std::min(w - 1, x0 + 1); y0 = std::min(h - 1, y0 + 1); bw -= 2; bh -= 2; }
            if (bw < cfg_.ballMinPx || bh < cfg_.ballMinPx || bw > maxBox || bh > maxBox) continue;
            if ((float)std::min(bw, bh) / (float)std::max(bw, bh) < 0.6f) continue;
            if (++evaluated > 16) return;

            int px = 0, holes = 0;
            holeStats(ym_.data(), w, x0, y0, bw, bh, px, holes);
            if (px < 3) continue;
            const double outerA = std::max(cv::contourArea(contours[i]), (double)(px + holes));
            double innerA = 0.0;
            for (int c = hier[i][2]; c != -1; c = hier[c][0]) innerA += cv::contourArea(contours[(size_t)c]);
            const bool hasChild = hier[i][2] != -1;
            float ratio = (float)std::max(innerA / outerA, (double)holes / (double)(px + holes));
            const float ccx = x0 + 0.5f * bw, ccy = y0 + 0.5f * bh;

            bool ring = hasChild && ratio >= cfg_.ballMinHoleRatio;
            float score = -1.f;
            if (ring) {
                score = 0.55f + 0.3f * clampf(ratio / 0.3f, 0.f, 1.f);
            } else if (!hasChild) {
                const int pad = 3;
                const int rx = std::max(0, x0 - pad), ry = std::max(0, y0 - pad);
                const int rw = std::min(w - rx, bw + 2 * pad), rh = std::min(h - ry, bh + 2 * pad);
                if (rw >= 8 && rh >= 8) {
                    mask.convertTo(scaled, CV_8UC1, 255.0);
                    const cv::Mat roi = scaled(cv::Rect(rx, ry, rw, rh));
                    circles.clear();
                    cv::HoughCircles(roi, circles, cv::HOUGH_GRADIENT, 1.0, 4.0, 100.0, 6.0, 2, 5);
                    for (const cv::Vec3f& c : circles) {
                        const int cx = rx + (int)std::lround(c[0]), cy = ry + (int)std::lround(c[1]);
                        if (cx < 0 || cy < 0 || cx >= w || cy >= h) continue;
                        if (ym_[(size_t)cy * w + cx] == 0 && holes > 0) { ring = true; score = 0.5f; break; }
                    }
                }
                if (!ring && cfg_.ballAllowSolidFallback && !yellowKit_ && ratio < cfg_.ballMinHoleRatio) {
                    const float fill = (float)px / (float)(bw * bh);
                    if (fill >= 0.5f) score = 0.2f + 0.1f * fill;
                }
            }
            if (score < 0.f) continue;
            if (ballWasKnown_ && frameTimeMs_ - lastBallSeenMs_ < ballHoldMs()) {
                const float d = distance(hom_.apply(ccx, ccy), state_.ball);
                score += 0.15f * std::exp(-d / 8.f);
            }
            if (score > bestScore) { bestScore = score; bx = ccx; by = ccy; bestRing = ring; }
        }
    };

    consider(mask, false);
    if (bestScore < 0.5f) {
        cv::dilate(mask, dil, kernel);
        consider(dil, true);
    }
    if (bestScore >= 0.f) {
        Det& d = dets_[2][0];
        d.x = bx; d.y = by; d.area = 1.f;
        nDets_[2] = 1;
        ballRing_ = bestRing;
        ballScore_ = bestScore;
    }
}
#endif

void PitchEngine::emitDetections(Det* d, int n, MetricVector2D* out, int& nOut) {
    nOut = 0;
    if (n <= 0) return;
    float areas[MAX_DET];
    for (int i = 0; i < n; i++) areas[i] = d[i].area;
    for (int i = 1; i < n; i++) {
        const float a = areas[i];
        int j = i - 1;
        while (j >= 0 && areas[j] > a) {
            areas[j + 1] = areas[j];
            j--;
        }
        areas[j + 1] = a;
    }
    const float med = std::max(1.f, areas[n / 2]);

    bool dropped[MAX_DET] = {false};
    int kOf[MAX_DET];
    int total = 0;
    for (int i = 0; i < n; i++) {
        kOf[i] = 1;
        if (d[i].area >= 1.8f * med && d[i].elong >= cfg_.splitMinElong) kOf[i] = (int)clampf(std::floor(d[i].area / med + 0.5f), 1.f, 3.f);
        total += kOf[i];
    }
    if (cfg_.colorOnly) {
        while (total > cfg_.maxPerTeam) {
            int worstI = -1;
            for (int i = 0; i < n; i++) {
                if (dropped[i]) continue;
                if (worstI < 0 || d[i].score > d[worstI].score ||
                    (d[i].score == d[worstI].score && d[i].area < d[worstI].area))
                    worstI = i;
            }
            if (worstI < 0) break;
            dropped[worstI] = true;
            total -= kOf[worstI];
        }
    }
    for (int i = 0; i < n; i++) {
        if (dropped[i]) continue;
        const int k = kOf[i];
        const MetricVector2D p = hom_.apply(d[i].x, d[i].y);
        if (k == 1) {
            if (nOut < MAX_DET) out[nOut++] = p;
        } else if (nOut + k <= MAX_DET) {
            const float dia = std::sqrt(4.f * med / PI_F);
            const float off = d[i].len > dia ? 0.5f * (d[i].len - dia) : 0.f;
            if (off > 0.2f) {
                for (int q = 0; q < k; q++) {
                    const float f = k == 2 ? (q == 0 ? -1.f : 1.f) : (float)(q - 1);
                    out[nOut++] = hom_.apply(d[i].x + f * off * d[i].ax, d[i].y + f * off * d[i].ay);
                }
            } else if (k == 2) {
                out[nOut++] = {p.x - 0.9f, p.y};
                out[nOut++] = {p.x + 0.9f, p.y};
            } else {
                out[nOut++] = {p.x - 1.6f, p.y};
                out[nOut++] = p;
                out[nOut++] = {p.x + 1.6f, p.y};
            }
        }
    }
}

void PitchEngine::setHumanControl(bool on) {
    if (human_ && !on) {
        holding_ = false;
        ballHoldStartMs_ = -1;
        cueHoldUntil_ = 0;
        cueRun_ = 0;
        pendingCue_ = 0;
        crowdSinceMs_ = -1;
        nCand_ = 0;
        chosen_ = -1;
        gesture_ = GestureOutput();
    }
    human_ = on;
}

int PitchEngine::processFrame(unsigned char* buffer, int width, int height) {
    const auto t0 = std::chrono::steady_clock::now();
    int status = ST_NOT_LIVE;
    if (buffer && haveColors_ && width >= 16 && height >= 16 && (long)width * height <= MAX_PIXELS) {
        rebuildHomography(width, height);
        segment(buffer, width, height);
        detectControlled(buffer, width, height);
        emitDetections(dets_[0], nDets_[0], mHome_, nMHome_);
        emitDetections(dets_[1], nDets_[1], mAway_, nMAway_);

        mBallSeen_ = false;
        if (nDets_[2] > 0) {
            int bi = 0;
            if (ballWasKnown_ && frameTimeMs_ - lastBallSeenMs_ < ballHoldMs()) {
                float bd = 1e9f;
                for (int i = 0; i < nDets_[2]; i++) {
                    const float d = distance(hom_.apply(dets_[2][i].x, dets_[2][i].y), state_.ball);
                    if (d < bd) {
                        bd = d;
                        bi = i;
                    }
                }
            } else {
                for (int i = 1; i < nDets_[2]; i++)
                    if (dets_[2][i].area > dets_[2][bi].area) bi = i;
            }
            mBall_ = hom_.apply(dets_[2][bi].x, dets_[2][bi].y);
            mBallSeen_ = true;
        }
        if (cfg_.colorOnly && mBallSeen_ && ballGlow_ && nMHome_ < TEAM_SIZE && nMHome_ < MAX_DET) {
            bool hasDot = false;
            for (int i = 0; i < nMHome_ && !hasDot; i++) hasDot = distance(mHome_[i], mBall_) <= cfg_.ownRadius;
            for (int i = 0; i < nMAway_ && !hasDot; i++) hasDot = distance(mAway_[i], mBall_) <= cfg_.ownRadius * 0.6f;
            if (!hasDot) mHome_[nMHome_++] = mBall_;
        }
        status = ingestDetections(mHome_, nMHome_, mAway_, nMAway_, &mBall_, mBallSeen_);
    } else {
        status = ingestDetections(nullptr, 0, nullptr, 0, nullptr, false);
    }
    lastMicros_ = (float)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    return status;
}

static inline void kalmanPredictAxis(float* P, float dt, float q2) {
    const float dt2 = dt * dt;
    const float P00 = P[0] + 2.f * dt * P[1] + dt2 * P[2] + q2 * dt2 * dt2 * 0.25f;
    const float P01 = P[1] + dt * P[2] + q2 * dt2 * dt * 0.5f;
    const float P11 = P[2] + q2 * dt2;
    P[0] = P00; P[1] = P01; P[2] = P11;
}

void PitchEngine::kalmanCoast(PlayerState& p, float dt) {
    if (!p.kInit) return;
    const float q2 = cfg_.kalQ * cfg_.kalQ;
    kalmanPredictAxis(p.kP[0], dt, q2);
    kalmanPredictAxis(p.kP[1], dt, q2);
    for (int a = 0; a < 2; a++) {
        p.kP[a][0] = std::min(p.kP[a][0], 400.f);
        p.kP[a][2] = std::min(p.kP[a][2], 100.f);
    }
}

void PitchEngine::kalmanUpdate(PlayerState& p, const MetricVector2D& z, float dt) {
    const float q2 = cfg_.kalQ * cfg_.kalQ;
    const float rMin = cfg_.kalRMin * cfg_.kalRMin, rMax = std::max(cfg_.kalRMax * cfg_.kalRMax, rMin);
    if (!p.kInit) {
        const float r0 = clampf(cfg_.kalR * cfg_.kalR, rMin, rMax);
        for (int a = 0; a < 2; a++) { p.kP[a][0] = r0; p.kP[a][1] = 0.f; p.kP[a][2] = cfg_.kalV0 * cfg_.kalV0; }
        p.kRVar = r0;
        p.kInit = true;
    }
    float pos[2] = {p.pos.x, p.pos.y}, vel[2] = {p.vel.x, p.vel.y};
    const float zz[2] = {z.x, z.y};
    float y[2], pPred = 0.f;
    const float R = clampf(p.kRVar, rMin, rMax);
    for (int a = 0; a < 2; a++) {
        float* P = p.kP[a];
        kalmanPredictAxis(P, dt, q2);
        pos[a] += vel[a] * dt;
        y[a] = zz[a] - pos[a];
        pPred += P[0] * 0.5f;
        const float S = P[0] + R;
        const float K0 = P[0] / S, K1 = P[1] / S;
        pos[a] += K0 * y[a];
        vel[a] += K1 * y[a];
        const float P00 = (1.f - K0) * P[0], P01 = (1.f - K0) * P[1], P11 = P[2] - K1 * P[1];
        P[0] = P00; P[1] = P01; P[2] = std::max(P11, 1e-4f);
    }
    const float y2 = 0.5f * (y[0] * y[0] + y[1] * y[1]);
    const float al = clampf(cfg_.kalRAlpha, 0.f, 1.f);
    p.kRVar = clampf(p.kRVar + al * ((y2 - pPred) - p.kRVar), rMin, rMax);
    p.pos = MetricVector2D(pos[0], pos[1]);
    p.vel = MetricVector2D(vel[0], vel[1]);
}

void PitchEngine::trackTeam(PlayerState* tr, const MetricVector2D* det, int nDet, double dt, int idBase, int team) {
    if (nDet > MAX_DET) nDet = MAX_DET;
    int act[TEAM_SIZE];
    int nAct = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        tr[i].id = idBase + i;
        tr[i].team = team;
        tr[i].visible = false;
        if (tr[i].active) act[nAct++] = i;
    }
    for (int c = 0; c < nDet; c++) detToTrack_[c] = -1;

    float gate[TEAM_SIZE];
    MetricVector2D pred[TEAM_SIZE];
    for (int r = 0; r < nAct; r++) {
        const PlayerState& p = tr[act[r]];
        pred[r] = p.pos + p.vel * (float)dt;
        gate[r] = std::min(cfg_.gateMax, cfg_.trackGate + cfg_.gateGrowth * (float)p.missed);
    }

    if (nAct > 0 && nDet > 0) {
        const int K = std::min(HUNGARIAN_MAX, std::max(nAct, nDet));
        for (int r = 0; r < K; r++) {
            for (int c = 0; c < K; c++) {
                float v;
                if (r < nAct && c < nDet) v = distance(pred[r], det[c]);
                else if (r < nAct || c < nDet) v = cfg_.gateMax;
                else v = 0.f;
                hCost_[r * K + c] = v;
            }
        }
        hungarianSolve(hCost_, K, hAssign_);
        for (int r = 0; r < nAct; r++) {
            const int c = hAssign_[r];
            if (c < nDet && distance(pred[r], det[c]) <= gate[r]) detToTrack_[c] = act[r];
        }
    }

    bool matched[TEAM_SIZE] = {false};
    for (int c = 0; c < nDet; c++) {
        const int ti = detToTrack_[c];
        if (ti < 0) continue;
        PlayerState& p = tr[ti];
        const MetricVector2D prevVel = p.vel;
        if (cfg_.kalman) {
            kalmanUpdate(p, det[c], (float)dt);
        } else {
            const MetricVector2D predicted = p.pos + p.vel * (float)dt;
            const MetricVector2D rawV = (det[c] - p.pos) * (float)(1.0 / dt);
            p.vel = p.vel + (rawV - p.vel) * cfg_.velAlpha;
            p.pos = predicted + (det[c] - predicted) * cfg_.posAlpha;
        }
        const float sp = p.vel.length();
        if (sp > cfg_.maxSpeed) p.vel = p.vel * (cfg_.maxSpeed / sp);
        {
            const MetricVector2D a = (p.vel - prevVel) * (float)(1.0 / dt);
            p.acc = p.acc + (a - p.acc) * clampf(cfg_.accAlpha, 0.f, 1.f);
            const float am = p.acc.length();
            if (am > cfg_.accMax) p.acc = p.acc * (cfg_.accMax / am);
        }
        p.missed = 0;
        p.visible = true;
        p.lastSeenMs = frameTimeMs_;
        matched[ti] = true;
    }
    for (int r = 0; r < nAct; r++) {
        PlayerState& p = tr[act[r]];
        if (matched[act[r]]) continue;
        if (cfg_.kalman) kalmanCoast(p, (float)dt);
        p.pos = p.pos + p.vel * (float)dt;
        if (cfg_.occlusionHold && p.missed < cfg_.occHoldFrames && overlapNear(&p, p.pos)) {
            p.vel = p.vel * clampf(cfg_.occVelKeep, 0.8f, 1.f);
            p.acc = p.acc * 0.9f;
        } else {
            p.vel = p.vel * 0.92f;
            p.acc = p.acc * 0.8f;
        }
        p.missed++;
    }

    int freeSlots[TEAM_SIZE];
    int nFree = 0;
    for (int i = 0; i < TEAM_SIZE; i++)
        if (!tr[i].active) freeSlots[nFree++] = i;
    if (nFree > 0) {
        int idx[MAX_DET];
        int nu = 0;
        for (int c = 0; c < nDet; c++)
            if (detToTrack_[c] < 0) idx[nu++] = c;
        for (int i = 1; i < nu; i++) {
            const int a = idx[i];
            int j = i - 1;
            while (j >= 0 && (det[idx[j]].x > det[a].x || (det[idx[j]].x == det[a].x && det[idx[j]].y > det[a].y))) {
                idx[j + 1] = idx[j];
                j--;
            }
            idx[j + 1] = a;
        }
        for (int i = 0; i < nu && i < nFree; i++) {
            PlayerState& p = tr[freeSlots[i]];
            p.pos = det[idx[i]];
            p.vel = MetricVector2D();
            p.acc = MetricVector2D();
            p.kInit = false;
            p.active = true;
            p.visible = true;
            p.missed = 0;
            p.lastSeenMs = frameTimeMs_;
            detToTrack_[idx[i]] = freeSlots[i];
        }
    }

    for (int c = 0; c < nDet; c++) {            // a detection nowhere near any track: count it for the radar-jump check
        if (detToTrack_[c] >= 0) continue;
        float nearest = 1e9f;
        for (int i = 0; i < TEAM_SIZE; i++)
            if (tr[i].active) nearest = std::min(nearest, distance(tr[i].pos, det[c]));
        if (nearest < 1e8f && nearest > cfg_.metaJumpM) trackJumps_++;
    }
    for (int c = 0; c < nDet; c++) {
        if (detToTrack_[c] >= 0) continue;
        int bi = -1;
        float bd = 1e9f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            if (!tr[i].active || tr[i].visible || tr[i].missed < 3) continue;
            const float d = distance(tr[i].pos, det[c]);
            if (d < bd) { bd = d; bi = i; }
        }
        if (bi < 0) continue;
        PlayerState& p = tr[bi];
        p.pos = det[c];
        p.vel = MetricVector2D();
        p.acc = MetricVector2D();
        p.kInit = false;
        p.visible = true;
        p.missed = 0;
        p.lastSeenMs = frameTimeMs_;
        detToTrack_[c] = bi;
    }
}

void PitchEngine::updateCalibration(double nowMs) {
    const double T = (double)(cfg_.horizonSec + clampf(safe(effLatencyMs(), 0.f) * 0.001f, 0.f, 0.5f));
    double rs = 0; int rn = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& p = state_.team[i];
        Pending& q = pend_[i];
        if (p.active && p.kInit) { rs += p.kRVar; rn++; }
        if (q.on) {
            if (!p.active || !p.visible || p.missed > 0) { q.on = false; }
            else if (nowMs - q.t0 > 3.0 * T * 1000.0 + 400.0) { q.on = false; }
            else if (nowMs >= q.due) {
                const double el = (nowMs - q.t0) * 0.001;
                const MetricVector2D a = p.pos - q.pos0;
                const MetricVector2D d = q.vel0 * (float)el;
                if (a.length() < 12.f && d.length() > 0.25f) {
                    calAD_ += (double)a.dot(d);
                    calDD_ += (double)d.dot(d);
                    calN_ += 1.0;
                }
                q.on = false;
            }
        }
        if (!q.on && p.active && p.visible && p.missed == 0 && p.kInit && p.vel.length() > 0.5f) {
            q.on = true; q.t0 = nowMs; q.due = nowMs + T * 1000.0; q.pos0 = p.pos; q.vel0 = p.vel;
        }
    }
    if (rn > 0) { calR_ += rs / rn; calFrames_ += 1.0; }
}

void PitchEngine::takeTrackerStats(double* out5) {
    out5[0] = calAD_; out5[1] = calDD_; out5[2] = calN_; out5[3] = calR_; out5[4] = calFrames_;
    calAD_ = calDD_ = calN_ = calR_ = calFrames_ = 0;
}

static void pushVel5(PlayerState& p) {
    p.vh[p.vhN % 5][0] = p.vel.x;
    p.vh[p.vhN % 5][1] = p.vel.y;
    p.vhN++;
    const int n = p.vhN < 5 ? p.vhN : 5;
    float sx = 0.f, sy = 0.f;
    for (int k = 0; k < n; k++) { sx += p.vh[k][0]; sy += p.vh[k][1]; }
    p.vel5 = MetricVector2D(sx / (float)n, sy / (float)n);
}

void PitchEngine::extrapolateKinematics(float dtSec) {
    frameNo_++;
    for (int i = 0; i < TEAM_SIZE; i++) { pushVel5(state_.team[i]); pushVel5(state_.opp[i]); }
    dtSec = clampf(safe(dtSec, 0.f), 0.f, 1.f);
    for (int i = 0; i < TEAM_SIZE; i++) {
        PlayerState& a = state_.team[i];
        PlayerState& b = state_.opp[i];
        const float lead = cfg_.kalman ? clampf(safe(effLatencyMs(), 0.f) * 0.001f, 0.f, 0.5f) : 0.f;
        a.future = a.pos + a.vel * (std::min(dtSec + lead, 1.f) * (cfg_.kalman ? clampf(cfg_.kalProjGain, 0.3f, 1.3f) : 1.f));
        b.future = b.pos + b.vel * dtSec;
    }
}

void PitchEngine::updateAttackSign() {
    if (cfg_.attackDir == 0 || cfg_.attackDir == 1) {
        state_.attackSign = cfg_.attackDir == 1 ? 1 : -1;
        signInit_ = true;
        return;
    }
    double mt = 0, mo = 0;
    int nt = 0, no = 0;
    double tx[TEAM_SIZE], ox[TEAM_SIZE];
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (state_.team[i].active && state_.team[i].visible) { mt += state_.team[i].pos.x; tx[nt++] = state_.team[i].pos.x; }
        if (state_.opp[i].active && state_.opp[i].visible) { mo += state_.opp[i].pos.x; ox[no++] = state_.opp[i].pos.x; }
    }
    if (nt < 3 || no < 3) return;
    std::sort(tx, tx + nt);
    std::sort(ox, ox + no);
    auto lowMean = [](const double* a, int) { return (a[0] + a[1] + a[2]) / 3.0; };
    auto highMean = [](const double* a, int n) { return (a[n - 1] + a[n - 2] + a[n - 3]) / 3.0; };
    const double backDiff = 0.5 * ((highMean(ox, no) - highMean(tx, nt)) + (lowMean(tx, nt) - lowMean(ox, no)));
    const double meanDiff = mo / no - mt / nt;
    const double ev = 0.5 * meanDiff + 0.5 * backDiff;
    dirEma_ = signInit_ ? dirEma_ * 0.92 + ev * 0.08 : ev;
    const double diff = dirEma_;
    if (std::fabs(diff) < 2.0) return;
    const int cand = diff > 0 ? 1 : -1;
    if (!signInit_) {
        state_.attackSign = cand;
        signInit_ = true;
        signRun_ = 0;
        signLockMs_ = frameTimeMs_;
        return;
    }
    if (cand != state_.attackSign) {
        const int need = (frameTimeMs_ - signLockMs_ < (double)cfg_.signProbationMs) ? cfg_.signProbationRun : 25;
        if (++signRun_ >= need) {
            state_.attackSign = cand;
            signRun_ = 0;
        }
    } else {
        signRun_ = 0;
    }
}

bool PitchEngine::ballUsable() const {
    if (!ballWasKnown_) return false;
    const double age = frameTimeMs_ - lastBallSeenMs_;
    return age <= ballHoldMs() || (ballAssumed_ && age <= (double)cfg_.ballCarryHoldMs);
}

void PitchEngine::updatePossession(double dtSec) {
    const int prevOwnerForAttack = owner_;
    const MetricVector2D ball = state_.ball;
    int cand = 0, carrier = -1, oppIdx = -1;
    if (ballUsable()) {
        float best = cfg_.ownRadius;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& p = state_.team[i];
            if (p.active && p.visible) {
                const float d = distance(p.pos, ball);
                if (d <= best) { best = d; cand = 1; carrier = i; }
            }
        }
        const float margin = (cfg_.keepBall && cand == 1 && owner_ == 1) ? cfg_.ownerTieMargin : 0.f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& p = state_.opp[i];
            if (p.active && p.visible) {
                const float d = distance(p.pos, ball);
                if (d < best - margin) { best = d; cand = 2; carrier = -1; oppIdx = i; }
            }
        }
    }
    if (hudPoss_ != 0 && frameTimeMs_ - hudPossMs_ < 1500.0) {
        // the on-screen buttons tell us for certain who has the ball (Pass/Through/Shoot vs Switch/Match-up/Tackle)
        if (hudPoss_ == 2) { cand = 2; carrier = -1; }
        else if (cand != 1) {
            float bd = 1e9f; int bi = -1;
            if (ballUsable()) {
                for (int i = 0; i < TEAM_SIZE; i++) {
                    const PlayerState& p = state_.team[i];
                    if (!p.active || !p.visible) continue;
                    const float d = distance(p.pos, ball);
                    if (d < bd) { bd = d; bi = i; }
                }
            }
            if (bi >= 0) { cand = 1; carrier = bi; }
        }
    }
    if (cand == candOwner_) candRun_++;
    else { candOwner_ = cand; candRun_ = 1; }
    if (candOwner_ != owner_ && candRun_ >= 2) {
        const int prev = owner_;
        owner_ = candOwner_;
        ownerSinceMs_ = frameTimeMs_;
        if (owner_ == 1) {
            const float ax = state_.attackSign * ball.x;
            if ((prev == 2 || lastOwnerForCounter_ == 2) && ax <= 15.f) counterUntilMs_ = frameTimeMs_ + cfg_.counterHoldMs;
        }
        if (owner_ != 0) lastOwnerForCounter_ = owner_;
        if (prev == 1 && owner_ == 2) { lostBallMs_ = frameTimeMs_; cpCounted_ = false; turnoverCount_++; armPostLoss(); if (cfg_.plan) planOnTurnover(); }
        else if (prev == 2 && owner_ == 1) { wonBackCount_++; if (cfg_.plan) planOnWin(); }
    }
    if (cfg_.rescueRules) updateOppPass(owner_ == 2 && cand == 2 ? oppIdx : -1);
    state_.carrier = owner_ == 1 ? carrier : -1;
    if (owner_ == 1 && state_.carrier < 0) {
        float best = 1e9f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& p = state_.team[i];
            if (!p.active || !p.visible) continue;
            const float d = distance(p.pos, ball);
            if (d < best) { best = d; state_.carrier = i; }
        }
    }
    state_.status = owner_ == 1 ? ST_TEAM : (owner_ == 2 ? ST_OPP : ST_LOOSE);
    if (owner_ == 1 || owner_ == 2) {
        const float k = clampf((float)dtSec / std::max(cfg_.tempoWindowSec, 1.f), 0.f, 0.2f);
        possShare_ += ((owner_ == 1 ? 1.f : 0.f) - possShare_) * k;
        possOwnedSec_ += (float)dtSec;
    }
    attackTrack(prevOwnerForAttack);
}

int PitchEngine::ingestDetections(const MetricVector2D* home, int nHome, const MetricVector2D* away, int nAway,
                                  const MetricVector2D* ball, bool ballSeen) {
    const double now = frameTimeMs_;
    double dt = prevFrameMs_ > 0 ? (now - prevFrameMs_) / 1000.0 : 0.08;
    dt = std::min(0.25, std::max(0.01, dt));

    const bool live = home && away && nHome >= 5 && nAway >= 5;
    if (!live) {
        state_.status = ST_NOT_LIVE;
        gesture_ = GestureOutput();
        gesture_.status = ST_NOT_LIVE;
        nCand_ = 0;
        chosen_ = -1;
        notLiveFrames_++;
        if (lostSinceMs_ < 0) lostSinceMs_ = now;
        if (notLiveSinceMs_ < 0) notLiveSinceMs_ = now;
        holding_ = false;
        turnStartMs_ = -1;
        shotPending_ = false;
        if (notLiveFrames_ > 20) {
            // stoppage (goal, replay): clear the tracker but keep the learned attack direction and timing state,
            // so play resumes the instant the radar is back instead of re-learning the pitch
            if (notLiveFrames_ == 21 && cfg_.plus) plusStoppage();
            const PlusState plusKeep = plus_;
            const MetaState metaKeep = meta_;
            const PlanState planKeep = plan_;
            const V7State v7Keep = v7_;
            const V8State v8Keep = v8_;
            const TacticalConfig keep = cfg_;
            const OppModel om = opp_;
            const bool sInit = signInit_;
            const double sEma = dirEma_;
            const int sSign = state_.attackSign, sCand = signCand_;
            const double sNot = notLiveSinceMs_;
            reset();
            plus_ = plusKeep;
            meta_ = metaKeep;
            plan_ = planKeep;
            v7_ = v7Keep;
            v8_ = v8Keep;
            cfg_ = keep;
            opp_ = om;
            signInit_ = sInit;
            dirEma_ = sEma;
            state_.attackSign = sSign;
            signCand_ = sCand;
            signLockMs_ = now - 1e6;
            notLiveSinceMs_ = sNot;
        }
        prevFrameMs_ = now;
        return ST_NOT_LIVE;
    }
    if (notLiveSinceMs_ >= 0) {
        if (now - notLiveSinceMs_ >= (double)cfg_.restartMinGapMs) {
            restartUntilMs_ = now + (double)cfg_.restartGraceMs;   // play quick and safe straight after the restart
            nextDecisionAt_ = cooldownUntil_ = 0;
            cueArmed_ = shotArmed_ = true;
            shotPending_ = false;
        }
        notLiveSinceMs_ = -1;
    }
    notLiveFrames_ = 0;
    liveFrames_++;
    {   // raw radar read (before the tracker filters it): used by the health monitor
        rawN_ = std::min(nHome, (int)TEAM_SIZE);
        for (int i = 0; i < rawN_; i++) rawPts_[i] = home[i];
    }

    trackJumps_ = 0;
    trackTeam(state_.team, home, nHome, dt, 1, TEAM_HOME);
    trackTeam(state_.opp, away, nAway, dt, 101, TEAM_AWAY);

    state_.ballSeen = ballSeen && ball;
    if (state_.ballSeen) {
        if (ballWasKnown_) {
            const MetricVector2D rawV = (*ball - state_.ball) * (float)(1.0 / dt);
            state_.ballVel = state_.ballVel + (rawV - state_.ballVel) * 0.5f;
        } else {
            state_.ballVel = MetricVector2D();
        }
        state_.ball = *ball;
        lastBallSeenMs_ = now;
        ballWasKnown_ = true;
        ballAssumed_ = false;
        plan_.ballHyp = 0;
    } else if (ballWasKnown_ && now - lastBallSeenMs_ > ballHoldMs()) {
        const int c = state_.carrier;
        const bool carrying = owner_ == 1 && c >= 0 && c < TEAM_SIZE && state_.team[c].active && state_.team[c].visible;
        if (carrying && now - lastBallSeenMs_ <= (double)cfg_.ballCarryHoldMs) {
            state_.ball = state_.team[c].pos;
            state_.ballVel = state_.team[c].vel;
            ballAssumed_ = true;
        } else {
            ballWasKnown_ = false;
            ballAssumed_ = false;
            state_.ballVel = MetricVector2D();
        }
    } else if (cfg_.ballCoast && ballWasKnown_) {
        if (!planBallHypothesis(dt)) coastBall(dt);
    }

    extrapolateKinematics(cfg_.horizonSec);
    if (cfg_.kalman) updateCalibration(now);
    updateAttackSign();
    updatePossession(dt);
    updateOppShape();
    updateStyle(dt);
    state_.timeMs = now;
    state_.temperature = targetTemperature(state_);
    state_.counter = state_.temperature <= cfg_.tempCounter + 1e-4f;
    updateUpgrades(dt);
    if (cfg_.plus) plusFrame(dt);
    if (cfg_.plus && cfg_.meta) metaFrame(dt);
    if (cfg_.plan) planFrame(dt);
    if (cfg_.v7) v7Frame(dt);
    if (cfg_.v8) { v8Record(home, nHome, away, nAway, ball, ballSeen); v8Frame(dt); }

    if (human_) {
        if (state_.status == ST_TEAM && state_.carrier >= 0) {
            calculatePassProbabilities(state_);
            attackSnapshot();
        }
        gesture_ = GestureOutput();
        gesture_.status = state_.status;
        nCand_ = 0;
        chosen_ = -1;
        holding_ = false;
        pendingCue_ = 0;
        lostSinceMs_ = -1;
    } else if (state_.status == ST_TEAM && state_.carrier >= 0) {
        gesture_ = decide();
        attackSnapshot();
        lostSinceMs_ = -1;
        oppSinceMs_ = -1;
    } else {
        if (state_.status == ST_OPP && cfg_.defence) {
            gesture_ = decideDefence();
        } else {
            gesture_ = GestureOutput();
            gesture_.status = state_.status;
            oppSinceMs_ = -1;
            if (state_.status == ST_LOOSE) {
                GestureOutput ag;
                if (anticipatedPass(ag)) gesture_ = ag;       // ball in flight to a team-mate: next pass is dispatched early
                else {
                    const GestureOutput lg = decideLoose();   // loose ball we can win: go to the predicted intercept point
                    if (lg.valid) gesture_ = lg;
                }
            }
        }
        nCand_ = 0;
        chosen_ = -1;
        holding_ = false;
        turnStartMs_ = -1;
        if (lostSinceMs_ < 0) {
            lostSinceMs_ = now;
        } else if (now - lostSinceMs_ >= cfg_.cueRearmMs) {
            cueArmed_ = true;
            shotArmed_ = true;
            shotRun_ = 0;
            shotPending_ = false;
            cueRun_ = 0;
            cueHoldUntil_ = 0;
            ballHoldStartMs_ = -1;
        }
    }
    if (cfg_.v8) gesture_ = v8Sanitize(gesture_);
    if (cfg_.upgrades && cfg_.recovery) monitorHealth(now, dt);
    prevFrameMs_ = now;
    return state_.status;
}

float PitchEngine::targetTemperature(const MatchState& s) const {
    bool counter = s.timeMs < counterUntilMs_;
    if (!counter && s.status == ST_TEAM) {
        const float fwdSpeed = s.ballVel.x * (float)s.attackSign;
        const float bx = s.attackSign * s.ball.x;
        int ahead = 0;
        for (int i = 0; i < TEAM_SIZE; i++)
            if (s.opp[i].active && s.attackSign * s.opp[i].future.x > bx + 2.f) ahead++;
        if (fwdSpeed >= 6.f && ahead <= 5) counter = true;
    }
    return std::max(counter ? cfg_.tempCounter : cfg_.tempHold, cfg_.tempMin);
}

float PitchEngine::computeExpectedThreat(const PlayerState& player, const MetricVector2D& target) const {
    const float s = (float)state_.attackSign;
    const float a = grid_.valueAt(s * player.pos.x, player.pos.y);
    const float b = grid_.valueAt(s * target.x, target.y);
    return safe(b - a, 0.f);
}

float PitchEngine::nearestOppDistance(const MetricVector2D& p) const {
    float best = 1e3f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        best = std::min(best, distance(o.future, p));
    }
    return best;
}

float PitchEngine::lanePressure(const MetricVector2D& p) const {
    const float r = std::max(cfg_.pressRadius, 0.1f);
    return clampf(1.f - nearestOppDistance(p) / r, 0.f, 1.f);
}

float PitchEngine::offsideAmount(const MetricVector2D& recv) const {
    const float s = (float)state_.attackSign;
    const float rx = s * recv.x;
    const float bx = s * state_.ball.x;
    if (rx <= bx || rx <= 0.f) return 0.f;
    float top1 = -1e3f, top2 = -1e3f;
    int n = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        const float ox = s * o.future.x;
        n++;
        if (ox > top1) { top2 = top1; top1 = ox; }
        else if (ox > top2) top2 = ox;
    }
    if (n == 0) return 0.f;
    const float secondLast = n >= 2 ? top2 : top1;
    const float line = std::max(secondLast, bx);
    if (rx > line + cfg_.offsideMargin) return 1.f;
    if (rx > line - 1.0f) return 0.4f;
    return 0.f;
}

// Spare reach (metres) of the worst-placed defender: how far outside his reach he is, given that the ball needs
// time to arrive. Negative means a defender can get to the lane (or the receiver) before the ball.
bool PitchEngine::isKeeperIdx(int j) const {
    if (j < 0 || j >= TEAM_SIZE) return false;
    const PlayerState& r = state_.team[j];
    if (!r.active) return false;
    const float s = (float)state_.attackSign;
    const float depth = PITCH_HALF_X + s * r.pos.x;          // distance from our own goal line
    if (depth > cfg_.gkBanDepth) return false;
    for (int k = 0; k < TEAM_SIZE; k++) {
        if (k == j || !state_.team[k].active) continue;
        if (PITCH_HALF_X + s * state_.team[k].pos.x < depth) return false;   // someone is deeper: he is the keeper
    }
    return true;
}

// 0..1: how badly the team needs to turn possession into a shot (long spells and a high possession share)
float PitchEngine::urgencyLevel() const {
    if (state_.status != ST_TEAM) return 0.f;
    const float sec = (float)((state_.timeMs - ownerSinceMs_) * 0.001);
    const float t = clampf((sec - cfg_.urgencyAfterSec) / 8.f, 0.f, 1.f);
    const float sh = clampf((possShare_ - 0.55f) / 0.30f, 0.f, 1.f);
    float u = clampf(0.6f * t + 0.6f * sh, 0.f, 1.f);
    u = clampf(u + scoreClockUrgency(), 0.f, 1.f);
    // final third: climb urgency faster so sterile recycling ends sooner
    const float s = (float)state_.attackSign;
    const float depth = PITCH_HALF_X - s * state_.team[state_.carrier].pos.x;
    if (state_.carrier >= 0 && depth <= cfg_.finalThirdDepth)
        u = clampf(u + 0.20f, 0.f, 1.f);
    return u;
}

float PitchEngine::interceptMargin(const MetricVector2D& from, const MetricVector2D& to) const {
    const MetricVector2D seg = to - from;
    const float len2 = std::max(seg.x * seg.x + seg.y * seg.y, 1e-6f);
    const float len = std::sqrt(len2);
    const float bs = std::max(cfg_.passSpeed, 1.f);
    float worst = 50.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        const MetricVector2D op = o.future;
        const MetricVector2D rel = op - from;
        const float t = clampf((rel.x * seg.x + rel.y * seg.y) / len2, 0.f, 1.f);
        const MetricVector2D q = from + seg * t;
        const float dOpp = distance(op, q);
        const float tBall = len * t / bs;
        const float reach = cfg_.defSpeed * (tBall + defReactEff()) + cfg_.defBody;
        worst = std::min(worst, dOpp - reach);
    }
    return worst;
}

bool PitchEngine::inOwnBox(const MetricVector2D& p) const {
    const float own = -(float)state_.attackSign;           // own goal is at own * PITCH_HALF_X
    const float depth = PITCH_HALF_X - own * p.x;          // distance from own goal line
    return depth <= cfg_.boxDepth && std::fabs(p.y) <= cfg_.boxHalfY;
}

// Distance of a point from our own goal line (metres).
float PitchEngine::goalDistance(const MetricVector2D& p) const {
    return PITCH_HALF_X + (float)state_.attackSign * p.x;
}

// Localized pressure: every opponent inside pressRadius adds (1 - d / pressRadius). Two defenders at ~3 m score about 1.2.
float PitchEngine::localPressure(const MetricVector2D& p) const {
    const float R = std::max(cfg_.pressRadius, 0.5f);
    float sum = 0.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        const float d = distance(o.future, p);
        if (d < R) sum += 1.f - d / R;
    }
    return sum;
}

// Which of our players is under the stick (nearest dot to the controlled-player glow), or -1.
int PitchEngine::controlledIdx() const {
    if (!controlledSeen()) return -1;
    int bi = -1;
    float bd = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& p = state_.team[i];
        if (!p.active) continue;
        const float d = distance(p.pos, ctrlPos_);
        if (d < bd) { bd = d; bi = i; }
    }
    return bd < 3.5f ? bi : -1;
}

// ---- shot suppression telemetry ----
// 0 = nothing suppresses the shot, otherwise 1 pressure, 2 line of sight, 3 cooldown / not armed / human has control.
int PitchEngine::shotBlockReason(const PlayerState& car, int lvl) const {
    if (lvl >= 2) {
        if (!shotArmed_ || state_.timeMs < cooldownUntil_ || human_ || !cfg_.autoShoot) return 3;
        return 0;
    }
    if (nearestOppDistance(car.future) < cfg_.stunSafe) return 1;
    {
        const MetricVector2D g((float)state_.attackSign * PITCH_HALF_X, 0.f);
        float m = 99.f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (!o.active || (float)state_.attackSign * o.future.x >= PITCH_HALF_X - 3.5f) continue;
            m = std::min(m, segmentDistance(car.future, g, o.future));
        }
        const_cast<PitchEngine*>(this)->lastRayD_ = m;
    }
    return 2;
}

void PitchEngine::logShotBlock(int reason, const PlayerState& car) {
    if (reason <= 0 || !cfg_.shotLog) return;
    const double now = state_.timeMs;
    if (reason == lastBlkReason_ && now - lastBlkMs_ < 400.0) return;
    lastBlkReason_ = reason;
    lastBlkMs_ = now;
    const char* nm = reason == 1 ? "SHOT_BLOCKED_PRESSURE" : (reason == 2 ? "SHOT_BLOCKED_LINE_OF_SIGHT" : "SHOT_BLOCKED_COOLDOWN");
    if (reason == 1) nBlkPress_++; else if (reason == 2) nBlkLos_++; else nBlkCool_++;
    const float s = (float)state_.attackSign;
    const float dx = PITCH_HALF_X - s * car.future.x;
    const float dg = std::sqrt(dx * dx + car.future.y * car.future.y);
    char line[300];
    const float no = nearestOppDistance(car.future);
    char why[120];
    if (reason == 1) std::snprintf(why, sizeof why, "%s (%.2fm < %.2fm)", nm, no, cfg_.stunSafe);
    else if (reason == 2) std::snprintf(why, sizeof why, "DEFENDER_RAYCAST_INTERSECT (%.2fm < %.2fm)", lastRayD_, cfg_.rayMin);
    else std::snprintf(why, sizeof why, "%s", nm);
    const MetricVector2D aim((float)state_.attackSign * PITCH_HALF_X, 0.f);
    const MetricVector2D dv = aim - car.pos;
    const float sp = car.vel5.length(), dl = dv.length();
    const float align = (sp > 0.5f && dl > 1e-3f) ? car.vel5.dot(dv) / (sp * dl) : 1.f;
    std::snprintf(line, sizeof line, "[FRAME: %ld] SHOT_SUPPRESSED | REASON: %s | ALIGNMENT: %.2f | POWER_CALC: %.2f | DIST: %.1f | PRESSURE: %.2f\n",
                  frameNo_, why, align, stunPowerRatio(dg), dg, localPressure(car.future));
    (void)now;
    shotLogText_ += line;
    if (shotLogText_.size() > 60000) shotLogText_.erase(0, shotLogText_.size() / 2);
}

// ---- emergency clearance ----
bool PitchEngine::panicNeeded(const PlayerState& car) const {
    if (!cfg_.rescueRules || human_) return false;
    if (goalDistance(car.pos) > cfg_.panicZone) return false;
    return nearestOppDistance(car.pos) <= cfg_.panicRadius;
}

bool PitchEngine::tryPanicClear(const PlayerState& car, GestureOutput& out) {
    if (!panicNeeded(car)) return false;
    const double now = state_.timeMs;
    if (now < shotFiredMs_ + 600.0) return false;
    const float s = (float)state_.attackSign;
    float side = car.pos.y >= 0.f ? 1.f : -1.f;
    if (std::fabs(car.pos.y) < 2.0f) {   // central: away from the closest presser
        float bd = 1e9f, oy = 0.f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (!o.active) continue;
            const float d = distance(o.future, car.pos);
            if (d < bd) { bd = d; oy = o.future.y; }
        }
        side = oy > car.pos.y ? -1.f : 1.f;
    }
    if (cfg_.plan && cfg_.planDirClear && !isKeeperIdx(state_.carrier)) side = planClearSide(car, side);
    const float a = cfg_.panicAngleDeg * (PI_F / 180.f);
    const MetricVector2D t = car.pos + MetricVector2D(s * std::cos(a), side * std::sin(a)) * cfg_.panicReach;
    const bool gk = isKeeperIdx(state_.carrier);
    GestureOutput g = makeGesture(gk ? ACT_LOFTED : ACT_SHOOT, car.pos, t, 1.f, false);
    if (!g.valid) return false;
    if (gk) {
        g.durationMs = (int)clampf((float)cfg_.panicHoldMs, 150.f, 600.f);   // keeper: long lofted kick with the pass button
    } else {
        g.actionType = ACT_SHOT;
        g.durationMs = (int)clampf((float)cfg_.panicHoldMs, 150.f, 900.f);
    }
    shotFiredMs_ = now;
    lastActMs_ = now;
    cooldownUntil_ = now + (double)g.durationMs + 500.0;
    nextDecisionAt_ = cooldownUntil_;
    out = g;
    return true;
}

// ---- forward-only build-up ----
void PitchEngine::filterForwardOnly(const PlayerState& car) {
    if (!cfg_.rescueRules || human_) return;
    {   // ray-cast occlusion: a defender within rayMin of a ground pass line disqualifies the target
        int w = 0;
        for (int i = 0; i < nCand_; i++) {
            const PassCandidate& c = cand_[i];
            bool keep = true;
            if (c.type == ACT_PASS || c.type == ACT_THROUGH) {
                float m = 99.f;
                for (int k = 0; k < TEAM_SIZE; k++) {
                    const PlayerState& o = state_.opp[k];
                    if (o.active) m = std::min(m, segmentDistance(car.pos, c.target, o.future));
                }
                if (m < cfg_.rayMin) keep = false;
            }
            if (!keep) continue;
            if (w != i) cand_[w] = cand_[i];
            w++;
        }
        nCand_ = w;
    }
    if (!cfg_.forwardOnly) return;
    const float s = (float)state_.attackSign;
    const float cx = s * car.pos.x;
    bool fwdOpen = false;
    for (int i = 0; i < nCand_; i++) {
        const PassCandidate& c = cand_[i];
        if (c.type != ACT_PASS && c.type != ACT_THROUGH && c.type != ACT_LOFTED) continue;
        if (s * c.target.x - cx > cfg_.forwardMin && c.clearance > cfg_.forwardSafeMargin && c.offside < 0.5f && c.xP >= 0.5f) fwdOpen = true;   // open means playable, not merely a long lane
    }
    const bool sidewaysOk = !fwdOpen;   // no safe forward lane: a lateral recycle beats dribbling into the block
    // own-half is forward-only. No back-pass / recycle toward our goal — that was the stolen-GK pattern.
    const float ownDepth = PITCH_HALF_X + cx;   // metres from our goal line (cx is attack-signed)
    const bool deepOwn = ownDepth < 38.f;
    const float crowdBand = ownDepth >= PITCH_HALF_X ? cfg_.crowdSideBandMid : cfg_.crowdSideBand;   // midfield forward: a short release beats dribbling into the press
    const float deepBand = deepOwn ? 0.0f : (state_.crowded ? crowdBand : cfg_.sideBand);
    int w = 0;
    for (int i = 0; i < nCand_; i++) {
        const PassCandidate& c = cand_[i];
        bool keep = true;
        if (c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED) {
            const float d = s * c.target.x - cx;
            if (deepOwn) {
                // strictly progressive in our half: no zero-gain or backward ground passes
                if (d < cfg_.forwardMin) keep = false;
            } else if (d <= cfg_.forwardMin) {
                keep = sidewaysOk && d >= -deepBand;
            }
        }
        if (!keep) continue;
        if (w != i) cand_[w] = cand_[i];
        w++;
    }
    nCand_ = w;
}

// ---- wing switch: five or more opponents around the carrier, hit the opposite touchline ----
void PitchEngine::addWingSwitch(const PlayerState& car) {
    switchIdx_ = -1;
    if (!cfg_.rescueRules || !cfg_.wingSwitch || human_ || nCand_ >= MAX_CAND) return;
    int dense = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (o.active && distance(o.future, car.pos) <= cfg_.switchRadius) dense++;
    }
    if (dense < cfg_.switchMinOpp) return;
    const float s = (float)state_.attackSign;
    const float cx = s * car.pos.x;
    int best = -1;
    float bestScore = -1e9f;
    MetricVector2D bestT;
    float bestXp = 0.f, bestClear = 0.f;
    for (int j = 0; j < TEAM_SIZE; j++) {
        if (j == state_.carrier) continue;
        const PlayerState& r = state_.team[j];
        if (!r.active || r.missed > cfg_.maxCoastFrames || isKeeperIdx(j)) continue;
        const float lat = std::fabs(r.future.y - car.pos.y);
        const bool oppositeSide = (r.future.y * car.pos.y) < 0.f || lat >= cfg_.switchMinLat;
        if (!oppositeSide || lat < cfg_.switchMinLat * 0.6f) continue;
        if (s * r.future.x < cx - 6.f) continue;
        if (!isKeeperIdx(state_.carrier) && s * r.future.x <= -(PITCH_HALF_X - cfg_.noPassZone)) continue;
        const MetricVector2D rt = r.future;
        const float len = distance(car.pos, rt);
        if (len > cfg_.switchMaxLen || len < 10.f) continue;
        const float open = nearestOppDistance(rt);
        if (open < cfg_.switchOpenMin) continue;
        if (offsideAmount(rt) > 0.5f) continue;
        float clear = 0.f;
        const float xp = passProbability(car.pos, rt, clear, false, true) * receiverReliability(r);
        if (xp < 0.30f) continue;
        const float score = std::min(open, 12.f) + 0.06f * lat + 0.04f * (s * rt.x - cx);
        if (score > bestScore) { bestScore = score; best = j; bestT = rt; bestXp = xp; bestClear = clear; }
    }
    if (best < 0) return;
    PassCandidate& c = cand_[nCand_];
    c.targetId = state_.team[best].id;
    c.type = ACT_LOFTED;
    c.target = bestT;
    c.length = distance(car.pos, bestT);
    c.clearance = bestClear;
    c.xP = bestXp;
    c.xPRaw = bestXp;
    c.deltaXT = computeExpectedThreat(car, bestT);
    c.pressure = lanePressure(bestT);
    c.offside = 0.f;
    c.utility = 0.f;
    c.prob = 0.f;
    c.diag = 0.f;
    switchIdx_ = nCand_;
    nCand_++;
}

// ---- crowded central midfield: lofted through ball over the line ----
void PitchEngine::addLoftedThrough(const PlayerState& car) {
    if (!cfg_.rescueRules || human_ || switchIdx_ >= 0 || nCand_ >= MAX_CAND) return;
    const float s = (float)state_.attackSign;
    int n = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        const float ax = s * o.future.x;
        if (ax > -12.5f && ax < 22.5f && std::fabs(o.future.y) < 14.f) n++;     // central grid x 40-75, y 20-48 of a 105 x 68 pitch
    }
    if (n < (int)cfg_.densityLoftN) return;
    int bi = -1;
    float bs = 0.f, bxp = 0.f, blc = 0.f;
    for (int i = 0; i < nCand_; i++) {   // the best through target whose lofted version can actually land
        const PassCandidate& c = cand_[i];
        const bool fwdPass = c.type == ACT_PASS && s * (c.target.x - car.pos.x) >= 6.f;   // a stationary runner well ahead can still be found over the press
        if ((c.type != ACT_THROUGH && !fwdPass) || c.offside > 0.5f) continue;
        float lc = 0.f;
        const float lxp = passProbability(car.pos, c.target, lc, false, true);
        if (lxp < 0.05f) continue;
        const float sc = std::max(c.deltaXT, 0.f) + 0.1f + 0.05f * lxp;
        if (sc > bs) { bs = sc; bi = i; bxp = lxp; blc = lc; }
    }
    if (bi < 0) return;
    const PassCandidate src = cand_[bi];
    PassCandidate& c = cand_[nCand_];
    c = src;
    c.type = ACT_LOFTED;
    c.clearance = blc;
    c.xP = c.xPRaw = bxp;
    c.utility = 0.f;
    switchIdx_ = nCand_;
    nCand_++;
}

// ---- carry toward the next receiver so the body is already aligned when the pass goes out ----
bool PitchEngine::preAlignedCarry(const PlayerState& car, int bestIdx, GestureOutput& g) {
    if (!cfg_.rescueRules || !cfg_.preAlign || human_ || bestIdx < 0 || bestIdx >= nCand_) { aimDirOn_ = false; return false; }
    const PassCandidate& c = cand_[bestIdx];
    const MetricVector2D toT = c.target - car.pos;
    const float tl = toT.length();
    if (tl < 1e-3f) return false;
    // both headings are compared as stick angles (the mapping to the screen is applied by makeGesture)
    // J(t) = (1 - lambda) J + lambda * unit(target - carrier): the stick eases toward the receiver over a few frames
    MetricVector2D tdir = toT * (1.f / tl);
    if (!aimDirOn_) {
        const float sp = car.vel5.length();
        aimDir_ = sp > 0.5f ? car.vel5 * (1.f / sp) : tdir;
        aimDirOn_ = true;
    }
    aimDir_ = aimDir_ * (1.f - cfg_.stickLambda) + tdir * cfg_.stickLambda;
    const float al = aimDir_.length();
    if (al > 1e-4f) aimDir_ = aimDir_ * (1.f / al); else aimDir_ = tdir;
    GestureOutput tg = makeGesture(ACT_CARRY, car.pos, car.pos + aimDir_ * 5.f, 0.f, g.dash);
    if (!tg.valid) return false;
    float dth = tg.theta - g.theta;
    while (dth > 180.f) dth -= 360.f;
    while (dth < -180.f) dth += 360.f;
    if (std::fabs(dth) > cfg_.preAlignMaxDeg) return false;
    // never lean into a defender standing on that heading
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        const MetricVector2D v = o.future - car.pos;
        const float d = v.length();
        if (d < 4.f && d > 1e-3f && v.dot(toT) / (d * tl) > 0.82f) return false;
    }
    g = tg;
    return true;
}

// ---- one-touch: pass command goes out shortly before the receiver controls the ball ----
bool PitchEngine::anticipatedPass(GestureOutput& out) {
    if (!cfg_.rescueRules || !cfg_.anticipate || human_ || !ballUsable() || antBusy_) return false;
    const double now = state_.timeMs;
    if (now - lastAntMs_ < 280.0 || now - lastActMs_ < 90.0) return false;  // faster re-arm for one-touch tempo
    const float v = state_.ballVel.length();
    if (!(v >= cfg_.anticipateMinSpeed)) return false;
    const MetricVector2D B = state_.ball;
    int recv = -1;
    float bestEta = 1e9f;
    for (int j = 0; j < TEAM_SIZE; j++) {
        const PlayerState& p = state_.team[j];
        if (!p.active || !p.visible || j == lastCarrierIdx_) continue;
        const MetricVector2D d = p.pos - B;
        const float dl = d.length();
        if (dl < 1e-3f || dl > 14.f) continue;
        if (state_.ballVel.dot(d) / (v * dl) < 0.88f) continue;       // the ball is heading at him
        const float eta = std::max(0.f, dl - 0.8f) / v * 1000.f;      // ms until he can touch it
        if (eta < bestEta) { bestEta = eta; recv = j; }
    }
    if (recv < 0 || (recv == antLastRecv_ && now - lastAntMs_ < 900.0)) return false;
    if (bestEta > (float)(cfg_.oneTouchPreMs + cfg_.oneTouchSlackMs)) return false;
    // an opponent who gets there first makes this a lost ball, not a one-touch
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        const MetricVector2D d = o.pos - B;
        const float dl = d.length();
        if (dl < 1e-3f) continue;
        if (state_.ballVel.dot(d) / (v * dl) < 0.88f) continue;
        if (std::max(0.f, dl - 0.8f) / v * 1000.f < bestEta - 30.f) return false;
    }
    antBusy_ = true;
    const int savedCarrier = state_.carrier;
    const int savedStatus = state_.status;
    const bool savedFace = cfg_.faceBeforePass;
    const double savedCool = cooldownUntil_;
    state_.carrier = recv;
    state_.status = ST_TEAM;
    cfg_.faceBeforePass = false;      // one-touch: no turning tick
    cooldownUntil_ = std::min(cooldownUntil_, now);
    GestureOutput g = decide();
    cfg_.faceBeforePass = savedFace;
    state_.carrier = savedCarrier;
    state_.status = savedStatus;
    antBusy_ = false;
    const bool pass = g.valid && (g.actionType == ACT_PASS || g.actionType == ACT_THROUGH || g.actionType == ACT_LOFTED ||
                                  g.actionType == ACT_SHOT);
    if (!pass) {
        cooldownUntil_ = std::max(savedCool, cooldownUntil_ > now ? cooldownUntil_ : savedCool);
        return false;
    }
    lastAntMs_ = now;
    antLastRecv_ = recv;
    g.status = ST_TEAM;
    out = g;
    return true;
}

// ---- opposition sideways / backward pass in midfield: opens the line step-up window ----
void PitchEngine::updateOppPass(int oppIdx) {
    const double now = state_.timeMs;
    if (oppIdx < 0) {
        if (oppCarrier_ >= 0 && now - oppPassMs_ > 1500.0) oppCarrier_ = -1;
        return;
    }
    const MetricVector2D pos = state_.opp[oppIdx].pos;
    if (oppCarrier_ >= 0 && oppIdx != oppCarrier_ && now - oppPassMs_ <= 1500.0) {
        const float s = (float)state_.attackSign;
        const MetricVector2D d = pos - oppCarrierPos_;
        const float fp = -s * d.x;                 // their progress toward our goal (positive = forward)
        const bool backward = fp < -1.0f;
        const bool sideways = std::fabs(fp) <= 1.5f && std::fabs(d.y) >= 3.0f;
        float lastDef = 1e9f;   // distance from the opposition ball to our last (deepest outfield) defender
        {
            float bd = 1e9f;
            MetricVector2D lp;
            for (int i = 0; i < TEAM_SIZE; i++) {
                if (!state_.team[i].active || isKeeperIdx(i)) continue;
                const float dpt = goalDistance(state_.team[i].pos);
                if (dpt < bd) { bd = dpt; lp = state_.team[i].pos; }
            }
            if (bd < 1e8f) lastDef = distance(pos, lp);
        }
        if ((backward || sideways) && lastDef > cfg_.stepMinGap && std::fabs(pos.x) <= cfg_.lineStepMid && std::fabs(oppCarrierPos_.x) <= cfg_.lineStepMid)
            lineStepUntil_ = now + (double)cfg_.lineStepMs;
    }
    oppCarrier_ = oppIdx;
    oppCarrierPos_ = pos;
    oppPassMs_ = now;
}

float PitchEngine::passProbability(const MetricVector2D& from, const MetricVector2D& to, float& clearance,
                                   bool through, bool lofted) const {
    const float len = distance(from, to);
    if (!finite(len) || len < cfg_.minPassLen) {
        clearance = 0.f;
        return 0.f;
    }
    float lane = 1e3f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        lane = std::min(lane, segmentDistance(from, to, o.future));
    }
    const float lengthTerm = std::max(0.3f, 1.f - len / 90.f);
    float xp;
    if (lofted) {
        // a lofted ball is slower and can be contested at the landing spot: can a defender get there first?
        const float tFlight = len / std::max(cfg_.passSpeed * 0.7f, 1.f);
        float worst = 50.f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (!o.active) continue;
            worst = std::min(worst, distance(o.future, to) - (cfg_.defSpeed * (tFlight + defReactEff()) + cfg_.defBody));
        }
        clearance = clampf(worst + cfg_.clearHalf, 0.f, 50.f);
        xp = logistic(cfg_.clearSlope * (worst - 1.5f)) * 0.7f * lengthTerm;
    } else {
        (void)lane;
        const float margin = interceptMargin(from, to);
        clearance = clampf(margin + cfg_.clearHalf, 0.f, 50.f);   // effective metres, comparable to a lane clearance
        xp = logistic(cfg_.clearSlope * (margin - cfg_.marginHalf)) * lengthTerm;
        if (through) xp *= 0.92f;   // less penalty on calculated through balls (still require good margin)
    }
    return clampf(safe(xp, 0.f), 0.f, 1.f);
}

float PitchEngine::opennessOf(const MetricVector2D& p) const {
    return clampf(nearestOppDistance(p) / std::max(cfg_.openRadius, 0.1f), 0.f, 1.f);
}

float PitchEngine::receiverReliability(const PlayerState& r) const {
    if (r.visible || r.missed <= 0) return 1.f;
    return std::max(0.5f, std::pow(clampf(cfg_.occludedPassFactor, 0.5f, 1.f), (float)r.missed));
}

void PitchEngine::updateCrowd() {
    state_.crowd = 0.f;
    state_.crowdOpp = 0;
    state_.nearestOpp = 1e3f;
    state_.crowded = false;
    if (!cfg_.keepBall || state_.carrier < 0 || state_.carrier >= TEAM_SIZE) {
        crowdSinceMs_ = -1;
        return;
    }
    const MetricVector2D c = state_.team[state_.carrier].future;
    const float R = std::max(cfg_.crowdRadius, 0.5f);
    float sum = 0.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || o.missed > cfg_.maxCoastFrames) continue;
        const float d = distance(c, o.future);
        state_.nearestOpp = std::min(state_.nearestOpp, d);
        if (d < R) {
            state_.crowdOpp++;
            sum += 1.f - d / R;
        }
    }
    state_.crowd = clampf(sum / 2.5f, 0.f, 1.f);
    state_.crowded = state_.crowdOpp >= cfg_.crowdMinOpp || state_.nearestOpp < cfg_.crowdTight;
    if (state_.crowded) {
        if (crowdSinceMs_ < 0) crowdSinceMs_ = state_.timeMs;
    } else {
        crowdSinceMs_ = -1;
    }
}

void PitchEngine::applyKeepBall(const PlayerState&) {
    const float kb = clampf(cfg_.keepBias + effStrat().dKeep, 0.f, 1.f) * (cfg_.legendary ? 1.25f : 1.f);
    const bool crowded = state_.crowded;
    for (int i = 0; i < nCand_; i++) {
        PassCandidate& c = cand_[i];
        if (c.type == ACT_SHOOT) continue;
        if (c.type == ACT_CARRY) {
            c.utility -= cfg_.carryCrowdPenalty * kb * (crowded ? 0.5f + state_.crowd : state_.crowd * 0.5f);
            continue;
        }
        const float open = opennessOf(c.target);
        c.utility += cfg_.wOpen * kb * open * c.xP;
        if (c.offside > 0.5f) continue;
        if (crowded) {
            if (c.xP >= cfg_.safeXp)
                c.utility += cfg_.wKeep * kb * c.xP * c.xP;
            else
                c.utility -= cfg_.riskyPassPenalty * kb * (cfg_.safeXp - c.xP);
        }
    }
}

MetricVector2D PitchEngine::runnerLead(const PlayerState& r, float passLen) const {
    const float sp = r.vel.length();
    if (!(sp >= cfg_.leadMinSpeed)) return MetricVector2D();
    const MetricVector2D dir = r.vel * (1.f / sp);
    const float t = clampf(passLen / std::max(cfg_.passSpeed, 1.f), 0.f, cfg_.leadMaxSec);
    const float extraT = std::max(0.f, t - cfg_.horizonSec - (cfg_.kalman ? effLatencyMs() * 0.001f : 0.f));
    float along = r.acc.dot(dir);
    if (std::fabs(along) < cfg_.accMin) along = 0.f;
    along = clampf(along, -cfg_.accMax, cfg_.accMax);
    float d = sp * extraT * cfg_.leadVelFactor + 0.5f * along * t * t;
    d = clampf(d, -1.5f, cfg_.leadMaxM);
    // dynamic through lead: modest extra only; hard-capped at leadMaxM so playmaker lead tests stay in band
    if (cfg_.dynamicThroughLead) {
        const float s = (float)state_.attackSign;
        const float fwdSp = std::max(0.f, s * r.vel.x);
        if (fwdSp >= 2.5f)
            d += std::min(0.4f, fwdSp * cfg_.throughLeadPerMs);
        d = clampf(d, -1.5f, cfg_.leadMaxM);
    }
    return dir * d;
}

float PitchEngine::oppGkDepth() const {
    const float s = (float)state_.attackSign;
    float best = 0.f;
    bool any = false;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        const float ax = s * o.future.x;
        if (!any || ax > best) { best = ax; any = true; }
    }
    if (!any) return 0.f;
    return std::max(0.f, PITCH_HALF_X - best);
}

// 1) Conversion-first: reward high-xG shots and open-goal / 1v1 looks; demote low-angle contested efforts.
float PitchEngine::conversionShotBonus(const PlayerState& car, const PassCandidate& c) const {
    if (!cfg_.conversionFirst || c.type != ACT_SHOOT) return 0.f;
    const float s = (float)state_.attackSign;
    const float ax = s * car.future.x;
    const float dg = std::sqrt((PITCH_HALF_X - ax) * (PITCH_HALF_X - ax) + car.future.y * car.future.y);
    const float xg = xTGrid::shotValue(ax, car.future.y);
    float b = 0.f;
    // reward true quality hard — possession without goals loses matches
    b += 1.35f * xg;
    if (std::fabs(car.future.y) <= 10.f && dg <= 16.f) b += 0.40f;  // central box
    if (std::fabs(car.future.y) <= 6.f && dg <= 12.f) b += 0.35f;   // six-yard / near post
    const float no = nearestOppDistance(car.future);
    if (dg <= cfg_.boxShotRange && no >= cfg_.openGoalOpp) b += 0.70f;  // open look
    if (dg <= 12.f && no >= 2.2f) b += 0.40f;                       // close 1v1
    if (std::fabs(car.future.y) > 16.f && dg > 18.f) b -= 0.30f;    // wide long junk
    if (c.xP < 0.22f) b -= 0.12f;
    if (dg > 22.f && xg < 0.08f) b -= 0.35f;                        // kill low-xG from range
    return b;
}

// 2) Box occupation: reward passes that put a team-mate into the central shooting zone.
float PitchEngine::boxOccupationBonus(const PlayerState& car, const PassCandidate& c) const {
    if (!cfg_.boxOccupy) return 0.f;
    if (!(c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED)) return 0.f;
    if (c.xP < 0.50f || c.offside > 0.4f) return 0.f;
    const float s = (float)state_.attackSign;
    const float ax = s * c.target.x;
    const float depth = PITCH_HALF_X - ax;
    if (depth > cfg_.boxOccupyDepth || depth < 1.f) return 0.f;
    if (std::fabs(c.target.y) > cfg_.boxOccupyHalfY) return 0.f;
    const float free = nearestOppDistance(c.target);
    if (free < 2.0f) return 0.f;
    const float q = xTGrid::shotValue(ax, c.target.y);
    return cfg_.wBoxOccupy * c.xP * (0.4f + 0.6f * clampf(q / 0.25f, 0.f, 1.f)) * clampf(free / 5.f, 0.3f, 1.f);
}

// 3) Flank cross: early delivery from the touchline into the box when a target is free.
float PitchEngine::flankCrossBonus(const PlayerState& car, const PassCandidate& c) const {
    if (!cfg_.flankCross) return 0.f;
    if (!(c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED)) return 0.f;
    if (c.xP < 0.48f || c.offside > 0.4f) return 0.f;
    if (std::fabs(car.pos.y) < cfg_.flankCrossMinY) return 0.f;
    const float s = (float)state_.attackSign;
    const float axT = s * c.target.x;
    const float depth = PITCH_HALF_X - axT;
    if (depth > 18.f || depth < 1.f) return 0.f;
    if (std::fabs(c.target.y) > 14.f) return 0.f;
    if (c.length > cfg_.flankCrossMaxLen) return 0.f;
    const float free = nearestOppDistance(c.target);
    if (free < 2.2f) return 0.f;
    return cfg_.wFlankCross * c.xP * clampf(free / 5.f, 0.3f, 1.f);
}

// 4) Chip GK: when the keeper is well off his line, reward a lofted finish / chip-style delivery.
float PitchEngine::chipGkBonus(const PlayerState& car, const PassCandidate& c) const {
    if (!cfg_.chipGk) return 0.f;
    const float gkD = oppGkDepth();
    if (gkD < cfg_.chipGkDepth) return 0.f;
    const float s = (float)state_.attackSign;
    const float ax = s * car.future.x;
    const float dg = std::sqrt((PITCH_HALF_X - ax) * (PITCH_HALF_X - ax) + car.future.y * car.future.y);
    if (c.type == ACT_SHOOT && dg >= cfg_.chipMinDist && dg <= cfg_.chipMaxDist)
        return cfg_.wChip * clampf((gkD - cfg_.chipGkDepth) / 8.f, 0.f, 1.f);
    if ((c.type == ACT_LOFTED || c.type == ACT_THROUGH) && c.xP >= 0.50f) {
        const float axT = s * c.target.x;
        if (PITCH_HALF_X - axT <= 14.f && std::fabs(c.target.y) <= 10.f)
            return cfg_.wChip * 0.6f * c.xP;
    }
    return 0.f;
}

// ball from outside the box to a free man inside it (and a free carry into it)
float PitchEngine::boxEntryBonus(const PlayerState& car, const PassCandidate& c) const {
    if (!cfg_.boxEntry || c.offside > 0.4f) return 0.f;
    const float s = (float)state_.attackSign;
    const float axC = s * car.future.x;
    if (PITCH_HALF_X - axC <= cfg_.boxEntryDepth) return 0.f;   // already inside: boxOccupationBonus covers it
    if (c.type == ACT_CARRY) {
        const float axT = s * c.target.x;
        if (PITCH_HALF_X - axT <= cfg_.boxEntryDepth && std::fabs(c.target.y) <= cfg_.boxEntryHalfY && nearestOppDistance(car.future) >= 4.5f)
            return 0.35f * cfg_.wBoxEntry;
        return 0.f;
    }
    if (!(c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED) || c.xP < 0.62f) return 0.f;
    const float axT = s * c.target.x;
    if (PITCH_HALF_X - axT > cfg_.boxEntryDepth || std::fabs(c.target.y) > cfg_.boxEntryHalfY) return 0.f;
    const float free = nearestOppDistance(c.target);
    if (free < 2.5f) return 0.f;
    return cfg_.wBoxEntry * c.xP * clampf(free / 5.f, 0.4f, 1.f);
}

// first seconds after winning the ball: a clean forward ball into space
float PitchEngine::counterBreakBonus(const PlayerState& car, const PassCandidate& c) const {
    if (!cfg_.counterBreak || state_.timeMs - ballWonMs_ > (double)cfg_.counterBreakMs) return 0.f;
    if (!(c.type == ACT_PASS || c.type == ACT_THROUGH) || c.xP < 0.70f || c.offside > 0.4f) return 0.f;
    const float s = (float)state_.attackSign;
    const float gain = s * (c.target.x - car.pos.x);
    if (gain < 9.f) return 0.f;
    const float free = nearestOppDistance(c.target);
    if (free < 3.0f) return 0.f;
    return cfg_.wCounterBreak * c.xP * clampf(gain / 22.f, 0.3f, 1.f);
}

// a defender standing on the receiver wins the ball the moment it arrives
float PitchEngine::receiverContestPenalty(const PassCandidate& c) const {
    if (!(c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED)) return 0.f;
    const float d = nearestOppDistance(c.target);
    if (d >= cfg_.recvContestR) return 0.f;
    return cfg_.wRecvContest * (1.f - d / cfg_.recvContestR);
}

// losing the ball deep costs a counter-attack; losing it high is a counter-press chance
float PitchEngine::turnoverCostPenalty(const PlayerState& car, const PassCandidate& c) const {
    if (!cfg_.turnoverCost) return 0.f;
    if (!(c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED) || c.xP >= 0.60f) return 0.f;
    const float ax = (float)state_.attackSign * car.future.x;
    const float zone = 1.f - clampf((ax - cfg_.turnoverFullAx) / (30.f - cfg_.turnoverFullAx), 0.f, 1.f);
    return cfg_.wTurnover * zone * (0.60f - c.xP) / 0.60f;
}

// ---- Scoring Machine helpers ----

// Count net attackers vs defenders inside a zone measured from the opponent goal line.
int PitchEngine::countZoneNet(float depthMax, float halfY) const {
    const float s = (float)state_.attackSign;
    int att = 0, def = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (state_.team[i].active) {
            const float depth = PITCH_HALF_X - s * state_.team[i].future.x;
            if (depth >= 0.f && depth <= depthMax && std::fabs(state_.team[i].future.y) <= halfY) ++att;
        }
        if (state_.opp[i].active) {
            const float depth = PITCH_HALF_X - s * state_.opp[i].future.x;
            if (depth >= 0.f && depth <= depthMax && std::fabs(state_.opp[i].future.y) <= halfY) ++def;
        }
    }
    return att - def;
}

// Clinical finishing: demote low-xG contested shots so the bot creates better looks instead of blasting.
float PitchEngine::clinicalShotPenalty(const PlayerState& car, const PassCandidate& c) const {
    if (!cfg_.scoringMachine || !cfg_.clinicalShot || c.type != ACT_SHOOT) return 0.f;
    const float s = (float)state_.attackSign;
    const float ax = s * car.future.x;
    const float dg = std::sqrt((PITCH_HALF_X - ax) * (PITCH_HALF_X - ax) + car.future.y * car.future.y);
    const float xg = xTGrid::shotValue(ax, car.future.y);
    const float no = nearestOppDistance(car.future);
    // Open goal / 1v1 in the box: never demote.
    if (dg <= 12.f && no >= cfg_.openGoalOpp) return 0.f;
    const float thresh = (dg <= cfg_.boxShotRange) ? cfg_.clinicalBoxXg : cfg_.clinicalMinXg;
    if (xg >= thresh && c.xP >= 0.35f) return 0.f;
    // Contested low-xG: strong demote so cut-back / progress wins. (BEAST: harder on long spam)
    float pen = cfg_.wClinicalDemote * (thresh - xg) / std::max(thresh, 0.05f);
    if (no < 2.8f) pen += 0.22f;
    if (std::fabs(car.future.y) > 14.f && dg > 16.f) pen += 0.22f;
    if (dg > 16.f && xg < 0.10f) pen += 0.45f;  // kill low-xG from distance harder
    if (dg > 20.f && xg < 0.14f) pen += 0.35f;
    if (dg > 24.f) pen += 0.30f;                 // almost never prefer 25m+ efforts
    return clampf(pen, 0.f, 1.60f);
}

// Final-third / box overload: when we have numbers, reward the progressive option that uses the overload.
float PitchEngine::overloadBonus(const PlayerState& car, const PassCandidate& c) const {
    if (!cfg_.scoringMachine || !cfg_.finalOverload) return 0.f;
    const int net = countZoneNet(cfg_.overloadDepth, cfg_.overloadHalfY);
    if (net < cfg_.overloadMinNet) return 0.f;
    const float s = (float)state_.attackSign;
    const float depthCar = PITCH_HALF_X - s * car.future.x;
    if (depthCar > cfg_.overloadDepth + 8.f) return 0.f;  // carrier too far back
    if (c.type == ACT_SHOOT) {
        // Prefer the shot when the overload is already in the box.
        const int boxNet = countZoneNet(16.f, 12.f);
        if (boxNet >= 1) return cfg_.wOverload * 0.9f;
        return 0.f;
    }
    if (!(c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED || c.type == ACT_CARRY)) return 0.f;
    const float depthT = (c.type == ACT_CARRY) ? (PITCH_HALF_X - s * c.target.x) : (PITCH_HALF_X - s * c.target.x);
    if (depthT > cfg_.overloadDepth) return 0.f;
    if (c.type != ACT_CARRY && (c.xP < 0.55f || c.offside > 0.45f)) return 0.f;
    const float free = nearestOppDistance(c.target);
    const float q = clampf((float)net / 3.f, 0.3f, 1.f) * clampf(free / 4.f, 0.4f, 1.f);
    return cfg_.wOverload * q * (c.type == ACT_CARRY ? 0.7f : c.xP);
}

// Pressure-aware carry: reward a dribble that moves toward free support and away from the nearest defender.
float PitchEngine::supportSeekCarryBonus(const PlayerState& car, const PassCandidate& c) const {
    if (!cfg_.scoringMachine || !cfg_.pressureCarry || c.type != ACT_CARRY) return 0.f;
    const float press = nearestOppDistance(car.future);
    if (press >= cfg_.pressCarryR) return 0.f;
    // Find nearest free team-mate ahead or level.
    const float s = (float)state_.attackSign;
    float bestSupport = 1e9f;
    MetricVector2D bestPos;
    bool found = false;
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (i == state_.carrier || !state_.team[i].active) continue;
        const float dx = s * (state_.team[i].future.x - car.future.x);
        if (dx < -3.f) continue;  // not too deep
        const float d = distance(car.future, state_.team[i].future);
        if (d < 4.f || d > 18.f) continue;
        if (nearestOppDistance(state_.team[i].future) < 2.2f) continue;
        if (d < bestSupport) { bestSupport = d; bestPos = state_.team[i].future; found = true; }
    }
    if (!found) return 0.f;
    // Does the carry direction close the gap to that support?
    MetricVector2D toSupport = bestPos - car.future;
    const float ls = toSupport.length();
    if (ls < 1e-3f) return 0.f;
    toSupport = toSupport * (1.f / ls);
    MetricVector2D carryDir = c.target - car.future;
    const float lc = carryDir.length();
    if (lc < 1e-3f) return 0.f;
    carryDir = carryDir * (1.f / lc);
    const float align = carryDir.dot(toSupport);
    if (align < 0.25f) return 0.f;
    // Also prefer moving away from nearest opponent.
    float away = 0.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (!state_.opp[i].active) continue;
        const float d = distance(car.future, state_.opp[i].future);
        if (d > cfg_.pressCarryR || d < 1e-3f) continue;
        MetricVector2D awayDir = car.future - state_.opp[i].future;
        const float la = awayDir.length();
        if (la < 1e-3f) continue;
        awayDir = awayDir * (1.f / la);
        away = std::max(away, carryDir.dot(awayDir));
    }
    return cfg_.wSupportSeek * (0.55f * align + 0.45f * clampf(away, 0.f, 1.f)) * (1.f - press / cfg_.pressCarryR);
}

// Right after a turnover: prefer safe short options, demote speculative long balls, help recovery.
float PitchEngine::postLossAdjust(const PlayerState& car, const PassCandidate& c) const {
    if (!cfg_.scoringMachine || !cfg_.postLossCompact) return 0.f;
    if (state_.timeMs > postLossUntil_) return 0.f;
    const bool isPass = c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED;
    if (isPass) {
        if (c.length <= 12.f && c.xP >= 0.70f && c.clearance >= 2.5f) return cfg_.wPostLossSafe * c.xP;
        if (c.length > 22.f || c.xP < 0.55f) return -cfg_.wPostLossSafe * 0.9f;
    }
    if (c.type == ACT_CARRY) {
        // Prefer carries that stay compact (not exploding forward alone).
        const float s = (float)state_.attackSign;
        const float gain = s * (c.target.x - car.pos.x);
        if (gain > 8.f && nearestOppDistance(c.target) < 4.f) return -0.12f;
    }
    return 0.f;
}

void PitchEngine::armPostLoss() {
    if (!cfg_.scoringMachine || !cfg_.postLossCompact) return;
    postLossUntil_ = frameTimeMs_ + (double)cfg_.postLossMs;   // frame clock (state_.timeMs lags a frame at turnover)
}

// 5) Arm a short second-ball window after a shot so the nearest player hunts the rebound.
void PitchEngine::armSecondBall(const MetricVector2D& from) {
    if (!cfg_.secondBall) return;
    secondBallUntil_ = state_.timeMs + (double)cfg_.secondBallMs;
    secondBallAt_ = from;
}

float PitchEngine::scoreClockUrgency() const {
    if (!cfg_.scoreUrgency) return 0.f;
    float u = 0.f;
    const float phase = plus_.phase;
    if (phase >= cfg_.lateGameFrac) u += cfg_.lateUrgencyBoost;
    // extra urgency when trailing
    if (cfg_.scoringMachine && cfg_.highTempoTrail && plus_.goalsAgainst > plus_.goalsFor)
        u += cfg_.scoreBehindBoost;
    return clampf(u, 0.f, 0.55f);
}

bool PitchEngine::finalThirdMustProgress(const PlayerState& car, double now) const {
    if (cfg_.boxMustProgressSec <= 0.f) return false;
    const float s = (float)state_.attackSign;
    const float depth = PITCH_HALF_X - s * car.pos.x;
    if (depth > cfg_.finalThirdDepth) return false;
    if (finalThirdSince_ < 0) return false;
    return (now - finalThirdSince_) >= (double)cfg_.boxMustProgressSec * 1000.0;
}

int PitchEngine::pickBestProgressive(int n, const PlayerState& car) const {
    int best = -1;
    float bestSc = -1e9f;
    const float s = (float)state_.attackSign;
    for (int i = 0; i < n; i++) {
        const PassCandidate& c = cand_[i];
        float sc = c.utility;
        if (c.type == ACT_SHOOT) sc += 0.35f + conversionShotBonus(car, c);
        else if (c.type == ACT_THROUGH || c.type == ACT_LOFTED) sc += 0.50f * std::max(0.f, c.deltaXT) + 0.18f;  // ELITE: through priority
        else if (c.type == ACT_PASS && s * (c.target.x - car.pos.x) > 2.f) sc += 0.08f * c.xP;
        else if (c.type == ACT_CARRY) sc -= 0.20f;
        if (c.deltaXT <= 0.f && c.type != ACT_SHOOT) sc -= 0.45f;  // BEAST: harder penalty on non-progressive
        if (sc > bestSc) { bestSc = sc; best = i; }
    }
    return best;
}

float PitchEngine::nearestOppExcluding(const MetricVector2D& p, int skip) const {
    float best = 1e3f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (i == skip) continue;
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        best = std::min(best, distance(o.future, p));
    }
    return best;
}

float PitchEngine::zoneDensityAt(const MetricVector2D& p) const {
    const float R = std::max(cfg_.zoneR, 0.5f);
    float sum = 0.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active) continue;
        const float d = distance(o.future, p);
        if (d < R) sum += 1.f - d / R;
    }
    return clampf(sum / 2.5f, 0.f, 1.f);
}

void PitchEngine::updatePlaymaker() {
    state_.pressTtc = 1e3f;
    state_.committed = 0;
    state_.pressDist = 1e3f;
    state_.oneTouch = false;
    state_.congestion = 0.f;
    nCommit_ = 0;
    if (!cfg_.playmaker || state_.carrier < 0 || state_.carrier >= TEAM_SIZE) return;
    const PlayerState& car = state_.team[state_.carrier];
    const float s = (float)state_.attackSign;
    int inZone = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || o.missed > 2) continue;
        const MetricVector2D rel = car.pos - o.pos;
        const float d = rel.length();
        state_.pressDist = std::min(state_.pressDist, d);
        const float ax = s * (o.future.x - car.pos.x);
        if (ax >= -2.f && ax <= cfg_.zoneAhead && std::fabs(o.future.y - car.pos.y) <= cfg_.zoneHalfWidth) inZone++;
        if (!(d > 1e-3f)) continue;
        const MetricVector2D u = rel * (1.f / d);
        const float closing = (o.vel - car.vel).dot(u);
        const float toward = o.vel.dot(u);
        if (closing >= cfg_.closeMin) {
            const float ttc = std::max(0.f, d - cfg_.contactRadius) / closing;
            state_.pressTtc = std::min(state_.pressTtc, ttc);
        }
        if (d <= cfg_.baitRadius && toward >= cfg_.baitCommitSpeed && nCommit_ < TEAM_SIZE) commitIdx_[nCommit_++] = i;
    }
    state_.committed = nCommit_;
    state_.oneTouch = state_.pressDist <= cfg_.oneTouchRadius || state_.pressTtc <= cfg_.oneTouchTtc;
    const float span = (float)std::max(cfg_.zoneFull - cfg_.zoneFree, 1);
    state_.congestion = clampf((float)(inZone - cfg_.zoneFree) / span, 0.f, 1.f);
}

void PitchEngine::applyPlaymaker(const PlayerState& car) {
    const float s = (float)state_.attackSign;
    const int clutchNow = effClutch();
    const bool protect = clutchNow == 1, chase = clutchNow == 2;
    float urgency = 0.f;
    if (state_.pressTtc < cfg_.releaseTtc) urgency = clampf(1.f - state_.pressTtc / std::max(cfg_.releaseTtc, 0.1f), 0.f, 1.f);
    float deficit = 0.f;
    const float tempoTarget = clampf(cfg_.tempoTarget + effStrat().dTempo, 0.3f, 0.85f);
    if (possOwnedSec_ >= cfg_.tempoMinSec && tempoTarget > 0.05f)
        deficit = clampf((tempoTarget - possShare_) / tempoTarget, 0.f, 1.f);
    if (protect) deficit = std::max(deficit, 0.85f);
    if (chase) deficit = 0.f;
    const float baitFactor = nCommit_ >= 2 ? 1.f : (nCommit_ == 1 ? 0.6f : 0.f);
    const float cong = state_.congestion;
    const bool oneTouch = state_.oneTouch;

    for (int i = 0; i < nCand_; i++) {
        PassCandidate& c = cand_[i];
        if (c.type == ACT_SHOOT) continue;
        if (c.type == ACT_CARRY) {
            c.utility -= cfg_.wRelease * urgency;
            if (oneTouch) c.utility -= cfg_.wOneTouch;
            c.utility -= cfg_.wZone * cong * 0.5f;
            if (protect) c.utility -= cfg_.wClutch * 0.5f;
            continue;
        }
        if (c.offside > 0.5f) continue;
        const bool safePass = c.xP >= cfg_.safeXp;
        const float gain = s * (c.target.x - car.pos.x);
        const bool lateral = gain <= cfg_.tempoLateralFwd;
        const float dens = cong > 0.f ? zoneDensityAt(c.target) : 0.f;

        if (urgency > 0.f) {
            if (safePass) c.utility += cfg_.wRelease * urgency * c.xP;
            else c.utility -= cfg_.wRelease * urgency * 0.5f * (cfg_.safeXp - c.xP);
        }
        if (oneTouch) {
            if (safePass) {
                const float shortF = clampf(1.f - c.length / std::max(cfg_.oneTouchMaxLen, 5.f), 0.2f, 1.f);
                c.utility += cfg_.wOneTouch * c.xP * opennessOf(c.target) * shortF;
            } else {
                c.utility -= cfg_.wOneTouch * (cfg_.safeXp - c.xP);
            }
        }
        if (c.type == ACT_THROUGH && c.diag > 0.f) c.utility += cfg_.wDiag * c.xP;
        // Calculated through before the ball is stolen (BEAST: stronger, lower bar)
        if (c.type == ACT_THROUGH && c.xP >= 0.48f) {
            const float pressBonus = clampf(state_.crowd * 0.35f + (state_.nearestOpp < 3.5f ? 0.25f : 0.f), 0.f, 0.55f);
            c.utility += 0.48f * c.xP * (0.7f + pressBonus);
            // reward progressive through into the final third
            if (c.deltaXT > 0.04f) c.utility += 0.22f * c.xP;
        }
        if (c.type == ACT_THROUGH && c.xP >= 0.50f && c.deltaXT > 0.02f)
            c.utility += cfg_.wThrough * c.xP * clampf(c.deltaXT / 0.12f, 0.4f, 1.2f);
        if (cfg_.rescueRules && c.targetId > 0 && c.targetId != 98 && s * car.pos.x > cfg_.shotPriorityAx &&
            (PITCH_HALF_X - s * c.target.x) <= cfg_.boxShotRange && std::fabs(c.target.y) <= 20.f &&
            nearestOppDistance(c.target) >= cfg_.boxPassFree)
            c.utility += cfg_.wBoxPass * c.xP;

        if (baitFactor > 0.f && c.targetId > 0 && c.targetId != 98) {
            float best = 0.f;
            for (int k = 0; k < nCommit_; k++) {
                const PlayerState& o = state_.opp[commitIdx_[k]];
                const float osp = o.vel.length();
                if (!(osp > 0.1f)) continue;
                const MetricVector2D toTarget = c.target - o.pos;
                if (toTarget.dot(o.vel * (1.f / osp)) >= 0.f) continue;
                const float free = clampf(nearestOppExcluding(c.target, commitIdx_[k]) / std::max(cfg_.openRadius, 0.1f), 0.f, 1.f);
                best = std::max(best, free);
            }
            const float ay = std::fabs(c.target.y);
            const float sector = ay >= cfg_.wingY ? 1.f + cfg_.wingBonus : (ay >= 0.5f * cfg_.wingY ? 1.f + 0.5f * cfg_.wingBonus : 1.f);
            const float forwardF = 0.7f + 0.3f * clampf(gain / 15.f, 0.f, 1.f);
            c.utility += cfg_.wBait * baitFactor * c.xP * best * sector * forwardF;
        }
        if (cong > 0.f) {
            if (lateral) {
                if (safePass) c.utility += cfg_.wZone * cong * (1.f - dens) * c.xP;
            } else {
                c.utility -= cfg_.wZone * cong * dens * (0.5f + 0.5f * (1.f - c.xP));
            }
        }
        if (deficit > 0.f) {
            if (safePass && lateral) c.utility += cfg_.wTempo * deficit * c.xP;
            else if (!safePass) c.utility -= cfg_.wTempo * deficit * (cfg_.safeXp - c.xP);
        }
        if (protect) {
            if (!lateral && c.xP < cfg_.clutchSafeXp) c.utility -= cfg_.wClutch * (cfg_.clutchSafeXp - c.xP);
        } else if (chase) {
            if (!lateral) c.utility += cfg_.wChase * clampf(gain / 20.f, 0.f, 1.f) * c.xP;
            if (c.type == ACT_THROUGH) c.utility += cfg_.wChase * 0.5f * c.xP;
            if (gain <= 0.f) c.utility -= cfg_.wChase * 0.25f * c.xP;
        }
    }
}

int PitchEngine::calculatePassProbabilities(const MatchState& st) {
    nCand_ = 0;
    chosen_ = -1;
    if (st.carrier < 0 || st.carrier >= TEAM_SIZE) return 0;
    if (st.timeMs - lastOwnCallMs_ > 700.0) ballWonMs_ = st.timeMs;   // we have just won the ball
    lastOwnCallMs_ = st.timeMs;
    updateCrowd();
    updatePlaymaker();
    const PlayerState& car = st.team[st.carrier];
    const MetricVector2D from = car.pos;
    const float s = (float)st.attackSign;
    const MetricVector2D fwd(s, 0.f);

    const bool carIsGk = isKeeperIdx(st.carrier);
    auto add = [&](int id, int type, const MetricVector2D& to, float xp, float clear, bool clampDxt) {
        if (nCand_ >= MAX_CAND) return;
        if (type == ACT_PASS || type == ACT_THROUGH || type == ACT_LOFTED) {
            if (distance(from, to) > cfg_.safePassLen) return;                                   // tiki-taka: no long balls
            if (cfg_.gkPassBan && !carIsGk && s * to.x <= -(PITCH_HALF_X - cfg_.noPassZone)) return;   // nothing into our own goal mouth
        }
        PassCandidate& c = cand_[nCand_++];
        c.targetId = id;
        c.type = type;
        c.target = to;
        c.length = distance(from, to);
        c.clearance = clear;
        c.xP = xp;
        c.xPRaw = xp;
        float d = computeExpectedThreat(car, to);
        if (clampDxt) d = std::max(d, cfg_.minDeltaXT);
        c.deltaXT = d;
        c.pressure = lanePressure(to);
        c.offside = 0.f;
        c.utility = 0.f;
        c.prob = 0.f;
        c.diag = 0.f;
        if (attLearned_ && xp > 0.f && (type == ACT_PASS || type == ACT_THROUGH || type == ACT_LOFTED)) {
            float f[ATT_NF];
            attackFeatures(xp, c.pressure, st.crowd, c.length, s * (to.x - from.x), f);
            c.xP = clampf(attackProb(f), 0.01f, 0.99f);
        }
    };

    for (int j = 0; j < TEAM_SIZE; j++) {
        if (j == st.carrier) continue;
        const PlayerState& r = st.team[j];
        if (!r.active || r.missed > cfg_.maxCoastFrames) continue;
        if (cfg_.gkPassBan && isKeeperIdx(j)) continue;                       // the goalkeeper is never a target
        if (!carIsGk && s * r.future.x <= -(PITCH_HALF_X - cfg_.noPassZone)) continue;   // never goal-line players
        if (!carIsGk && s * (r.future.x - from.x) < -0.5f && goalDistance(r.future) < cfg_.backPassBox) continue;   // no back pass towards our keeper
        float len = distance(from, r.future);
        if (len < cfg_.minPassLen || len > cfg_.maxPassLen) continue;

        MetricVector2D rt = r.future;
        if (cfg_.playmaker) {
            rt = r.future + runnerLead(r, len);
            rt.x = clampf(rt.x, -PITCH_HALF_X, PITCH_HALF_X);
            rt.y = clampf(rt.y, -PITCH_HALF_Y, PITCH_HALF_Y);
            len = distance(from, rt);
            if (len < cfg_.minPassLen || len > cfg_.maxPassLen) { rt = r.future; len = distance(from, rt); }
        }
        const float rel = receiverReliability(r);
        float clear = 0.f;
        float xp = passProbability(from, rt, clear, false, false) * rel;
        int type = ACT_PASS;
        if (clear < cfg_.loftTriggerClear && len >= cfg_.loftMinLen) {
            float lc = 0.f;
            const float lxp = passProbability(from, rt, lc, false, true) * rel;
            if (lxp > xp) { xp = lxp; clear = lc; type = ACT_LOFTED; }
        }
        add(r.id, type, rt, xp, clear, true);
        if (nCand_ > 0) cand_[nCand_ - 1].offside = offsideAmount(rt);

        const float rx = s * r.future.x, cx = s * from.x;
        // through only when runner is clearly ahead and moving forward — fewer over-led misses
        if (rx > cx + 6.0f && s * r.vel.x > 0.5f) {
            MetricVector2D t = r.future + fwd * cfg_.throughLead + r.vel * 0.2f;
            float diag = 0.f;
            if (cfg_.playmaker) {
                const float sp = r.vel.length();
                MetricVector2D dir = fwd;
                if (sp >= cfg_.leadMinSpeed) {
                    const MetricVector2D ru = r.vel * (1.f / sp);
                    const float cosA = clampf(ru.dot(fwd), -1.f, 1.f);
                    const float deg = std::acos(cosA) * 57.2957795f;
                    if (cosA >= 0.2f) dir = ru;
                    if (deg >= cfg_.diagMinDeg && deg <= cfg_.diagMaxDeg) diag = 1.f;
                }
                t = r.future + runnerLead(r, distance(from, r.future)) + dir * cfg_.throughLead;
            }
            t.x = clampf(t.x, -PITCH_HALF_X, PITCH_HALF_X);
            t.y = clampf(t.y, -PITCH_HALF_Y, PITCH_HALF_Y);
            const float tl = distance(from, t);
            if (tl >= cfg_.minPassLen && tl <= cfg_.maxPassLen) {
                float tc = 0.f;
                const float txp = passProbability(from, t, tc, true, false) * rel;
                add(r.id, ACT_THROUGH, t, txp, tc, true);
                if (nCand_ > 0) {
                    cand_[nCand_ - 1].offside = offsideAmount(cfg_.playmaker ? t : r.future);
                    cand_[nCand_ - 1].diag = diag;
                }
            }
        }
    }

    if (cfg_.keepBall && cfg_.virtualKeeper && !cfg_.gkPassBan && state_.crowded && s * from.x <= -18.f) {
        bool haveKeeper = false;
        for (int j = 0; j < TEAM_SIZE && !haveKeeper; j++) {
            const PlayerState& r = st.team[j];
            haveKeeper = r.active && j != st.carrier && s * r.pos.x <= -PITCH_HALF_X + 12.f;
        }
        if (!haveKeeper) {
            const MetricVector2D kp(-s * (PITCH_HALF_X - 5.f), clampf(from.y * 0.3f, -6.f, 6.f));
            float clear = 0.f;
            const float xp = passProbability(from, kp, clear, false, false);
            if (xp > 0.f) add(98, ACT_PASS, kp, xp, clear, true);
        }
    }

    {
        MetricVector2D goal(s * PITCH_HALF_X, 0.f);
        MetricVector2D v(goal.x - from.x, (goal.y - from.y) * 0.6f);
        float l = v.length();
        if (l > 1e-4f) v = v * (1.f / l); else v = fwd;
        const bool fieldOn = cfg_.plus && cfg_.plusField;
        if (fieldOn) v = plusFieldDir(from);
        if (cfg_.plus && cfg_.meta && cfg_.metaMicro) {
            const float ma = metaMicroAngle(lanePressure(from)), mc = std::cos(ma), ms = std::sin(ma);
            v = MetricVector2D(v.x * mc - v.y * ms, v.x * ms + v.y * mc);
        }
        for (int i = 0; i < TEAM_SIZE && !fieldOn; i++) {
            const PlayerState& o = st.opp[i];
            if (!o.active) continue;
            const float d = distance(from, o.future);
            if (d < 10.f && d > 1e-3f) {
                const float k = (10.f - d) / 10.f;
                v = v + (from - o.future) * (k * 1.5f / d);
            }
        }
        if (cfg_.rescueRules && cfg_.blindSide && !human_) {   // first touch away from the pressure centroid (weights 1 / d^2)
            MetricVector2D cs;
            float wsum = 0.f;
            for (int i = 0; i < TEAM_SIZE; i++) {
                const PlayerState& o = st.opp[i];
                if (!o.active) continue;
                const float d = distance(from, o.future);
                if (d > 8.f || d < 1e-3f) continue;
                const float w = 1.f / (d * d);
                cs = cs + o.future * w;
                wsum += w;
            }
            if (wsum > 0.f) {
                MetricVector2D away = from - cs * (1.f / wsum);
                const float al = away.length();
                if (al > 1e-3f) { away = away * (1.f / al); v = v * 0.5f + away * 0.5f; }
                l = v.length();
                if (l > 1e-4f) v = v * (1.f / l);
            }
        }
        if (s * v.x < 0.f && s * from.x < -20.f) v.x = 0.f;   // deep in our own half: never dribble toward our own goal
        l = v.length();
        if (l > 1e-4f) v = v * (1.f / l); else v = fwd;
        if (cfg_.rescueRules && cfg_.forwardOnly && !human_ && s * v.x < 0.2f) {   // no forward pass: drive forward, never recycle backward
            v.x = s * 0.2f;
            l = v.length();
            if (l > 1e-4f) v = v * (1.f / l);
        }
        MetricVector2D to = from + v * cfg_.carryDist;
        to.x = clampf(to.x, -PITCH_HALF_X, PITCH_HALF_X);
        to.y = clampf(to.y, -PITCH_HALF_Y, PITCH_HALF_Y);
        float clear = 0.f;
        float xp = passProbability(from, to, clear, false, false);
        if (xp <= 0.f) {
            clear = nearestOppDistance(from);
            xp = logistic(cfg_.clearSlope * (clear - cfg_.clearHalf));
        }
        add(0, ACT_CARRY, to, xp, clear, false);
    }

    {
        const float ax = s * from.x;
        const float dg = std::sqrt((PITCH_HALF_X - ax) * (PITCH_HALF_X - ax) + from.y * from.y);
        if (dg <= cfg_.shootRange) {
            const MetricVector2D goal(s * PITCH_HALF_X, 0.f);
            float lane = 1e3f;
            for (int i = 0; i < TEAM_SIZE; i++) {
                const PlayerState& o = st.opp[i];
                if (!o.active) continue;
                lane = std::min(lane, segmentDistance(from, goal, o.future));
            }
            const float xp = logistic(cfg_.clearSlope * (std::min(lane, 50.f) - cfg_.clearHalf));
            if (nCand_ < MAX_CAND) {
                PassCandidate& c = cand_[nCand_++];
                c.targetId = 0;
                c.type = ACT_SHOOT;
                c.target = goal;
                c.length = dg;
                c.clearance = std::min(lane, 50.f);
                c.xP = xp;
                c.deltaXT = xTGrid::shotValue(ax, from.y);
                c.pressure = lanePressure(from);
                c.offside = 0.f;
            }
        }
    }

    {   // validation: drop every pass above the safe short-passing distance or aimed at the keeper
        int w = 0;
        for (int i = 0; i < nCand_; i++) {
            const PassCandidate& c = cand_[i];
            const bool pl = c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED;
            if (pl && (c.length > cfg_.safePassLen || (cfg_.gkPassBan && c.targetId >= 1 && c.targetId <= TEAM_SIZE &&
                                                     isKeeperIdx(c.targetId - 1))))
                continue;
            if (w != i) cand_[w] = cand_[i];
            w++;
        }
        nCand_ = w;
    }
    filterForwardOnly(car);
    addWingSwitch(car);
    addLoftedThrough(car);
    if (cfg_.plus) plusPrepare(car);
    const float urg = v7TempoLockedNow() ? 0.f : urgencyLevel();
    const float stratFwd = effStrat().fwd;
    for (int i = 0; i < nCand_; i++) {
        PassCandidate& c = cand_[i];
        float pr = c.pressure;
        if (c.type == ACT_CARRY || c.type == ACT_SHOOT) pr = lanePressure(from);
        c.pressure = pr;
        const float dx = c.deltaXT > 0.f ? c.deltaXT * stratFwd : c.deltaXT;
        c.utility = safe(c.xP * dx - cfg_.wPress * pr - cfg_.wOffside * c.offside, -1e3f);
        if ((c.type == ACT_PASS || c.type == ACT_THROUGH) && c.xP >= 0.45f) {
            // create chances: reward the pass that leaves a team-mate in a clearly better shooting position
            const float qT = xTGrid::shotValue(s * c.target.x, c.target.y);
            const float qC = xTGrid::shotValue(s * from.x, from.y);
            if (qT > qC + 0.025f) c.utility += cfg_.wChance * (qT - qC) * c.xP;
        }
        if (urg > 0.f) {   // attacking urgency: shoot, or at least go forward, instead of sterile retention
            if (c.type == ACT_SHOOT) c.utility += cfg_.urgency * urg * std::max(c.xP, 0.12f) * 14.0f;  // finish
            else if (c.deltaXT <= 0.01f) c.utility -= cfg_.urgency * urg * 1.80f;  // no sterile recycle
        }
        // in the final third a playable shot outranks a sideways pass
        if (c.type == ACT_SHOOT) {
            const float axS = s * from.x;
            const float dGoal = std::sqrt((PITCH_HALF_X - axS) * (PITCH_HALF_X - axS) + from.y * from.y);
            if (dGoal <= cfg_.boxShotRange && c.xP >= cfg_.boxShotXp * 0.7f)
                c.utility += 0.55f;
            else if (dGoal <= 22.f && c.xP >= 0.35f && c.deltaXT >= 0.04f)
                c.utility += 0.28f;
        }
        // goals pack: conversion, box occupation, flank cross, chip GK
        c.utility += conversionShotBonus(car, c);
        c.utility += boxOccupationBonus(car, c);
        c.utility += flankCrossBonus(car, c);
        c.utility += chipGkBonus(car, c);
        c.utility += boxEntryBonus(car, c);
        c.utility += counterBreakBonus(car, c);
        c.utility -= receiverContestPenalty(c);
        c.utility -= turnoverCostPenalty(car, c);
        // Scoring Machine (net contribution capped so stacked bonuses cannot swamp safety terms)
        const float smBefore = c.utility;
        c.utility -= clinicalShotPenalty(car, c);
        c.utility += overloadBonus(car, c);
        c.utility += supportSeekCarryBonus(car, c);
        c.utility += postLossAdjust(car, c);
        if (cfg_.scoringMachine && cfg_.highTempoTrail) {
            const float behind = (plus_.goalsAgainst > plus_.goalsFor) ? 1.f : 0.f;
            const float level  = (plus_.goalsAgainst == plus_.goalsFor) ? 0.55f : 0.f;  // still hunt when level
            const float late = (plus_.phase >= cfg_.lateGameFrac) ? 1.f : 0.f;
            const float hung = std::max(behind, std::max(level, late * 0.85f));
            if (hung > 0.f && (c.type == ACT_SHOOT || c.deltaXT > 0.02f))
                c.utility += cfg_.trailUrgency * hung * (c.type == ACT_SHOOT ? 1.45f : 0.85f);
        }
        if (cfg_.scoringMachine) c.utility = smBefore + clampf(c.utility - smBefore, -cfg_.smNetCap, cfg_.smNetCap);
        if (cfg_.legendary && (c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED))
            c.utility -= 0.12f * std::max(0.f, 0.70f - c.xP);
        const bool isPass = c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED;
        if (isPass) {
            float minXp = cfg_.minPassXp + (cfg_.legendary ? 0.08f : 0.f) + (cfg_.upgrades && cfg_.online ? onlineAdj_ : 0.f);
            // when trailing / late, slightly relax the safety floor for progressive passes
            if (cfg_.scoringMachine && cfg_.highTempoTrail && c.deltaXT > 0.04f) {
                const bool behind = plus_.goalsAgainst > plus_.goalsFor;
                const bool late = plus_.phase >= cfg_.lateGameFrac;
                if (behind || late) minXp = std::max(0.45f, minXp - cfg_.trailXpRelax);
            }
            if (c.xP < minXp)
                c.utility -= 0.08f + 0.25f * (minXp - c.xP) / std::max(minXp, 0.1f);   // a pass that is likely to be cut out
            if (cfg_.rescueRules && cfg_.triangle && (c.type == ACT_PASS || c.type == ACT_THROUGH) && c.clearance >= cfg_.triLane) {
                const MetricVector2D v1 = c.target - from;
                const float l1 = v1.length();
                for (int k = 0; k < TEAM_SIZE && l1 > 1e-3f; k++) {
                    const PlayerState& sp = st.team[k];
                    if (!sp.active || k == st.carrier || sp.id == c.targetId || s * (sp.future.x - from.x) < 0.f) continue;
                    const MetricVector2D v2 = sp.future - from;
                    const float l2 = v2.length();
                    if (l2 < 3.f || l2 > 25.f) continue;
                    const float deg = std::acos(clampf(v1.dot(v2) / (l1 * l2), -1.f, 1.f)) * 57.2957795f;
                    if (deg >= cfg_.triMinDeg && deg <= cfg_.triMaxDeg) { c.utility += cfg_.wTriangle * c.xP; break; }
                }
            }
            if (cfg_.longPassPenalty && c.length > cfg_.longPassFrom)
                c.utility -= cfg_.wLongPass * std::min(1.f, (c.length - cfg_.longPassFrom) / 12.f) * (1.f - c.xP);
            // short progressive ground passes win the tempo battle
            if (c.type == ACT_PASS && c.length <= 14.f && c.deltaXT > 0.02f && c.xP >= 0.70f)
                c.utility += 0.12f * c.xP;
            if (cfg_.cutBack && (c.type == ACT_PASS || c.type == ACT_THROUGH) && c.offside < 0.5f && c.xP >= 0.55f) {
                // the carrier's own shot lane against the receiver's: square or cut the ball back to the better-placed shooter
                const float axC = s * from.x;
                const float axT = s * c.target.x;
                if (axC >= PITCH_HALF_X - cfg_.cutBackRange && axT >= PITCH_HALF_X - 17.f && std::fabs(c.target.y) <= cfg_.cutBackBoxHalfY) {
                    const MetricVector2D goal(s * PITCH_HALF_X, 0.f);
                    float laneC = 50.f, laneT = 50.f, free = 50.f;
                    int gk = -1;
                    float gkAx = -1e9f;
                    for (int k = 0; k < TEAM_SIZE; k++)   // their keeper: the deepest opponent while he is within 14 m of the goal line
                        if (st.opp[k].active && s * st.opp[k].future.x > gkAx) { gkAx = s * st.opp[k].future.x; gk = k; }
                    if (gk >= 0 && PITCH_HALF_X - gkAx > 14.f) gk = -1;
                    for (int k = 0; k < TEAM_SIZE; k++) {
                        const PlayerState& o = st.opp[k];
                        if (!o.active || k == gk) continue;
                        laneC = std::min(laneC, segmentDistance(from, goal, o.future));
                        laneT = std::min(laneT, segmentDistance(c.target, goal, o.future));
                        free = std::min(free, distance(c.target, o.future));
                    }
                    if (free >= cfg_.cutBackMinFree && laneT > laneC + 0.8f) {
                        const float gain = std::min(1.f, (laneT - laneC) / 3.5f);
                        c.utility += cfg_.wCutBack * gain * c.xP;   // free man in the box beats a low-angle carrier shot
                    }
                }
            }
            if (c.type == ACT_PASS && c.xP >= 0.70f && c.length >= 4.f && c.length <= 22.f)
                c.utility += cfg_.wTiki * c.xP * (1.f - c.length / 26.f);                // short, safe, retain the ball
            if (cfg_.upgrades && c.offside < 0.5f) c.utility += upgradeBonus(car, c);   // lookahead, back-line height, receiver role
            // Adaptive Attack Intelligence v1: attack-only, bounded candidate scoring.
            // It never changes defensive decisions or bypasses the existing safety filters.
            if (cfg_.upgrades && c.offside < 0.5f) c.utility += adaptiveAttackBonus(car, c);
            // coordinate progression with receiver confidence and transition cover.
            if (cfg_.upgrades && c.offside < 0.5f) c.utility += coordinatedRiskAdjustment(car, c);
        }
    }
    {
        int safePasses = 0;
        for (int i = 0; i < nCand_; i++) {
            const PassCandidate& c = cand_[i];
            if ((c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED) && c.clearance >= cfg_.clearHalf && c.offside < 0.5f) safePasses++;
        }
        for (int i = 0; i < nCand_; i++) {
            PassCandidate& c = cand_[i];
            const bool isPass = c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED;
            // a defender can get to the lane before the ball: only played when there is no safe pass at all
            if (isPass && c.clearance < cfg_.clearHalf && safePasses > 0) c.utility -= 5.0f;
        }
        if (cfg_.safeFirst) {   // possession first: a clean pass exists, so a doubtful pass or a dribble into a tackle loses out
            bool clean = false;
            for (int i = 0; i < nCand_; i++) {
                const PassCandidate& c = cand_[i];
                if ((c.type == ACT_PASS || c.type == ACT_THROUGH) && c.xP >= cfg_.safeFirstXp && c.clearance >= cfg_.safeFirstMargin && c.offside < 0.5f) { clean = true; break; }
            }
            if (clean) {
                const float press = nearestOppDistance(car.future);
                for (int i = 0; i < nCand_; i++) {
                    PassCandidate& c = cand_[i];
                    const bool isPass = c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED;
                    if (isPass && c.xP < 0.60f) c.utility -= cfg_.wUnsafeDemote * (0.60f - c.xP) / 0.60f;
                    else if (c.type == ACT_CARRY && press < 4.0f) c.utility -= 0.25f * (1.f - press / 4.0f);
                }
            }
        }
    }
    if (cfg_.plus) plusApply(car);
    if (cfg_.plus && cfg_.meta) metaApply(car);
    if (cfg_.plan) planApply(car);
    if (cfg_.v7) v7Apply(car);
    if (cfg_.v8) v8Apply(car);
    if (cfg_.keepBall) applyKeepBall(car);
    if (cfg_.playmaker) applyPlaymaker(car);
    if (cfg_.context) applyContext(car);
    float T = (cfg_.legendary ? st.temperature * 0.55f : st.temperature) * cfg_.tempScale;
    if (cfg_.keepBall && state_.crowded) T *= clampf(cfg_.crowdTempScale, 0.1f, 1.f);
    if (cfg_.styles) T *= effStrat().tempMul;
    if (cfg_.v7) T *= v7TempScale();
    if (cfg_.antiAdapt && opp_.noise > 0.001f && !state_.crowded && !state_.oneTouch)
        T *= 1.f + cfg_.noiseTemp * opp_.noise;
    softmaxAll(T);
    return nCand_;
}

void PitchEngine::setAdaptive(bool ballCoast, bool occHold, bool context, bool styles, bool antiAdapt) {
    cfg_.ballCoast = ballCoast;
    cfg_.occlusionHold = occHold;
    cfg_.context = context;
    cfg_.styles = styles;
    cfg_.antiAdapt = antiAdapt;
}

bool PitchEngine::overlapNear(const PlayerState* self, const MetricVector2D& p) const {
    const float R = std::max(cfg_.occRadius, 0.5f);
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState* o[2] = {&state_.team[i], &state_.opp[i]};
        for (int k = 0; k < 2; k++) {
            if (o[k] == self || !o[k]->active || o[k]->missed > 1) continue;
            if (distance(o[k]->pos, p) < R) return true;
        }
    }
    return false;
}

void PitchEngine::coastBall(double dtSec) {
    const float d = (float)dtSec;
    const int c = state_.carrier;
    const bool carrying = owner_ == 1 && c >= 0 && c < TEAM_SIZE && state_.team[c].active && state_.team[c].visible &&
                          distance(state_.team[c].pos, state_.ball) <= cfg_.ownRadius;
    if (carrying) {
        const PlayerState& p = state_.team[c];
        state_.ball = state_.ball + p.vel * d + p.acc * (0.5f * d * d);
        state_.ballVel = p.vel;
    } else {
        state_.ball = state_.ball + state_.ballVel * d;
        state_.ballVel = state_.ballVel * std::exp(-std::max(cfg_.ballCoastDecay, 0.f) * d);
    }
    state_.ball.x = clampf(state_.ball.x, -PITCH_HALF_X, PITCH_HALF_X);
    state_.ball.y = clampf(state_.ball.y, -PITCH_HALF_Y, PITCH_HALF_Y);
}

void PitchEngine::updateOppShape() {
    MetricVector2D c;
    int n = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || o.missed > cfg_.maxCoastFrames) continue;
        c = c + o.pos;
        n++;
    }
    if (n < 4) return;
    c = c * (1.f / (float)n);
    float sum = 0.f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || o.missed > cfg_.maxCoastFrames) continue;
        sum += distance(o.pos, c);
    }
    const float a = 0.15f;
    state_.oppSpread += (sum / (float)n - state_.oppSpread) * a;
    const float depth = clampf(((float)state_.attackSign * c.x + PITCH_HALF_X) / (2.f * PITCH_HALF_X), 0.f, 1.f);
    state_.oppDepth += (depth - state_.oppDepth) * a;
}

PitchEngine::Strategy PitchEngine::effStrat() const {
    Strategy s = strat_;
    if (!cfg_.styles) return s;
    static const float P[N_STYLES][4] = {
        {0.12f, -0.04f, -0.12f, -0.10f},
        {-0.08f, 0.03f, 0.15f, 0.00f},
        {-0.10f, 0.05f, 0.25f, -0.20f},
        {0.00f, 0.00f, 0.00f, 0.50f},
    };
    const float g = clampf(cfg_.styleGain, 0.f, 1.f);
    for (int i = 0; i < N_STYLES; i++) {
        const float w = g * opp_.w[i];
        s.dKeep += w * P[i][0];
        s.dTempo += w * P[i][1];
        s.fwd += w * P[i][2];
        s.tempMul += w * P[i][3];
    }
    s.dKeep = clampf(s.dKeep, -0.4f, 0.3f);
    s.dTempo = clampf(s.dTempo, -0.1f, 0.1f);
    s.fwd = clampf(s.fwd, 0.75f, 1.35f);
    s.tempMul = clampf(s.tempMul, 0.7f, 1.5f);
    return s;
}

// In-match learning, deliberately slow: a fast-moving completion rate, a confidence that grows with the number of passes
// seen, a gain below 1, a smoothed output and hard limits, so one bad sequence cannot swing the threshold.
void PitchEngine::onlineLearn(bool kept) {
    if (!cfg_.upgrades || !cfg_.online) return;
    const float x = kept ? 1.f : 0.f;
    onlineKept_ += (x - onlineKept_) * clampf(cfg_.onlineAlpha, 0.01f, 0.5f);
    onlineN_++;
    const float conf = clampf((float)onlineN_ / 12.f, 0.f, 1.f);
    const float tgt = clampf((cfg_.onlineTarget - onlineKept_) * cfg_.onlineGain * conf, cfg_.onlineMaxDown, cfg_.onlineMaxUp);
    onlineAdj_ += (tgt - onlineAdj_) * 0.3f;
}

void PitchEngine::noteOutcome(bool kept) {
    const float x = kept ? 1.f : 0.f;
    onlineLearn(kept);
    opp_.keptFast += (x - opp_.keptFast) * 0.25f;
    opp_.keptSlow += (x - opp_.keptSlow) * 0.05f;
    opp_.events++;
    opp_.adapting = opp_.events >= cfg_.adaptMinEvents && (opp_.keptSlow - opp_.keptFast) > cfg_.adaptDrop;
}

void PitchEngine::updateStyle(double dtSec) {
    const float dt = (float)dtSec;
    if (cfg_.antiAdapt) {
        const float target = opp_.adapting ? 1.f : 0.f;
        const float sec = target > opp_.noise ? cfg_.noiseRiseSec : cfg_.noiseFallSec;
        const float step = dt / std::max(sec, 0.5f);
        if (opp_.noise < target) opp_.noise = std::min(target, opp_.noise + step);
        else opp_.noise = std::max(target, opp_.noise - step);
    } else {
        opp_.noise = 0.f;
    }
    if (!cfg_.styles) return;
    if (owner_ == 1 && state_.carrier >= 0) {
        const float k = clampf(dt / std::max(cfg_.styleTauSec, 1.f), 0.f, 0.2f);
        opp_.press += ((state_.crowded ? 1.f : 0.f) - opp_.press) * k;
    }
    const float deep = state_.oppDepth;
    const float compact = clampf((cfg_.compactRef - state_.oppSpread) / (cfg_.compactRef * 0.5f), 0.f, 1.f);
    const float sc[N_STYLES] = {
        0.6f * deep + 0.5f * compact + 0.4f * opp_.press,
        0.5f * (1.f - deep) + 0.5f * (1.f - compact),
        0.6f * (1.f - deep) + 0.6f * (1.f - possShare_),
        0.9f * opp_.noise,
    };
    float e[N_STYLES], sum = 0.f, mx = -1e9f;
    for (int i = 0; i < N_STYLES; i++) mx = std::max(mx, sc[i]);
    for (int i = 0; i < N_STYLES; i++) { e[i] = std::exp(3.f * (sc[i] - mx)); sum += e[i]; }
    const float conf = clampf(possOwnedSec_ / std::max(cfg_.styleWarmSec, 1.f), 0.f, 1.f);
    const float a = clampf(dt / std::max(cfg_.styleTauSec, 1.f), 0.f, 0.2f);
    for (int i = 0; i < N_STYLES; i++) {
        const float tgt = conf * e[i] / std::max(sum, 1e-6f);
        opp_.w[i] += (tgt - opp_.w[i]) * a;
    }
}

int PitchEngine::jitteredDecisionMs() {
    const int base = effDecisionMs();
    if (!cfg_.antiAdapt || opp_.noise <= 0.001f || state_.counter) return base;
    const float j = cfg_.noiseJitter * opp_.noise * (2.f * rand01() - 1.f);
    return std::max(40, (int)((float)base * (1.f + j)));
}

void PitchEngine::applyContext(const PlayerState& car) {
    const float s = (float)state_.attackSign;
    const MetricVector2D fwd(s, 0.f);
    const float compact = clampf((cfg_.compactRef - state_.oppSpread) / (cfg_.compactRef * 0.5f), 0.f, 1.f);
    const float R = std::max(cfg_.supportRadius, 0.5f);
    for (int i = 0; i < nCand_; i++) {
        PassCandidate& c = cand_[i];
        if (c.type != ACT_PASS && c.type != ACT_THROUGH && c.type != ACT_LOFTED) continue;
        const int ri = c.targetId - 1;
        if (ri < 0 || ri >= TEAM_SIZE || ri == state_.carrier) continue;
        const PlayerState& r = state_.team[ri];
        float bonus = 0.f;
        const float sp = r.vel.length();
        if (sp >= cfg_.leadMinSpeed) bonus += cfg_.wFacing * clampf(r.vel.dot(fwd) / sp, -1.f, 1.f);
        const MetricVector2D back = c.target - fwd * cfg_.supportDepth;
        float sum = 0.f;
        for (int j = 0; j < TEAM_SIZE; j++) {
            const PlayerState& t = state_.team[j];
            if (!t.active || j == state_.carrier || j == ri || t.missed > cfg_.maxCoastFrames) continue;
            const float d = distance(t.future, back);
            if (d < R) sum += 1.f - d / R;
        }
        bonus += cfg_.wSupport * clampf(sum / 2.f, 0.f, 1.f);
        const float lat = std::fabs(c.target.y - car.pos.y);
        const float gain = s * (c.target.x - car.pos.x);
        if (lat > gain && lat > 8.f) bonus += cfg_.wCompact * compact * clampf(lat / 30.f, 0.f, 1.f);
        else if (c.type == ACT_THROUGH) bonus += cfg_.wCompact * (1.f - compact) * 0.5f;
        c.utility += bonus * clampf(c.xP, 0.f, 1.f);
    }
}

void PitchEngine::softmaxAll(float temperature) {
    if (nCand_ <= 0) return;
    const float T = std::max(safe(temperature, cfg_.tempHold), cfg_.tempMin);
    float logit[MAX_CAND];
    float mx = -1e30f;
    for (int i = 0; i < nCand_; i++) {
        logit[i] = safe(cand_[i].utility * cfg_.utilityScale / T, -1e6f);
        mx = std::max(mx, logit[i]);
    }
    float sum = 0.f;
    float e[MAX_CAND];
    for (int i = 0; i < nCand_; i++) {
        e[i] = std::exp(clampf(logit[i] - mx, -80.f, 0.f));
        sum += e[i];
    }
    if (!(sum > 1e-20f)) {
        for (int i = 0; i < nCand_; i++) cand_[i].prob = 1.f / (float)nCand_;
        return;
    }
    for (int i = 0; i < nCand_; i++) cand_[i].prob = e[i] / sum;
}

uint32_t PitchEngine::nextRand() {
    uint32_t x = rng_ ? rng_ : 1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
}

float PitchEngine::rand01() { return (float)(nextRand() >> 8) * (1.0f / 16777216.0f); }

int PitchEngine::sampleCandidate() {
    if (nCand_ <= 0) return -1;
    const float r = rand01();
    float acc = 0.f;
    int last = 0;
    for (int i = 0; i < nCand_; i++) {
        if (cand_[i].prob > 0.f) last = i;
        acc += cand_[i].prob;
        if (r < acc) return i;
    }
    return last;
}

GestureOutput PitchEngine::makeGesture(int type, const MetricVector2D& from, const MetricVector2D& to, float power,
                                       bool dash) const {
    GestureOutput g;
    g.status = state_.status;
    g.actionType = type;
    float dx = to.x - from.x, dy = to.y - from.y;
    if (cfg_.invertX) dx = -dx;
    if (cfg_.invertY) dy = -dy;
    if (!finite(dx) || !finite(dy) || (dx * dx + dy * dy) < 1e-8f) {
        g.valid = false;
        g.r = 0.f;
        g.theta = 0.f;
        return g;
    }
    float th = std::atan2(dy, dx) * (180.f / PI_F);
    if (th < 0) th += 360.f;
    g.theta = th;
    g.r = 1.0f;
    power = clampf(safe(power, 0.5f), 0.f, 1.f);
    const int span = cfg_.maxPassHoldMs - cfg_.minPassHoldMs;
    int dur = cfg_.minPassHoldMs + (int)std::lround(power * (float)span);
    if (type == ACT_CARRY) dur = cfg_.carryHoldMs;
    else if (type == ACT_LOFTED) dur += cfg_.loftExtraMs;
    else if (type == ACT_SHOOT) dur += 20;
    g.durationMs = (int)clampf((float)dur, 1.f, 4095.f);
    g.dash = dash;
    g.valid = true;
    return g;
}

GestureOutput PitchEngine::generateGestureVector(const PlayerState& target, float passPower) {
    GestureOutput g;
    g.status = state_.status;
    if (state_.carrier < 0 || state_.carrier >= TEAM_SIZE) return g;
    const MetricVector2D from = state_.team[state_.carrier].pos;
    int type = ACT_PASS;
    MetricVector2D to = target.future;
    for (int i = 0; i < nCand_; i++) {
        if (cand_[i].targetId == target.id && cand_[i].type != ACT_CARRY && cand_[i].type != ACT_SHOOT) {
            type = cand_[i].type;
            to = cand_[i].target;
            if (i == chosen_) break;
        }
    }
    return makeGesture(type, from, to, passPower, false);
}

int PitchEngine::effDecisionMs() const {
    if (state_.timeMs < restartUntilMs_) return std::min(cfg_.decisionIntervalMs, 80);
    return planCadenceMs(cfg_.legendary ? std::min(cfg_.decisionIntervalMs, 100) : cfg_.decisionIntervalMs);
}

int PitchEngine::effCooldownMs() const {
    if (state_.timeMs < restartUntilMs_) return std::min(cfg_.actionCooldownMs, 200);
    const int base = std::min(cfg_.actionCooldownMs, cfg_.fastCooldownMs);
    const int v = cfg_.legendary ? std::min(base, 240) : base;
    if (cfg_.plan && cfg_.planCadence && state_.status == ST_TEAM && state_.carrier >= 0 && state_.carrier < TEAM_SIZE &&
        planPressure(state_.team[state_.carrier].pos) > cfg_.planPressHigh)
        return (int)((float)v * 0.75f);
    return v;
}

int PitchEngine::consumeShotCue() {
    const int v = pendingCue_;
    pendingCue_ = 0;
    return v;
}

// Where the keeper is: the opponent standing deepest toward his own goal line (within 12 m of it).
// Returns false when nobody qualifies.
static bool findKeeper(const PitchEngine& e, float s, MetricVector2D* out) {
    float kx = -1e9f;
    bool found = false;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = e.state().opp[i];
        if (!o.active) continue;
        if (s * o.future.x > kx) {
            kx = s * o.future.x; *out = o.future; found = true;
            if (e.config().rescueRules) *out = o.pos + o.vel5 * e.config().gkLeadSec;   // where he will stand after the wind-up
        }
    }
    return found && kx >= PITCH_HALF_X - 12.f;
}

// Aim at the post farther from the keeper, a little inside it (the wider open gap).
float PitchEngine::stunAimY(const MetricVector2D& from) const {
    const float s = (float)state_.attackSign;
    const float gx = s * PITCH_HALF_X;
    const float inner = cfg_.goalHalfWidth - cfg_.stunPostInset;
    MetricVector2D k;
    if (!findKeeper(*this, s, &k)) return from.y > 0.f ? -inner : inner;   // no keeper seen: far corner from the shooter
    const float dLow = std::sqrt((k.x - gx) * (k.x - gx) + (k.y + cfg_.goalHalfWidth) * (k.y + cfg_.goalHalfWidth));
    const float dHigh = std::sqrt((k.x - gx) * (k.x - gx) + (k.y - cfg_.goalHalfWidth) * (k.y - cfg_.goalHalfWidth));
    return planKeeperAim(dLow > dHigh ? -inner : inner);
}

// Non-linear power gauge: a linear touch time overcharges long shots over the bar.
float PitchEngine::stunPowerRatio(float d) const {
    const float t = std::max(0.f, (d - 18.f) / 12.f);
    return std::min(0.85f, 0.40f + 0.35f * std::sqrt(t));
}

// Range 20-30 m, nobody within 2.5 m for the wind-up, and (extra) no outfield defender sitting on the shot line.
// Heading alignment (45 degrees) is enforced when the shot is fired, with a short turn first.
bool PitchEngine::stunConditions(const MetricVector2D& from, float* aimYOut) const {
    const float s = (float)state_.attackSign;
    const float dx = PITCH_HALF_X - s * from.x;
    const float dg = std::sqrt(dx * dx + from.y * from.y);
    if (!(dx > 1.f) || !stunBand(dg)) return false;
    if (nearestOppDistance(from) < cfg_.stunSafe) return false;
    const float ay = stunAimY(from);
    if (cfg_.stunLaneMin > 0.f) {
        const MetricVector2D t(s * PITCH_HALF_X, ay);
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (!o.active) continue;
            if (s * o.future.x >= PITCH_HALF_X - 3.5f) continue;   // the keeper is not a blocker we can avoid
            if (segmentDistance(from, t, o.future) < cfg_.stunLaneMin) return false;
        }
    }
    if (aimYOut) *aimYOut = ay;
    return true;
}

int PitchEngine::shotWindowLevel(const MetricVector2D& from, float* clearXp, float* xgOut, float* aimYOut) const {
    const float s = (float)state_.attackSign;
    const float ax = s * from.x;
    const float dx = PITCH_HALF_X - ax;
    const float dy = from.y;
    if (!finite(dx) || !finite(dy) || !(dx > 1.f)) return 0;
    const float dg = std::sqrt(dx * dx + dy * dy);
    if (dg > cfg_.shootRange * cfg_.cueRangeFactor) return 0;
    if (cfg_.rescueRules && dg > cfg_.shotMaxDist) return 0;   // no shots from long range
    if (cfg_.rescueRules && shotFiredMs_ > 0 && state_.timeMs - shotFiredMs_ < (double)cfg_.shotSpacingMs) return 0;

    const float AIM[2] = {-cfg_.aimInset, cfg_.aimInset};
    float lanes[2];
    for (int k = 0; k < 2; k++) {
        const MetricVector2D goal(s * PITCH_HALF_X, AIM[k]);
        float lane = 50.f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (!o.active) continue;
            if (s * o.future.x >= PITCH_HALF_X - 3.5f) continue;
            lane = std::min(lane, segmentDistance(from, goal, o.future));
        }
        lanes[k] = lane;
    }
    int farK = from.y > 0.f ? 0 : 1;
    {   // aim away from the keeper (the opponent nearest his own goal line), inside the frame
        float kx = -1e9f, ky = 0.f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (!o.active) continue;
            if (s * o.future.x > kx) { kx = s * o.future.x; ky = cfg_.rescueRules ? o.pos.y + o.vel5.y * cfg_.gkLeadSec : o.future.y; }
        }
        if (kx >= PITCH_HALF_X - 9.f && std::fabs(ky) > 0.4f) farK = ky > 0.f ? 0 : 1;
    }
    int pick = farK;
    if (lanes[1 - farK] > lanes[farK] + 0.75f) pick = 1 - farK;
    const float bestLane = lanes[pick];
    const float bestAim = AIM[pick];
    const float xp = logistic(cfg_.clearSlope * (std::min(bestLane, 50.f) - cfg_.clearHalf));
    const float xg = xTGrid::shotValue(ax, from.y);
    if (clearXp) *clearXp = xp;
    if (xgOut) *xgOut = xg;
    if (aimYOut) *aimYOut = bestAim;
    int lvl = 0;
    const bool laneOpen = bestLane >= cfg_.convMinLane;   // a man standing on the shot line is a block, not a lane
    if (laneOpen && xp >= cfg_.cueLaneXp && xg >= cfg_.cueMinXg) lvl = 1;
    if (laneOpen && xp >= cfg_.cuePrimeXp && xg >= cfg_.cuePrimeXg + shotXgOff_) lvl = 2;
    // inside the box any real lane is a shot — take it (possession without goals loses)
    if (dg < cfg_.stunMin && dg <= cfg_.boxShotRange && std::fabs(dy) <= 22.f && xp >= cfg_.boxShotXp && bestLane >= cfg_.convMinLane) lvl = 2;
    // conversion-first: open / 1v1 / free-ish central look forces the finish
    if (cfg_.conversionFirst && dg <= cfg_.boxShotRange && std::fabs(dy) <= 18.f) {
        const float no = nearestOppDistance(from);
        if (no >= cfg_.openGoalOpp && xp >= cfg_.boxShotXp * 0.5f && bestLane >= cfg_.convMinLane) lvl = 2;
        if (dg <= 14.f && std::fabs(dy) <= 10.f && xp >= 0.08f && bestLane >= cfg_.convMinLane) lvl = 2;  // central box
    }
    // outside the box only arm a stun when lane + space + non-junk xG
    if (stunBand(dg)) {
        float ay = 0.f;
        if (stunConditions(from, &ay) && xp >= cfg_.longShotXp && xg >= cfg_.longShotMinXg) {
            lvl = 2;
            if (aimYOut) *aimYOut = ay;
        } else if (lvl >= 2) {
            lvl = 1;
        }
    }
    return lvl;
}

void PitchEngine::updateShotCue(const PlayerState& car) {
    const double now = state_.timeMs;
    holding_ = false;
    if (!cfg_.shotAlert && !cfg_.shotHold && !cfg_.autoShoot) {
        cueLevelNow_ = 0;
        cueRun_ = 0;
        shotRun_ = 0;
        return;
    }
    float aim = 0.f;
    const int lvl = shotWindowLevel(car.future, nullptr, nullptr, &aim);
    cueLevelNow_ = lvl;
    const bool rr = cfg_.rescueRules && cfg_.shotEveryFrame;
    if (rr && !shotArmed_ && now - shotFiredMs_ >= (double)cfg_.shotRearmMs) shotArmed_ = true;   // a missed or blocked look must not end shooting for the match
    if (lvl > 0) {
        cueAimY_ = aim;
        cueRun_++;
    } else {
        cueRun_ = 0;
    }
    if (lvl > 0 && cueRun_ >= cfg_.cueConfirmFrames && cueArmed_ && now - lastCueMs_ >= (double)cfg_.cueMinGapMs) {
        cueArmed_ = false;
        lastCueMs_ = now;
        if (cfg_.shotAlert) pendingCue_ = std::max(pendingCue_, lvl);
        if (cfg_.shotHold) cueHoldUntil_ = now + (double)cfg_.cueHoldMs;
    }
    holding_ = cfg_.shotHold && !cfg_.autoShoot && lvl > 0 && now < cueHoldUntil_;   // with auto-shoot on, never stand and wait
    if (cfg_.autoShoot) {
        shotRun_ = lvl >= 2 ? shotRun_ + 1 : 0;
        const float axs = (float)state_.attackSign * car.future.x;
        const bool inBox = (PITCH_HALF_X - axs) <= cfg_.boxShotRange && std::fabs(car.future.y) <= 22.f;
        const float dgs = std::sqrt((PITCH_HALF_X - axs) * (PITCH_HALF_X - axs) + car.future.y * car.future.y);
        const bool oneFrame = inBox || (rr && stunBand(dgs));      // the exact frame the conditions turn true
        if (lvl >= 2 && shotRun_ >= (oneFrame ? 1 : cfg_.cueConfirmFrames) && shotArmed_ && !human_) {
            shotArmed_ = false;
            shotPending_ = true;
            shotPendingUntil_ = now + (double)cfg_.cueHoldMs;
            shotAimY_ = aim;
        }
    }
    if (cfg_.rescueRules && cfg_.shotLog) {   // why is the bot in range and not shooting?
        const float axl = (float)state_.attackSign * car.future.x;
        const float dxl = PITCH_HALF_X - axl;
        const float dgl = std::sqrt(dxl * dxl + car.future.y * car.future.y);
        const bool inRange = axl > 0.f && dgl <= cfg_.shootRange;
        if (inRange) {
            const bool firing = shotPending_ && shotArmed_ == false && now <= shotPendingUntil_;
            if (!firing) logShotBlock(shotBlockReason(car, lvl), car);
        }
        wasInShotBand_ = inRange;
    }
}

GestureOutput PitchEngine::decide() {
    GestureOutput none;
    none.status = ST_TEAM;
    const int n = calculatePassProbabilities(state_);
    if (n <= 0) return none;
    const double now = state_.timeMs;
    const PlayerState& car = state_.team[state_.carrier];

    updateShotCue(car);

    if (shotPending_ && now > shotPendingUntil_) {
        shotPending_ = false;
        if (cfg_.rescueRules && cfg_.shotEveryFrame) shotArmed_ = true;
    }
    // ball still in flight to this player: wait for the trap window (pre-trap lead), then go straight through the pass
    // cooldown and without a turning tick (true one-touch distribution)
    bool inboundOneTouch = false;
    if (cfg_.rescueRules && cfg_.anticipate && !antBusy_ && ballUsable()) {
        const float bv = state_.ballVel.length();
        const MetricVector2D dd = car.pos - state_.ball;
        const float ddl = dd.length();
        if (bv >= cfg_.anticipateMinSpeed && ddl > 0.25f && ddl <= 14.f && state_.ballVel.dot(dd) / (bv * ddl) >= 0.88f) {
            const float eta = std::max(0.f, ddl - 0.8f) / bv * 1000.f;
            const float window = (float)(cfg_.oneTouchPreMs + cfg_.oneTouchSlackMs);
            if (eta > window && eta < 600.f) return none;
            if (eta <= window) {
                inboundOneTouch = true;
                if (now - lastActMs_ >= 150.0) cooldownUntil_ = std::min(cooldownUntil_, now);
            }
        }
    }
    // the ball has already left this carrier at speed (the radar still shows him as the owner): no new action
    if (cfg_.v7 && !human_ && ballUsable()) {
        const MetricVector2D away = state_.ball - car.pos;
        const float bvl = state_.ballVel.length(), al = away.length();
        if (bvl > 8.f && al > 1.8f && state_.ballVel.dot(away) / (bvl * al) > 0.7f) return none;
    }
    // a pass cooldown never delays a shot or an emergency clearance
    const bool coolActive = now < cooldownUntil_;
    const bool panicWanted = panicNeeded(car);
    const bool quickWindow = cfg_.rescueRules && cfg_.shotEveryFrame && now >= shotFiredMs_ + 700.0;
    if (coolActive && !(quickWindow && ((shotPending_ && !human_) || panicWanted))) return none;

    if (shotPending_ && !human_) {
        float aimY = shotAimY_;
        bool stun = false;
        {
            const float axq = (float)state_.attackSign * car.future.x;
            const float dxq = PITCH_HALF_X - axq;
            const float dgq = std::sqrt(dxq * dxq + car.future.y * car.future.y);
            if (stunBand(dgq)) {
                float ay = 0.f;
                if (stunConditions(car.future, &ay)) { stun = true; aimY = ay; }   // re-checked at the moment of the shot
                else {
                    shotPending_ = false;                                           // a defender closed in: no wind-up
                    if (cfg_.rescueRules && cfg_.shotEveryFrame) shotArmed_ = true;   // look again next frame
                }
            }
        }
    if (shotPending_) {
        const MetricVector2D aimPt((float)state_.attackSign * PITCH_HALF_X, aimY);
        if (cfg_.rescueRules && cfg_.rollTouch && car.vel5.length() >= cfg_.rollMinSpeed && nearestOppDistance(car.pos) >= cfg_.rollMinOpp &&
            now - rollAtMs_ > 1500.0) {
            rollAtMs_ = now;           // sprinting: push the ball ~1 m ahead with a 0.1 s nudge, then shoot
            GestureOutput rg = makeGesture(ACT_CARRY, car.pos, aimPt, 0.f, false);
            if (rg.valid) { rg.durationMs = cfg_.rollMs; return rg; }
        }
        bool openLook = false;
        if (cfg_.rescueRules && cfg_.shotEveryFrame && nearestOppDistance(car.pos) >= cfg_.shotSkipTurnOpp) {
            // nobody within shotSkipTurnOpp: fire off the current stride unless he is running away from the goal
            const MetricVector2D dv = aimPt - car.pos;
            const float dvl = dv.length();
            const float sp = car.vel5.length();
            openLook = sp < cfg_.faceMinSpeed || dvl < 1e-3f || car.vel5.dot(dv) / (sp * dvl) > -0.17f;
        }
        if (cfg_.faceBeforePass && !openLook && nearestOppDistance(car.pos) >= (stun ? 0.f : cfg_.turnSkipOppDist)) {
            // body orientation: a shot only fires when the shooter faces the aim point (accuracy), otherwise turn first
            const MetricVector2D d = aimPt - car.pos;
            const float dl = d.length();
            const float spd = car.vel.length();
            bool facing = true;
            if (dl > 1e-3f) {
                if (spd >= cfg_.faceMinSpeed) {
                    const float cosT = clampf(car.vel.dot(d) / (spd * dl), -1.f, 1.f);
                    facing = stun ? (cosT >= cfg_.stunCos)
                                  : std::acos(cosT) * 57.2957795f <= cfg_.shotFaceTolDeg;
                }
                else
                    facing = turnStartMs_ >= 0 && turnTargetId_ == -2;
            }
            if (!facing) {
                if (turnStartMs_ < 0 || turnTargetId_ != -2) { turnStartMs_ = now; turnTargetId_ = -2; }
                if (now - turnStartMs_ < (double)cfg_.turnMaxMs) {
                    GestureOutput tg = makeGesture(ACT_CARRY, car.pos, aimPt, 0.f, false);
                    if (tg.valid) { tg.durationMs = cfg_.turnHoldMs; return tg; }
                }
            }
        }
        turnStartMs_ = -1;
        turnTargetId_ = -1;
        shotPending_ = false;
        GestureOutput sg = makeGesture(ACT_SHOOT, car.pos, aimPt, 1.f, false);
        if (sg.valid) {
            sg.actionType = ACT_SHOT;
            {
                const float axs = (float)state_.attackSign * car.future.x;
                const bool inBox = (PITCH_HALF_X - axs) <= cfg_.boxShotRange;
                const float dgoal = std::sqrt((PITCH_HALF_X - axs) * (PITCH_HALF_X - axs) + car.future.y * car.future.y);
                const float longHold = std::min((float)cfg_.shootHoldMs, cfg_.longShotHoldBase + cfg_.longShotHoldPerM * dgoal);
                sg.durationMs = (int)clampf(inBox ? (float)cfg_.boxShotHoldMs : longHold, cfg_.rescueRules ? 100.f : 200.f, 1500.f);
                if (cfg_.firstTimeFinish && inBox && !stun) {
                    const float spd = car.vel5.length();
                    const float no = nearestOppDistance(car.future);
                    if (spd >= 3.5f || no >= cfg_.openGoalOpp * 0.85f)
                        sg.durationMs = (int)clampf(cfg_.firstTimeHoldMs, 80.f, 180.f);
                }
                if (stun) {
                    // tau = P * tau_max with the square-root power curve
                    sg.durationMs = (int)clampf(stunPowerRatio(dgoal) * cfg_.stunMaxMs, 200.f, 950.f);
                    // curl toward the corner: flick right if the corner lies clockwise (on screen) of the straight shot, else left
                    const GestureOutput centre = makeGesture(ACT_SHOOT, car.pos, MetricVector2D((float)state_.attackSign * PITCH_HALF_X, 0.f), 1.f, false);
                    if (centre.valid) {
                        float dth = sg.theta - centre.theta;
                        while (dth > 180.f) dth -= 360.f;
                        while (dth < -180.f) dth += 360.f;
                        sg.flick = dth >= 0.f ? 2 : 1;
                    }
                }
            }
            if (cfg_.rescueRules) {   // momentum: tau_adj = tau * (1 - gamma * v / vmax)
                const float f = 1.f - cfg_.momentumGamma * clampf(car.vel5.length() / std::max(cfg_.maxSpeed, 1.f), 0.f, 1.f);
                sg.durationMs = (int)std::max(cfg_.rescueRules ? 100.f : 200.f, (float)sg.durationMs * f);
            }
            if (cfg_.shotLog) {   // one line per fired shot, so on-target numbers from the game can be matched to distance, hold and speed
                const float axl = (float)state_.attackSign * car.future.x;
                const float dxl = PITCH_HALF_X - axl;
                char fl[240];
                std::snprintf(fl, sizeof fl, "[FRAME: %ld] SHOT_FIRED | DIST: %.1f | HOLD_MS: %d | SPEED: %.1f | NEAREST_OPP: %.1f | AIM_Y: %.1f | STUN: %d\n",
                              frameNo_, std::sqrt(dxl * dxl + car.future.y * car.future.y), sg.durationMs, car.vel5.length(), nearestOppDistance(car.future), aimPt.y, stun ? 1 : 0);
                shotLogText_ += fl;
                if (shotLogText_.size() > 60000) shotLogText_.erase(0, shotLogText_.size() / 2);
            }
            if (shotLive_) shotsLive_++; else shotsDry_++;
            if (cfg_.plus) plusEndSequence(1, car.pos);
            if (cfg_.plan) planOnShot(car);
            if (cfg_.v7) { PassCandidate sc; sc.type = ACT_SHOOT; sc.xP = 0.3f; v7NoteAction(sc, sg, car); }
            shotFiredMs_ = now;
            if (cfg_.shotLog) {
                const float axf = (float)state_.attackSign * car.future.x, dxf = PITCH_HALF_X - axf;
                char fl[260];
                std::snprintf(fl, sizeof fl, "[FRAME: %ld] SHOT_FIRED | DIST: %.1f | XG: %.2f | NEAREST_OPP: %.1f | EVT: %d | SCORE: %d-%d | PHASE: %.2f | SM: %d\n", frameNo_, std::sqrt(dxf * dxf + car.future.y * car.future.y), xTGrid::shotValue(axf, car.future.y), nearestOppDistance(car.future), ++shotSeq_, plus_.goalsFor, plus_.goalsAgainst, plus_.phase, cfg_.scoringMachine ? 1 : 0);
                shotLogText_ += fl;
            }
            armSecondBall(car.pos);
            lastActMs_ = now;
            cooldownUntil_ = now + (double)sg.durationMs + 700.0;
            nextDecisionAt_ = cooldownUntil_;
            return sg;
        }
    }
    }

    if (panicWanted) {
        GestureOutput pg;
        if (tryPanicClear(car, pg)) return pg;
    }
    if (coolActive) return none;

    if (holding_) {
        const MetricVector2D aimPt((float)state_.attackSign * PITCH_HALF_X, cueAimY_);
        return makeGesture(ACT_SHOOT, car.pos, aimPt, 0.8f, false);
    }

    // our own penalty area: do not gamble, hoof it away when pressed and nothing safe is on
    if (!human_ && inOwnBox(car.pos)) {
        const float own = -(float)state_.attackSign;
        const bool carIsGk = isKeeperIdx(state_.carrier);
        const bool nearGoalLine = !carIsGk && (PITCH_HALF_X - own * car.pos.x) < 4.0f;   // deep defenders: normal play
        float bestSafe = 0.f;
        for (int i = 0; i < n; i++) {
            const PassCandidate& c = cand_[i];
            if (c.type != ACT_PASS && c.type != ACT_THROUGH) continue;
            if (c.offside > 0.5f) continue;
            bestSafe = std::max(bestSafe, c.xP);
        }
        const bool pressed = nearestOppDistance(car.future) < cfg_.clearPressDist * (carIsGk ? 1.3f : 1.f);
        if (!nearGoalLine && pressed && bestSafe < cfg_.clearSafeXp) {
            const float s = (float)state_.attackSign;
            // flanks only, never across the face of our own goal: a safe touch-line clearance, not a corner
            static const float TY[4] = {-28.f, -18.f, 18.f, 28.f};
            float bestScore = -1e9f;
            MetricVector2D bestT(car.pos.x + s * 35.f, 18.f);
            for (int k = 0; k < 4; k++) {
                MetricVector2D t(clampf(car.pos.x + s * 38.f, -PITCH_HALF_X + 20.f, PITCH_HALF_X), TY[k]);
                const float margin = interceptMargin(car.pos, t);
                const float score = margin + 0.06f * std::fabs(TY[k]);   // prefer the flanks, away from the centre
                if (score > bestScore) { bestScore = score; bestT = t; }
            }
            GestureOutput cg = makeGesture(ACT_SHOOT, car.pos, bestT, 1.f, false);
            if (cg.valid) {
                cg.actionType = ACT_SHOT;
                cg.durationMs = (int)clampf((float)cfg_.clearHoldMs, 150.f, 900.f);
                cooldownUntil_ = now + (double)cg.durationMs + 600.0;
                nextDecisionAt_ = cooldownUntil_;
                return cg;
            }
        }
    }

    int carryIdx = -1;
    for (int i = 0; i < n; i++)
        if (cand_[i].type == ACT_CARRY) carryIdx = i;

    // hold cap and press release: track how long this carrier has had the ball and how long he has been pressed
    if (holdCarrier_ != state_.carrier || holdLastAt_ < 0 || now - holdLastAt_ > 800.0) { holdCarrier_ = state_.carrier; holdStartAt_ = now; pressStartAt_ = -1; pressCloseSince_ = -1e9; }
    holdLastAt_ = now;
    const float nearOpp = nearestOppDistance(car.pos);
    if (nearOpp < cfg_.pressReleaseDist) { if (pressStartAt_ < 0) pressStartAt_ = now; } else pressStartAt_ = -1;
    // tighter sustained-press force so we stop carrying into Legendary tackles
    if (cfg_.scoringMachine && cfg_.forceReleasePress) {
        if (nearOpp < cfg_.forcePressDist) { if (pressCloseSince_ < 0) pressCloseSince_ = now; }
        else pressCloseSince_ = -1;
    }
    const bool tightPressForce = cfg_.scoringMachine && cfg_.forceReleasePress && !human_ &&
        pressCloseSince_ >= 0 && (now - pressCloseSince_) >= (double)cfg_.forcePressMs;
    const bool holdForce = (cfg_.rescueRules && !human_ &&
        ((pressStartAt_ >= 0 && now - pressStartAt_ >= (double)cfg_.pressReleaseMs) ||
         now - holdStartAt_ >= (double)(nearOpp < 4.0f ? cfg_.holdCapPressMs : cfg_.holdCapFreeMs)))
        || tightPressForce;
    const bool crowdForce = holdForce || (cfg_.keepBall && state_.crowded && crowdSinceMs_ >= 0 &&
                            now - crowdSinceMs_ >= (double)cfg_.crowdMaxHoldMs);
    const bool touchForce = cfg_.playmaker && state_.oneTouch;
    const bool pressForce = cfg_.plan && cfg_.planPressure && !human_ && planPressure(car.pos) > cfg_.planPressHigh;
    // final-third clock: force progressive play after boxMustProgressSec of sterile possession
    {
        const float sft = (float)state_.attackSign;
        const float depth = PITCH_HALF_X - sft * car.pos.x;
        if (depth <= cfg_.finalThirdDepth) {
            if (finalThirdCarrier_ != state_.carrier || finalThirdSince_ < 0) {
                finalThirdCarrier_ = state_.carrier;
                finalThirdSince_ = now;
            }
        } else {
            finalThirdSince_ = -1;
            finalThirdCarrier_ = -1;
        }
    }
    const bool progressForce = !human_ && finalThirdMustProgress(car, now);
    int pick;
    if (now < nextDecisionAt_ && carryIdx >= 0 && !crowdForce && !touchForce && !pressForce && !progressForce) {
        pick = carryIdx;
    } else {
        pick = sampleCandidate();
        nextDecisionAt_ = now + jitteredDecisionMs();
        if (progressForce) {
            const int prog = pickBestProgressive(n, car);
            if (prog >= 0) pick = prog;
        }

        if (cfg_.legendary && pick >= 0) {
            int gap = -1;
            float gapScore = 0.03f;
            for (int i = 0; i < n; i++) {
                const PassCandidate& c = cand_[i];
                const bool through = c.type == ACT_THROUGH && c.xP >= 0.52f && c.deltaXT >= 0.03f;  // BEAST: more through gaps
                const bool fwdPass = c.type == ACT_PASS && c.xP >= 0.78f && c.deltaXT >= 0.08f;
                if (!(through || fwdPass) || c.offside > 0.01f || c.pressure > 0.6f) continue;
                const float sc = c.xP * c.deltaXT;
                if (sc > gapScore) {
                    gapScore = sc;
                    gap = i;
                }
            }
            if (gap >= 0) {
                pick = gap;
                ballHoldStartMs_ = -1;
            } else if (carryIdx >= 0) {
                const PassCandidate& pc = cand_[pick];
                const bool recycling = (pc.type == ACT_PASS || pc.type == ACT_LOFTED || pc.type == ACT_THROUGH) &&
                                       pc.deltaXT <= 0.01f;
                if (recycling && lanePressure(car.pos) < 0.25f) {
                    if (ballHoldStartMs_ < 0) ballHoldStartMs_ = now;
                    if (now - ballHoldStartMs_ < (double)cfg_.legendHoldMs) pick = carryIdx;
                    else ballHoldStartMs_ = -1;
                } else if (pc.type != ACT_CARRY) {
                    ballHoldStartMs_ = -1;
                }
            }
        }
    }
    const bool restartSafe = now < restartUntilMs_;
    bool switchForced = false;
    if (switchIdx_ >= 0 && switchIdx_ < n && cand_[switchIdx_].type == ACT_LOFTED && pick >= 0 && cand_[pick].type != ACT_SHOOT) {
        pick = switchIdx_;           // crowded: override the short game and hit the far touchline
        switchForced = true;
        turnStartMs_ = -1;
        turnTargetId_ = -1;
    }
    if (!switchForced && pick >= 0 && turnStartMs_ >= 0 && now - turnStartMs_ < (double)cfg_.turnMaxMs) {
        // keep the pass we started turning for, so the player does not dither between targets
        for (int i = 0; i < n; i++) {
            const PassCandidate& c = cand_[i];
            if (c.targetId == turnTargetId_ && (c.type == ACT_PASS || c.type == ACT_THROUGH) && c.xP >= 0.3f) { pick = i; break; }
        }
    }
    if (cfg_.plan && cfg_.planHoldMargin && pick >= 0 && carryIdx >= 0 && cand_[pick].type != ACT_CARRY && cand_[pick].type != ACT_SHOOT &&
        !crowdForce && !touchForce && !pressForce && !switchForced && planPressure(car.pos) < cfg_.planPressCalm) {
        // calm carrier: a pass has to beat the carry by a margin that fades the longer he has had the ball
        const float holdSec = (float)((now - holdStartAt_) / 1000.0);
        const float margin = std::max(0.f, 0.10f - 0.05f * holdSec);
        if (cand_[pick].utility - cand_[carryIdx].utility < margin) pick = carryIdx;
    }
    if (pick >= 0 && ((cfg_.keepBall && state_.crowded) || touchForce || pressForce || restartSafe || holdForce) && cand_[pick].type != ACT_SHOOT &&
        (cand_[pick].type == ACT_CARRY || cand_[pick].xP < cfg_.safeXp)) {
        int safeIdx = -1;
        for (int i = 0; i < n; i++) {
            const PassCandidate& c = cand_[i];
            if (c.type != ACT_PASS && c.type != ACT_THROUGH && c.type != ACT_LOFTED) continue;
            if (c.xP < cfg_.safeXp || c.offside > 0.5f) continue;
            if (safeIdx < 0 || c.utility > cand_[safeIdx].utility) safeIdx = i;
        }
        if (safeIdx < 0 && (holdForce || pressForce)) {   // nothing is clearly safe: still release to the best forward-or-open pass
            for (int i = 0; i < n; i++) {
                const PassCandidate& c = cand_[i];
                if (c.type != ACT_PASS && c.type != ACT_THROUGH) continue;
                if (c.xP < (holdForce ? 0.45f : 0.5f) || c.offside > 0.5f) continue;
                if (safeIdx < 0 || c.utility > cand_[safeIdx].utility) safeIdx = i;
            }
        }
        if (safeIdx >= 0) {
            pick = safeIdx;
            ballHoldStartMs_ = -1;
            holdStartAt_ = now;
            pressStartAt_ = -1;
        }
    }
    const bool forcedNow = holdForce || crowdForce || touchForce || pressForce || restartSafe || switchForced;
    if (cfg_.v7 && pick >= 0 && !human_ && !switchForced && !touchForce) v7SurvivalPick(&pick, car);
    if (cfg_.plan && pick >= 0 && !human_) {
        planForcePick(&pick, car);
        GestureOutput sg;
        if (planSidestep(pick, car, forcedNow, &sg)) {
            sg.status = ST_TEAM;
            sg.actionType = ACT_CARRY;
            if (carryIdx >= 0) chosen_ = carryIdx;
            return sg;
        }
    }
    if (cfg_.plus && cfg_.meta && pick >= 0 && !(cfg_.v7 && v7_.surv)) {
        GestureOutput mg;
        if (metaPreCommit(&pick, car, forcedNow, &mg)) {
            mg.status = ST_TEAM;
            mg.actionType = ACT_CARRY;
            if (carryIdx >= 0) chosen_ = carryIdx;
            return mg;
        }
        if (!forcedNow) metaDecorrelate(pick, car);
    }
    if (pick < 0) return none;
    chosen_ = pick;
    if (cfg_.v7) v7TimingLead(cand_[pick]);
    const PassCandidate& c = cand_[pick];

    // body orientation: rotate to face the receiver first, then pass (one-touch under pressure skips the turn)
    if (cfg_.faceBeforePass && !human_ && c.type != ACT_SHOOT && c.type != ACT_CARRY && !switchForced) {
        bool turning = false;
        if (!touchForce && !inboundOneTouch && nearestOppDistance(car.pos) >= cfg_.turnSkipOppDist) {
            const MetricVector2D d = c.target - car.pos;
            const float dl = d.length();
            if (dl > 1e-3f) {
                const float spd = car.vel.length();
                bool facing;
                if (spd >= cfg_.faceMinSpeed) {
                    const float cosA = clampf(car.vel.dot(d) / (spd * dl), -1.f, 1.f);
                    facing = std::acos(cosA) * 57.2957795f <= cfg_.faceTolDeg;
                } else {
                    facing = turnStartMs_ >= 0 && turnTargetId_ == c.targetId;   // standing: one turn tick is done
                }
                if (!facing) {
                    if (turnStartMs_ < 0 || turnTargetId_ != c.targetId) { turnStartMs_ = now; turnTargetId_ = c.targetId; }
                    if (now - turnStartMs_ < (double)cfg_.turnMaxMs) {
                        GestureOutput tg = makeGesture(ACT_CARRY, car.pos, c.target, 0.f, false);
                        if (tg.valid) {
                            tg.durationMs = cfg_.turnHoldMs;
                            nextDecisionAt_ = now;
                            if (carryIdx >= 0) chosen_ = carryIdx;
                            return tg;
                        }
                    }
                }
            }
        }
        (void)turning;
        turnStartMs_ = -1;
        turnTargetId_ = -1;
    }

    if (cfg_.v7 && !human_ && !switchForced) {
        GestureOutput pg;
        if (v7PreAlignStep(c, car, forcedNow, &pg)) {
            pg.status = ST_TEAM; pg.actionType = ACT_CARRY;
            if (carryIdx >= 0) chosen_ = carryIdx;
            return pg;
        }
        if (v7TimingHold(c, car, forcedNow)) {
            GestureOutput hg = makeGesture(ACT_CARRY, car.pos, c.target, 0.f, false);
            if (hg.valid) {
                hg.durationMs = 60; hg.status = ST_TEAM; hg.actionType = ACT_CARRY;
                nextDecisionAt_ = now;
                if (carryIdx >= 0) chosen_ = carryIdx;
                return hg;
            }
        }
    }
    float power;
    if (c.type == ACT_SHOOT) power = 0.6f + 0.4f * clampf(c.length / std::max(cfg_.shootRange, 1.f), 0.f, 1.f);
    else if (c.type == ACT_THROUGH) power = clampf(c.length / 40.f, 0.1f, 1.f);
    else power = clampf(c.length / 45.f, 0.1f, 1.f);
    if (switchForced) power = clampf(cfg_.switchPower, 0.5f, 1.f);
    else if (cfg_.rescueRules && (c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED)) {
        int nd = 0;   // defenders within 1.5 m of the pass ray
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (o.active && segmentDistance(car.pos, c.target, o.future) < 1.5f) nd++;
        }
        power = clampf(power * (1.f + cfg_.passAlpha * c.length + cfg_.passBeta * (float)nd), 0.1f, 1.f);
    }

    bool dash = false;
    if (c.type == ACT_CARRY && cfg_.dashWhenFree)
        dash = nearestOppDistance(car.future) > (cfg_.legendary ? 11.f : 12.f) && state_.attackSign * car.pos.x < 40.f;
    if (dash && cfg_.v7 && cfg_.v7Stamina && staminaOf(state_.carrier) < 0.3f) dash = false;

    GestureOutput g = makeGesture(c.type, car.pos, c.target, power, dash);
    if (g.valid && c.type == ACT_CARRY && cfg_.rescueRules && cfg_.preAlign) {
        // keep moving, but with the stick already pointing at the best receiver (no turning tick before the pass)
        int bi = -1;
        for (int i = 0; i < n; i++) {
            const PassCandidate& pc = cand_[i];
            if (pc.type != ACT_PASS && pc.type != ACT_THROUGH) continue;
            if (pc.xP < cfg_.minPassXp || pc.offside > 0.5f) continue;
            if (bi < 0 || pc.utility > cand_[bi].utility) bi = i;
        }
        preAlignedCarry(car, bi, g);
    }
    if (g.valid && cfg_.plus) { plusNoteAction(c, car); metaNoteAction(c, g, car); }
    if (g.valid && cfg_.plan) planNoteAction(c, car);
    if (g.valid && cfg_.v7) v7NoteAction(c, g, car);
    if (g.valid && c.type != ACT_CARRY) {
        cooldownUntil_ = now + effCooldownMs();
        nextDecisionAt_ = cooldownUntil_;
        lastActMs_ = now;
    }
    return g;
}

// The game draws the player you control as a larger glowing aqua disc on the radar.
// While you do not have the ball it is a separate blob; while you carry it, it merges with the ball ring.
void PitchEngine::detectControlled(const unsigned char* buf, int w, int h) {
    if (!buf || w < 16 || h < 16) { ctrlSeen_ = false; return; }
    const int n = w * h;
    const int stride = rowStride_ > 0 ? rowStride_ : w * pixelStride_;
    const int ri = bgr_ ? 2 : 0, bi = bgr_ ? 0 : 2;
    if ((int)ctrlMask_.size() < n) ctrlMask_.assign((size_t)n, 0);
    else std::fill(ctrlMask_.begin(), ctrlMask_.begin() + n, (uint8_t)0);
    int any = 0;
    for (int y = 0; y < h; y++) {
        const unsigned char* row = buf + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            const unsigned char* px = row + (size_t)x * pixelStride_;
            const int r = px[ri], g = px[1], b = px[bi];
            const int mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
            if (mx < 199 || (mx - mn) * 20 < 11 * mx) continue;
            const float d = (float)(mx - mn);
            float hue;
            if (mx == b) hue = 60.f * (4.f + (float)(r - g) / d);
            else if (mx == g) hue = 60.f * (2.f + (float)(b - r) / d);
            else continue;
            if (hue >= 165.f && hue <= 190.f) { ctrlMask_[(size_t)y * w + x] = 1; any++; }
        }
    }
    if (!any) { ctrlSeen_ = false; return; }
    if ((int)ctrlStack_.size() < n) ctrlStack_.assign((size_t)n, 0);
    const float minA = std::max(10.f, 0.0012f * (float)w * (float)w);
    const float maxA = 0.012f * (float)w * (float)w;
    float bestA = 0.f, bx = 0.f, by = 0.f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int start = y * w + x;
            if (!ctrlMask_[(size_t)start]) continue;
            int sp = 0;
            ctrlStack_[(size_t)sp++] = start;
            ctrlMask_[(size_t)start] = 0;
            double sx = 0, sy = 0;
            int area = 0, x0 = w, x1 = -1, y0 = h, y1 = -1;
            while (sp > 0) {
                const int i = ctrlStack_[(size_t)--sp];
                const int ix = i % w, iy = i / w;
                sx += ix + 0.5; sy += iy + 0.5; area++;
                x0 = std::min(x0, ix); x1 = std::max(x1, ix);
                y0 = std::min(y0, iy); y1 = std::max(y1, iy);
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy) continue;
                        const int jx = ix + dx, jy = iy + dy;
                        if (jx < 0 || jy < 0 || jx >= w || jy >= h) continue;
                        const int j = jy * w + jx;
                        if (ctrlMask_[(size_t)j]) { ctrlMask_[(size_t)j] = 0; ctrlStack_[(size_t)sp++] = j; }
                    }
            }
            const float a = (float)area;
            if (a < minA || a > maxA) continue;
            const float bw = (float)(x1 - x0 + 1), bh = (float)(y1 - y0 + 1);
            if (std::max(bw, bh) > 2.2f * std::min(bw, bh)) continue;
            if (a > bestA) { bestA = a; bx = (float)(sx / area); by = (float)(sy / area); }
        }
    }
    if (bestA > 0.f) {
        ctrlPos_ = hom_.apply(bx, by);
        ctrlSeen_ = true;
        ctrlSeenMs_ = frameTimeMs_;
    } else {
        ctrlSeen_ = false;
    }
}

// Out of possession: switch to the player nearest the ball, close the carrier down goal-side, tackle only when
// very close, and use match-up only when we are far from the ball with an attacker next to us.
GestureOutput PitchEngine::decideDefence() {
    GestureOutput none;
    none.status = ST_OPP;
    const double now = state_.timeMs;
    if (oppSinceMs_ < 0) oppSinceMs_ = now;
    if (!ballUsable() || now < defNextAt_) return none;
    if (now - oppSinceMs_ < 150.0) return none;

    const MetricVector2D B = state_.ball;
    int nearIdx = -1;
    float nearD = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& p = state_.team[i];
        if (!p.active || !p.visible) continue;
        const float d = distance(p.pos, B);
        if (d < nearD) { nearD = d; nearIdx = i; }
    }
    const bool haveCtrl = controlledSeen();
    const float dC = haveCtrl ? distance(ctrlPos_, B) : 1e9f;

    auto tap = [&](int type, int ms, bool stick, const MetricVector2D& to) {
        GestureOutput g;
        if (stick && haveCtrl) g = makeGesture(type, ctrlPos_, to, 1.f, false);
        else { g.valid = true; g.r = 0.f; g.theta = 0.f; }
        g.status = ST_OPP;
        g.actionType = type;
        g.durationMs = ms;
        g.valid = true;
        return g;
    };

    if (!haveCtrl) {
        // do not know who we control: let the game's own switch pick the player nearest the ball
        if (now - defSwitchAt_ > 2500.0 && now - oppSinceMs_ > 700.0) {
            defSwitchAt_ = now;
            defNextAt_ = now + 450.0;
            return tap(ACT_SWITCH, 70, false, B);
        }
        return none;
    }

    { GestureOutput vg; if (v7DefEarly(&vg, now)) return vg; }
    if (cfg_.v8) {
        GestureOutput vg;
        if (v8DefKickoff(&vg, now)) return vg;
        {
            const MetricVector2D Bq = state_.ball;
            const bool dIn = cfg_.rescueRules && cfg_.boxLockout && goalDistance(ctrlPos_) < cfg_.defLockDepth && std::fabs(ctrlPos_.y) <= cfg_.boxHalfY;
            const bool bDg = cfg_.rescueRules && cfg_.boxLockout && goalDistance(Bq) < cfg_.defLockDepth;
            const bool np8 = dIn || bDg;
            if (v8DefSafe(&vg, now, np8)) return vg;
            if (v8DefShot(&vg, now, np8, dC)) return vg;
        }
    }
    // 1) tackle when on the ball carrier, but NEVER in our own box (penalty) and only from the goal side (no foul from behind)
    const float ownDir = -(float)state_.attackSign;
    const bool goalSide = ownDir * (ctrlPos_.x - B.x) > -0.3f;
    // contain vs commit: stay on his goal side and jockey; only go to ground when he is within easy reach or the touch ran loose
    float carrierToBall = 1e9f;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (o.active && o.visible) carrierToBall = std::min(carrierToBall, distance(o.pos, B));
    }
    const bool looseTouch = carrierToBall > cfg_.looseTouchDist;
    const bool rr = cfg_.rescueRules;
    // penalty-box lockout: no tackle and no press call while the defender or the ball is inside our own box
    const bool defInBox = rr && cfg_.boxLockout && goalDistance(ctrlPos_) < cfg_.defLockDepth && std::fabs(ctrlPos_.y) <= cfg_.boxHalfY;
    const bool ballDanger = rr && cfg_.boxLockout && goalDistance(B) < cfg_.defLockDepth;
    const bool noTackle = inOwnBox(B) || defInBox || goalDistance(B) < cfg_.noTackleDepth;   // close to our keeper: never go to ground
    const bool noPress = defInBox || ballDanger;
    // containment: inside the buffer is a breach, so only then (or on a loose touch) does the defender go to ground
    const float commitD = (rr && cfg_.containment) ? std::min(cfg_.commitDist, cfg_.containMin - 0.2f) : cfg_.commitDist;
    const bool commit = dC < commitD || (looseTouch && dC < cfg_.tackleMaxDist);
    if (commit && dC < cfg_.tackleMaxDist && goalSide && !noTackle && now - defTackleAt_ > (double)cfg_.tackleGapMs) {
        defTackleAt_ = now;
        defNextAt_ = now + 350.0;
        if (!cfg_.tackleEnabled) {
            // shadow the carrier with Match-up instead of lunging in (no fouls, no penalties)
            if (now - defMatchAt_ > 900.0) {
                defMatchAt_ = now;
                return tap(ACT_MATCHUP, 140, false, B);
            }
        } else {
            return tap(ACT_TACKLE, 80, true, B);
        }
    }
    // 1a) a pass is in flight: send the player who wins the race to it (switch to him, or run the controlled player onto it)
    if (cfg_.plus && cfg_.plusRace && !noPress && state_.ballVel.length() > 6.f && carrierToBall > 2.0f) {
        int bi = -1;
        MetricVector2D where;
        float lead = 0.f, cT = 0.f;
        const int cidx = controlledIdx();
        if (plusRaceAssign(&bi, &where, &lead, &cT) && bi >= 0) {
            if (bi != cidx) {
                if (cT - plusTta(state_.team[bi], where, 7.5f) > 0.35f && now - defSwitchAt_ > 600.0) {
                    defSwitchAt_ = now;
                    defNextAt_ = now + 450.0;
                    return tap(ACT_SWITCH, 70, false, B);
                }
            } else {
                GestureOutput rg = makeGesture(ACT_PRESS, ctrlPos_, where, 1.f, false);
                if (rg.valid) {
                    rg.status = ST_OPP;
                    rg.actionType = ACT_PRESS;
                    rg.durationMs = 200;
                    rg.dash = distance(ctrlPos_, where) > 3.f;
                    rg.r = 1.0f;
                    defNextAt_ = now + 120.0;
                    return rg;
                }
            }
        }
    }
    { GestureOutput lg; if (v7DefLanding(&lg, now, noPress)) return lg; }
    if (cfg_.v8) { GestureOutput hg; if (v8DefHook(&hg, now, noPress, dC)) return hg; }
    // 1b) counter-press: for a few seconds after a turnover, swarm the ball instead of dropping back
    if (counterPressActive() && !noPress && (float)state_.attackSign * B.x > cfg_.counterPressMinAx) {
        if (!cpCounted_) { cpCounted_ = true; cpCount_++; plan_.cpUsed = true; }
        if (nearIdx >= 0 && nearD + cfg_.counterPressSwitchGap < dC && now - cpSwitchAt_ > 450.0) {
            cpSwitchAt_ = defSwitchAt_ = now;
            defNextAt_ = now + 350.0;
            return tap(ACT_SWITCH, 70, false, B);
        }
        const MetricVector2D T = B + state_.ballVel * 0.3f;
        GestureOutput pg = makeGesture(ACT_PRESS, ctrlPos_, T, 1.f, false);
        if (pg.valid) {
            pg.status = ST_OPP;
            pg.actionType = ACT_PRESS;
            pg.durationMs = 220;
            pg.dash = dC > 2.5f;
            pg.r = 1.0f;
            defNextAt_ = now + 120.0;
            return pg;
        }
    }
    // 2) someone else is clearly closer to the ball: take them over
    float swMargin = 2.5f;
    if (cfg_.plan && cfg_.planShape && nearIdx >= 0 && (role_[nearIdx] == ROLE_CB || role_[nearIdx] == ROLE_FB) && goalDistance(B) > 28.f)
        swMargin = 5.5f;                      // a line player leaves the line only when he is clearly the closer one
    if (nearIdx >= 0 && nearD + swMargin < dC && now - defSwitchAt_ > 600.0) {
        defSwitchAt_ = now;
        defNextAt_ = now + 450.0;
        return tap(ACT_SWITCH, 70, false, B);
    }
    // 3a) lock onto the ball carrier with Match-up once he is within reach, so the defender shadows him goal-side
    if (dC < 16.f && dC > 3.0f && now - defMatchAt_ > 1800.0 && now - oppSinceMs_ > 300.0) {
        defMatchAt_ = now;
        defNextAt_ = now + 450.0;
        return tap(ACT_MATCHUP, 140, false, B);
    }
    // 3a2) cover: min-conflicts assignment of the defenders to the dangerous opponents; the controlled one takes his point
    if (cfg_.plus && cfg_.plusCover && !noPress && (dC > 14.f || (cfg_.meta && meta_.restartKind == 5)) && now - plus_.coverAt > (cfg_.meta ? 100.0 : 250.0)) {
        MetricVector2D cp;
        bool line = false;
        plus_.coverAt = now;
        if (plusCoverPoint(&cp, &line)) {
            const float dd = distance(ctrlPos_, cp);
            if (dd > (line ? 2.5f : 1.3f)) {
                GestureOutput cg = makeGesture(ACT_PRESS, ctrlPos_, cp, 1.f, false);
                if (cg.valid) {
                    cg.status = ST_OPP;
                    cg.actionType = ACT_PRESS;
                    cg.durationMs = 220;
                    cg.dash = dd > 9.f;
                    cg.r = dd < 3.f ? 0.5f : 1.0f;
                    defNextAt_ = now + 200.0;
                    return cg;
                }
            }
        }
    }
    // 3a2b) cover shadow: a midfielder who is not pressing blocks the lane to the most dangerous attacker
    if (cfg_.plan && cfg_.planShape && !noPress && !counterPressActive() && dC > 9.f && now - plan_.shadowAt > 120.0) {
        MetricVector2D sp;
        plan_.shadowAt = now;
        if (planShadowPoint(&sp) && distance(ctrlPos_, sp) > 1.3f) {
            GestureOutput sg = makeGesture(ACT_PRESS, ctrlPos_, sp, 1.f, false);
            if (sg.valid) {
                sg.status = ST_OPP;
                sg.actionType = ACT_PRESS;
                sg.durationMs = 200;
                sg.dash = distance(ctrlPos_, sp) > 7.f;
                sg.r = distance(ctrlPos_, sp) < 3.f ? 0.6f : 1.0f;
                defNextAt_ = now + 120.0;
                return sg;
            }
        }
    }
    { GestureOutput mg2; if (v7DefMid(&mg2, now, noPress, dC)) return mg2; }
    // 3a3) meta defence: recovery runs after a turnover, the second man cutting the escape lane, the bait lane
    if (cfg_.plus && cfg_.meta && !noPress && !counterPressActive()) {
        MetricVector2D mp;
        auto go = [&](const MetricVector2D& to, double* stamp) -> bool {
            GestureOutput mg = makeGesture(ACT_PRESS, ctrlPos_, to, 1.f, false);
            if (!mg.valid) return false;
            const float dd = distance(ctrlPos_, to);
            if (dd < 1.2f) return false;
            mg.status = ST_OPP;
            mg.actionType = ACT_PRESS;
            mg.durationMs = 200;
            mg.dash = dd > 7.f;
            mg.r = dd < 3.f ? 0.6f : 1.0f;
            *stamp = now;
            defNextAt_ = now + 120.0;
            defGesture_ = mg;
            return true;
        };
        if (cfg_.metaRecovery && now - lostBallMs_ < 2500.0 && dC > 6.f && now - meta_.recAt > 120.0 && metaRecoveryPoint(&mp) && go(mp, &meta_.recAt)) return defGesture_;
        if (cfg_.metaSecondMan && dC > 3.5f && dC < 14.f && now - meta_.secAt > 120.0 && metaSecondMan(&mp) && go(mp, &meta_.secAt)) return defGesture_;
        if (cfg_.metaBait && dC > 6.f && dC < 16.f && now - meta_.baitAt > 150.0 && metaBaitPoint(&mp) && go(mp, &meta_.baitAt)) return defGesture_;
    }
    // 3b) far from the ball with an attacker beside us: mark him
    if (dC > 14.f && now - defMatchAt_ > 4000.0) {
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (!o.active || !o.visible) continue;
            if (distance(o.pos, B) < 3.f) continue;
            if (distance(o.pos, ctrlPos_) < 6.f) {
                defMatchAt_ = now;
                defNextAt_ = now + 500.0;
                return tap(ACT_MATCHUP, 120, false, B);
            }
        }
    }
    // 3c) interception cone: stand on the carrier-to-receiver lane in Match-up so the game's auto-intercept can fire
    const float own = -(float)state_.attackSign;
    if (rr && cfg_.coneRadius > 0.f && dC > 3.0f && now - defConeAt_ > 600.0) {
        int ci = -1;
        float cd = 1e9f;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& o = state_.opp[i];
            if (!o.active || !o.visible) continue;
            const float d = distance(o.pos, B);
            if (d < cd) { cd = d; ci = i; }
        }
        if (ci >= 0 && cd < 3.0f && !isKeeperIdx(controlledIdx())) {
            const MetricVector2D cp = state_.opp[ci].pos;
            float bestLane = 1e9f;
            MetricVector2D bestRecv;
            for (int i = 0; i < TEAM_SIZE; i++) {
                if (i == ci) continue;
                const PlayerState& o = state_.opp[i];
                if (!o.active || !o.visible) continue;
                if (own * (o.pos.x - cp.x) <= 1.0f) continue;             // forward passing channels only (toward our goal)
                if (distance(o.pos, cp) > 38.f) continue;
                const float d = segmentDistance(cp, o.pos, ctrlPos_);
                if (d < bestLane) { bestLane = d; bestRecv = o.pos; }
            }
            if (bestLane <= cfg_.coneRadius) {
                const MetricVector2D seg = bestRecv - cp;
                const float l2 = std::max(seg.x * seg.x + seg.y * seg.y, 1e-6f);
                const float t = clampf(((ctrlPos_.x - cp.x) * seg.x + (ctrlPos_.y - cp.y) * seg.y) / l2, 0.2f, 0.8f);
                const MetricVector2D snap = cp + seg * t;
                defConeAt_ = now;
                defNextAt_ = now + 350.0;
                return tap(ACT_MATCHUP, 160, distance(snap, ctrlPos_) > 0.3f, snap);
            }
        }
    }
    // 3d) offside line: the opposition just passed sideways or backward in midfield, so a back-line player steps up
    if (rr && cfg_.lineStep && now < lineStepUntil_ && now - defStepAt_ > (double)cfg_.lineStepMs) {
        const int ci = controlledIdx();
        if (ci >= 0 && !isKeeperIdx(ci)) {
            // the four deepest outfield players are the back line
            float depth[TEAM_SIZE];
            int order[TEAM_SIZE];
            int m = 0;
            for (int i = 0; i < TEAM_SIZE; i++) {
                if (!state_.team[i].active || isKeeperIdx(i)) continue;
                depth[m] = goalDistance(state_.team[i].pos);
                order[m++] = i;
            }
            for (int a = 0; a < m; a++)
                for (int b = a + 1; b < m; b++)
                    if (depth[b] < depth[a]) { std::swap(depth[a], depth[b]); std::swap(order[a], order[b]); }
            bool inLine = false;
            for (int a = 0; a < std::min(m, 4); a++) if (order[a] == ci) inLine = true;
            const float myDepth = goalDistance(state_.team[ci].pos);
            const float lineHead = m > 0 ? depth[std::min(m, 4) - 1] : myDepth;   // the most advanced of the four
            const float lsm = cfg_.lineStepM * ((cfg_.plus && cfg_.plusIntent && plus_.im[3] > 0.6f) ? 0.5f : 1.f) * (cfg_.plus ? metaShapeScale() : 1.f);   // a long-ball side: do not push the line up
            if (inLine && myDepth + 0.3f < lineHead + lsm) {
                const float stepTo = std::min(myDepth + lsm, lineHead + lsm);   // never ahead of the line's head by more than one step
                const MetricVector2D tgt(ctrlPos_.x - own * (stepTo - myDepth), ctrlPos_.y);
                GestureOutput sg = makeGesture(ACT_STEPUP, ctrlPos_, tgt, 1.f, false);
                if (sg.valid) {
                    defStepAt_ = now;
                    defNextAt_ = now + 300.0;
                    sg.status = ST_OPP;
                    sg.actionType = ACT_STEPUP;
                    sg.durationMs = 300;
                    sg.r = 0.8f;
                    return sg;
                }
            }
        }
    }
    if (cfg_.v7 && cfg_.v7GkShadow && nearIdx >= 0 && nearIdx == controlledIdx() && dC < 12.f) {
        MetricVector2D gp;
        if (v7GkShadowPoint(&gp) && distance(ctrlPos_, gp) > 0.5f) {
            defNextAt_ = now + 150.0;
            if (noPress) {                    // inside our box: jockey only, no press call
                GestureOutput jg = makeGesture(ACT_MATCHUP, ctrlPos_, gp, 1.f, false);
                jg.status = ST_OPP; jg.actionType = ACT_MATCHUP; jg.durationMs = 280; jg.valid = true;
                jg.r = std::min(jg.r, 0.7f);
                return jg;
            }
            return v7PressTo(gp, 200, false, distance(ctrlPos_, gp) < 2.f ? 0.5f : 1.f);
        }
    }
    // 3e) lockdown zone: pure Match-up / jockey on the goal-centre axis, no dash, no press call
    if (noPress) {
        const MetricVector2D gc(own * PITCH_HALF_X, 0.f);
        MetricVector2D toG = gc - B;
        const float gl = toG.length();
        if (gl > 1e-3f) toG = toG * (1.f / gl);
        const MetricVector2D post = B + state_.ballVel * 0.2f + toG * 1.75f;
        defNextAt_ = now + 300.0;
        GestureOutput jg = tap(ACT_MATCHUP, 280, distance(post, ctrlPos_) > 0.35f, post);
        jg.r = std::min(jg.r, 0.7f);
        return jg;
    }
    bool v7NoDash = false;
    MetricVector2D v7Shift;
    if (cfg_.v7) {
        int ci2 = -1;
        v7PredCarrierNear(B, &ci2);
        if (ci2 >= 0 && cfg_.v7Recurrent) {
            v7Shift = v7_.predDv[ci2] * 0.25f;
            if (v7_.pSharp[ci2] > 0.55f && dC < 7.f) v7NoDash = true;      // a stop-fake or sharp cut is likely: do not commit
        }
        if (cfg_.v7Stamina) { const int cI = controlledIdx(); if (cI >= 0 && staminaOf(cI) < 0.3f && dC > 4.f) v7NoDash = true; }
    }
    // 4) close down: aim a little goal-side of the ball, dash/press while still some distance away
    MetricVector2D T = B + state_.ballVel * 0.35f;
    if (dC < 9.f) T.x += own * 2.2f;                  // stay goal-side of the carrier
    if (inOwnBox(B) || (PITCH_HALF_X - own * B.x) < 28.f) T.x += own * 0.8f;   // in our third: close the shooting lane
    T = T + v7Shift;
    if (rr && cfg_.containment && dC < 14.f) {
        // outside the box: hold containMin..containMax in front of the attacker, on the line to our goal centre
        const MetricVector2D gc(own * PITCH_HALF_X, 0.f);
        MetricVector2D toG = gc - B;
        const float gl = toG.length();
        if (gl > 1e-3f) toG = toG * (1.f / gl);
        float buf = (cfg_.plus && cfg_.meta && cfg_.metaContain) ? metaContainBuf() : 0.5f * (cfg_.containMin + cfg_.containMax);
        buf *= v7DefBufMul();
        T = B + state_.ballVel * 0.2f + toG * buf;
        if (cfg_.plus && cfg_.meta && cfg_.metaContain) T.y += metaSideBias();
        T = T + v7Shift;
        const float dT = distance(T, ctrlPos_);
        GestureOutput cg = makeGesture(ACT_PRESS, ctrlPos_, T, 1.f, false);
        if (!cg.valid || dT < 0.3f) {            // on the buffer already: hold the position
            GestureOutput hold;
            hold.valid = true;
            hold.status = ST_OPP;
            hold.actionType = ACT_PRESS;
            hold.durationMs = 150;
            hold.r = 0.f;
            return hold;
        }
        cg.status = ST_OPP;
        cg.actionType = ACT_PRESS;
        cg.durationMs = 200;
        cg.dash = dT > 6.f && !v7NoDash;         // sprint to get goal-side, then jockey without the dash
        cg.r = dT < 1.5f ? 0.5f : 1.0f;
        return cg;
    }
    GestureOutput g = makeGesture(ACT_PRESS, ctrlPos_, T, 1.f, false);
    if (!g.valid) return none;
    g.status = ST_OPP;
    g.actionType = ACT_PRESS;
    g.durationMs = 220;
    g.dash = dC > 4.f && dC < 30.f && !v7NoDash;
    g.r = dC < 3.f ? 0.55f : 1.0f;
    return g;
}

int PitchEngine::debugDots(float* out, int maxFloats) const {
    if (!out || maxFloats < 5) return 0;
    int k = 5, nh = 0, na = 0;
    for (int i = 0; i < TEAM_SIZE && k + 2 <= maxFloats; i++) {
        const PlayerState& p = state_.team[i];
        if (p.active && p.visible) {
            out[k++] = (p.pos.x + PITCH_HALF_X) / (2.f * PITCH_HALF_X);
            out[k++] = (p.pos.y + PITCH_HALF_Y) / (2.f * PITCH_HALF_Y);
            nh++;
        }
    }
    for (int i = 0; i < TEAM_SIZE && k + 2 <= maxFloats; i++) {
        const PlayerState& p = state_.opp[i];
        if (p.active && p.visible) {
            out[k++] = (p.pos.x + PITCH_HALF_X) / (2.f * PITCH_HALF_X);
            out[k++] = (p.pos.y + PITCH_HALF_Y) / (2.f * PITCH_HALF_Y);
            na++;
        }
    }
    out[0] = (float)nh;
    out[1] = (float)na;
    out[2] = state_.ballSeen ? 1.f : 0.f;
    out[3] = (state_.ball.x + PITCH_HALF_X) / (2.f * PITCH_HALF_X);
    out[4] = (state_.ball.y + PITCH_HALF_Y) / (2.f * PITCH_HALF_Y);
    return k;
}

namespace {
const float ATT_PRIOR[PitchEngine::ATT_NF] = {0.f, 1.f, 0.f, 0.f, 0.f, 0.f};
}

// Keeps the learned pass model physically sensible: more pressure, more crowding and longer balls can only make a pass LESS likely to
// be kept, and the bias cannot lift every pass to "safe". (A match with only completed passes once drove the bias to +1.9, the lane
// weight to its floor and pressure/crowd weights positive: every pass looked 80-95 % safe.)
static void sanitizeAttackWeights(float* w, int nf) {
    if (nf < 6) return;
    w[0] = clampf(w[0], -1.0f, 0.8f);
    w[1] = clampf(w[1], 0.3f, 2.5f);
    w[2] = clampf(w[2], -3.f, 0.f);
    w[3] = clampf(w[3], -3.f, 0.f);
    w[4] = clampf(w[4], -3.f, 0.3f);
    w[5] = clampf(w[5], -1.5f, 1.5f);
}

void PitchEngine::attackFeatures(float xpRaw, float pressure, float crowd, float len, float fwd, float* f) {
    const float x = clampf(xpRaw, 0.02f, 0.98f);
    f[0] = 1.f;
    f[1] = std::log(x / (1.f - x));
    f[2] = clampf(pressure, 0.f, 1.5f);
    f[3] = clampf(crowd, 0.f, 1.f);
    f[4] = clampf(len / 40.f, 0.f, 2.f);
    f[5] = clampf(fwd / 40.f, -1.5f, 1.5f);
}

float PitchEngine::attackProb(const float* f) const {
    float z = 0.f;
    for (int k = 0; k < ATT_NF; k++) z += attW_[k] * f[k];
    return logistic(safe(z, 0.f));
}

void PitchEngine::attackSnapshot() {
    const int ci = state_.carrier;
    if (state_.status != ST_TEAM || ci < 0 || ci >= TEAM_SIZE || nCand_ <= 0) return;
    if (!state_.ballSeen || ballAssumed_) return;
    const PlayerState& car = state_.team[ci];
    if (distance(state_.ball, car.pos) > cfg_.ownRadius) return;
    const float s = (float)state_.attackSign;
    int n = 0;
    bool shot = false;
    for (int i = 0; i < nCand_ && n < MAX_CAND; i++) {
        const PassCandidate& c = cand_[i];
        if (c.type == ACT_SHOOT) { shot = true; continue; }
        if (c.type != ACT_PASS && c.type != ACT_THROUGH && c.type != ACT_LOFTED) continue;
        if (c.xPRaw <= 0.f) continue;
        SnapCand& d = snap_[n++];
        d.id = c.targetId;
        d.type = c.type;
        d.target = c.target;
        d.xpRaw = c.xPRaw;
        d.pressure = c.pressure;
        d.len = c.length;
        d.fwd = s * (c.target.x - car.pos.x);
    }
    nSnap_ = n;
    snapOk_ = n > 0;
    snapShotWindow_ = shot;
    snapCarrier_ = ci;
    snapFrom_ = car.pos;
    snapCrowd_ = state_.crowd;
    snapT_ = frameTimeMs_;
    flight_ = false;
}

void PitchEngine::attackTrack(int prev) {
    const double now = frameTimeMs_;
    const int cur = owner_;
    // honest counters for the per-game graph: every pass that left a carrier is one attempt and ends kept or lost,
    // independent of whether the detailed matcher below could attribute it
    if (prev == 1 && cur == 0) {
        flight_ = snapOk_ && now - snapT_ <= 900.0;
        flightT_ = now;
        if (flight_) statPassOut_++;
    } else if (prev == 1 && cur == 2) {
        flight_ = false;
        snapOk_ = false;
        statTurnovers_++;
    } else if (prev == 0 && cur == 1 && flight_) {
        if (now - flightT_ <= 4000.0) { statPassKept_++; if (cfg_.meta) metaPassResult(true); if (cfg_.plan) planPassResult(true); if (cfg_.v7) v7PassResult(true); }
        if (now - flightT_ <= 4000.0 && state_.carrier >= 0 && state_.carrier != snapCarrier_) attackEvent(true, state_.carrier);
        flight_ = false;
    } else if (prev == 0 && cur == 2 && flight_) {
        if (now - flightT_ <= 4000.0) {
            statPassLost_++;
            statTurnovers_++;
            if (cfg_.meta) metaPassResult(false);
            if (cfg_.plan) planPassResult(false);
            if (cfg_.v7) v7PassResult(false);
            const size_t before = attLog_.size();
            attackEvent(false, -1);
            if (attLog_.size() == before) onlineLearn(false);      // a lost pass the detailed matcher could not attribute
        }
        flight_ = false;
    } else if (cur == 0 && flight_ && now - flightT_ > 4500.0) {
        flight_ = false;
    } else if (prev == 1 && cur == 1 && !flight_ && snapOk_ && state_.carrier >= 0 && state_.carrier != snapCarrier_ &&
               now - snapT_ <= 400.0) {
        const PlayerState& nc = state_.team[state_.carrier];
        if (distance(nc.pos, snapFrom_) >= 4.f && distance(state_.ball, snapFrom_) >= 3.f) attackEvent(true, state_.carrier);
    }
    if (cur == 2) lastCarrierIdx_ = -1;
    else if (cur == 1 && state_.carrier >= 0) lastCarrierIdx_ = state_.carrier;
}

void PitchEngine::attackEvent(bool kept, int recvIdx) {
    if (!snapOk_ || nSnap_ <= 0 || (int)attLog_.size() >= ATT_LOG_MAX) return;
    const SnapCand* best = nullptr;
    if (kept) {
        if (recvIdx < 0 || recvIdx >= TEAM_SIZE) return;
        const PlayerState& r = state_.team[recvIdx];
        float bd = 1e9f;
        for (int i = 0; i < nSnap_; i++) {
            if (snap_[i].id != r.id) continue;
            const float d = distance(snap_[i].target, r.pos);
            if (d < bd) { bd = d; best = &snap_[i]; }
        }
        if (!best || bd > 20.f) return;
    } else {
        const MetricVector2D disp = state_.ball - snapFrom_;
        const float d = disp.length();
        if (d < 4.5f) return;
        if (snapShotWindow_) {
            const MetricVector2D gv(state_.attackSign * PITCH_HALF_X - snapFrom_.x, -snapFrom_.y);
            const float gl = gv.length();
            if (gl > 1e-3f && disp.dot(gv) / (d * gl) > 0.97f) return;
        }
        // a lost pass travels roughly towards its target but is cut out early (short ratio) or deflected: match loosely, otherwise
        // failures are almost never recorded and the learner only ever sees completed passes
        float bc = 0.7f;
        for (int i = 0; i < nSnap_; i++) {
            const MetricVector2D v = snap_[i].target - snapFrom_;
            const float l = v.length();
            if (l < 1e-3f) continue;
            const float cs = disp.dot(v) / (d * l), ratio = d / l;
            if (cs > bc && ratio >= 0.2f && ratio <= 1.7f) { bc = cs; best = &snap_[i]; }
        }
        if (!best) return;
    }
    noteOutcome(kept);
    AttRec r;
    r.t = frameTimeMs_;
    r.type = best->type;
    r.xp = best->xpRaw;
    r.pressure = best->pressure;
    r.crowd = snapCrowd_;
    r.len = best->len;
    r.fwd = best->fwd;
    r.ax = clampf((state_.attackSign * snapFrom_.x + PITCH_HALF_X) / (2.f * PITCH_HALF_X), 0.f, 1.f);
    r.ay = clampf((snapFrom_.y + PITCH_HALF_Y) / (2.f * PITCH_HALF_Y), 0.f, 1.f);
    r.outcome = kept ? 1 : 0;
    attLog_.push_back(r);
    attAtt_++;
    if (kept) attOk_++;
    snapOk_ = false;
}

void PitchEngine::setShootConfig(bool autoShoot, int holdMs, bool live) {
    cfg_.autoShoot = autoShoot;
    cfg_.shootHoldMs = holdMs < 100 ? 100 : (holdMs > 1500 ? 1500 : holdMs);
    shotLive_ = live;
    if (!autoShoot) { shotPending_ = false; shotRun_ = 0; }
}

void PitchEngine::clearAttackLog() {
    attLog_.clear();
    attAtt_ = attOk_ = 0;
    shotsLive_ = shotsDry_ = 0;
    nSnap_ = 0; snapOk_ = false; snapShotWindow_ = false; snapCarrier_ = -1; snapT_ = flightT_ = -1e9; flight_ = false; lastCarrierIdx_ = -1;
}

std::string PitchEngine::exportAttackLog() const {
    std::string out;
    char buf[200];
    std::snprintf(buf, sizeof buf, "S %.4f %.4f %.4f\n", strat_.dKeep, strat_.dTempo, strat_.fwd);
    out += buf;
    std::snprintf(buf, sizeof buf, "X %d %d\n", shotsLive_, shotsDry_);
    out += buf;
    for (const AttRec& r : attLog_) {
        std::snprintf(buf, sizeof buf, "A %.0f %d %.4f %.4f %.4f %.3f %.3f %.4f %.4f %d\n", r.t, r.type, r.xp, r.pressure, r.crowd,
                      r.len, r.fwd, r.ax, r.ay, r.outcome);
        out += buf;
    }
    return out;
}

void PitchEngine::resetAttackWeights() {
    for (int k = 0; k < ATT_NF; k++) attW_[k] = ATT_PRIOR[k];
    attLearned_ = false;
    attLearnedN_ = 0;
    strat_ = Strategy();
    sh_.clear();
    shotXgOff_ = 0.f;
    shotLearned_ = false;
}

std::string PitchEngine::exportAttackWeights() const {
    std::string out;
    char buf[96];
    if (attLearned_) {
        out = "pa";
        for (int k = 0; k < ATT_NF; k++) {
            std::snprintf(buf, sizeof buf, " %.5f", attW_[k]);
            out += buf;
        }
        std::snprintf(buf, sizeof buf, "\npan %d\n", attLearnedN_);
        out += buf;
    }
    if (shotLearned_) {
        std::snprintf(buf, sizeof buf, "sx %.5f\n", shotXgOff_);
        out += buf;
    }
    if (!sh_.empty()) {
        std::snprintf(buf, sizeof buf, "st %.5f %.5f %.5f\n", strat_.dKeep, strat_.dTempo, strat_.fwd);
        out += buf;
        for (const StratRec& r : sh_) {
            std::snprintf(buf, sizeof buf, "sh %.4f %.4f %.4f %.2f\n", r.k, r.t, r.f, r.r);
            out += buf;
        }
    }
    return out;
}

bool PitchEngine::importAttackWeights(const std::string& str) {
    float w[ATT_NF];
    int n = 0;
    bool foundPa = false, foundSt = false, foundSx = false;
    float sx = 0.f;
    Strategy st;
    std::vector<StratRec> hist;
    size_t pos = 0;
    while (pos < str.size()) {
        size_t e = str.find('\n', pos);
        if (e == std::string::npos) e = str.size();
        const std::string line = str.substr(pos, e - pos);
        pos = e + 1;
        if (line.compare(0, 3, "pa ") == 0) {
            const char* c = line.c_str() + 3;
            bool ok = true;
            for (int k = 0; k < ATT_NF && ok; k++) {
                char* end = nullptr;
                w[k] = std::strtof(c, &end);
                if (end == c || !finite(w[k])) ok = false;
                c = end;
            }
            foundPa = ok;
        } else if (line.compare(0, 4, "pan ") == 0) {
            n = std::atoi(line.c_str() + 4);
        } else if (line.compare(0, 3, "st ") == 0) {
            float a, b, c2;
            if (std::sscanf(line.c_str(), "st %f %f %f", &a, &b, &c2) == 3 && finite(a) && finite(b) && finite(c2)) {
                st.dKeep = clampf(a, -0.4f, 0.3f);
                st.dTempo = clampf(b, -0.1f, 0.1f);
                st.fwd = clampf(c2, 0.75f, 1.35f);
                foundSt = true;
            }
        } else if (line.compare(0, 3, "sx ") == 0) {
            float v;
            if (std::sscanf(line.c_str(), "sx %f", &v) == 1 && finite(v)) { sx = clampf(v, -0.04f, 0.08f); foundSx = true; }
        } else if (line.compare(0, 3, "sh ") == 0 && hist.size() < 200) {
            StratRec r;
            if (std::sscanf(line.c_str(), "sh %f %f %f %f", &r.k, &r.t, &r.f, &r.r) == 4 && finite(r.k) && finite(r.t) && finite(r.f) && finite(r.r))
                hist.push_back(r);
        }
    }
    resetAttackWeights();
    if (foundPa) {
        // a model saved by an older build that learned from completed passes only (bias high, lane weight at its floor): drop it
        const bool degenerate = w[0] > 0.9f && w[1] <= 0.35f;
        if (!degenerate) {
            for (int k = 0; k < ATT_NF; k++) attW_[k] = clampf(w[k], -3.f, 3.f);
            sanitizeAttackWeights(attW_, ATT_NF);
            attLearnedN_ = std::max(0, n);
            attLearned_ = true;
        }
    }
    if (foundSt) {
        strat_ = st;
        sh_ = hist;
    }
    if (foundSx) {
        shotXgOff_ = sx;
        shotLearned_ = true;
    }
    return foundPa || foundSt || foundSx;
}

std::string PitchEngine::learnAttack(const std::string& logs) {
    struct S { float f[ATT_NF]; int y; };
    std::vector<S> samples;
    size_t pos = 0;
    while (pos < logs.size()) {
        size_t e = logs.find('\n', pos);
        if (e == std::string::npos) e = logs.size();
        if (e - pos > 2 && logs[pos] == 'A' && logs[pos + 1] == ' ') {
            double t = 0;
            int type = 0, y = 0;
            float xp, pr, cr, len, fwd, ax, ay;
            const std::string line = logs.substr(pos, e - pos);
            if (std::sscanf(line.c_str(), "A %lf %d %f %f %f %f %f %f %f %d", &t, &type, &xp, &pr, &cr, &len, &fwd, &ax, &ay, &y) == 10 &&
                (y == 0 || y == 1) && finite(xp) && finite(pr) && finite(cr) && finite(len) && finite(fwd)) {
                S s;
                attackFeatures(xp, pr, cr, len, fwd, s.f);
                s.y = y;
                samples.push_back(s);
            }
        }
        pos = e + 1;
    }
    const int n = (int)samples.size();
    char buf[400];
    std::string out;
    int kept = 0;
    for (const S& s : samples) kept += s.y;
    if (n < 12) {
        std::snprintf(buf, sizeof buf, "Attack passes: %d logged (%d kept). Need 12+ to learn: attack model unchanged.\n", n, kept);
        return std::string(buf) + learnStrategy(logs) + learnShooting(logs);
    }
    // success-only (or failure-only) data teaches nothing about what separates a safe pass from a risky one and pushes every
    // probability to one end: refuse to update unless both outcomes are well represented
    const int failed = n - kept;
    if (kept >= failed && failed < std::max(4, (int)(0.12f * (float)n))) {
        std::snprintf(buf, sizeof buf,
                      "Attack passes: %d logged (%d kept, %d lost). Too few lost passes in this log (need at least 4 and 12%%) to learn what makes a pass risky: pass model unchanged.\n",
                      n, kept, failed);
        return std::string(buf) + learnStrategy(logs) + learnShooting(logs);
    }
    auto metrics = [&](float& meanP, float& brier) {
        meanP = 0.f; brier = 0.f;
        for (const S& s : samples) {
            const float p = attackProb(s.f);
            meanP += p;
            brier += (p - (float)s.y) * (p - (float)s.y);
        }
        meanP /= n; brier /= n;
    };
    float p0, b0;
    metrics(p0, b0);
    const float lr = 0.05f / (1.f + 0.002f * (float)n);
    for (int epoch = 0; epoch < 8; epoch++) {
        for (const S& s : samples) {
            const float err = (float)s.y - attackProb(s.f);
            for (int k = 0; k < ATT_NF; k++) {
                attW_[k] += lr * err * s.f[k] - 0.003f * (attW_[k] - ATT_PRIOR[k]);
                attW_[k] = clampf(attW_[k], -3.f, 3.f);
            }
            sanitizeAttackWeights(attW_, ATT_NF);
        }
    }
    attLearned_ = true;
    attLearnedN_ += n;
    float p1, b1;
    metrics(p1, b1);
    std::snprintf(buf, sizeof buf,
                  "Attack passes: %d logged, %d kept (%.0f%%). Pass model: predicted %.0f%% kept before, %.0f%% after (error %.3f -> %.3f).\n",
                  n, kept, 100.f * kept / n, 100.f * p0, 100.f * p1, b0, b1);
    out += buf;
    std::snprintf(buf, sizeof buf, "Attack weights: xP x%.2f, pressure %+.2f, crowd %+.2f, length %+.2f, forward gain %+.2f, bias %+.2f.\n",
                  attW_[1], attW_[2], attW_[3], attW_[4], attW_[5], attW_[0]);
    out += buf;
    return out + learnStrategy(logs) + learnShooting(logs);
}

std::string PitchEngine::learnStrategy(const std::string& logs) {
    int added = 0;
    uint32_t hh = 0x9E3779B9u;
    bool haveS = false;
    StratRec cur;
    size_t pos = 0;
    while (pos < logs.size()) {
        size_t e = logs.find('\n', pos);
        if (e == std::string::npos) e = logs.size();
        const std::string line = logs.substr(pos, e - pos);
        pos = e + 1;
        if (line.size() > 2 && line[0] == 'S' && line[1] == ' ') {
            haveS = std::sscanf(line.c_str(), "S %f %f %f", &cur.k, &cur.t, &cur.f) == 3 && finite(cur.k) && finite(cur.t) && finite(cur.f);
        } else if (line.size() > 2 && line[0] == 'R' && line[1] == ' ') {
            int gf = 0, ga = 0;
            if (haveS && std::sscanf(line.c_str(), "R %d %d", &gf, &ga) == 2) {
                cur.r = (float)std::max(-4, std::min(4, gf - ga));
                sh_.push_back(cur);
                added++;
                hh = hh * 31u + (uint32_t)(int32_t)(cur.k * 997.f + cur.t * 9973.f + cur.f * 99991.f + cur.r * 7919.f);
            }
            haveS = false;
        }
    }
    while (sh_.size() > 60) sh_.erase(sh_.begin());
    const int n = (int)sh_.size();
    char buf[400];
    if (added == 0) return "Results: no scored match with a strategy line in this batch (matches from an older version): style unchanged.\n";

    float mean = 0.f;
    for (const StratRec& r : sh_) mean += r.r;
    mean /= n;

    const float U[3] = {0.3f, 0.1f, 0.3f};
    float x[3] = {strat_.dKeep / U[0], strat_.dTempo / U[1], (strat_.fwd - 1.f) / U[2]};
    float beta[3] = {0, 0, 0};
    float bn = 0.f;
    float tstat[3] = {0, 0, 0}, gate[3] = {0, 0, 0};
    if (n >= 12) {
        double m[3] = {0, 0, 0}, ym = 0;
        for (const StratRec& r : sh_) { m[0] += r.k / U[0]; m[1] += r.t / U[1]; m[2] += (r.f - 1.f) / U[2]; ym += r.r; }
        for (double& v : m) v /= n;
        ym /= n;
        double A[3][4] = {{0}};
        for (const StratRec& r : sh_) {
            const double xs[3] = {r.k / U[0] - m[0], r.t / U[1] - m[1], (r.f - 1.f) / U[2] - m[2]};
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) A[i][j] += xs[i] * xs[j];
                A[i][3] += xs[i] * (r.r - ym);
            }
        }
        for (int i = 0; i < 3; i++) A[i][i] += 0.05;
        for (int i = 0; i < 3; i++) {
            for (int k = i + 1; k < 3; k++) {
                const double f = A[k][i] / A[i][i];
                for (int j = i; j < 4; j++) A[k][j] -= f * A[i][j];
            }
        }
        double bsol[3];
        for (int i = 2; i >= 0; i--) {
            double v = A[i][3];
            for (int j = i + 1; j < 3; j++) v -= A[i][j] * bsol[j];
            bsol[i] = v / A[i][i];
        }
        for (int i = 0; i < 3; i++) { beta[i] = (float)bsol[i]; bn += beta[i] * beta[i]; }
        bn = std::sqrt(bn);
        double rss = 0;
        double sxx[3] = {0, 0, 0};
        for (const StratRec& r : sh_) {
            const double xs[3] = {r.k / U[0] - m[0], r.t / U[1] - m[1], (r.f - 1.f) / U[2] - m[2]};
            double pred = 0;
            for (int i = 0; i < 3; i++) { pred += bsol[i] * xs[i]; sxx[i] += xs[i] * xs[i]; }
            const double res = (r.r - ym) - pred;
            rss += res * res;
        }
        const double sigma = std::sqrt(rss / std::max(1, n - 4));
        for (int i = 0; i < 3; i++) {
            const double se = sigma / std::sqrt(std::max(sxx[i], 1e-6));
            tstat[i] = (float)(std::fabs(bsol[i]) / std::max(se, 1e-6));
            gate[i] = clampf((tstat[i] - 1.5f) / 1.5f, 0.f, 1.f);
        }
        const float eta = 0.35f / std::sqrt(1.f + n / 20.f);
        for (int i = 0; i < 3; i++) x[i] = x[i] * 0.85f + eta * clampf(beta[i], -1.f, 1.f) * gate[i];
    }
    uint32_t seed = hh ^ ((uint32_t)n * 2654435761u);
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return ((seed >> 8) & 0xFFFF) / 65535.f * 2.f - 1.f; };
    const float maxGate = std::max(gate[0], std::max(gate[1], gate[2]));
    const float amp = n < 12 ? 0.30f : std::max(0.10f, 0.25f / std::sqrt(1.f + n / 20.f));
    for (int i = 0; i < 3; i++) x[i] += amp * rnd();
    const Strategy before = strat_;
    strat_.dKeep = clampf(x[0] * U[0], -0.4f, 0.3f);
    strat_.dTempo = clampf(x[1] * U[1], -0.1f, 0.1f);
    strat_.fwd = clampf(1.f + x[2] * U[2], 0.75f, 1.35f);

    std::snprintf(buf, sizeof buf, "Results: %d scored match(es) in memory, average goal difference %+.2f.\n", n, mean);
    std::string out = buf;
    if (n < 12) {
        std::snprintf(buf, sizeof buf, "Style: still collecting results (%d of 12 needed): trying small variations, not drawing conclusions yet.\n", n);
    } else if (maxGate <= 0.f) {
        std::snprintf(buf, sizeof buf, "Style: results too noisy to show any reliable effect (signal %.1f / %.1f / %.1f, needs 1.5+): style held where it is.\n",
                      tstat[0], tstat[1], tstat[2]);
    } else {
        std::snprintf(buf, sizeof buf, "Style: results point towards keep-ball %+.2f, possession target %+.2f, forward %+.2f (confidence %.0f%% / %.0f%% / %.0f%%).\n",
                      bn > 1e-6f ? beta[0] / bn : 0.f, bn > 1e-6f ? beta[1] / bn : 0.f, bn > 1e-6f ? beta[2] / bn : 0.f,
                      100.f * gate[0], 100.f * gate[1], 100.f * gate[2]);
    }
    out += buf;
    std::snprintf(buf, sizeof buf, "Style for the next matches: keep-ball %+.2f -> %+.2f, possession target %+.2f -> %+.2f, forward x%.2f -> x%.2f.\n",
                  before.dKeep, strat_.dKeep, before.dTempo, strat_.dTempo, before.fwd, strat_.fwd);
    out += buf;
    return out;
}

std::string PitchEngine::learnShooting(const std::string& logs) {
    int shots = 0, goals = 0, matches = 0;
    int curShots = -1;
    size_t pos = 0;
    while (pos < logs.size()) {
        size_t e = logs.find('\n', pos);
        if (e == std::string::npos) e = logs.size();
        const std::string line = logs.substr(pos, e - pos);
        pos = e + 1;
        if (line.size() > 2 && line[0] == 'X' && line[1] == ' ') {
            int l = 0, d = 0;
            curShots = std::sscanf(line.c_str(), "X %d %d", &l, &d) >= 1 ? l : -1;
        } else if (line.size() > 2 && line[0] == 'R' && line[1] == ' ') {
            int gf = 0, ga = 0;
            if (curShots >= 0 && std::sscanf(line.c_str(), "R %d %d", &gf, &ga) == 2) {
                shots += curShots;
                goals += gf;
                matches++;
            }
            curShots = -1;
        }
    }
    char buf[300];
    if (matches == 0) return "";
    if (shots < 6) {
        std::snprintf(buf, sizeof buf, "Shooting: %d auto shot(s) in %d scored match(es): need 6+ before the shot-quality bar is tuned.\n", shots, matches);
        return buf;
    }
    const float conv = (float)goals / (float)shots;
    // Beta-binomial shrinkage: a Beta(2,6) prior encodes a conservative 25% baseline.
    // Small samples therefore cannot swing the shot gate as violently as raw conversion.
    const float posterior = (float)(goals + 2) / (float)(shots + 8);
    const float confidence = (float)shots / (float)(shots + 20);
    const float before = shotXgOff_;
    const float target = clampf(0.25f - posterior, -0.20f, 0.20f);
    const float step = clampf(0.018f * target * confidence, -0.004f, 0.004f);
    shotXgOff_ = clampf(shotXgOff_ + step, -0.04f, 0.08f);
    shotLearned_ = true;
    // Wilson 95% interval communicates uncertainty rather than pretending the observed rate is exact.
    const float n = (float)shots, z = 1.96f;
    const float den = 1.f + z*z/n;
    const float centre = (conv + z*z/(2.f*n))/den;
    const float half = z * std::sqrt((conv*(1.f-conv)/n) + z*z/(4.f*n*n))/den;
    std::snprintf(buf, sizeof buf, "Finishing calibration: %d auto shots, %d goals; raw conversion %.1f%% (Wilson 95%% %.1f-%.1f%%), shrinkage estimate %.1f%%. Confidence-weighted shot gate %.3f -> %.3f (%s).\n",
                  shots, goals, 100.f*conv, 100.f*std::max(0.f, centre-half), 100.f*std::min(1.f, centre+half), 100.f*posterior,
                  cfg_.cuePrimeXg + before, cfg_.cuePrimeXg + shotXgOff_,
                  shotXgOff_ > before ? "demanding better chances" : (shotXgOff_ < before ? "allowing somewhat earlier attempts" : "unchanged"));
    return buf;
}

std::string PitchEngine::attackCalibration(const std::string& logs) const {
    constexpr int NB = 5;
    const float edge[NB + 1] = {0.f, 0.40f, 0.55f, 0.70f, 0.85f, 1.01f};
    int cnt[NB] = {0}, okc[NB] = {0};
    double sumP[NB] = {0}, sumRaw[NB] = {0};
    int n = 0, kept = 0;
    double brierP = 0, brierRaw = 0;
    size_t pos = 0;
    while (pos < logs.size()) {
        size_t e = logs.find('\n', pos);
        if (e == std::string::npos) e = logs.size();
        if (e - pos > 2 && logs[pos] == 'A' && logs[pos + 1] == ' ') {
            double t = 0;
            int type = 0, y = 0;
            float xp, pr, cr, len, fwd, ax, ay;
            const std::string line = logs.substr(pos, e - pos);
            if (std::sscanf(line.c_str(), "A %lf %d %f %f %f %f %f %f %f %d", &t, &type, &xp, &pr, &cr, &len, &fwd, &ax, &ay, &y) == 10 &&
                (y == 0 || y == 1) && finite(xp) && finite(pr) && finite(cr) && finite(len) && finite(fwd)) {
                float f[ATT_NF];
                attackFeatures(xp, pr, cr, len, fwd, f);
                const float p = clampf(attackProb(f), 0.01f, 0.99f);
                const float raw = clampf(xp, 0.01f, 0.99f);
                int b = 0;
                while (b < NB - 1 && p >= edge[b + 1]) b++;
                cnt[b]++;
                okc[b] += y;
                sumP[b] += p;
                sumRaw[b] += raw;
                brierP += (p - y) * (p - y);
                brierRaw += (raw - y) * (raw - y);
                n++;
                kept += y;
            }
        }
        pos = e + 1;
    }
    char buf[240];
    std::string out;
    if (n == 0) return "Pass accuracy: no logged attack passes yet. Play a few dry-run matches first.\n";
    std::snprintf(buf, sizeof buf, "Pass accuracy check: %d passes, %.0f%% actually kept.\n", n, 100.0 * kept / n);
    out += buf;
    out += "Bot says -> really happened:\n";
    double gapSum = 0;
    int gapN = 0;
    for (int b = 0; b < NB; b++) {
        if (cnt[b] == 0) continue;
        const double pm = sumP[b] / cnt[b], ar = (double)okc[b] / cnt[b];
        std::snprintf(buf, sizeof buf, "  %2.0f-%3.0f%%: said %2.0f%%, was %2.0f%%  (%d passes)\n", 100.0 * edge[b],
                      100.0 * std::min(edge[b + 1], 1.f), 100.0 * pm, 100.0 * ar, cnt[b]);
        out += buf;
        gapSum += std::fabs(pm - ar) * cnt[b];
        gapN += cnt[b];
    }
    const double gap = gapN ? gapSum / gapN : 0.0;
    std::snprintf(buf, sizeof buf, "Average gap %.0f points. Error score now %.3f (built-in estimate alone %.3f, lower is better).\n", 100.0 * gap,
                  brierP / n, brierRaw / n);
    out += buf;
    if (n < 40) out += "Verdict: too few passes to judge (need 40+).\n";
    else if (gap <= 0.07) out += "Verdict: the estimate is trustworthy.\n";
    else if (gap <= 0.13) out += "Verdict: usable but off. Submit & Learn should shrink the gap.\n";
    else out += "Verdict: the estimate is unreliable. Send me this report.\n";
    return out;
}

// =====================================================================================================================
// Upgrade pack
// =====================================================================================================================

const char* PitchEngine::roleName(int r) {
    switch (r) {
        case ROLE_GK: return "GK";
        case ROLE_CB: return "CB";
        case ROLE_FB: return "FB";
        case ROLE_DM: return "DM";
        case ROLE_CM: return "CM";
        case ROLE_AM: return "AM";
        case ROLE_WG: return "WG";
        case ROLE_ST: return "ST";
        default: return "?";
    }
}

// Role inference: rank the outfield players by distance from our own goal line (smoothed over frames so a single
// forward run does not flip a defender into a midfielder), split the ranks into back line / midfield / front line,
// then use the width to tell centre-backs from full-backs and strikers from wingers.
void PitchEngine::updateRoles() {
    const float s = (float)state_.attackSign;
    int idx[TEAM_SIZE];
    float depth[TEAM_SIZE];
    int m = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        role_[i] = ROLE_UNK;
        const PlayerState& p = state_.team[i];
        if (!p.active || p.missed > cfg_.maxCoastFrames) continue;
        if (isKeeperIdx(i)) { role_[i] = ROLE_GK; continue; }
        idx[m] = i;
        depth[m] = PITCH_HALF_X + s * p.pos.x;
        m++;
    }
    if (m < 6) return;
    int order[TEAM_SIZE];
    for (int a = 0; a < m; a++) order[a] = a;
    for (int a = 0; a < m; a++)
        for (int b = a + 1; b < m; b++)
            if (depth[order[b]] < depth[order[a]]) std::swap(order[a], order[b]);
    for (int r = 0; r < m; r++) {
        const int i = idx[order[r]];
        if (!roleInit_) rankEma_[i] = (float)r;
        else rankEma_[i] += ((float)r - rankEma_[i]) * 0.10f;
    }
    roleInit_ = true;
    const int defEnd = (int)std::lround(m * 0.40f);
    const int midEnd = (int)std::lround(m * 0.70f);
    for (int r = 0; r < m; r++) {
        const int i = idx[order[r]];
        const float re = rankEma_[i];
        const float ay = std::fabs(state_.team[i].pos.y);
        if (re < (float)defEnd - 0.5f) role_[i] = ay > 13.f ? ROLE_FB : ROLE_CB;
        else if (re < (float)midEnd - 0.5f) {
            if (re < (float)defEnd + 0.5f) role_[i] = ROLE_DM;
            else if (re > (float)midEnd - 1.5f) role_[i] = ROLE_AM;
            else role_[i] = ROLE_CM;
        } else role_[i] = ay > 15.f ? ROLE_WG : ROLE_ST;
    }
}

// Opponent back line, live: the four opposition outfielders closest to their own goal (the keeper excluded).
// depth = metres of that line from their goal line; velocity from the smoothed change; confidence from how many of
// the four are really seen and how tight the line is (a ragged "line" is a weak signal).
void PitchEngine::updateOppLine(double dtSec) {
    const float s = (float)state_.attackSign;
    float d[TEAM_SIZE], y[TEAM_SIZE];
    int m = 0, seen = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const PlayerState& o = state_.opp[i];
        if (!o.active || o.missed > cfg_.maxCoastFrames) continue;
        d[m] = PITCH_HALF_X - s * o.pos.x;          // distance from their goal line
        y[m] = o.pos.y;
        if (o.visible) seen++;
        m++;
    }
    if (m < 7) { oppLineConf_ *= 0.95f; return; }
    int order[TEAM_SIZE];
    for (int a = 0; a < m; a++) order[a] = a;
    for (int a = 0; a < m; a++)
        for (int b = a + 1; b < m; b++)
            if (d[order[b]] < d[order[a]]) std::swap(order[a], order[b]);
    // order[0] is the keeper (deepest); the line is the next four
    float sum = 0.f, ymin = 1e9f, ymax = -1e9f, dmin = 1e9f, dmax = -1e9f;
    const int n = std::min(4, m - 1);
    for (int k = 1; k <= n; k++) {
        const int j = order[k];
        sum += d[j];
        ymin = std::min(ymin, y[j]); ymax = std::max(ymax, y[j]);
        dmin = std::min(dmin, d[j]); dmax = std::max(dmax, d[j]);
    }
    const float depth = sum / (float)n;
    const float tight = clampf(1.f - (dmax - dmin) / 14.f, 0.f, 1.f);
    const float vis = clampf((float)seen / 9.f, 0.f, 1.f);
    const float conf = tight * vis;
    const float a = cfg_.oppLineAlpha * (0.4f + 0.6f * conf);
    if (!oppLineInit_) { oppLineDepth_ = depth; oppLineWidth_ = ymax - ymin; oppLineConf_ = conf; oppLineInit_ = true; return; }
    const float prev = oppLineDepth_;
    oppLineDepth_ += (depth - oppLineDepth_) * a;
    oppLineWidth_ += ((ymax - ymin) - oppLineWidth_) * a;
    oppLineConf_ += (conf - oppLineConf_) * 0.10f;
    const float v = (float)((oppLineDepth_ - prev) / std::max(dtSec, 0.02));
    oppLineVel_ += (v - oppLineVel_) * 0.08f;
}

// Rolling-friction prediction of the loose ball. The radar is 2-D, so bounce and spin are not observable; this predicts
// the ground path and flags a ball that will cross a touch line / end line.
bool PitchEngine::predictBall(float t, MetricVector2D* pos, MetricVector2D* vel, bool* leavesPitch) const {
    if (!ballUsable()) return false;
    const float k = std::max(cfg_.ballFric, 0.05f);
    const float tt = std::max(t, 0.f);
    const float e = std::exp(-k * tt);
    MetricVector2D p = state_.ball + state_.ballVel * ((1.f - e) / k);
    const bool out = std::fabs(p.x) > PITCH_HALF_X || std::fabs(p.y) > PITCH_HALF_Y;
    p.x = clampf(p.x, -PITCH_HALF_X, PITCH_HALF_X);
    p.y = clampf(p.y, -PITCH_HALF_Y, PITCH_HALF_Y);
    if (pos) *pos = p;
    if (vel) *vel = state_.ballVel * e;
    if (leavesPitch) *leavesPitch = out;
    return true;
}

bool PitchEngine::counterPressActive() const {
    return cfg_.upgrades && cfg_.counterPress && state_.timeMs - lostBallMs_ < (double)cfg_.counterPressMs * (plusModeNow() == 1 ? 1.5 : 1.0) && planCounterPressOk() && v8PressGate();
}

// Extra utility for a pass candidate: (1) what the receiver can do next (shoot or progress again), (2) back-line height,
// (3) role of the receiver. Everything is a small, bounded term on top of the existing xP * dXT utility.
float PitchEngine::adaptiveAttackBonus(const PlayerState& car, const PassCandidate& c) const {
    // All rewards are deliberately small and bounded: existing safety / xP filters remain authoritative.
    const float s = (float)state_.attackSign;
    const MetricVector2D from = car.pos;
    const MetricVector2D to = c.target;
    const bool pass = c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED;
    const float prog = s * (to.x - from.x);
    const float targetThreat = xTGrid::shotValue(s * to.x, to.y);
    const float currentThreat = xTGrid::shotValue(s * from.x, from.y);
    float b = 0.f;

    // 1) Bait-and-release: reward a lateral/diagonal release only when it also progresses or improves threat.
    const float lateral = std::fabs(to.y - from.y);
    if (pass && c.xP >= 0.68f && lateral >= 10.f && (prog > 1.5f || targetThreat > currentThreat + 0.04f))
        b += 0.025f * c.xP;

    // 2) Patient possession: retain safe short options while under pressure, but don't reward sterile backward play.
    if (c.type == ACT_PASS && c.xP >= 0.78f && c.length >= 3.f && c.length <= 15.f && c.pressure > 0.25f && prog >= -1.f)
        b += 0.025f * c.xP;

    // 3) Opponent pattern adaptation: use the learned line estimate already maintained by the engine.
    if (pass && oppLineConf_ > 0.35f && c.xP >= 0.62f) {
        const float lineAx = PITCH_HALF_X - oppLineDepth_;
        if (s * to.x > lineAx && prog > 2.f) b += 0.02f * oppLineConf_ * c.xP;
    }

    // 4) Controlled unpredictability: a useful switch/diagonal option gets a small bonus, never overriding safety.
    if (pass && c.xP >= 0.72f && lateral >= 7.f && c.length <= cfg_.safePassLen && c.clearance >= cfg_.clearHalf)
        b += 0.012f;

    // 5) Overload / third-man support: reward targets with nearby support, especially ahead of the carrier.
    if (pass && c.xP >= 0.65f) {
        int support = 0, pressureCount = 0;
        for (int i = 0; i < TEAM_SIZE; ++i) {
            const PlayerState& q = state_.team[i];
            if (!q.active || q.id == car.id || (c.targetId >= 1 && c.targetId <= TEAM_SIZE && q.id == c.targetId)) continue;
            if (distance(q.future, to) <= 9.f && s * (q.future.x - from.x) > 0.f) ++support;
        }
        for (int i = 0; i < TEAM_SIZE; ++i) {
            const PlayerState& o = state_.opp[i];
            if (o.active && distance(o.future, to) <= 8.f) ++pressureCount;
        }
        if (support >= 1 && support > pressureCount) b += 0.018f * c.xP;
    }

    // 6) Cut-back / box-entry creation: favour safe passes into central, advanced shooting zones.
    if (pass && c.xP >= 0.68f && s * to.x > 28.f && std::fabs(to.y) < 18.f && targetThreat > currentThreat + 0.025f)
        b += 0.035f * c.xP;

    // 7) Tempo switch: when forward space is available, reward progressive actions; under pressure favour safe exits.
    if (pass && prog > 5.f && c.xP >= 0.68f) b += 0.018f * c.xP;
    if (pass && c.pressure > 0.45f && c.xP >= 0.82f && prog >= -2.f) b += 0.015f * c.xP;

    // 8) Chance quality: prefer meaningful threat gain, not merely any forward pass.
    if (pass && targetThreat > currentThreat + 0.03f && c.xP >= 0.60f)
        b += std::min(0.04f, (targetThreat - currentThreat) * 0.12f) * c.xP;

    // 9) Press resistance: a short, safe outlet under pressure helps preserve possession.
    if (c.type == ACT_PASS && c.pressure > 0.35f && c.xP >= 0.84f && c.length <= 10.f)
        b += 0.025f * c.xP;

    // 10) Risk-aware match-state proxy: never encourage a risky long ball over a safe pass.
    // Score-aware risk is handled by the existing urgency/clutch systems; this term protects baseline safety.
    if (pass && c.length > 20.f && c.xP < 0.76f) b -= 0.025f;

    return std::max(-0.04f, std::min(0.12f, b));
}

// coordinated decision layer. This is a bounded correction to candidate utility,
// not a replacement for the engine's existing legality, offside, interception or pass-safety filters.
float PitchEngine::coordinatedRiskAdjustment(const PlayerState& car, const PassCandidate& c) const {
    const bool pass = c.type == ACT_PASS || c.type == ACT_THROUGH || c.type == ACT_LOFTED;
    const float s = (float)state_.attackSign;
    float adj = 0.f;

    // A stale / occluded receiver is less trustworthy. The existing pass-probability gate remains authoritative.
    if (pass && c.targetId >= 1 && c.targetId <= TEAM_SIZE) {
        const PlayerState& r = state_.team[c.targetId - 1];
        const float rel = receiverReliability(r);
        adj -= (1.f - rel) * (c.type == ACT_THROUGH ? 0.10f : 0.07f);
    }

    // Estimate transition cover before rewarding an aggressive pass out of our own half.
    // Only active, reasonably fresh teammates behind the carrier count as cover.
    if (pass && s * car.pos.x < -4.f && s * (c.target.x - car.pos.x) > 5.f) {
        int cover = 0;
        for (int i = 0; i < TEAM_SIZE; ++i) {
            const PlayerState& q = state_.team[i];
            if (!q.active || q.id == car.id || q.missed > 5) continue;
            if (s * (q.future.x - car.pos.x) < -5.f && distance(q.future, car.pos) < 32.f) ++cover;
        }
        if (cover == 0 && c.xP < 0.88f) adj -= 0.035f;
        else if (cover >= 2 && c.xP >= 0.72f) adj += 0.012f;
    }

    // Under intense pressure, prefer a genuinely safe outlet over a low-confidence forward gamble.
    if (pass && c.pressure > 0.55f) {
        if (c.xP >= 0.84f && c.length <= 12.f && c.deltaXT >= -1.f) adj += 0.018f;
        else if (c.xP < 0.68f) adj -= 0.025f;
    }

    // Penalise over-ambitious balls when a simpler option is available; never penalise a clearly safe release.
    if (pass && c.length > 22.f && c.xP < 0.78f) adj -= 0.025f;

    // The bounded adjustment cannot overwhelm the main xP / threat / safety terms.
    return std::max(-0.12f, std::min(0.035f, adj));
}

float PitchEngine::upgradeBonus(const PlayerState& car, const PassCandidate& c) const {
    const float s = (float)state_.attackSign;
    const MetricVector2D from = car.pos;
    const MetricVector2D R = c.target;
    float bonus = 0.f;
    const int ri = (c.targetId >= 1 && c.targetId <= TEAM_SIZE) ? c.targetId - 1 : -1;

    if (cfg_.lookahead && c.xP >= cfg_.lookMinXp) {
        // second touch: the better of "shoot from there" and "pass on to a team-mate further up the pitch"
        float onward = 0.85f * xTGrid::shotValue(s * R.x, R.y);
        const float here = grid_.valueAt(s * R.x, R.y);
        for (int k = 0; k < TEAM_SIZE; k++) {
            const PlayerState& q = state_.team[k];
            if (!q.active || k == state_.carrier || k == ri || isKeeperIdx(k)) continue;
            const MetricVector2D Q = q.future;
            const float l2 = distance(R, Q);
            if (l2 < 3.f || l2 > 26.f || s * (Q.x - R.x) < 1.f) continue;
            const float gain = grid_.valueAt(s * Q.x, Q.y) - here;
            if (gain <= 0.f) continue;
            float clr = 0.f;
            const float xp2 = passProbability(R, Q, clr, false, false);
            onward = std::max(onward, xp2 * gain);
        }
        bonus += cfg_.wLook * c.xP * onward;
    }

    if (cfg_.oppLine && oppLineConf_ > 0.25f && (c.type == ACT_THROUGH || c.type == ACT_LOFTED) && c.xP >= 0.5f) {
        const float lineAx = PITCH_HALF_X - oppLineDepth_;          // attack-axis position of their back line
        if (s * R.x > lineAx + 1.f) {
            const float hi = clampf((oppLineDepth_ - (cfg_.highLineDepth - 6.f)) / 12.f, 0.f, 1.f);
            const float deep = clampf((cfg_.deepLineDepth - oppLineDepth_) / 8.f, 0.f, 1.f);
            const float stepping = clampf(oppLineVel_ / 1.5f, 0.f, 1.f);      // a line that is still stepping up is the easiest to beat
            bonus += cfg_.wLine * oppLineConf_ * c.xP * (hi * (1.f + 0.5f * stepping) - 0.6f * deep);
        }
    }

    if (cfg_.roles && ri >= 0) {
        const float prog = s * (R.x - from.x);
        const int r = role_[ri];
        if (prog > 3.f && (r == ROLE_ST || r == ROLE_WG || r == ROLE_AM)) bonus += cfg_.wRole * c.xP;
        else if (r == ROLE_CB && prog < 3.f && s * R.x < -10.f) bonus -= cfg_.wRole * c.xP;
    }
    return bonus;
}

// Loose ball: if the player we control wins the race to the predicted ball path (against every opponent and every
// team-mate) he goes to the intercept point; otherwise we stay out of it (the game's own switch handles team-mates).
GestureOutput PitchEngine::decideLoose() {
    GestureOutput none;
    none.status = ST_LOOSE;
    // second-ball window after a shot: switch to the nearest team-mate to the expected rebound and press
    if (cfg_.secondBall && state_.timeMs <= secondBallUntil_) {
        const double now = state_.timeMs;
        int best = -1;
        float bestD = 1e9f;
        const MetricVector2D target = (state_.ballVel.length() > 1.f) ? state_.ball : secondBallAt_;
        for (int i = 0; i < TEAM_SIZE; i++) {
            const PlayerState& q = state_.team[i];
            if (!q.active || isKeeperIdx(i)) continue;
            const float d = distance(q.future, target);
            if (d < bestD && d <= cfg_.secondBallRadius) { bestD = d; best = i; }
        }
        const int cidx = controlledIdx();
        if (best >= 0 && best != cidx && bestD + 1.0f < distance(ctrlPos_, target) && now - defSwitchAt_ > 400.0) {
            GestureOutput sg;
            sg.valid = true;
            sg.status = ST_LOOSE;
            sg.actionType = ACT_SWITCH;
            sg.durationMs = 70;
            defSwitchAt_ = now;
            looseNextAt_ = now + 300.0;
            return sg;
        }
    }
    if (cfg_.plus && cfg_.plusRebound) {
        const GestureOutput rb = plusReboundGesture();
        if (rb.valid) return rb;
    }
    if (!cfg_.upgrades || !cfg_.looseChase || !cfg_.ballFlight || !ballUsable() || !controlledSeen()) return none;
    const double now = state_.timeMs;
    if (now < looseNextAt_) return none;
    if (state_.ballVel.length() < 0.8f) return none;                 // a stationary ball is the game's to collect
    const float me = cfg_.chaseSpeed, opS = cfg_.defSpeed;
    const float kBody = cfg_.defBody;
    float tMe = 1e9f, tOpp = 1e9f, tMate = 1e9f;
    MetricVector2D meAt;
    for (int step = 1; step <= 8; step++) {
        const float t = 0.2f * (float)step;
        MetricVector2D bp;
        bool out = false;
        if (!predictBall(t, &bp, nullptr, &out) || out) break;
        if (tMe > 1e8f && distance(ctrlPos_, bp) <= me * t + kBody) { tMe = t; meAt = bp; }
        if (tOpp > 1e8f)
            for (int i = 0; i < TEAM_SIZE; i++) {
                const PlayerState& o = state_.opp[i];
                if (o.active && distance(o.future, bp) <= opS * (t + defReactEff()) + kBody) { tOpp = t; break; }
            }
        if (tMate > 1e8f)
            for (int i = 0; i < TEAM_SIZE; i++) {
                const PlayerState& q = state_.team[i];
                if (!q.active || !q.visible || distance(q.pos, ctrlPos_) < 0.8f) continue;   // skip the controlled player himself
                if (distance(q.future, bp) <= me * t + kBody) { tMate = t; break; }
            }
    }
    if (cfg_.plus && cfg_.plusRace) {   // somebody else wins the race to the loose ball: switch to him
        int bi = -1;
        MetricVector2D where;
        float lead = 0.f, cT = 0.f;
        const int cidx = controlledIdx();
        if (plusRaceAssign(&bi, &where, &lead, &cT) && bi >= 0 && bi != cidx &&
            cT - plusTta(state_.team[bi], where, 7.5f) > 0.35f && now - defSwitchAt_ > 700.0) {
            GestureOutput sg;
            sg.valid = true;
            sg.status = ST_LOOSE;
            sg.actionType = ACT_SWITCH;
            sg.durationMs = 70;
            defSwitchAt_ = now;
            looseNextAt_ = now + 450.0;
            return sg;
        }
    }
    if (tMe < 1e8f) {      // the controlled player can reach the predicted path at all
        if (distance(ctrlPos_, state_.ball) > cfg_.chaseMaxDist) return none;
        if (tOpp - tMe < cfg_.chaseMargin) return none;
        if (tMate < tMe + cfg_.chaseTeamMargin) return none;
        GestureOutput g = makeGesture(ACT_PRESS, ctrlPos_, meAt, 1.f, false);
        if (!g.valid) return none;
        g.status = ST_LOOSE;
        g.actionType = ACT_PRESS;
        g.durationMs = 200;
        g.dash = distance(ctrlPos_, meAt) > 3.f;
        looseNextAt_ = now + 220.0;
        chaseCount_++;
        return g;
    }
    return none;
}

// Health monitor: (1) the team centroid teleporting between frames means the radar read is garbage, so drop the
// trackers and re-acquire; (2) the same non-committal action for seconds while the ball goes nowhere means the
// decision loop is stuck, so clear its timers and force a release.
void PitchEngine::monitorHealth(double nowMs, double dtSec) {
    // (1) radar consistency: every dot of a real read has a dot of the previous frame close to it, whichever players happen
    // to be visible; a garbage read does not
    const int n = rawN_;
    if (n >= 5 && haveCentroid_ && prevRawN_ >= 5 && dtSec <= 0.5) {
        float dn[TEAM_SIZE];
        for (int i = 0; i < n; i++) {
            float best = 1e9f;
            for (int j = 0; j < prevRawN_; j++) best = std::min(best, distance(rawPts_[i], prevRawPts_[j]));
            dn[i] = best;
        }
        std::sort(dn, dn + n);
        const float med = dn[n / 2];
        if (med > cfg_.jumpM) {
            if (jumpRun_ == 0) jumpFirstMs_ = nowMs;
            jumpRun_++;
        } else if (jumpRun_ > 0 && nowMs - jumpFirstMs_ > 1500.0) jumpRun_ = 0;
    }
    if (n >= 5) {
        for (int i = 0; i < n; i++) prevRawPts_[i] = rawPts_[i];
        prevRawN_ = n;
        haveCentroid_ = true;
        if (jumpRun_ >= cfg_.jumpRuns && nowMs - jumpFirstMs_ <= 1500.0) {
            char b[120];
            std::snprintf(b, sizeof b, "t=%.0fms radar inconsistent (%d jumps): trackers re-acquired\n", nowMs, jumpRun_);
            healthLog_ += b;
            if (healthLog_.size() > 6000) healthLog_.erase(0, healthLog_.size() / 2);
            recoveries_++;
            jumpRun_ = 0;
            haveCentroid_ = false;
            softResetTracking();
            return;
        }
    }
    // (2) stuck decision loop
    const bool teamPlay = state_.status == ST_TEAM && !human_ && gesture_.valid;
    if (!teamPlay) { stuckSinceMs_ = -1; return; }
    const int act = gesture_.actionType;
    const bool passive = act == ACT_CARRY || act == ACT_NONE;
    if (!passive || stuckSinceMs_ < 0 || act != stuckAction_ || distance(state_.ball, stuckBall_) > cfg_.stuckBallM) {
        stuckSinceMs_ = passive ? nowMs : -1;
        stuckAction_ = act;
        stuckBall_ = state_.ball;
        return;
    }
    if (nowMs - stuckSinceMs_ >= (double)cfg_.stuckMs) {
        char b[140];
        std::snprintf(b, sizeof b, "t=%.0fms stuck %.1fs on action %d without moving the ball: decision timers cleared, release forced\n",
                      nowMs, (nowMs - stuckSinceMs_) / 1000.0, act);
        healthLog_ += b;
        if (healthLog_.size() > 6000) healthLog_.erase(0, healthLog_.size() / 2);
        recoveries_++;
        nextDecisionAt_ = cooldownUntil_ = 0;
        turnStartMs_ = -1;
        turnTargetId_ = -1;
        shotPending_ = false;
        shotArmed_ = true;
        holdStartAt_ = nowMs - 1e6;        // the hold cap is already exceeded: decide must release the ball
        holdLastAt_ = nowMs;
        pressStartAt_ = -1;
        restartUntilMs_ = nowMs + 1500.0;  // quick, safe passes for a moment
        stuckSinceMs_ = -1;
    }
}

// Drop the trackers and every per-frame decision state, but keep the configuration, the learned attack direction and the
// opponent model (the same rules as the stoppage reset).
void PitchEngine::softResetTracking() {
    const TacticalConfig keep = cfg_;
    const OppModel om = opp_;
    const bool sInit = signInit_;
    const double sEma = dirEma_;
    const int sSign = state_.attackSign, sCand = signCand_;
    const double now = frameTimeMs_;
    reset();
    cfg_ = keep;
    opp_ = om;
    signInit_ = sInit;
    dirEma_ = sEma;
    state_.attackSign = sSign;
    signCand_ = sCand;
    signLockMs_ = now - 1e6;
}

void PitchEngine::updateUpgrades(double dtSec) {
    if (!cfg_.upgrades) return;
    if (cfg_.roles) updateRoles();
    if (cfg_.oppLine) updateOppLine(dtSec);
    // match-summary accumulators
    const float s = (float)state_.attackSign;
    if (state_.status == ST_TEAM) {
        sumTeamSec_ += dtSec;
        const float ax = s * state_.ball.x;
        thirdSec_[ax < -PITCH_HALF_X / 3.f ? 0 : (ax > PITCH_HALF_X / 3.f ? 2 : 1)] += dtSec;
        const float by = state_.ball.y;
        flankSec_[by < -PITCH_HALF_Y / 3.f ? 0 : (by > PITCH_HALF_Y / 3.f ? 2 : 1)] += dtSec;
    } else if (state_.status == ST_OPP) sumOppSec_ += dtSec;
    else if (state_.status == ST_LOOSE) sumLooseSec_ += dtSec;
    if (cfg_.oppLine && oppLineConf_ > 0.4f) { lineDepthSum_ += oppLineDepth_ * dtSec; lineDepthW_ += dtSec; }
}

void PitchEngine::clearMatchStats() {
    sumTeamSec_ = sumOppSec_ = sumLooseSec_ = 0;
    for (int i = 0; i < 3; i++) thirdSec_[i] = flankSec_[i] = 0;
    lineDepthSum_ = lineDepthW_ = 0;
    turnoverCount_ = wonBackCount_ = 0;
    cpCount_ = recoveries_ = chaseCount_ = 0;
    healthLog_.clear();
    onlineKept_ = 0.75f;
    onlineAdj_ = 0.f;
    onlineN_ = 0;
    oppLineInit_ = false;
    oppLineConf_ = 0.f;
    oppLineVel_ = 0.f;
    roleInit_ = false;
    plus_.clear();
    meta_.clear();
    plan_.clear();
    statPassOut_ = statPassKept_ = statPassLost_ = statTurnovers_ = 0;
}

// Short plain-language read of the match, built only from what the engine really measured.
std::string PitchEngine::matchSummary() const {
    std::string out;
    char b[300];
    const double tot = sumTeamSec_ + sumOppSec_;
    if (tot < 20.0) return "Not enough live play was recorded for a summary.\n";
    const double poss = 100.0 * sumTeamSec_ / tot;
    std::snprintf(b, sizeof b, "Possession %.0f%% by radar estimate (%.0f s of %.0f s with a clear owner); the game's own figure is usually lower.\n", poss, sumTeamSec_, tot);
    out += b;
    if (sumTeamSec_ > 5.0) {
        const double fin = 100.0 * thirdSec_[2] / sumTeamSec_, own = 100.0 * thirdSec_[0] / sumTeamSec_;
        std::snprintf(b, sizeof b, "Territory: %.0f%% of our possession in the final third, %.0f%% in our own third.\n", fin, own);
        out += b;
        const double top = 100.0 * flankSec_[0] / sumTeamSec_, bot = 100.0 * flankSec_[2] / sumTeamSec_, mid = 100.0 * flankSec_[1] / sumTeamSec_;
        if (top > mid && top > bot && top > 40.0) out += "We played mostly down the top side of the radar.\n";
        else if (bot > mid && bot > top && bot > 40.0) out += "We played mostly down the bottom side of the radar.\n";
        else out += "Play was spread across the width of the pitch.\n";
    }
    if (attAtt_ > 0) {
        std::snprintf(b, sizeof b, "Passes: %d of %d kept (%.0f%%); lost passes are under-counted by the radar, so the game's own figure is lower.\n", attOk_, attAtt_, 100.0 * attOk_ / attAtt_);
        out += b;
        int lostLong = 0, lostAll = 0, nLong = 0;
        for (size_t i = 0; i < attLog_.size(); i++) {
            const bool lng = attLog_[i].type == ACT_THROUGH || attLog_[i].type == ACT_LOFTED;
            if (lng) nLong++;
            if (attLog_[i].outcome == 0) { lostAll++; if (lng) lostLong++; }
        }
        if (lostAll >= 3 && lostLong * 2 > lostAll) {
            std::snprintf(b, sizeof b, "Key issue: %d of the %d lost passes were through or lofted balls (out of %d played).\n", lostLong, lostAll, nLong);
            out += b;
        }
    }
    std::snprintf(b, sizeof b, "Shots taken: %d.\n", shotsLive_ + shotsDry_);
    out += b;
    if (lineDepthW_ > 20.0) {
        const double d = lineDepthSum_ / lineDepthW_;
        std::snprintf(b, sizeof b, "The opponent's back line averaged %.0f m from their goal (%s).\n", d,
                      d >= cfg_.highLineDepth ? "a high line: balls in behind were favoured" : (d <= cfg_.deepLineDepth ? "a deep block: patience was favoured" : "a mid block"));
        out += b;
    }
    std::snprintf(b, sizeof b, "Counter-press swarms: %d. Soft recoveries (stuck / radar glitch): %d. Live pass-safety adjustment: %+.2f.\n",
                  cpCount_, recoveries_, onlineAdj_);
    out += b;
    if (cfg_.v7 && v7_.frames > 30) {
        static const char* SN[3] = {"Control", "Vertical", "Chaos"};
        static const char* IN[5] = {"build-up", "progress", "direct play", "switching", "protecting"};
        std::snprintf(b, sizeof b, "v6.17: style %s; opponent mostly %s; team stamina estimate %.0f%%; %d pass results seen by the learning heads.\n",
                      SN[v7_.styleCur], IN[v7_.oiTop], 100.0 * v7_.stamAvg, v7_.passKept + v7_.passLost);
        out += b;
    }
    out += healthLog_;
    return out;
}

#include "pitch_plus.inc"
#include "pitch_meta.inc"
#include "pitch_plan.inc"
#include "pitch_v617.inc"
#include "pitch_v618.inc"

}  // namespace pe
