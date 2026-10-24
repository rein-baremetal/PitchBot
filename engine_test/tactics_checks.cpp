// Synthetic checks for: time-aware pass interception, clearing in our own box, outside-the-box power shots.
#include "pitch_engine.hpp"
#include <cstdio>
#include <cmath>
using namespace pe;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); fails++; } else std::printf("ok:   %s\n", msg); } while (0)

static void spread(MetricVector2D* h, MetricVector2D* a) {
    for (int i = 0; i < 11; i++) {
        h[i] = MetricVector2D(-45.f + 3.f * i, -25.f + 5.f * (i % 6));
        a[i] = MetricVector2D(30.f + 2.f * (i % 4), -28.f + 5.5f * i);
    }
}

int main() {
    // 1) a defender sitting in the lane makes a pass unattractive, a clear lane is fine
    {
        PitchEngine e; TacticalConfig c = e.config(); c.longShotMinXg = 0.f; c.longShotXp = 0.62f; c.longShotFree = 3.5f; c.stunLaneMin = 1.2f; c.stunSafe = 2.4f; c.attackDir = 1; e.setConfig(c);
        MetricVector2D h[11], a[11]; spread(h, a);
        h[10] = MetricVector2D(0.f, 0.f);          // carrier
        h[9] = MetricVector2D(14.f, 0.f);          // receiver straight ahead
        h[8] = MetricVector2D(0.f, 14.f);          // receiver to the side
        a[0] = MetricVector2D(7.f, 0.f);           // defender in the middle of the forward lane
        MetricVector2D ball(0.3f, 0.f);
        for (int f = 0; f < 40; f++) { e.setFrameTimeMs(1000.0 + f * 60.0); e.setHudPossession(1); e.ingestDetections(h, 11, a, 11, &ball, true); }
        float xBlocked = -1, xSide = -1;
        for (int i = 0; i < e.candidateCount(); i++) {
            const PassCandidate& pc = e.candidates()[i];
            if (pc.type != ACT_PASS) continue;
            if (std::fabs(pc.target.x - 14.f) < 3.f && std::fabs(pc.target.y) < 3.f) xBlocked = pc.xP;
            if (std::fabs(pc.target.y - 14.f) < 3.f && std::fabs(pc.target.x) < 3.f) xSide = pc.xP;
        }
        std::printf("   xP through defender %.2f, clear side lane %.2f\n", xBlocked, xSide);
        CHECK(xBlocked < 0.25f, "pass straight through a defender is pruned or rated as very likely intercepted (-1 = not offered)");
        CHECK(xSide > 0.70f, "short pass on a clear lane is rated safe");
    }
    // 2) in our own box with a defender close and no safe pass, the bot clears
    {
        PitchEngine e; TacticalConfig c = e.config(); c.longShotMinXg = 0.f; c.longShotXp = 0.62f; c.longShotFree = 3.5f; c.stunLaneMin = 1.2f; c.stunSafe = 2.4f; c.attackDir = 1; e.setConfig(c);
        MetricVector2D h[11], a[11];
        for (int i = 0; i < 11; i++) { h[i] = MetricVector2D(10.f + 3.f * i, -20.f + 4.f * i); a[i] = MetricVector2D(5.f + 2.f * i, -25.f + 5.f * i); }
        h[10] = MetricVector2D(-44.f, 3.f);        // carrier inside our box (own goal at -52.5)
        a[0] = MetricVector2D(-39.f, 3.f);         // pressing opponent 5 m away
        a[1] = MetricVector2D(-35.f, -4.f);
        a[2] = MetricVector2D(-30.f, 8.f);
        MetricVector2D ball(-43.7f, 3.f);
        bool cleared = false;
        for (int f = 0; f < 60; f++) {
            e.setFrameTimeMs(1000.0 + f * 60.0); e.setHudPossession(1);
            e.ingestDetections(h, 11, a, 11, &ball, true);
            const GestureOutput& g = e.lastGesture();
            if (g.valid && g.actionType == ACT_SHOT) { cleared = true; break; }
        }
        CHECK(cleared, "pressed in its own box with no safe pass, the bot clears the ball");
    }
    // 3) outside the box, open lane, nobody close: power shot
    {
        PitchEngine e; TacticalConfig c = e.config(); c.longShotMinXg = 0.f; c.longShotXp = 0.62f; c.longShotFree = 3.5f; c.stunLaneMin = 1.2f; c.stunSafe = 2.4f; c.attackDir = 1; c.autoShoot = true; e.setConfig(c);
        e.setShootConfig(true, 520, true);
        MetricVector2D h[11], a[11]; spread(h, a);
        for (int i = 0; i < 11; i++) a[i] = MetricVector2D(-20.f + 3.f * i, -30.f + 6.f * (i % 3) * 0.f - 30.f + 0.f);
        for (int i = 0; i < 11; i++) a[i].y = (i % 2 ? 31.f : -31.f);            // everyone out on the flanks
        a[10] = MetricVector2D(50.f, 0.f);                                      // keeper on the line
        h[10] = MetricVector2D(26.f, 0.f);                                      // 26 m from goal, central
        MetricVector2D ball(26.3f, 0.f);
        bool shot = false;
        for (int f = 0; f < 80; f++) {
            e.setFrameTimeMs(1000.0 + f * 60.0); e.setHudPossession(1);
            e.ingestDetections(h, 11, a, 11, &ball, true);
            const GestureOutput& g = e.lastGesture();
            if (g.valid && g.actionType == ACT_SHOT) { shot = true; break; }
        }
        CHECK(shot, "open lane from outside the box triggers a power shot");
    }
    // 4) never pass to the keeper or to players hugging our goal line
    {
        PitchEngine e; TacticalConfig c = e.config(); c.longShotMinXg = 0.f; c.longShotXp = 0.62f; c.longShotFree = 3.5f; c.stunLaneMin = 1.2f; c.stunSafe = 2.4f; c.attackDir = 1; e.setConfig(c);
        MetricVector2D h[11], a[11]; spread(h, a);
        h[10] = MetricVector2D(-20.f, 0.f);        // carrier
        h[0]  = MetricVector2D(-50.f, 0.f);        // goalkeeper, wide open
        h[1]  = MetricVector2D(-45.f, 8.f);        // defender on the goal line
        h[2]  = MetricVector2D(-12.f, 10.f);       // normal outfield option
        MetricVector2D ball(-19.7f, 0.f);
        for (int f = 0; f < 30; f++) { e.setFrameTimeMs(1000.0 + f * 60.0); e.setHudPossession(1); e.ingestDetections(h, 11, a, 11, &ball, true); }
        bool towardGoal = false, outfield = false;
        for (int i = 0; i < e.candidateCount(); i++) {
            const PassCandidate& pc = e.candidates()[i];
            if (pc.type != ACT_PASS && pc.type != ACT_THROUGH && pc.type != ACT_LOFTED) continue;
            if (pc.target.x < -38.f) towardGoal = true;
            if (std::fabs(pc.target.x + 12.f) < 4.f) outfield = true;
        }
        CHECK(!towardGoal, "no pass candidate to the keeper or a player on our goal line");
        CHECK(outfield, "normal outfield receivers are still offered");
    }
    // 5) inside the box with a lane: shoots straight away
    {
        PitchEngine e; TacticalConfig c = e.config(); c.longShotMinXg = 0.f; c.longShotXp = 0.62f; c.longShotFree = 3.5f; c.stunLaneMin = 1.2f; c.stunSafe = 2.4f; c.attackDir = 1; c.autoShoot = true; e.setConfig(c);
        e.setShootConfig(true, 520, true);
        MetricVector2D h[11], a[11]; spread(h, a);
        for (int i = 0; i < 11; i++) a[i] = MetricVector2D(10.f + 2.f * i, (i % 2 ? 28.f : -28.f));
        a[10] = MetricVector2D(51.5f, 0.f);        // keeper
        a[9]  = MetricVector2D(44.f, -9.f);        // a defender, not on the lane
        h[10] = MetricVector2D(41.f, 4.f);         // 11.5 m from goal
        MetricVector2D ball(41.3f, 4.f);
        int shotFrame = -1;
        for (int f = 0; f < 40; f++) {
            e.setFrameTimeMs(1000.0 + f * 60.0); e.setHudPossession(1);
            e.ingestDetections(h, 11, a, 11, &ball, true);
            const GestureOutput& g = e.lastGesture();
            if (g.valid && g.actionType == ACT_SHOT) { shotFrame = f; break; }
        }
        std::printf("   in-box shot fired at frame %d\n", shotFrame);
        CHECK(shotFrame >= 0 && shotFrame <= 8, "inside the box the bot shoots within a few frames, no hesitation");
    }
    // 6) a pass through a defender is vetoed while a clear pass exists
    {
        PitchEngine e; TacticalConfig c = e.config(); c.longShotMinXg = 0.f; c.longShotXp = 0.62f; c.longShotFree = 3.5f; c.stunLaneMin = 1.2f; c.stunSafe = 2.4f; c.attackDir = 1; e.setConfig(c);
        MetricVector2D h[11], a[11]; spread(h, a);
        h[10] = MetricVector2D(0.f, 0.f);
        h[9] = MetricVector2D(15.f, 0.f);
        h[8] = MetricVector2D(0.f, 12.f);
        a[0] = MetricVector2D(7.5f, 0.2f);
        MetricVector2D ball(0.3f, 0.f);
        for (int f = 0; f < 30; f++) { e.setFrameTimeMs(1000.0 + f * 60.0); e.setHudPossession(1); e.ingestDetections(h, 11, a, 11, &ball, true); }
        float pBlocked = -1, pClear = -1;
        for (int i = 0; i < e.candidateCount(); i++) {
            const PassCandidate& pc = e.candidates()[i];
            if (pc.type != ACT_PASS) continue;
            if (std::fabs(pc.target.x - 15.f) < 3.f && std::fabs(pc.target.y) < 3.f) pBlocked = pc.prob;
            if (std::fabs(pc.target.y - 12.f) < 3.f && std::fabs(pc.target.x) < 3.f) pClear = pc.prob;
        }
        std::printf("   selection probability: through defender %.4f, clear lane %.4f\n", pBlocked, pClear);
        CHECK(pBlocked < 0.01f, "a pass through a defender is never chosen (-1 = not offered)");
    }
    std::printf(fails ? "tactics_checks: %d FAILED\n" : "tactics_checks: all passed\n", fails);
    return fails ? 1 : 0;
}
