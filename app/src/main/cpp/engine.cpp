#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>

namespace pb {

namespace {

const float ASPECT = 0.65f;
const float GOAL_Y = 0.325f;

const float PRIOR_WP[NF] = {0.2f, 2.2f, 1.4f, 0.6f, 0.8f, 0.8f, -1.6f, -1.2f};
const float PRIOR_WT[NF] = {-0.4f, 2.0f, 1.2f, 1.0f, 0.3f, 0.2f, -1.0f, -1.0f};

inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline float sigm(float z) { return 1.f / (1.f + std::exp(-z)); }
inline float hyp(float a, float b) { return std::sqrt(a * a + b * b); }
inline float dist(const P& a, const P& b) { return hyp(a.x - b.x, a.y - b.y); }

inline int l1c(int a, int b) {
    return std::abs(((a >> 16) & 255) - ((b >> 16) & 255)) +
           std::abs(((a >> 8) & 255) - ((b >> 8) & 255)) +
           std::abs((a & 255) - (b & 255));
}

float segDist(const P& a, const P& b, const P& p) {
    float vx = b.x - a.x, vy = b.y - a.y;
    float l2 = vx * vx + vy * vy;
    if (l2 < 1e-9f) return dist(a, p);
    float t = clampf(((p.x - a.x) * vx + (p.y - a.y) * vy) / l2, 0.f, 1.f);
    return hyp(p.x - (a.x + t * vx), p.y - (a.y + t * vy));
}

}

Engine::Engine() {
    resetWeights();
    newMatch();
}

void Engine::resetWeights() {
    for (int i = 0; i < NF; i++) {
        wp_[i] = PRIOR_WP[i];
        wt_[i] = PRIOR_WT[i];
    }
    shootThr_ = 0.45f;
    for (int i = 0; i < GX * GY; i++) {
        gridVisit_[i] = 0;
        gridLoss_[i] = 0;
    }
    matchesLearned_ = 0;
}

void Engine::newMatch() {
    pending_.clear();
    done_.clear();
    lossLog_.clear();
    passAtt_ = passOk_ = shots_ = losses_ = 0;
    framesLive_ = framesTeam_ = framesOpp_ = 0;
    pLineSum_ = pWidthSum_ = pPressSum_ = 0;
    pAttN_ = 0;
    pFlankSum_ = 0;
    pFlankN_ = 0;
    pFastN_ = pOppPossN_ = 0;
    dirInit_ = false;
    emaL_ = emaR_ = 0.5f;
    flipSince_ = -1;
    liveMs_ = lastChangeLive_ = 0;
    half_ = 1;
    owner_ = candOwner_ = candRun_ = 0;
    ownerSince_ = carryStart_ = 0;
    ballKnown_ = false;
    lastTeamT_ = -1e9;
    nextActionAt_ = 0;
    lastSwitchMs_ = lastTackleMs_ = -1e9;
    now_ = prevNow_ = 0;
    notLiveSince_ = -1;
    carrierIdx_ = -1;
}

void Engine::setColors(int teamRGB, int oppRGB, int ballRGB) {
    teamC_ = teamRGB & 0xFFFFFF;
    oppC_ = oppRGB & 0xFFFFFF;
    ballC_ = ballRGB & 0xFFFFFF;
    haveColors_ = true;
}

void Engine::extractBlobs(const int* px, int w, int h, std::vector<Blob>& out) const {
    out.clear();
    if (w < 16 || h < 16) return;
    const int n = w * h;

    int hr[256] = {0}, hg[256] = {0}, hb[256] = {0};
    for (int i = 0; i < n; i++) {
        int c = px[i];
        hr[(c >> 16) & 255]++;
        hg[(c >> 8) & 255]++;
        hb[c & 255]++;
    }
    auto med = [&](const int* hist) {
        int acc = 0;
        for (int i = 0; i < 256; i++) {
            acc += hist[i];
            if (acc >= n / 2) return i;
        }
        return 0;
    };
    const int bgc = (med(hr) << 16) | (med(hg) << 8) | med(hb);

    std::vector<uint8_t> mask(n, 0), core(n, 0), seen(n, 0);
    for (int i = 0; i < n; i++) mask[i] = l1c(px[i], bgc) > 100;

    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            if (!mask[y * w + x]) continue;
            int c = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) c += mask[(y + dy) * w + x + dx];
            if (c >= 7) core[y * w + x] = 1;
        }
    }

    std::vector<int> stack;
    for (int i = 0; i < n; i++) {
        if (!core[i] || seen[i]) continue;
        stack.clear();
        stack.push_back(i);
        seen[i] = 1;
        const int seedC = px[i];
        long sx = 0, sy = 0, cnt = 0, sr = 0, sg = 0, sb = 0;
        int minx = w, maxx = 0, miny = h, maxy = 0;
        while (!stack.empty()) {
            int j = stack.back();
            stack.pop_back();
            int x = j % w, y = j / w;
            sx += x;
            sy += y;
            cnt++;
            int c = px[j];
            sr += (c >> 16) & 255;
            sg += (c >> 8) & 255;
            sb += c & 255;
            minx = std::min(minx, x);
            maxx = std::max(maxx, x);
            miny = std::min(miny, y);
            maxy = std::max(maxy, y);
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    int k = ny * w + nx;
                    if (core[k] && !seen[k] && l1c(px[k], seedC) < 90) {
                        seen[k] = 1;
                        stack.push_back(k);
                    }
                }
            }
        }
        int bw = maxx - minx + 1, bh = maxy - miny + 1;
        if (bw > 14 || bh > 14 || cnt > 140) continue;
        Blob b;
        b.x = ((float)sx / cnt + 0.5f) / w;
        b.y = ((float)sy / cnt + 0.5f) / h;
        b.area = (float)cnt;
        b.r = (int)(sr / cnt);
        b.g = (int)(sg / cnt);
        b.b = (int)(sb / cnt);
        out.push_back(b);
    }
}

int Engine::scanPalette(const int* px, int w, int h, PaletteEntry* out, int maxOut) {
    std::vector<Blob> bl;
    extractBlobs(px, w, h, bl);
    struct Cl { double r, g, b; int n; };
    std::vector<Cl> cl;
    for (const Blob& b : bl) {
        int best = -1, bd = 80;
        for (size_t i = 0; i < cl.size(); i++) {
            int d = (int)(std::fabs(b.r - cl[i].r) + std::fabs(b.g - cl[i].g) + std::fabs(b.b - cl[i].b));
            if (d < bd) {
                bd = d;
                best = (int)i;
            }
        }
        if (best < 0) {
            cl.push_back({(double)b.r, (double)b.g, (double)b.b, 1});
        } else {
            Cl& c = cl[best];
            c.r = (c.r * c.n + b.r) / (c.n + 1);
            c.g = (c.g * c.n + b.g) / (c.n + 1);
            c.b = (c.b * c.n + b.b) / (c.n + 1);
            c.n++;
        }
    }
    std::sort(cl.begin(), cl.end(), [](const Cl& a, const Cl& b) { return a.n > b.n; });
    int n = 0;
    for (const Cl& c : cl) {
        if (n >= maxOut) break;
        out[n].r = (int)c.r;
        out[n].g = (int)c.g;
        out[n].b = (int)c.b;
        out[n].blobs = c.n;
        n++;
    }
    return n;
}

void Engine::classify(int, int) {
    rawTeam_.clear();
    rawOpp_.clear();
    ballSeen_ = false;
    struct Q { P p; float a; };
    std::vector<Q> team, opp, ball, unk;
    for (const Blob& b : blobs_) {
        int c = (b.r << 16) | (b.g << 8) | b.b;
        int dt = l1c(c, teamC_), dop = l1c(c, oppC_), db = l1c(c, ballC_);
        int best = std::min(dt, std::min(dop, db));
        Q q;
        q.p.x = b.x;
        q.p.y = b.y;
        q.a = b.area;
        if (best <= (opts_.colorOnly ? 100 : 120)) {
            if (best == db) ball.push_back(q);
            else if (best == dt) team.push_back(q);
            else opp.push_back(q);
        } else {
            unk.push_back(q);
        }
    }

    auto emit = [](std::vector<Q>& src, std::vector<P>& dst) {
        if (src.empty()) return;
        std::vector<float> areas;
        for (const Q& q : src) areas.push_back(q.a);
        std::nth_element(areas.begin(), areas.begin() + areas.size() / 2, areas.end());
        float medA = std::max(1.f, areas[areas.size() / 2]);
        for (const Q& q : src) {
            int k = 1;
            if (q.a >= 1.8f * medA) k = (int)clampf(std::floor(q.a / medA + 0.5f), 1.f, 3.f);
            if (k == 1) {
                dst.push_back(q.p);
            } else if (k == 2) {
                P a = q.p, b = q.p;
                a.x -= 0.008f;
                b.x += 0.008f;
                dst.push_back(a);
                dst.push_back(b);
            } else {
                P a = q.p, c = q.p;
                a.x -= 0.014f;
                c.x += 0.014f;
                dst.push_back(a);
                dst.push_back(q.p);
                dst.push_back(c);
            }
        }
    };
    emit(team, rawTeam_);
    emit(opp, rawOpp_);

    if (!opts_.colorOnly) {
        for (const Q& q : unk) {
            if (q.p.x < 0.08f || q.p.x > 0.92f) {
                if (rawTeam_.size() < 11) rawTeam_.push_back(q.p);
                else if (rawOpp_.size() < 11) rawOpp_.push_back(q.p);
            }
        }
    }

    if (!ball.empty()) {
        size_t bi = 0;
        if (ballKnown_ && ball.size() > 1) {
            float bd = 1e9f;
            for (size_t i = 0; i < ball.size(); i++) {
                float rx = dir_ > 0 ? ball[i].p.x : 1 - ball[i].p.x;
                float d = hyp(rx - ballA_.x, (ball[i].p.y * ASPECT) - ballA_.y);
                if (d < bd) {
                    bd = d;
                    bi = i;
                }
            }
        }
        ballRaw_ = ball[bi].p;
        ballSeen_ = true;
    }
}

void Engine::updateDirection() {
    float minX = 1.f, maxX = 0.f;
    for (const P& p : rawTeam_) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
    }
    if (!dirInit_) {
        emaL_ = minX;
        emaR_ = 1.f - maxX;
    } else {
        emaL_ = emaL_ * 0.97f + minX * 0.03f;
        emaR_ = emaR_ * 0.97f + (1.f - maxX) * 0.03f;
    }
    const int cand = (emaL_ < emaR_) ? 1 : -1;

    if (opts_.attackDir >= 0) {
        int want = opts_.attackDir == 1 ? 1 : -1;
        if (want != dir_) ballKnown_ = false;
        dir_ = want;
        dirInit_ = true;
    } else if (!dirInit_) {
        dir_ = cand;
        dirInit_ = true;
        lastChangeLive_ = liveMs_;
    } else if (cand != dir_) {
        if (flipSince_ < 0) flipSince_ = liveMs_;
        if (liveMs_ - flipSince_ > 8000 && liveMs_ - lastChangeLive_ > 60000) {
            dir_ = cand;
            ballKnown_ = false;
            half_ = std::min(2, half_ + 1);
            lastChangeLive_ = liveMs_;
            flipSince_ = -1;
        }
    } else {
        flipSince_ = -1;
    }
    if (opts_.half == 1 || opts_.half == 2) half_ = opts_.half;
}

float Engine::nearestOpp(const P& p) const {
    float m = 1.f;
    for (const P& o : oA_) m = std::min(m, dist(p, o));
    return m;
}

float Engine::gridLoss(float ax, float ay01) const {
    int gx = (int)clampf(ax * GX, 0.f, GX - 1.f);
    int gy = (int)clampf(ay01 * GY, 0.f, GY - 1.f);
    int i = gy * GX + gx;
    float rate = (gridLoss_[i] + 1.f) / (gridVisit_[i] + 5.f);
    return clampf(rate * 2.f, 0.f, 1.f);
}

void Engine::updatePossession() {
    int cand = 0;
    carrierIdx_ = -1;
    if (ballKnown_ && !tA_.empty()) {
        float dtm = 1e9f, dom = 1e9f;
        int ti = -1;
        for (size_t i = 0; i < tA_.size(); i++) {
            float d = dist(tA_[i], ballA_);
            if (d < dtm) {
                dtm = d;
                ti = (int)i;
            }
        }
        for (const P& o : oA_) dom = std::min(dom, dist(o, ballA_));
        if (dtm < opts_.ownRadius && dtm <= dom) cand = 1;
        else if (dom < opts_.ownRadius && dom < dtm) cand = 2;
        carrierIdx_ = ti;
    }
    if (cand == candOwner_) candRun_++;
    else {
        candOwner_ = cand;
        candRun_ = 1;
    }
    const int need = (cand == 0) ? 5 : 2;
    if (candRun_ >= need && cand != owner_) {
        int old = owner_;
        owner_ = cand;
        ownerSince_ = now_;
        if (owner_ == 2 && (old == 1 || (old == 0 && now_ - lastTeamT_ < 2500))) {
            int blame = AT_CARRY;
            double bt = -1;
            for (Dec& d : pending_) {
                if (now_ - d.t <= 2400) {
                    d.lostSeen = true;
                    if (d.t > bt) {
                        bt = d.t;
                        blame = d.type;
                    }
                }
            }
            losses_++;
            lossLog_.push_back(lastCarrier_.x);
            lossLog_.push_back(lastCarrier_.y / ASPECT);
            lossLog_.push_back((float)blame);
        }
        if (owner_ == 1) carryStart_ = now_;
    }
    if (owner_ == 1) {
        lastTeamT_ = now_;
        if (carrierIdx_ >= 0) lastCarrier_ = tA_[carrierIdx_];
    }
}

void Engine::updateProfile() {
    if (oA_.size() < 6) return;
    if (owner_ == 1) {
        std::vector<float> xs;
        for (const P& o : oA_) xs.push_back(o.x);
        std::sort(xs.begin(), xs.end(), std::greater<float>());
        float line = 0;
        int n = 0;
        for (size_t i = 1; i < xs.size() && n < 4; i++, n++) line += xs[i];
        if (n > 0) pLineSum_ += line / n;
        double my = 0;
        for (const P& o : oA_) my += o.y / ASPECT;
        my /= oA_.size();
        double v = 0;
        for (const P& o : oA_) v += (o.y / ASPECT - my) * (o.y / ASPECT - my);
        pWidthSum_ += std::sqrt(v / oA_.size());
        std::vector<float> ds;
        for (const P& o : oA_) ds.push_back(dist(o, ballA_));
        std::sort(ds.begin(), ds.end());
        float pr = 0;
        for (int i = 0; i < 3 && i < (int)ds.size(); i++) pr += ds[i];
        pPressSum_ += pr / 3.f;
        pAttN_++;
    } else if (owner_ == 2) {
        pOppPossN_++;
        if (ballA_.x < 0.5f) {
            pFlankSum_ += ballA_.y / ASPECT - 0.5f;
            pFlankN_++;
        }
        if (ballSpeed_ > 0.22f && ballA_.x < 0.6f) pFastN_++;
    }
}

void Engine::profile(float* o) const {
    o[0] = pAttN_ ? (float)(pLineSum_ / pAttN_) : 0.f;
    o[1] = pAttN_ ? (float)(pWidthSum_ / pAttN_) : 0.f;
    o[2] = pAttN_ ? (float)(pPressSum_ / pAttN_) : 0.f;
    o[3] = pFlankN_ ? (float)(pFlankSum_ / pFlankN_) : 0.f;
    o[4] = pOppPossN_ ? clampf((float)pFastN_ / pOppPossN_ * 6.f, 0.f, 1.f) : 0.f;
    o[5] = (float)pAttN_;
}

void Engine::resolveDecisions() {
    for (size_t i = 0; i < pending_.size();) {
        Dec& d = pending_[i];
        if (now_ - d.t >= 2400) {
            d.outcome = d.lostSeen ? 0 : 1;
            if (d.type == AT_PASS || d.type == AT_THROUGH) passOk_ += d.outcome;
            done_.push_back(d);
            pending_.erase(pending_.begin() + i);
        } else {
            i++;
        }
    }
}

void Engine::logDecision(int type, float p, const float* f, const P& at) {
    if (observe_) return;
    Dec d;
    d.t = now_;
    d.type = type;
    d.p = p;
    if (f) for (int i = 0; i < NF; i++) d.f[i] = f[i];
    d.ax = at.x;
    d.ay = at.y / ASPECT;
    if (type == AT_PASS || type == AT_THROUGH) {
        passAtt_++;
        pending_.push_back(d);
    } else if (type == AT_SHOOT) {
        shots_++;
        d.outcome = -1;
        done_.push_back(d);
    }
}

int Engine::process(const int* px, int w, int h, double tMs, Action& out) {
    out = Action();
    prevNow_ = now_;
    now_ = tMs;
    double dtMs = prevNow_ > 0 ? std::min(200.0, now_ - prevNow_) : 80.0;
    if (dtMs < 1) dtMs = 1;

    extractBlobs(px, w, h, blobs_);
    classify(w, h);

    bool live = haveColors_ && rawTeam_.size() >= 5 && rawOpp_.size() >= 5;
    if (!live) {
        status_ = 0;
        if (notLiveSince_ < 0) notLiveSince_ = now_;
        if (now_ - notLiveSince_ > 1500) {
            pending_.clear();
            owner_ = candOwner_ = 0;
            candRun_ = 0;
            ballKnown_ = false;
        }
        return 0;
    }
    notLiveSince_ = -1;
    liveMs_ += dtMs;

    updateDirection();

    auto toA = [&](const P& r) {
        P p;
        p.x = dir_ > 0 ? r.x : 1.f - r.x;
        p.y = r.y * ASPECT;
        return p;
    };
    tA_.clear();
    oA_.clear();
    for (const P& p : rawTeam_) tA_.push_back(toA(p));
    for (const P& p : rawOpp_) oA_.push_back(toA(p));

    if (ballSeen_) {
        P nb = toA(ballRaw_);
        if (ballKnown_) ballSpeed_ = dist(nb, ballA_) / (float)(dtMs / 1000.0);
        else ballSpeed_ = 0;
        ballA_ = nb;
        lastBallSeenT_ = now_;
        ballKnown_ = true;
    } else if (ballKnown_ && now_ - lastBallSeenT_ > 700) {
        ballKnown_ = false;
    }

    updatePossession();
    resolveDecisions();
    updateProfile();

    framesLive_++;
    if (owner_ == 1) framesTeam_++;
    else if (owner_ == 2) framesOpp_++;

    status_ = owner_ == 1 ? 2 : (owner_ == 2 ? 3 : 1);

    if (now_ < nextActionAt_) return status_;

    if (owner_ == 1) out = decideAttack();
    else if (owner_ == 2) out = decideDefend();
    else out = decideLoose();
    return status_;
}

void Engine::toStick(float vx, float vy, Action& a) const {
    vy = vy / ASPECT;
    float l = hyp(vx, vy);
    if (l < 1e-5f) {
        a.sx = a.sy = 0;
        return;
    }
    vx /= l;
    vy /= l;
    float sx = dir_ > 0 ? vx : -vx;
    float sy = vy;
    if (opts_.invertX) sx = -sx;
    if (opts_.invertY) sy = -sy;
    a.sx = sx;
    a.sy = sy;
}

void Engine::passFeatures(const P& c, const P& geo, const P& recv, int ri, int ci, float* f) const {
    float lane = 1e9f;
    for (const P& o : oA_) lane = std::min(lane, segDist(c, geo, o));
    float free = nearestOpp(recv);
    float d = dist(c, geo);
    int support = 0;
    for (size_t i = 0; i < tA_.size(); i++) {
        if ((int)i == ri || (int)i == ci) continue;
        if (dist(tA_[i], recv) < 0.2f) support++;
    }
    float pressure = clampf(1.f - nearestOpp(c) / 0.10f, 0.f, 1.f);
    float prog = clampf((geo.x - c.x) / 0.25f, -1.f, 1.f);
    f[0] = 1.f;
    f[1] = clampf(lane / 0.08f, 0.f, 1.f);
    f[2] = clampf(free / 0.10f, 0.f, 1.f);
    f[3] = prog;
    f[4] = std::min(support, 3) / 3.f;
    f[5] = clampf(1.f - std::fabs(d - 0.13f) / 0.13f, 0.f, 1.f);
    f[6] = pressure * std::max(0.f, prog);
    f[7] = gridLoss(recv.x, recv.y / ASPECT);
}

float Engine::scoreWith(const float* w, const float* f, int tactic) const {
    float z = 0;
    for (int i = 0; i < NF; i++) z += w[i] * f[i];
    z += (tactic - 1) * 0.35f * f[3];
    return sigm(z);
}

float Engine::shotQuality(const P& c, float& d) const {
    float dx = 1.f - c.x, dy = c.y - GOAL_Y;
    d = hyp(dx, dy);
    float qd = clampf(1.f - d / 0.32f, 0.f, 1.f);
    float ang = clampf(1.f - std::fabs(dy) / GOAL_Y, 0.f, 1.f);
    float fr = clampf(nearestOpp(c) / 0.08f, 0.f, 1.f);
    return 0.6f * qd + 0.2f * ang + 0.2f * fr;
}

Action Engine::decideAttack() {
    Action a;
    if (carrierIdx_ < 0) return a;
    const P c = tA_[carrierIdx_];
    const float press = nearestOpp(c);
    const double heldS = (now_ - carryStart_) / 1000.0;

    float pr[6];
    profile(pr);
    const bool adapt = (half_ == 2 && pr[5] > 100);
    float maxHold = 1.1f - 0.7f * opts_.passSpeed;
    float throughBonus = 0.f;
    if (adapt) {
        if (pr[2] > 0 && pr[2] < 0.08f) maxHold *= 0.8f;
        if (pr[0] > 0 && pr[0] < 0.72f) throughBonus = 0.05f;
    }
    if (opts_.legendary) { maxHold *= 0.8f; throughBonus += 0.03f; }
    const bool forced = heldS > maxHold;

    float sd;
    float q = shotQuality(c, sd);
    float thr = shootThr_ - (press < 0.05f ? 0.08f : 0.f) - 0.04f * (opts_.aggression - 0.5f);
    if (sd < opts_.shootReach && q >= thr) {
        float aimY = c.y < GOAL_Y ? GOAL_Y + 0.05f : GOAL_Y - 0.05f;
        toStick(1.f - c.x, aimY - c.y, a);
        a.type = AT_SHOOT;
        a.button = B_TOP;
        a.holdMs = sd < 0.15f ? 110 : 240;
        nextActionAt_ = now_ + 1400;
        float f[NF] = {0};
        logDecision(AT_SHOOT, q, f, c);
        return a;
    }

    {
        std::vector<float> xs;
        for (const P& o : oA_) xs.push_back(o.x);
        std::sort(xs.begin(), xs.end(), std::greater<float>());
        float lastDef = xs.size() >= 2 ? xs[1] : 1.f;
        float thrT = opts_.tactic == 2 ? 0.55f : (opts_.tactic == 0 ? 0.72f : 0.62f);
        thrT -= throughBonus;
        float bestP = 0;
        int bi = -1;
        P bT;
        float bf[NF];
        if (c.x > 0.35f) {
            for (size_t i = 0; i < tA_.size(); i++) {
                if ((int)i == carrierIdx_) continue;
                const P& t = tA_[i];
                if (t.x - c.x < 0.06f || t.x < lastDef - 0.03f) continue;
                P T;
                T.x = std::min(t.x + 0.05f, 1.f);
                T.y = t.y;
                float d = dist(c, T);
                if (d < 0.06f || d > 0.5f) continue;
                float f[NF];
                passFeatures(c, T, t, (int)i, carrierIdx_, f);
                float p = scoreWith(wt_, f, opts_.tactic);
                if (p > bestP) {
                    bestP = p;
                    bi = (int)i;
                    bT = T;
                    std::memcpy(bf, f, sizeof(f));
                }
            }
        }
        if (bi >= 0 && bestP >= thrT) {
            toStick(bT.x - c.x, bT.y - c.y, a);
            a.type = AT_THROUGH;
            a.button = B_THROUGH;
            a.holdMs = 60;
            nextActionAt_ = now_ + 380;
            logDecision(AT_THROUGH, bestP, bf, bT);
            return a;
        }
    }

    float bestP = 0;
    int bi = -1;
    float bf[NF];
    for (size_t i = 0; i < tA_.size(); i++) {
        if ((int)i == carrierIdx_) continue;
        const P& t = tA_[i];
        float d = dist(c, t);
        if (d < 0.04f || d > 0.45f) continue;
        float f[NF];
        passFeatures(c, t, t, (int)i, carrierIdx_, f);
        float p = scoreWith(wp_, f, opts_.tactic);
        if (p > bestP) {
            bestP = p;
            bi = (int)i;
            std::memcpy(bf, f, sizeof(f));
        }
    }
    float passThr = forced ? 0.35f : (press < 0.04f ? 0.40f : 0.55f);
    if (opts_.legendary && !forced) passThr += 0.05f;
    if (bi >= 0 && bestP >= passThr) {
        const P& t = tA_[bi];
        toStick(t.x - c.x, t.y - c.y, a);
        a.type = AT_PASS;
        a.button = B_PASS;
        a.holdMs = 55;
        nextActionAt_ = now_ + 300;
        logDecision(AT_PASS, bestP, bf, t);
        return a;
    }

    if (c.x < 0.18f && press < 0.05f) {
        toStick(1.f, c.y < GOAL_Y ? -0.8f * ASPECT : 0.8f * ASPECT, a);
        a.type = AT_CLEAR;
        a.button = B_TOP;
        a.holdMs = 90;
        nextActionAt_ = now_ + 700;
        return a;
    }

    float vx = 1.f - c.x, vy = (GOAL_Y - c.y) * 0.6f;
    float l = hyp(vx, vy);
    if (l > 1e-5f) {
        vx /= l;
        vy /= l;
    }
    for (const P& o : oA_) {
        float d = dist(c, o);
        if (d < 0.12f && d > 1e-4f) {
            float k = (0.12f - d) / 0.12f;
            vx += (c.x - o.x) / d * k * 1.5f;
            vy += (c.y - o.y) / d * k * 1.5f;
        }
    }
    toStick(vx, vy, a);
    a.type = AT_CARRY;
    a.dash = press > 0.09f && c.x < 0.8f;
    return a;
}

Action Engine::decideDefend() {
    Action a;
    if (!ballKnown_ || tA_.empty()) return a;
    const P goal = {0.f, GOAL_Y};
    const P b = ballA_;

    int di = -1;
    float dBall = 1e9f;
    for (size_t i = 0; i < tA_.size(); i++) {
        float d = dist(tA_[i], b);
        if (d < dBall) {
            dBall = d;
            di = (int)i;
        }
    }
    if (di < 0) return a;
    const P d = tA_[di];

    float pr[6];
    profile(pr);
    const bool adapt = (half_ == 2 && pr[5] > 100);
    float stand = 0.035f;
    float flank = 0.f;
    if (adapt) {
        stand = clampf(0.02f + 0.06f * pr[4] + 0.02f * (1.f - opts_.aggression), 0.02f, 0.08f);
        flank = clampf(pr[3], -0.4f, 0.4f);
    }

    // no tackle within ~26 m of our own goal (own goal is at x = 0 in this frame): jockey instead, a lunge there gives away a penalty
    if (dBall < 0.032f && b.x > 0.25f && now_ - lastTackleMs_ > 650) {
        toStick(b.x - d.x, b.y - d.y, a);
        a.type = AT_TACKLE;
        a.button = B_TOP;
        a.holdMs = 60;
        a.dash = opts_.aggression > 0.4f;
        lastTackleMs_ = now_;
        nextActionAt_ = now_ + 300;
        return a;
    }

    if (now_ - lastSwitchMs_ > 800) {
        float moved = dist(b, lastSwitchBall_);
        bool engaged = dBall < 0.06f;
        int target = -1;
        if (!engaged && moved > 0.05f) {
            target = di;
        } else if (engaged && adapt) {
            float bestS = 1e9f;
            int bi = -1;
            for (size_t i = 0; i < tA_.size(); i++) {
                if ((int)i == di) continue;
                if (dist(tA_[i], d) < 0.04f) continue;
                float s = segDist(b, goal, tA_[i]) + 0.5f * dist(tA_[i], b);
                if (s < bestS) {
                    bestS = s;
                    bi = (int)i;
                }
            }
            float lane = segDist(b, goal, d);
            float curS = lane + 0.5f * dBall;
            if (bi >= 0 && bestS + 0.04f < curS && lane > 0.05f) target = bi;
        }
        if (target >= 0) {
            if (target == di) toStick(b.x - d.x, b.y - d.y, a);
            else toStick(tA_[target].x - d.x, tA_[target].y - d.y, a);
            a.type = AT_SWITCH;
            a.button = B_PASS;
            a.holdMs = 50;
            lastSwitchMs_ = now_;
            lastSwitchBall_ = b;
            nextActionAt_ = now_ + 260;
            return a;
        }
    }

    P tgt;
    tgt.x = std::max(0.f, b.x - stand);
    float y01 = b.y / ASPECT;
    y01 = y01 + (0.5f - y01) * 0.3f + flank * 0.15f;
    tgt.y = clampf(y01, 0.05f, 0.95f) * ASPECT;
    toStick(tgt.x - d.x, tgt.y - d.y, a);
    a.dash = dist(d, tgt) > 0.06f || (dBall < 0.08f && opts_.aggression > 0.5f);
    a.type = dBall < 0.08f ? AT_PRESS : AT_CHASE;
    return a;
}

Action Engine::decideLoose() {
    Action a;
    if (!ballKnown_ || tA_.empty()) return a;
    int ti = -1;
    float dm = 1e9f;
    for (size_t i = 0; i < tA_.size(); i++) {
        float d = dist(tA_[i], ballA_);
        if (d < dm) {
            dm = d;
            ti = (int)i;
        }
    }
    if (ti < 0 || dm < 0.012f) return a;
    toStick(ballA_.x - tA_[ti].x, ballA_.y - tA_[ti].y, a);
    a.type = AT_CHASE;
    a.dash = dm > 0.05f;
    return a;
}

void Engine::stats(float* o) const {
    long tot = framesTeam_ + framesOpp_;
    o[0] = tot ? 100.f * framesTeam_ / tot : 0.f;
    o[1] = (float)passAtt_;
    o[2] = (float)passOk_;
    o[3] = (float)shots_;
    o[4] = (float)losses_;
    o[5] = (float)framesLive_;
    o[6] = (float)half_;
    o[7] = (float)dir_;
}

std::string Engine::exportLog() const {
    std::ostringstream o;
    float s[8];
    stats(s);
    float p[6];
    profile(p);
    char buf[256];
    o << "H 1 " << half_ << "\n";
    std::snprintf(buf, sizeof buf, "T %.2f %d %d %d %d\n", s[0], passAtt_, passOk_, shots_, losses_);
    o << buf;
    std::snprintf(buf, sizeof buf, "P %.4f %.4f %.4f %.4f %.4f\n", p[0], p[1], p[2], p[3], p[4]);
    o << buf;
    for (const Dec& d : done_) {
        std::snprintf(buf, sizeof buf, "D %.0f %d %.3f", d.t, d.type, d.p);
        o << buf;
        for (int i = 0; i < NF; i++) {
            std::snprintf(buf, sizeof buf, " %.4f", d.f[i]);
            o << buf;
        }
        std::snprintf(buf, sizeof buf, " %d %.4f %.4f\n", d.outcome, d.ax, d.ay);
        o << buf;
    }
    for (size_t i = 0; i + 2 < lossLog_.size(); i += 3) {
        std::snprintf(buf, sizeof buf, "L %.4f %.4f %d\n", lossLog_[i], lossLog_[i + 1], (int)lossLog_[i + 2]);
        o << buf;
    }
    return o.str();
}

int Engine::debugDots(float* out, int maxFloats) const {
    int n = 0;
    auto put = [&](float v) {
        if (n < maxFloats) out[n++] = v;
    };
    put((float)rawTeam_.size());
    put((float)rawOpp_.size());
    put(ballSeen_ ? 1.f : 0.f);
    put(ballRaw_.x);
    put(ballRaw_.y);
    for (const P& p : rawTeam_) {
        put(p.x);
        put(p.y);
    }
    for (const P& p : rawOpp_) {
        put(p.x);
        put(p.y);
    }
    return n;
}

void Engine::lossGrid(float* rate, float* visits) const {
    for (int i = 0; i < GX * GY; i++) {
        rate[i] = (gridLoss_[i] + 1.f) / (gridVisit_[i] + 5.f);
        visits[i] = gridVisit_[i];
    }
}

std::string Engine::exportWeights() const {
    std::ostringstream o;
    char buf[64];
    o << "v 1\n";
    o << "ml " << matchesLearned_ << "\n";
    o << "wp";
    for (int i = 0; i < NF; i++) {
        std::snprintf(buf, sizeof buf, " %.5f", wp_[i]);
        o << buf;
    }
    o << "\nwt";
    for (int i = 0; i < NF; i++) {
        std::snprintf(buf, sizeof buf, " %.5f", wt_[i]);
        o << buf;
    }
    std::snprintf(buf, sizeof buf, "\nshoot %.5f\n", shootThr_);
    o << buf;
    o << "gv";
    for (int i = 0; i < GX * GY; i++) {
        std::snprintf(buf, sizeof buf, " %.2f", gridVisit_[i]);
        o << buf;
    }
    o << "\ngl";
    for (int i = 0; i < GX * GY; i++) {
        std::snprintf(buf, sizeof buf, " %.2f", gridLoss_[i]);
        o << buf;
    }
    o << "\n";
    return o.str();
}

bool Engine::importWeights(const std::string& s) {
    std::istringstream in(s);
    std::string line;
    bool ok = false;
    float wp[NF], wt[NF], gv[GX * GY], gl[GX * GY];
    float shoot = shootThr_;
    int ml = 0;
    for (int i = 0; i < NF; i++) {
        wp[i] = wp_[i];
        wt[i] = wt_[i];
    }
    for (int i = 0; i < GX * GY; i++) {
        gv[i] = gridVisit_[i];
        gl[i] = gridLoss_[i];
    }
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string k;
        ls >> k;
        if (k == "v") ok = true;
        else if (k == "ml") ls >> ml;
        else if (k == "wp") for (int i = 0; i < NF; i++) ls >> wp[i];
        else if (k == "wt") for (int i = 0; i < NF; i++) ls >> wt[i];
        else if (k == "shoot") ls >> shoot;
        else if (k == "gv") for (int i = 0; i < GX * GY; i++) ls >> gv[i];
        else if (k == "gl") for (int i = 0; i < GX * GY; i++) ls >> gl[i];
    }
    if (!ok) return false;
    for (int i = 0; i < NF; i++) {
        wp_[i] = clampf(wp[i], -4.f, 4.f);
        wt_[i] = clampf(wt[i], -4.f, 4.f);
    }
    shootThr_ = clampf(shoot, 0.25f, 0.7f);
    for (int i = 0; i < GX * GY; i++) {
        gridVisit_[i] = std::max(0.f, gv[i]);
        gridLoss_[i] = std::max(0.f, gl[i]);
    }
    matchesLearned_ = ml;
    return true;
}

std::string Engine::learn(const std::string& logs) {
    struct S { int type; float f[NF]; int y; float ax, ay; };
    std::vector<S> samples;
    std::istringstream in(logs);
    std::string line;
    int matches = 0, curShots = 0, carryLosses = 0;
    float shootBefore = shootThr_;
    while (std::getline(in, line)) {
        if (line.size() < 2) continue;
        char tag = line[0];
        std::istringstream ls(line.substr(1));
        if (tag == 'D') {
            double t;
            int type;
            float p;
            S s;
            ls >> t >> type >> p;
            for (int k = 0; k < NF; k++) ls >> s.f[k];
            ls >> s.y >> s.ax >> s.ay;
            s.type = type;
            if (ls && (type == AT_PASS || type == AT_THROUGH) && (s.y == 0 || s.y == 1)) samples.push_back(s);
        } else if (tag == 'T') {
            float poss;
            int pa, po, sh, lo;
            ls >> poss >> pa >> po >> sh >> lo;
            curShots = sh;
        } else if (tag == 'L') {
            float ax, ay;
            int type;
            ls >> ax >> ay >> type;
            if (ls && type == AT_CARRY) {
                int gx = (int)clampf(ax * GX, 0.f, GX - 1.f), gy = (int)clampf(ay * GY, 0.f, GY - 1.f);
                gridVisit_[gy * GX + gx] += 1.f;
                gridLoss_[gy * GX + gx] += 1.f;
                carryLosses++;
            }
        } else if (tag == 'R') {
            int gf, ga;
            ls >> gf >> ga;
            matches++;
            if (curShots >= 2) {
                float conv = (float)gf / curShots;
                shootThr_ = clampf(shootThr_ + 0.05f * (0.25f - conv), 0.30f, 0.65f);
            }
            curShots = 0;
        }
    }

    auto accuracy = [&](const std::vector<S>& v) {
        if (v.empty()) return 0.f;
        int ok = 0;
        for (const S& s : v) {
            const float* w = s.type == AT_PASS ? wp_ : wt_;
            float z = 0;
            for (int k = 0; k < NF; k++) z += w[k] * s.f[k];
            if ((sigm(z) > 0.5f) == (s.y == 1)) ok++;
        }
        return 100.f * ok / v.size();
    };
    float accBefore = accuracy(samples);
    int kept = 0;
    for (const S& s : samples) kept += s.y;

    const long n = (long)samples.size();
    if (n >= 8) {
        for (int epoch = 0; epoch < 4; epoch++) {
            for (const S& s : samples) {
                float* w = s.type == AT_PASS ? wp_ : wt_;
                const float* w0 = s.type == AT_PASS ? PRIOR_WP : PRIOR_WT;
                float z = 0;
                for (int k = 0; k < NF; k++) z += w[k] * s.f[k];
                float err = (float)s.y - sigm(z);
                float lr = 0.06f / (1.f + 0.002f * n);
                for (int k = 0; k < NF; k++) {
                    w[k] += lr * err * s.f[k] - 0.004f * (w[k] - w0[k]);
                    w[k] = clampf(w[k], -4.f, 4.f);
                }
            }
        }
    }
    for (const S& s : samples) {
        int gx = (int)clampf(s.ax * GX, 0.f, GX - 1.f), gy = (int)clampf(s.ay * GY, 0.f, GY - 1.f);
        gridVisit_[gy * GX + gx] += 1.f;
        if (s.y == 0) gridLoss_[gy * GX + gx] += 1.f;
    }
    float accAfter = accuracy(samples);
    matchesLearned_ += matches;

    int worst = -1;
    float wr = 0;
    for (int i = 0; i < GX * GY; i++) {
        if (gridVisit_[i] < 4) continue;
        float r = gridLoss_[i] / gridVisit_[i];
        if (r > wr) {
            wr = r;
            worst = i;
        }
    }
    char buf[512];
    std::string out;
    std::snprintf(buf, sizeof buf, "Learned from %d match(es): %ld pass samples (%d kept, %.0f%%), %d carry losses.\n",
                  matches, n, kept, n ? 100.f * kept / n : 0.f, carryLosses);
    out += buf;
    if (n >= 8) {
        std::snprintf(buf, sizeof buf, "Pass model fit: %.0f%% -> %.0f%%.\n", accBefore, accAfter);
        out += buf;
    } else {
        out += "Not enough pass samples yet (need 8+): weights unchanged.\n";
    }
    std::snprintf(buf, sizeof buf, "Shoot threshold: %.2f -> %.2f.\n", shootBefore, shootThr_);
    out += buf;
    if (worst >= 0) {
        std::snprintf(buf, sizeof buf, "Worst zone: column %d of %d, row %d of %d (%.0f%% of plays lost there).\n",
                      worst % GX + 1, GX, worst / GX + 1, GY, 100.f * wr);
        out += buf;
    }
    return out;
}

}
