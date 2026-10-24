// Upgrade pack: live opponent line, role inference, two-step lookahead, ball-flight prediction, loose-ball
// interception, counter-press, stuck / radar-glitch recovery, in-match pass-safety learning, match summary.
// Build: g++ -std=c++17 -O1 -I../app/src/main/cpp upgrade_checks.cpp ../app/src/main/cpp/pitch_engine.cpp
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cstdio>
#include <cmath>
#define private public   // the test supplies the controlled-player marker that normally comes from pixels
#include "pitch_engine.hpp"
using namespace pe;
static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) std::printf("ok:   %s\n", msg); else { std::printf("FAIL: %s\n", msg); g_fail++; } } while (0)

struct Scene {
    PitchEngine e;
    MetricVector2D home[11], away[11];
    double t = 1000;
    explicit Scene(bool upgrades = true) {
        TacticalConfig c;
        c.attackDir = 1;
        c.upgrades = upgrades;
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
};

int main() {
    {
        TacticalConfig d;
        CHECK(d.upgrades && d.lookahead && d.counterPress && d.recovery && d.online && d.oppLine && d.roles && d.looseChase,
              "defaults: the upgrade pack is on in the production rule set");
    }

    // ---- 1. role inference on a 4-3-3 ----
    {
        Scene s;
        const MetricVector2D gk(-51, 0), lb(-35, -22), lcb(-36, -8), rcb(-36, 8), rb(-35, 22);
        const MetricVector2D dm(-20, 0), cm(-17, 10), am(-13, -8);
        const MetricVector2D lw(-3, -22), st(-1, 0), rw(-3, 22);
        const MetricVector2D P[11] = {gk, lb, lcb, rcb, rb, dm, cm, am, lw, st, rw};
        for (int i = 0; i < 11; i++) s.home[i] = P[i];
        for (int i = 0; i < 11; i++) s.away[i] = MetricVector2D(10.f + 3.f * i, -20.f + 4.f * i);
        for (int f = 0; f < 6; f++) s.step(MetricVector2D(0.f, 0.f));
        const PitchEngine& e = s.e;
        CHECK(e.roleOf(s.idxAt(gk)) == ROLE_GK, "roles: the deepest player is the goalkeeper");
        CHECK(e.roleOf(s.idxAt(lcb)) == ROLE_CB && e.roleOf(s.idxAt(rcb)) == ROLE_CB, "roles: the two central defenders are CB");
        CHECK(e.roleOf(s.idxAt(lb)) == ROLE_FB && e.roleOf(s.idxAt(rb)) == ROLE_FB, "roles: the wide defenders are FB");
        CHECK(e.roleOf(s.idxAt(dm)) == ROLE_DM, "roles: the deepest midfielder is the DM");
        CHECK(e.roleOf(s.idxAt(cm)) == ROLE_CM, "roles: the middle one is the CM");
        CHECK(e.roleOf(s.idxAt(am)) == ROLE_AM, "roles: the most advanced midfielder is the AM");
        CHECK(e.roleOf(s.idxAt(lw)) == ROLE_WG && e.roleOf(s.idxAt(rw)) == ROLE_WG, "roles: wide forwards are wingers");
        CHECK(e.roleOf(s.idxAt(st)) == ROLE_ST, "roles: the central forward is the striker");
        // a single sprint forward must not flip a centre-back into a midfielder at once (smoothed ranks)
        const int runner = s.idxAt(lcb);
        for (int i = 0; i < 11; i++) if (s.home[i].x == lcb.x && s.home[i].y == lcb.y) s.home[i].x = -15.f;
        s.step(MetricVector2D(0.f, 0.f));
        CHECK(e.roleOf(runner) == ROLE_CB, "roles: one frame of a forward run does not change the role");
    }
    {
        Scene s(false);
        for (int f = 0; f < 6; f++) s.step(MetricVector2D(0.f, 0.f));
        bool any = false;
        for (int i = 0; i < 11; i++) if (s.e.roleOf(i) != ROLE_UNK) any = true;
        CHECK(!any && s.e.oppLineConf() == 0.f, "switch: with upgrades off no role or line estimate is produced");
    }

    // ---- 2. live opponent line ----
    {
        Scene s;
        auto setLine = [&](float x) {
            s.away[0] = MetricVector2D(51.f, 0.f);
            for (int k = 1; k <= 4; k++) s.away[k] = MetricVector2D(x, -15.f + 10.f * (k - 1));
            for (int k = 5; k < 11; k++) s.away[k] = MetricVector2D(x - 12.f - 2.f * k, -20.f + 7.f * (k - 5));
        };
        setLine(17.f);   // 35 m from their goal: a high line
        for (int f = 0; f < 60; f++) s.step(MetricVector2D(0.f, 0.f));
        CHECK(std::fabs(s.e.oppLineDepth() - 35.f) < 2.5f, "line: a back line 35 m from their goal is measured as about 35 m");
        CHECK(s.e.oppLineConf() > 0.6f, "line: a tight, fully seen line gives high confidence");
        CHECK(std::fabs(s.e.oppLineWidth() - 30.f) < 3.f, "line: width of the four-man line is about 30 m");
        float vMin = 0.f;
        for (int f = 0; f < 60; f++) {
            setLine(17.f + 0.35f * (float)(f + 1));     // they drop 0.35 m per frame: the line goes deep
            s.step(MetricVector2D(0.f, 0.f));
            vMin = std::min(vMin, s.e.oppLineVel());
        }
        CHECK(s.e.oppLineDepth() < 25.f, "line: it follows the opponent dropping back mid-half (continuous, not half-time only)");
        CHECK(vMin < -0.5f, "line: the velocity reads negative while they drop");
    }

    // ---- 3. lookahead and line height act on the pass utility ----
    {
        Scene s;
        s.e.cfg_.roles = false; s.e.cfg_.oppLine = false;
        for (int i = 0; i < 11; i++) s.home[i] = MetricVector2D(-40.f + 1.f * i, -20.f + 4.f * i);   // everybody deep and behind
        s.home[0] = MetricVector2D(-51.f, 0.f);
        s.home[5] = MetricVector2D(-2.f, 0.f);       // carrier
        s.home[6] = MetricVector2D(36.f, 0.f);       // receiver A: a shooting position
        s.home[7] = MetricVector2D(4.f, 26.f);       // receiver B: a harmless flank spot
        for (int i = 0; i < 11; i++) s.away[i] = MetricVector2D(-20.f + 2.f * i, 30.f - 5.f * i);
        for (int f = 0; f < 5; f++) s.step(MetricVector2D(-2.f, 0.f));
        s.e.state_.carrier = s.idxAt(MetricVector2D(-2.f, 0.f));
        const PlayerState& car = s.e.state_.team[s.e.state_.carrier];
        const int ia = s.idxAt(MetricVector2D(36.f, 0.f)), ib = s.idxAt(MetricVector2D(4.f, 26.f));
        PassCandidate a, b;
        a.type = b.type = ACT_PASS; a.xP = b.xP = 0.8f;
        a.targetId = s.e.state_.team[ia].id; b.targetId = s.e.state_.team[ib].id;
        a.target = MetricVector2D(36.f, 0.f); b.target = MetricVector2D(4.f, 26.f);
        const float ba = s.e.upgradeBonus(car, a), bb = s.e.upgradeBonus(car, b);
        CHECK(ba > bb + 0.008f, "lookahead: a pass to a man who can shoot next scores above one to a dead-end spot");
        s.e.cfg_.lookahead = false;
        CHECK(std::fabs(s.e.upgradeBonus(car, a)) < 1e-6f, "lookahead: off means no bonus");
        s.e.cfg_.lookahead = true;
        // a good second pass also counts: B now has a runner far up the pitch
        s.home[8] = MetricVector2D(24.f, 16.f);
        for (int f = 0; f < 5; f++) s.step(MetricVector2D(-2.f, 0.f));
        s.e.state_.carrier = s.idxAt(MetricVector2D(-2.f, 0.f));
        const PlayerState& car2 = s.e.state_.team[s.e.state_.carrier];
        CHECK(s.e.upgradeBonus(car2, b) > bb + 0.005f, "lookahead: a receiver with an on-pass towards goal gains value");
    }
    {
        auto lineBonus = [&](float lineX, float recvX) {
            Scene s;
            s.e.cfg_.roles = false; s.e.cfg_.lookahead = false;
            s.away[0] = MetricVector2D(51.f, 0.f);
            for (int k = 1; k <= 4; k++) s.away[k] = MetricVector2D(lineX, -15.f + 10.f * (k - 1));
            for (int k = 5; k < 11; k++) s.away[k] = MetricVector2D(lineX - 14.f, -20.f + 7.f * (k - 5));
            for (int i = 0; i < 11; i++) s.home[i] = MetricVector2D(-40.f + 1.f * i, -20.f + 4.f * i);
            s.home[0] = MetricVector2D(-51.f, 0.f);
            s.home[5] = MetricVector2D(0.f, 0.f);
            for (int f = 0; f < 80; f++) s.step(MetricVector2D(0.f, 0.f));
            s.e.state_.carrier = s.idxAt(MetricVector2D(0.f, 0.f));
            PassCandidate c;
            c.type = ACT_THROUGH; c.xP = 0.8f; c.targetId = 0; c.target = MetricVector2D(recvX, 0.f);
            return s.e.upgradeBonus(s.e.state_.team[s.e.state_.carrier], c);
        };
        const float high = lineBonus(17.f, 30.f);        // line 35 m from goal, ball in behind
        const float deep = lineBonus(41.f, 47.f);        // line 11 m from goal, ball in behind
        CHECK(high > 0.02f, "line height: a through ball in behind a HIGH line is rewarded");
        CHECK(deep < 0.f, "line height: a through ball in behind a DEEP block is discouraged");
        CHECK(high > deep + 0.03f, "line height: the same pass is worth clearly more against a high line");
    }

    // ---- 4. ball-flight prediction ----
    {
        Scene s;
        for (int f = 0; f < 6; f++) s.step(MetricVector2D(-10.f + 0.8f * (float)f, 0.f));     // 10 m/s along +x
        MetricVector2D p, v; bool out = true;
        const bool ok = s.e.predictBall(1.0f, &p, &v, &out);
        const float bx = s.e.state_.ball.x, bv = s.e.state_.ballVel.x;
        const float k = s.e.cfg_.ballFric;
        const float expect = bx + bv * (1.f - std::exp(-k)) / k;
        CHECK(ok && std::fabs(p.x - expect) < 0.05f && !out, "ball flight: position after 1 s follows rolling friction");
        CHECK(v.x < bv && v.x > 0.f, "ball flight: speed decays but the ball keeps moving");
        Scene s2;
        for (int f = 0; f < 6; f++) s2.step(MetricVector2D(40.f + 1.4f * (float)f, 0.f));      // 17 m/s toward the end line
        const bool ok2 = s2.e.predictBall(1.5f, &p, nullptr, &out);
        CHECK(ok2 && out && std::fabs(p.x) <= PITCH_HALF_X, "ball flight: a ball running out is flagged and clamped to the pitch");
    }

    // ---- 5. loose-ball interception ----
    {
        auto run = [&](bool opponentNear, int* presses) {
            Scene s;
            for (int i = 0; i < 11; i++) { s.home[i] = MetricVector2D(-40.f + 1.f * i, -25.f + 5.f * i); s.away[i] = MetricVector2D(30.f + 1.f * i, -25.f + 5.f * i); }
            s.home[0] = MetricVector2D(-51.f, 0.f);
            const MetricVector2D ctrl(8.f, 0.5f);
            s.home[5] = ctrl;
            if (opponentNear) s.away[3] = MetricVector2D(7.f, -1.0f);
            *presses = 0;
            for (int f = 0; f < 14; f++) {
                const MetricVector2D ball(-6.f + 0.56f * (float)f, 0.f);        // 7 m/s toward the controlled player
                s.t += 0;
                s.e.setFrameTimeMs(s.t); s.e.ctrlSeen_ = true; s.e.ctrlPos_ = ctrl; s.e.ctrlSeenMs_ = s.t;
                s.e.ingestDetections(s.home, 11, s.away, 11, &ball, true);
                s.t += 80;
                const GestureOutput& g = s.e.lastGesture();
                if (f >= 5 && g.valid && g.status == ST_LOOSE && g.actionType == ACT_PRESS) (*presses)++;
            }
        };
        int free_ = 0, contested = 0;
        run(false, &free_);
        run(true, &contested);
        CHECK(free_ > 0, "loose ball: the controlled player goes for a ball he wins the race to");
        CHECK(contested == 0, "loose ball: no chase when an opponent gets there first");
    }

    // ---- 6. counter-press ----
    {
        auto run = [&](bool cp, int* presses, int* dashes, int* count, int* firstDash) {
            Scene s;
            s.e.cfg_.counterPress = cp;
            for (int i = 0; i < 11; i++) { s.home[i] = MetricVector2D(-40.f + 2.f * i, -22.f + 4.4f * i); s.away[i] = MetricVector2D(25.f + 1.f * i, -25.f + 5.f * i); }
            s.home[0] = MetricVector2D(-51.f, 0.f);
            const MetricVector2D ctrl(5.2f, 0.2f);
            s.home[5] = ctrl;                                                   // our controlled player is the carrier
            s.home[6] = MetricVector2D(2.f, 5.f); s.home[7] = MetricVector2D(3.f, -5.f); s.home[8] = MetricVector2D(-2.f, 1.f);   // numbers around the ball
            for (int f = 0; f < 15; f++) s.step(MetricVector2D(5.f, 0.f), 1);   // we have it
            s.away[4] = MetricVector2D(9.3f, 0.f);                              // an opponent wins it and takes it away
            const MetricVector2D ball(9.0f, 0.f);
            *presses = *dashes = 0;
            *firstDash = -1;
            for (int f = 0; f < 36; f++) {                                      // 2.9 s
                s.e.ctrlSeen_ = true; s.e.ctrlPos_ = ctrl; s.e.ctrlSeenMs_ = s.t;
                const GestureOutput g = s.step(ball, 2);
                if (g.valid && g.actionType == ACT_PRESS) { (*presses)++; if (g.dash) { (*dashes)++; if (*firstDash < 0) *firstDash = f; } }
            }
            *count = s.e.counterPressCount();
        };
        int p1, d1, c1, p0, d0, c0, f1, f0;
        run(true, &p1, &d1, &c1, &f1);
        run(false, &p0, &d0, &c0, &f0);
        CHECK(c1 == 1 && c0 == 0, "counter-press: one swarm is counted per turnover (and none when switched off)");
        CHECK(d1 > d0 && d1 >= 8, "counter-press: the controlled player sprints at the ball for the first seconds");
        CHECK(f1 >= 0 && f1 <= 4 && f0 < 0, "counter-press: the sprint starts within about 0.3 s of the turnover (the containing shape never sprints here)");
        // far from the ball in our own deepest third: shape first, no swarm
        Scene s;
        for (int i = 0; i < 11; i++) { s.home[i] = MetricVector2D(-40.f + 2.f * i, -22.f + 4.4f * i); s.away[i] = MetricVector2D(-10.f + 1.f * i, -25.f + 5.f * i); }
        s.home[0] = MetricVector2D(-51.f, 0.f);
        const MetricVector2D ball(-34.f, 0.f), ctrl(-30.f, 2.f);
        s.home[5] = MetricVector2D(-33.8f, 0.2f);
        for (int f = 0; f < 15; f++) s.step(ball, 1);
        s.away[4] = MetricVector2D(-34.3f, 0.f);
        for (int f = 0; f < 20; f++) { s.e.ctrlSeen_ = true; s.e.ctrlPos_ = ctrl; s.e.ctrlSeenMs_ = s.t; s.step(ball, 2); }
        CHECK(s.e.counterPressCount() == 0, "counter-press: not when the ball is lost deep in our own third (box shape takes priority)");
    }

    // ---- 7. recovery: radar glitch and stuck decision loop ----
    {
        Scene s;
        for (int f = 0; f < 10; f++) s.step(MetricVector2D(0.f, 0.f));
        MetricVector2D base[11];
        for (int i = 0; i < 11; i++) base[i] = s.home[i];
        for (int f = 0; f < 8; f++) {                      // the radar read jumps 30 m back and forth
            for (int i = 0; i < 11; i++) { s.home[i] = base[i]; s.home[i].x += (f % 2 ? 30.f : 0.f); s.home[i].x = std::min(s.home[i].x, 50.f); }
            s.step(MetricVector2D(0.f, 0.f));
        }
        CHECK(s.e.recoveryCount() >= 1 && !s.e.healthLog().empty(), "recovery: repeated radar jumps trigger a tracker re-acquire and are logged");
        for (int i = 0; i < 11; i++) s.home[i] = base[i];
        for (int f = 0; f < 10; f++) s.step(MetricVector2D(0.f, 0.f));
        int act = 0;
        for (int i = 0; i < 11; i++) if (s.e.state_.team[i].active) act++;
        CHECK(act >= 8, "recovery: tracking is re-acquired afterwards");
        // the real radar sees only some of the players and different ones from frame to frame: that is not a glitch
        Scene d;
        for (int f = 0; f < 10; f++) d.step(MetricVector2D(0.f, 0.f));
        MetricVector2D all[11];
        for (int i = 0; i < 11; i++) all[i] = d.home[i];
        const int recBefore = d.e.recoveryCount();
        for (int f = 0; f < 200; f++) {
            MetricVector2D sub[11];
            int k = 0;
            for (int i = 0; i < 11; i++) if (((i * 7 + f * 3) % 11) < 6) sub[k++] = all[i];     // a different 6 of 11 each frame
            d.e.setFrameTimeMs(d.t);
            const MetricVector2D ball(0.f, 0.f);
            d.e.ingestDetections(sub, k, d.away, 11, &ball, true);
            d.t += 200;
        }
        CHECK(d.e.recoveryCount() == recBefore, "recovery: players dropping in and out of the radar read (6 of 11 visible, changing every frame) is not a glitch");
        CHECK(s.e.config().attackDir == 1 && s.e.state_.attackSign == 1, "recovery: configuration and attack direction survive the soft reset");
    }
    {
        Scene s;
        for (int f = 0; f < 5; f++) s.step(MetricVector2D(0.f, 0.f));
        PitchEngine& e = s.e;
        e.state_.status = ST_TEAM; e.human_ = false;
        e.gesture_ = GestureOutput(); e.gesture_.valid = true; e.gesture_.actionType = ACT_CARRY;
        e.nextDecisionAt_ = 1e9; e.cooldownUntil_ = 1e9;
        int before = e.recoveryCount();
        double t = 100000;
        for (int f = 0; f < 60; f++) { t += 100; e.state_.ball = MetricVector2D(1.f, 0.f); e.monitorHealth(t, 0.1); }
        CHECK(e.recoveryCount() == before, "recovery: 6 s of carrying is not yet 'stuck'");
        for (int f = 0; f < 10; f++) { t += 100; e.monitorHealth(t, 0.1); }
        CHECK(e.recoveryCount() == before + 1 && e.nextDecisionAt_ == 0 && e.cooldownUntil_ == 0, "recovery: a stuck loop clears the decision timers");
        CHECK(t - e.holdStartAt_ > 1e5, "recovery: the hold cap is marked exceeded so the ball is released");
        // moving ball: never stuck
        before = e.recoveryCount();
        for (int f = 0; f < 120; f++) { t += 100; e.state_.ball = MetricVector2D(0.3f * (float)f, 0.f); e.monitorHealth(t, 0.1); }
        CHECK(e.recoveryCount() == before, "recovery: a ball that is travelling never counts as stuck");
    }

    // ---- 8. online learning ----
    {
        Scene s;
        for (int i = 0; i < 16; i++) s.e.noteOutcome(false);
        const float up = s.e.onlinePassAdj();
        CHECK(up > 0.03f && up <= 0.08f + 1e-4f, "online: a run of lost passes raises the safety threshold, within the limit");
        for (int i = 0; i < 80; i++) s.e.noteOutcome(true);
        const float down = s.e.onlinePassAdj();
        CHECK(down >= 0.f && down < up, "online: a long run of kept passes relaxes it back towards the base, never below it");
        Scene s2;
        s2.e.noteOutcome(false); s2.e.noteOutcome(false);
        CHECK(std::fabs(s2.e.onlinePassAdj()) < 0.025f, "online: two bad passes barely move it (regularised)");
        Scene s3;
        s3.e.cfg_.online = false;
        for (int i = 0; i < 20; i++) s3.e.noteOutcome(false);
        CHECK(s3.e.onlinePassAdj() == 0.f, "online: off means off");
    }

    // ---- 9. match summary ----
    {
        Scene s;
        for (int i = 0; i < 11; i++) { s.home[i] = MetricVector2D(-10.f + 2.f * i, -22.f + 4.4f * i); s.away[i] = MetricVector2D(20.f + 1.f * i, -25.f + 5.f * i); }
        s.home[0] = MetricVector2D(-51.f, 0.f);
        s.home[5] = MetricVector2D(30.f, -18.f);
        CHECK(s.e.matchSummary().find("Not enough") != std::string::npos, "summary: says so when there is too little play");
        for (int f = 0; f < 400; f++) s.step(MetricVector2D(30.2f, -18.f), 1);
        const std::string sum = s.e.matchSummary();
        CHECK(sum.find("Possession") != std::string::npos && sum.find("final third") != std::string::npos, "summary: reports possession and territory");
        CHECK(sum.find("top side") != std::string::npos, "summary: names the side we attacked down");
        s.e.clearMatchStats();
        CHECK(s.e.matchSummary().find("Not enough") != std::string::npos, "summary: cleared for a new match");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "SOME UPGRADE CHECKS FAILED" : "ALL UPGRADE CHECKS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
