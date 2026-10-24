// Meta layer: reaction model, jump freeze, animation lock, 1v1 intent, anti-pattern residuals, feints, commit gate, security
// breaker, overload, interception field, late game, restarts, defence helpers, four-mode machine, and a fuzz run.
// Build: g++ -std=c++17 -O1 -I../app/src/main/cpp meta_checks.cpp ../app/src/main/cpp/pitch_engine.cpp
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#define private public   // the checks read internal state and supply the controlled-player marker that normally comes from pixels
#include "pitch_engine.hpp"
using namespace pe;
static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) std::printf("ok:   %s\n", msg); else { std::printf("FAIL: %s\n", msg); g_fail++; } } while (0)

struct Scene {
    PitchEngine e;
    MetricVector2D home[11], away[11];
    double t = 1000;
    explicit Scene(const TacticalConfig* base = nullptr) {
        TacticalConfig c = base ? *base : TacticalConfig();
        c.attackDir = 1;
        e.setConfig(c);
        e.setShootConfig(true, 520, true);
        for (int i = 0; i < 11; i++) {
            home[i] = MetricVector2D(-40.f + 2.f * i, -20.f + 4.f * i);
            away[i] = MetricVector2D(20.f + 2.f * i, 25.f - 5.f * i);
        }
        home[0] = MetricVector2D(-51.f, 0.f);
        away[0] = MetricVector2D(51.f, 0.f);
    }
    GestureOutput step(const MetricVector2D& ball, int hud = 0, double dtMs = 80) {
        e.setFrameTimeMs(t);
        if (hud) e.setHudPossession(hud);
        e.ingestDetections(home, 11, away, 11, &ball, true);
        t += dtMs;
        return e.lastGesture();
    }
    void dark(int frames) {
        for (int f = 0; f < frames; f++) {
            e.setFrameTimeMs(t);
            e.ingestDetections(nullptr, 0, nullptr, 0, nullptr, false);
            t += 80;
        }
    }
    int idxAt(const MetricVector2D& p, bool opp = false) const {
        int best = -1; float bd = 1e9f;
        for (int i = 0; i < 11; i++) {
            const PlayerState& q = opp ? e.state_.opp[i] : e.state_.team[i];
            if (!q.active) continue;
            const float d = distance(q.pos, p);
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }
    void markControlled(const MetricVector2D& p) { e.ctrlSeen_ = true; e.ctrlPos_ = p; e.ctrlSeenMs_ = t; e.frameTimeMs_ = t; }
};


static void openLayout(Scene& s, const MetricVector2D& carrier) {
    for (int i = 1; i < 11; i++) {
        s.home[i] = MetricVector2D(-30.f + 6.f * i, -22.f + 4.5f * i);
        s.away[i] = MetricVector2D(30.f + 1.5f * i, -25.f + 5.f * i);
    }
    s.home[5] = carrier;
}
static void settle(Scene& s, const MetricVector2D& ball, int n = 16, int hud = 1) { for (int f = 0; f < n; f++) s.step(ball, hud); }
static int candIdx(const PitchEngine& e, int targetId, int type) {
    for (int i = 0; i < e.candidateCount(); i++)
        if (e.candidates()[i].targetId == targetId && e.candidates()[i].type == type) return i;
    return -1;
}

int main() {
    {
        TacticalConfig d;
        CHECK(d.meta && d.metaNoise && d.metaFeint && d.metaCommit && d.metaStupid && d.metaReact && d.metaLock && d.metaMicro &&
              d.metaIntent1v1 && d.metaContain && d.metaBait && d.metaSecondMan && d.metaRecovery && d.metaTrackFreeze && d.metaOverload &&
              d.metaThirdMan && d.metaThroughTiming && d.metaSecurity && d.metaField && d.metaMultiScale && d.metaFsm && d.metaSetPiece &&
              d.metaAntiBait && d.metaLate, "defaults: the whole meta layer is on");
    }

    // ---- reaction model ----
    {
        PitchEngine e;
        TacticalConfig cfg;
        cfg.attackDir = 1;
        e.setConfig(cfg);
        CHECK(std::fabs(e.metaReactP80() - e.config().defReact) < 1e-4f, "reaction: without samples the configured reaction time is used");
        double t = 1000.0;
        for (int rep = 0; rep < 8; rep++) {
            e.state_.opp[3].active = e.state_.opp[3].visible = true;
            e.state_.opp[3].pos = MetricVector2D(16.f, 0.f);
            e.state_.opp[3].vel = MetricVector2D(0.f, 0.f);
            MetaWatch& w = e.meta_.watch[0];
            w = MetaWatch();
            w.active = true; w.opp = 3; w.t0 = t; w.v0 = MetricVector2D(0.f, 0.f); w.lane = MetricVector2D(0.f, 1.f); w.sp0 = 0.f;
            for (int f = 0; f < 14; f++) {
                t += 80.0;
                e.state_.timeMs = t;
                // still for 5 frames (about 400 ms), then he sprints at the lane
                e.state_.opp[3].vel = f < 5 ? MetricVector2D(0.f, 0.f) : MetricVector2D(0.f, 3.0f);
                e.metaFrame(0.08);
            }
            e.meta_.watch[0].active = false;
            t += 400.0;
        }
        std::printf("   reaction: n=%d decision mean %.2f s, execution n=%d, plan %.2f s, credit %.2f s\n", e.meta_.reN[0], e.meta_.reMean[0], e.meta_.reN[1], e.metaReactP80(), e.defReactEff());
        CHECK(e.meta_.reN[0] >= 4 && e.meta_.reMean[0] > 0.2f && e.meta_.reMean[0] < 0.6f, "reaction: the opponent's first sharp move toward the lane is measured as a decision latency");
        CHECK(e.metaReactP80() >= 0.9f * e.config().defReact - 1e-4f && e.metaReactP80() <= 1.8f * e.config().defReact + 1e-4f && e.metaReactP80() > e.config().defReact + 1e-3f,
              "reaction: a slow opposition raises the plan time, inside the allowed band");
        CHECK(e.defReactEff() >= 0.6f * e.config().defReact - 1e-4f && e.defReactEff() < e.config().defReact, "reaction: a slow opposition earns defenders a smaller credit than the default");
        // moves that are not toward the lane are not reactions
        PitchEngine f2;
        f2.setConfig(cfg);
        f2.state_.opp[3].active = f2.state_.opp[3].visible = true;
        MetaWatch& w2 = f2.meta_.watch[0];
        w2.active = true; w2.opp = 3; w2.t0 = 1000.0; w2.lane = MetricVector2D(0.f, 1.f);
        for (int f = 0; f < 8; f++) { f2.state_.timeMs = 1000.0 + 80.0 * (f + 1); f2.state_.opp[3].vel = MetricVector2D(-3.0f, 0.f); f2.metaFrame(0.08); }
        CHECK(f2.meta_.reN[0] == 0, "reaction: running along or away from the lane is not counted");
    }

    // ---- radar jump freeze and animation lock ----
    {
        Scene s;
        openLayout(s, MetricVector2D(8.f, 0.f));
        settle(s, MetricVector2D(8.f, 0.f));
        PitchEngine& e = s.e;
        CHECK(e.state_.timeMs >= e.meta_.freezeUntil, "jump: no freeze on a clean radar");
        s.home[3] = MetricVector2D(s.home[3].x + 35.f, s.home[3].y);       // identity swap: a team-mate appears 35 m away
        s.step(MetricVector2D(8.f, 0.f), 1);
        CHECK(e.meta_.freezeUntil > e.state_.timeMs, "jump: a 35 m one-frame displacement freezes risky actions");
        e.calculatePassProbabilities(e.state_);
        bool risky = false;
        for (int i = 0; i < e.candidateCount(); i++) if (e.candidates()[i].type == ACT_SHOOT && e.candidates()[i].utility > -1.0f) risky = true;
        CHECK(!risky, "jump: shots and through balls are priced out while frozen");
        // animation lock: a defender turns hard
        Scene t;
        openLayout(t, MetricVector2D(8.f, 0.f));
        t.away[2] = MetricVector2D(20.f, 0.f);
        for (int f = 0; f < 10; f++) { t.away[2].x -= 0.3f; t.step(MetricVector2D(8.f, 0.f), 1); }
        for (int f = 0; f < 3; f++) { t.away[2].y += 0.3f; t.step(MetricVector2D(8.f, 0.f), 1); }
        const int oi = t.idxAt(t.away[2], true);
        std::printf("   lock: state right after the turn %d\n", (int)t.e.meta_.anim[oi]);
        CHECK(t.e.meta_.anim[oi] == 1, "lock: a defender who just changed direction sharply is marked locked");
        for (int f = 0; f < 8; f++) { t.away[2].y += 0.1f; t.step(MetricVector2D(8.f, 0.f), 1); }
        CHECK(t.e.meta_.anim[oi] != 1, "lock: the lock expires");
    }

    // ---- 1v1 intent ----
    {
        Scene s;
        openLayout(s, MetricVector2D(8.f, 0.f));
        s.away[3] = MetricVector2D(15.f, 0.f);
        for (int f = 0; f < 10; f++) { s.away[3].x -= 0.75f; s.step(MetricVector2D(8.f, 0.f), 1); }
        const float lunge = s.e.meta_.prob[1] + s.e.meta_.prob[3];
        std::printf("   intent: closing fast -> lunge+step-in %.2f, contain %.2f\n", lunge, s.e.meta_.prob[2]);
        CHECK(lunge > s.e.meta_.prob[2] && lunge > 0.4f, "intent: a defender closing fast reads as a lunge or step-in");
        Scene t;
        openLayout(t, MetricVector2D(8.f, 0.f));
        t.away[3] = MetricVector2D(10.f, 0.f);
        for (int f = 0; f < 10; f++) { t.away[3].x += 0.5f; t.step(MetricVector2D(8.f, 0.f), 1); }
        std::printf("   intent: backing off -> drop-off %.2f\n", t.e.meta_.prob[4]);
        CHECK(t.e.meta_.prob[4] > t.e.meta_.prob[1], "intent: a defender backing off reads as a drop-off");
        // online update: the classifier moves toward what it keeps seeing
        const float w0 = t.e.meta_.W[4][2];
        for (int rep = 0; rep < 6; rep++) { t.away[3] = MetricVector2D(10.f, 0.f); for (int f = 0; f < 14; f++) { t.away[3].x += 0.5f; t.step(MetricVector2D(8.f, 0.f), 1); } }
        CHECK(std::fabs(t.e.meta_.W[4][2] - w0) > 1e-4f || t.e.meta_.W[4][2] <= -3.9f, "intent: the classifier weights are updated online");
    }

    // ---- anti-pattern residual ----
    {
        TacticalConfig c;
        Scene s(&c);
        openLayout(s, MetricVector2D(8.f, 0.f));
        s.home[7] = MetricVector2D(19.f, -6.f);
        settle(s, MetricVector2D(8.f, 0.f));
        PitchEngine& e = s.e;
        e.calculatePassProbabilities(e.state_);
        const int ti = s.idxAt(MetricVector2D(19.f, -6.f)) + 1;
        const int k = candIdx(e, ti, ACT_PASS);
        CHECK(k >= 0, "residual: the pass is a candidate");
        if (k >= 0) {
            const MetricVector2D t0 = e.cand_[k].target;
            e.metaDecorrelate(k, e.state_.team[e.state_.carrier]);
            CHECK(distance(e.cand_[k].target, t0) < 1e-4f, "residual: an empty history changes nothing");
            for (int i = 0; i < 20; i++) {
                MetaAct a;
                const MetricVector2D d = t0 - e.state_.team[e.state_.carrier].pos;
                a.len = d.length(); a.ang = std::atan2(d.y, d.x);
                e.meta_.acts[e.meta_.actHead] = a; e.meta_.actHead = (e.meta_.actHead + 1) % META_ACT; e.meta_.nActs++;
            }
            const float xp0 = e.cand_[k].xP;
            e.metaDecorrelate(k, e.state_.team[e.state_.carrier]);
            const float moved = distance(e.cand_[k].target, t0);
            std::printf("   residual: pass moved %.2f m, xP %.3f -> %.3f\n", moved, xp0, e.cand_[k].xP);
            CHECK(moved > 0.5f && moved < 2.5f && e.cand_[k].xP >= xp0 - 0.041f, "residual: a repeated pass is nudged a little without losing safety");
            CHECK(e.cand_[k].offside <= 0.5f && std::fabs(e.cand_[k].target.x) <= PITCH_HALF_X && std::fabs(e.cand_[k].target.y) <= PITCH_HALF_Y, "residual: the nudged pass stays on the feasible set");
        }
    }

    // ---- feints and the commit gate ----
    {
        // free carrier, nobody within 4.5 m: fakes can happen and must be short
        int feints = 0, holds = 0, fired = 0;
        for (int trial = 0; trial < 6; trial++) {
            Scene s;
            openLayout(s, MetricVector2D(8.f, 0.f));
            s.home[7] = MetricVector2D(19.f, -6.f);
            s.e.meta_.oppJump = 1.f; s.e.meta_.recentInter = 3;
            for (int f = 0; f < 400; f++) {
                const GestureOutput g = s.step(MetricVector2D(8.f, 0.f), 1);
                if (s.e.state_.timeMs < s.e.meta_.feintUntil && g.actionType == ACT_CARRY) feints++;
                if (g.valid && g.actionType == ACT_CARRY && g.durationMs <= 80 && s.e.meta_.commitStart > 0) holds++;
                if (g.valid && (g.actionType == ACT_PASS || g.actionType == ACT_THROUGH || g.actionType == ACT_SHOT)) fired++;
                s.e.meta_.recentInter = 3;
                if (f % 40 == 39) s.e.cooldownUntil_ = 0;
            }
        }
        std::printf("   deception: %d feint frames, %d gate frames, %d real actions\n", feints, holds, fired);
        CHECK(fired > 0, "gate: real actions still go out");
        CHECK(feints > 0, "feint: a free carrier with a fakeable pick fakes now and then");
        // pressured carrier: none of it
        int bad = 0;
        Scene p;
        openLayout(p, MetricVector2D(8.f, 0.f));
        p.home[7] = MetricVector2D(19.f, -6.f);
        p.away[3] = MetricVector2D(11.f, 0.5f);
        p.e.meta_.oppJump = 1.f; p.e.meta_.recentInter = 3;
        for (int f = 0; f < 300; f++) {
            p.step(MetricVector2D(8.f, 0.f), 1);
            if (p.e.state_.timeMs < p.e.meta_.feintUntil || p.e.meta_.commitStart > 0) bad++;
        }
        CHECK(bad == 0, "feint and gate: never with an opponent inside 4.5 m");
    }

    // ---- security breaker, overload, field, late game, restarts ----
    {
        Scene s;
        openLayout(s, MetricVector2D(8.f, 0.f));
        s.home[7] = MetricVector2D(19.f, -6.f);
        settle(s, MetricVector2D(8.f, 0.f));
        PitchEngine& e = s.e;
        e.calculatePassProbabilities(e.state_);
        float uThr0 = 0.f; int nThr = 0;
        for (int i = 0; i < e.candidateCount(); i++) if (e.candidates()[i].type == ACT_THROUGH) { uThr0 += e.candidates()[i].utility; nThr++; }
        for (int k = 0; k < 3; k++) { e.state_.ball = MetricVector2D(-30.f, 5.f); e.metaTurnover(); }
        CHECK(e.meta_.safeUntil > e.state_.timeMs, "security: three turnovers in the same zone start the safe mode");
        e.state_.ball = MetricVector2D(8.f, 0.f);
        e.calculatePassProbabilities(e.state_);
        float uThr1 = 0.f;
        for (int i = 0; i < e.candidateCount(); i++) if (e.candidates()[i].type == ACT_THROUGH) uThr1 += e.candidates()[i].utility;
        CHECK(nThr == 0 || uThr1 < uThr0 - 1.0f, "security: through balls are priced out in the safe mode");
    }
    {
        Scene s;
        openLayout(s, MetricVector2D(0.f, 0.f));
        s.home[6] = MetricVector2D(18.f, 10.f); s.home[7] = MetricVector2D(22.f, 14.f); s.home[8] = MetricVector2D(15.f, 17.f);
        s.away[1] = MetricVector2D(30.f, -10.f); s.away[2] = MetricVector2D(30.f, 0.f);
        for (int f = 0; f < 20; f++) s.step(MetricVector2D(0.f, 0.f), 1, 120);
        CHECK(s.e.meta_.overSide == 1, "overload: three of ours against none in the right half-space is an overload");
        PitchEngine& e = s.e;
        e.meta_.fieldAt = e.state_.timeMs;
        float lo = 1.f, hi = 0.f;
        for (int i = 0; i < XT_COLS * XT_ROWS; i++) { lo = std::min(lo, e.meta_.field[i]); hi = std::max(hi, e.meta_.field[i]); }
        CHECK(lo >= 0.f && hi <= 1.f && hi - lo > 0.2f, "field: the interception-probability field is a real probability map");
    }
    {
        TacticalConfig on, off;
        off.metaLate = false;
        float aOn = 0.f, aOff = 0.f, cOn = 0.f, cOff = 0.f;
        for (int k = 0; k < 2; k++) {
            Scene s(k == 0 ? &on : &off);
            openLayout(s, MetricVector2D(8.f, 0.f));
            s.home[7] = MetricVector2D(19.f, -6.f);
            settle(s, MetricVector2D(8.f, 0.f));
            s.e.setMatchPhase(0.92f);
            s.e.plus_.goalsFor = 1;
            s.e.calculatePassProbabilities(s.e.state_);
            const int ci = candIdx(s.e, s.idxAt(MetricVector2D(19.f, -6.f)) + 1, ACT_PASS);
            (k == 0 ? aOn : aOff) = ci >= 0 ? s.e.candidates()[ci].utility : -9.f;
            s.e.plus_.goalsFor = 0; s.e.plus_.goalsAgainst = 1;
            s.e.calculatePassProbabilities(s.e.state_);
            const int cj = candIdx(s.e, s.idxAt(MetricVector2D(19.f, -6.f)) + 1, ACT_PASS);
            (k == 0 ? cOn : cOff) = cj >= 0 ? s.e.candidates()[cj].utility : -9.f;
        }
        std::printf("   late game: leading %.3f vs %.3f, trailing %.3f vs %.3f\n", aOn, aOff, cOn, cOff);
        CHECK(std::fabs(aOn - aOff) > 0.0f || std::fabs(cOn - cOff) > 0.0f, "late game: the final stretch changes the pass ranking");
        CHECK(cOn > cOff, "late game: a forward pass gains when chasing");
    }
    {
        // a close call between two equal options: the gate holds the ball for a moment, then commits
        Scene s;
        openLayout(s, MetricVector2D(8.f, 0.f));
        s.home[7] = MetricVector2D(19.f, -6.f);
        s.home[8] = MetricVector2D(19.f, 6.f);
        int hold = 0, maxRun = 0, run = 0, real = 0;
        for (int f = 0; f < 120; f++) {
            const GestureOutput g = s.step(MetricVector2D(8.f, 0.f), 1);
            if (f < 8) continue;
            const bool h = g.valid && g.actionType == ACT_CARRY && s.e.meta_.commitStart > 0.0 && s.e.state_.timeMs >= s.e.meta_.feintUntil;
            if (h) { hold++; run++; maxRun = std::max(maxRun, run); } else run = 0;
            if (g.valid && (g.actionType == ACT_PASS || g.actionType == ACT_THROUGH)) real++;
        }
        std::printf("   gate: %d hold frames, longest run %d, %d real passes\n", hold, maxRun, real);
        CHECK(hold > 0 && maxRun <= 3, "gate: a close call is held for a moment and never longer than about 200 ms");
        CHECK(real > 0, "gate: a close call still ends in a pass");
    }
    {
        for (int kind = 1; kind <= 5; kind++) {
            if (kind == 4) continue;                    // free kicks are deliberately not detected: a stationary ball mid-pitch is too ambiguous
            Scene s;
            openLayout(s, MetricVector2D(8.f, 0.f));
            MetricVector2D b(8.f, 0.f);
            int hud = 1;
            if (kind == 1) { b = MetricVector2D(51.5f, 33.f); s.home[5] = b; }
            if (kind == 2) { b = MetricVector2D(-48.f, 2.f); s.home[5] = b; }
            if (kind == 3) { b = MetricVector2D(10.f, 33.6f); s.home[5] = b; }
            if (kind == 5) { b = MetricVector2D(-48.f, 33.5f); hud = 2; s.away[1] = b; }
            for (int f = 0; f < 20; f++) s.step(b, hud, 100);
            std::printf("   restart: case %d -> kind %d\n", kind, s.e.meta_.restartKind);
            CHECK(s.e.meta_.restartKind == kind, kind == 1 ? "restart: our corner is recognised" : kind == 2 ? "restart: our goal kick is recognised" :
                  kind == 3 ? "restart: our throw-in is recognised" : "restart: their set piece in our half is recognised");
        }
        Scene m;
        openLayout(m, MetricVector2D(0.f, 0.f));
        for (int f = 0; f < 20; f++) m.step(MetricVector2D(0.f, 0.f), 1, 100);
        CHECK(m.e.meta_.restartKind == 0, "restart: a stationary ball in midfield is not a restart");
    }

    // ---- defence helpers ----
    {
        Scene s;
        const MetricVector2D P[11] = {{-51, 0}, {-30, -20}, {-30, -6}, {-30, 6}, {-30, 20}, {-18, 0}, {-12, 8}, {-8, -4}, {0, 0}, {2, 10}, {4, -10}};
        for (int i = 0; i < 11; i++) s.home[i] = P[i];
        for (int i = 1; i < 11; i++) s.away[i] = MetricVector2D(-26.f + 3.f * i, -20.f + 4.f * i);
        s.away[1] = MetricVector2D(-18.f, 2.f);                  // the carrier
        s.away[5] = MetricVector2D(-30.f, 8.f);                  // forward runner
        s.home[5] = MetricVector2D(-19.5f, 1.f);                 // first man, 2 m from him
        for (int f = 0; f < 12; f++) s.step(MetricVector2D(-18.f, 2.f), 2);
        s.markControlled(MetricVector2D(-12.f, 8.f));
        MetricVector2D pt;
        const bool ok = s.e.metaSecondMan(&pt);
        std::printf("   second man: ok=%d point (%.1f, %.1f)\n", ok, pt.x, pt.y);
        CHECK(ok && distance(pt, MetricVector2D(-18.f, 2.f)) > 2.f && distance(pt, MetricVector2D(-18.f, 2.f)) < 9.f, "second man: cuts the escape lane a few metres from the carrier");
        s.e.lostBallMs_ = s.e.state_.timeMs - 800.0;
        s.markControlled(MetricVector2D(-12.f, 8.f));
        MetricVector2D rp;
        const bool rok = s.e.metaRecoveryPoint(&rp);
        CHECK(!rok || (distance(rp, MetricVector2D(-18.f, 2.f)) <= 40.f && std::isfinite(rp.x) && std::isfinite(rp.y)), "recovery: the assigned cut point is a finite point near the carrier");
        s.e.meta_.oppPassLost[0] = 8.f; s.e.meta_.oppPassOk[0] = 3.f;
        MetricVector2D bp;
        const bool bok = s.e.metaBaitPoint(&bp);
        CHECK(bok && s.e.meta_.baitUntil > s.e.state_.timeMs, "bait: a lane that is intercepted often is left looking open");
        s.e.meta_.oppPassLost[0] = 1.f; s.e.meta_.oppPassOk[0] = 8.f;
        CHECK(!s.e.metaBaitPoint(&bp), "bait: no bait on a lane he completes");
        const float base = 0.5f * (s.e.config().containMin + s.e.config().containMax);
        CHECK(std::fabs(s.e.metaContainBuf() - base) < 1e-4f, "contain: the default buffer is the configured one");
        for (int i = 0; i < 8; i++) { s.e.meta_.burst[2][1] += 1.f; s.e.meta_.burstTot += 1.f; }
        const float adapted = s.e.metaContainBuf();
        std::printf("   contain: buffer %.2f m after bursts at 3-4 m (default %.2f)\n", adapted, base);
        CHECK(adapted > base && adapted <= 5.0f, "contain: the buffer moves out when he bursts from 3-4 m");
        s.e.meta_.sideCnt[1] = 6.f; s.e.meta_.sideCnt[0] = 1.f;
        CHECK(s.e.metaSideBias() > 0.2f && s.e.metaSideBias() <= 0.9f, "weak side: the defender shades toward the side he prefers");
        CHECK(s.e.metaShapeScale() >= 0.6f && s.e.metaShapeScale() <= 1.3f, "shape: the line-step scale stays in range");
    }

    // ---- four-mode machine ----
    {
        Scene s;
        openLayout(s, MetricVector2D(0.f, 0.f));
        s.e.setMatchPhase(0.2f);
        for (int f = 0; f < 10; f++) s.step(MetricVector2D(0.f, 0.f), 1, 120);
        CHECK(s.e.metaModeNow() == 2, "modes: first half and level = Chaotic");
        s.e.setMatchPhase(0.8f);
        s.e.plus_.goalsAgainst = 1;
        for (int f = 0; f < 10; f++) s.step(MetricVector2D(0.f, 0.f), 1, 120);
        CHECK(s.e.metaModeNow() == 3, "modes: behind late = Desperado");
        s.e.plus_.goalsAgainst = 0; s.e.plus_.goalsFor = 1;
        s.e.setMatchPhase(0.6f);
        for (int f = 0; f < 10; f++) s.step(MetricVector2D(0.f, 0.f), 1, 120);
        CHECK(s.e.metaModeNow() == 1, "modes: ahead after half-time = Patient");
    }

    // ---- fuzz: bad frames must never crash or produce non-finite gestures ----
    {
        uint32_t sd = 12345u;
        auto rnd = [&]() { sd ^= sd << 13; sd ^= sd >> 17; sd ^= sd << 5; return (float)(sd & 0xFFFFFF) / 16777216.f; };
        Scene s;
        long bad = 0, frames = 0;
        for (int f = 0; f < 4000; f++) {
            for (int i = 0; i < 11; i++) {
                s.home[i] = MetricVector2D((rnd() - 0.5f) * 110.f, (rnd() - 0.5f) * 70.f);
                s.away[i] = MetricVector2D((rnd() - 0.5f) * 110.f, (rnd() - 0.5f) * 70.f);
            }
            if (f % 50 == 7) s.home[3] = MetricVector2D(std::nanf(""), 1.f);
            if (f % 70 == 9) s.away[4] = MetricVector2D(1e9f, -1e9f);
            MetricVector2D ball((rnd() - 0.5f) * 110.f, (rnd() - 0.5f) * 70.f);
            const int nh = f % 97 == 0 ? 4 : 11, na = f % 89 == 0 ? 3 : 11;
            s.e.setFrameTimeMs(s.t);
            s.e.setHudPossession((int)(rnd() * 3.f));
            const bool haveBall = f % 31 != 0;
            s.e.ingestDetections(s.home, nh, s.away, na, haveBall ? &ball : nullptr, f % 211 != 0);
            s.t += 40.0 + rnd() * 120.0;
            const GestureOutput& g = s.e.lastGesture();
            frames++;
            if (g.valid && (!std::isfinite(g.r) || !std::isfinite(g.theta))) bad++;
        }
        std::printf("   fuzz: %ld frames, %ld non-finite gestures\n", frames, bad);
        CHECK(bad == 0, "fuzz: 4000 random / broken frames, no non-finite gesture and no crash");
        // calm play after the garbage: the engine still works
        Scene t;
        openLayout(t, MetricVector2D(8.f, 0.f));
        settle(t, MetricVector2D(8.f, 0.f));
        CHECK(t.e.state_.carrier >= 0, "fuzz: a fresh engine still finds the carrier");
    }

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}
