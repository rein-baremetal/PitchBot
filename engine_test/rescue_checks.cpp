// Synthetic checks for the rescue rule set (emergency clearance, forward-only build-up, one-touch timing,
// zero-hesitation shooting, shot telemetry, wing switch, penalty-box lockout, interception cones, containment).
#define private public   // the controlled-player marker normally comes from pixels
#include "pitch_engine.hpp"
#undef private
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
using namespace pe;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) std::printf("ok:   %s\n", msg); else { std::printf("FAIL: %s\n", msg); g_fail++; } } while (0)

struct Scene {
    PitchEngine e;
    MetricVector2D home[11], away[11];
    double t = 1000;
    explicit Scene(bool rescue = true, bool autoShoot = true) {
        TacticalConfig c;
        c.attackDir = 1;
        c.rescueRules = rescue;
        c.stunEnabled = true; c.shotMaxDist = 60.f; c.aimInset = 2.86f; c.momentumGamma = 0.18f; c.shotFaceTolDeg = 35.f; c.shotSpacingMs = 0;   c.longShotMinXg = 0.f; c.longShotXp = 0.62f; c.longShotFree = 3.5f; c.stunLaneMin = 1.2f; c.stunSafe = 2.4f; c.stunCos = 0.5f; c.stunMaxMs = 1000.f;   // long-shot profile these checks were written for
        e.setConfig(c);
        e.setShootConfig(autoShoot, 520, true);
        for (int i = 0; i < 11; i++) {
            home[i] = MetricVector2D(-30.f + 3.f * i, -20.f + 4.f * i);
            away[i] = MetricVector2D(10.f + 2.f * i, 25.f - 5.f * i);
        }
        home[0] = MetricVector2D(-51.f, 0.f);   // keeper
    }
    GestureOutput step(const MetricVector2D& ball, int status = 0) {
        e.setFrameTimeMs(t);
        if (status) e.setHudPossession(status);
        e.ingestDetections(home, 11, away, 11, &ball, true);
        t += 80;
        return e.lastGesture();
    }
};

static float angleTo(float x0, float y0, float x1, float y1) {
    float a = std::atan2(y1 - y0, x1 - x0) * 180.f / 3.14159265f;
    return a < 0 ? a + 360.f : a;
}

int main() {
    // 1. emergency clearance: carrier within 14 m of our goal line, opponent within 3 m -> instant 45 degree clearance
    {
        Scene s;
        s.e.cfg_.planDirClear = false;      // this check isolates the touchline rule; the directional clear has its own checks
        s.home[5] = MetricVector2D(-45.f, 6.f);
        s.away[0] = MetricVector2D(-43.f, 6.5f);
        bool seen = false; float th = -1;
        for (int f = 0; f < 6; f++) {
            GestureOutput g = s.step(MetricVector2D(-44.8f, 6.f));
            if (g.valid && g.actionType == ACT_SHOT && !seen) { seen = true; th = g.theta; }
        }
        CHECK(seen, "panic zone: an opponent within 3 m of a deep carrier triggers a clearance");
        CHECK(seen && std::fabs(th - 45.f) < 1.5f, "the clearance leaves at 45 degrees toward the nearer touchline");
        Scene s2;
        s2.home[5] = MetricVector2D(-45.f, 6.f);
        s2.away[0] = MetricVector2D(-40.f, 6.f);
        bool needed = false;
        for (int f = 0; f < 6; f++) { s2.step(MetricVector2D(-44.8f, 6.f)); needed = needed || s2.e.panicNeeded(s2.e.state().team[std::max(0, s2.e.state().carrier)]); }
        CHECK(!needed, "an opponent 5 m away: the panic rule does not fire");
        s2.away[0] = MetricVector2D(-41.8f, 6.f);
        for (int f = 0; f < 10; f++) s2.step(MetricVector2D(-44.8f, 6.f));
        CHECK(!s2.e.panicNeeded(s2.e.state().team[std::max(0, s2.e.state().carrier)]), "an opponent 3.2 m away: still outside the 3.0 m radius");
        s2.away[0] = MetricVector2D(-42.5f, 6.f);
        for (int f = 0; f < 10; f++) s2.step(MetricVector2D(-44.8f, 6.f));
        CHECK(s2.e.panicNeeded(s2.e.state().team[std::max(0, s2.e.state().carrier)]), "an opponent 2.5 m away: the panic rule fires");
        Scene s3;
        s3.home[0] = MetricVector2D(-50.f, 2.f);
        s3.away[0] = MetricVector2D(-48.5f, 2.f);
        bool lofted = false;
        for (int f = 0; f < 6; f++) { GestureOutput g = s3.step(MetricVector2D(-49.8f, 2.f)); if (g.valid && g.actionType == ACT_LOFTED) lofted = true; }
        CHECK(lofted, "keeper under pressure: a long lofted kick on the pass button");
    }
    // 2. forward-only build-up
    {
        auto layout = [](Scene& s) {
            s.home[1] = MetricVector2D(0.f, 0.f);          // carrier
            s.home[2] = MetricVector2D(-10.f, 8.f);        // behind
            s.home[3] = MetricVector2D(0.5f, 14.f);        // level
            s.home[4] = MetricVector2D(12.f, -6.f);        // ahead
            s.home[5] = MetricVector2D(15.f, 10.f);        // ahead
            for (int i = 6; i < 11; i++) s.home[i] = MetricVector2D(-25.f + 3.f * i, -28.f);
            for (int i = 0; i < 11; i++) s.away[i] = MetricVector2D(30.f + i, -30.f + 6.f * i);
        };
        Scene s; layout(s);
        int back = 0, n = 0;
        for (int f = 0; f < 8; f++) {
            s.step(MetricVector2D(0.2f, 0.f));
            for (int i = 0; i < s.e.candidateCount(); i++) {
                const PassCandidate& c = s.e.candidates()[i];
                if (c.type != ACT_PASS && c.type != ACT_THROUGH && c.type != ACT_LOFTED) continue;
                n++;
                if (c.target.x - 0.f <= 1.0f) back++;
            }
        }
        CHECK(n > 0 && back == 0, "no backward or level pass candidates while the forward lanes are open");
        Scene legacy(false); layout(legacy);
        int backLegacy = 0;
        for (int f = 0; f < 8; f++) {
            legacy.step(MetricVector2D(0.2f, 0.f));
            for (int i = 0; i < legacy.e.candidateCount(); i++) {
                const PassCandidate& c = legacy.e.candidates()[i];
                if ((c.type == ACT_PASS || c.type == ACT_LOFTED) && c.target.x <= 1.0f) backLegacy++;
            }
        }
        CHECK(backLegacy > 0, "control: with the rule set off the backward balls are still offered");
        // heavy press and both forward lanes shut: the sideways release is allowed
        Scene p; layout(p);
        { TacticalConfig pc = p.e.config(); pc.crowdSideBand = pc.sideBand; p.e.setConfig(pc); }   // the base rule: only a level release, never backward
        p.away[0] = MetricVector2D(-2.3f, 1.0f);
        p.away[1] = MetricVector2D(1.0f, -2.8f);
        p.away[2] = MetricVector2D(12.4f, -6.0f);
        p.away[3] = MetricVector2D(15.4f, 10.0f);
        p.away[4] = MetricVector2D(6.f, -3.f);
        p.away[5] = MetricVector2D(7.f, 5.f);
        int lateral = 0, backP = 0;
        for (int f = 0; f < 8; f++) {
            p.step(MetricVector2D(0.2f, 0.f));
            for (int i = 0; i < p.e.candidateCount(); i++) {
                const PassCandidate& c = p.e.candidates()[i];
                if (c.type != ACT_PASS && c.type != ACT_LOFTED) continue;
                if (std::fabs(c.target.x) < 2.0f && c.target.y > 5.f) lateral++;
                if (c.target.x < -2.0f) backP++;
            }
        }
        CHECK(p.e.localPressure(MetricVector2D(0.f, 0.f)) >= 1.2f, "scene check: localized pressure is at least 1.2");
        CHECK(lateral > 0, "emergency release: a sideways pass is allowed under heavy press with no forward lane");
        CHECK(backP == 0, "even then, nothing goes backward");
    }
    // 3. zero-hesitation shooting
    {
        auto layout = [](Scene& s, float oppDist) {
            s.home[5] = MetricVector2D(28.f, 0.f);
            s.away[10] = MetricVector2D(51.f, 0.5f);
            for (int i = 0; i < 10; i++) s.away[i] = MetricVector2D(-20.f + 2.f * i, 20.f - 4.f * i);
            s.away[9] = MetricVector2D(28.f + oppDist, -oppDist * 0.0f);
            s.away[9].y = 12.f; s.away[9].x = 28.f;
            s.away[9].x = 28.f - oppDist; s.away[9].y = -oppDist * 0.0f;
        };
        auto framesToShot = [&](bool rescue, float oppDist, GestureOutput* out, int* carryTicks) {
            Scene s(rescue); layout(s, oppDist);
            int f = 0, ticks = 0;
            for (; f < 12; f++) {
                GestureOutput g = s.step(MetricVector2D(28.2f, 0.f));
                if (g.valid && g.actionType == ACT_SHOT) { if (out) *out = g; break; }
                if (g.valid && g.actionType == ACT_CARRY) ticks++;
            }
            if (carryTicks) *carryTicks = ticks;
            return f;
        };
        GestureOutput gNew, gOld; int tNew = 0, tOld = 0;
        const int fNew = framesToShot(true, 5.f, &gNew, &tNew);
        const int fOld = framesToShot(false, 5.f, &gOld, &tOld);
        std::printf("   frames to the first shot: rescue %d (turn ticks %d), legacy %d (turn ticks %d)\n", fNew, tNew, fOld, tOld);
        CHECK(fNew < 12, "25 m effort with nobody within 4 m: the shot is fired");
        CHECK(fNew <= fOld && tNew == 0, "no turning tick before an open shot (nearest defender at least 4 m)");
        const int fClose = framesToShot(true, 3.0f, nullptr, nullptr);
        CHECK(fClose < 12, "a defender 3 m away does not block the shot (2 m of room is enough)");
        const int fBlock = framesToShot(true, 1.5f, nullptr, nullptr);
        CHECK(fBlock == 12, "a defender 1.5 m away: no wind-up for a power shot");
        // aim: corner away from the keeper, 0.4 m inside the post
        Scene a; layout(a, 5.f); a.away[10] = MetricVector2D(51.f, 1.0f);
        GestureOutput g; for (int f = 0; f < 12; f++) { g = a.step(MetricVector2D(28.2f, 0.f)); if (g.valid && g.actionType == ACT_SHOT) break; }
        const float want = angleTo(28.f, 0.f, 52.f, -2.86f);
        std::printf("   shot angle %.1f, corner %.1f\n", g.theta, want);
        CHECK(g.valid && std::fabs(g.theta - want) < 2.0f, "the shot snaps to the post corner (2.86 m) farthest from the keeper");
        // repeated looks: after a shot the next look comes after the re-arm time, not never
        Scene r; layout(r, 5.f);
        int shots = 0;
        for (int f = 0; f < 80; f++) { GestureOutput x = r.step(MetricVector2D(28.2f, 0.f)); if (x.valid && x.actionType == ACT_SHOT) shots++; }
        CHECK(shots >= 2, "shooting is not limited to one attempt per possession");
    }
    // 4. shot suppression telemetry
    {
        Scene s;
        s.home[5] = MetricVector2D(28.f, 0.f);
        s.away[10] = MetricVector2D(51.f, 0.5f);
        for (int i = 0; i < 10; i++) s.away[i] = MetricVector2D(-20.f + 2.f * i, 20.f - 4.f * i);
        s.away[9] = MetricVector2D(26.8f, 0.5f);   // 1.2 m away
        for (int f = 0; f < 6; f++) s.step(MetricVector2D(28.2f, 0.f));
        std::string log = s.e.exportShotLog();
        CHECK(log.find("SHOT_BLOCKED_PRESSURE") != std::string::npos, "log: pressure suppression is recorded");
        Scene l;
        l.home[5] = MetricVector2D(28.f, 0.f);
        l.away[10] = MetricVector2D(51.f, 0.5f);
        for (int i = 0; i < 10; i++) l.away[i] = MetricVector2D(-20.f + 2.f * i, 20.f - 4.f * i);
        l.away[9] = MetricVector2D(37.f, -1.2f);   // 9 m away, square on the shot line
        for (int f = 0; f < 6; f++) l.step(MetricVector2D(28.2f, 0.f));
        log = l.e.exportShotLog();
        CHECK(log.find("DEFENDER_RAYCAST_INTERSECT") != std::string::npos, "log: a blocked shooting line is recorded");
        Scene c;
        c.home[5] = MetricVector2D(28.f, 0.f);
        c.away[10] = MetricVector2D(51.f, 0.5f);
        for (int i = 0; i < 10; i++) c.away[i] = MetricVector2D(-20.f + 2.f * i, 20.f - 4.f * i);
        for (int f = 0; f < 14; f++) c.step(MetricVector2D(28.2f, 0.f));
        log = c.e.exportShotLog();
        CHECK(log.find("SHOT_BLOCKED_COOLDOWN") != std::string::npos, "log: the cooldown after a shot is recorded");
        int a = 0, b = 0, d = 0; c.e.shotBlockCounts(a, b, d);
        CHECK(d > 0, "counters follow the log");
        c.e.clearShotLog();
        CHECK(c.e.exportShotLog().empty(), "the log can be cleared");
    }
    // 5. wing switch
    {
        Scene s;
        s.home[1] = MetricVector2D(5.f, -12.f);          // carrier
        s.home[2] = MetricVector2D(14.f, 24.f);          // far side, open
        for (int i = 3; i < 11; i++) s.home[i] = MetricVector2D(-20.f + 2.f * i, 30.f - 2.f * i);
        for (int i = 0; i < 11; i++) s.away[i] = MetricVector2D(40.f, -30.f + 5.f * i);
        s.away[0] = MetricVector2D(8.f, -12.f);
        s.away[1] = MetricVector2D(5.f, -8.f);
        s.away[2] = MetricVector2D(2.f, -14.f);
        s.away[3] = MetricVector2D(8.f, -16.f);
        s.away[4] = MetricVector2D(4.f, -18.f);
        s.away[5] = MetricVector2D(10.f, -9.f);
        bool sw = false; float len = 0;
        for (int f = 0; f < 8; f++) {
            GestureOutput g = s.step(MetricVector2D(5.2f, -12.f));
            if (g.valid && g.actionType == ACT_LOFTED) {
                sw = true;
                if (s.e.chosenCandidate() >= 0) len = s.e.candidates()[s.e.chosenCandidate()].length;
            }
        }
        CHECK(sw, "five or more opponents around the carrier: a lofted switch is played");
        CHECK(len > 30.f, "the switch goes across the pitch, beyond the short-pass limit");
    }
    // 6. one-touch: the pass goes out shortly before the receiver controls the ball
    {
        Scene s;
        s.home[1] = MetricVector2D(0.f, 0.f);
        s.home[2] = MetricVector2D(12.f, 0.f);
        s.home[3] = MetricVector2D(22.f, -8.f);
        for (int i = 4; i < 11; i++) s.home[i] = MetricVector2D(-25.f + 3.f * i, -26.f);
        for (int i = 0; i < 11; i++) s.away[i] = MetricVector2D(30.f + i, -30.f + 6.f * i);
        for (int f = 0; f < 6; f++) s.step(MetricVector2D(0.2f, 0.f));
        double fireBall = -1;
        float bx = 0.2f;
        for (int f = 0; f < 14; f++) {
            bx += 1.28f;
            GestureOutput g = s.step(MetricVector2D(bx, 0.f));
            if (fireBall < 0 && bx >= 4.0f && g.valid && g.status == ST_TEAM &&
                (g.actionType == ACT_PASS || g.actionType == ACT_THROUGH || g.actionType == ACT_LOFTED)) fireBall = bx;
        }
        std::printf("   next pass dispatched with the ball at x=%.1f (receiver at 12.0, trap at about 11.2)\n", fireBall);
        CHECK(fireBall > 0 && fireBall < 12.0f, "the next pass is dispatched before the ball reaches the receiver");
        CHECK(fireBall >= 8.0f, "... and not much earlier than the trap window");
    }
    // 7. penalty-box lockout, interception cone, containment (defending)
    {
        auto run = [&](const MetricVector2D& ballPos, const MetricVector2D& ctrl, void (*mod)(MetricVector2D*, MetricVector2D*), int frames, int* tackles, int* matchups, int* presses, bool* dashSeen, GestureOutput* lastG) {
            Scene s(true, false);
            for (int i = 0; i < 11; i++) { s.home[i] = MetricVector2D(-30.f + 4.f * i, -20.f + 4.f * i); s.away[i] = MetricVector2D(-10.f + 3.f * i, 18.f - 3.f * i); }
            s.home[0] = ctrl;
            s.away[5] = ballPos + MetricVector2D(0.4f, 0.f);
            if (mod) mod(s.home, s.away);
            *tackles = *matchups = *presses = 0; *dashSeen = false;
            for (int f = 0; f < frames; f++) {
                s.e.ctrlSeen_ = true; s.e.ctrlPos_ = ctrl; s.e.ctrlSeenMs_ = s.t;
                GestureOutput g = s.step(ballPos, 2);
                if (!g.valid) continue;
                if (g.actionType == ACT_TACKLE) (*tackles)++;
                if (g.actionType == ACT_MATCHUP) (*matchups)++;
                if (g.actionType == ACT_PRESS) { (*presses)++; if (g.dash) *dashSeen = true; }
                if (lastG) *lastG = g;
            }
        };
        int tk, mu, pr; bool dash; GestureOutput lg;
        // defender inside our box (ball outside the box? no: ball carrier at 20 m depth, defender inside the box)
        run(MetricVector2D(-30.f, 4.f), MetricVector2D(-39.f, 3.f), nullptr, 50, &tk, &mu, &pr, &dash, &lg);
        CHECK(tk == 0 && pr == 0, "defender inside our box: no tackle and no press call");
        run(MetricVector2D(-40.f, 4.f), MetricVector2D(-26.f, 3.f), nullptr, 50, &tk, &mu, &pr, &dash, &lg);
        CHECK(tk == 0 && pr == 0 && mu > 0, "opponent inside our defensive 18 m: pure Match-up, no press call");
        // outside the box the containment buffer is kept (1.5-2.0 m) and the press call is allowed
        run(MetricVector2D(-10.f, 2.f), MetricVector2D(-20.f, 1.f), nullptr, 45, &tk, &mu, &pr, &dash, &lg);
        CHECK(pr > 0, "outside the box the defender closes down");
        // settled on the buffer: hold
        Scene h(true, false);
        for (int i = 0; i < 11; i++) { h.home[i] = MetricVector2D(-30.f + 4.f * i, -20.f + 4.f * i); h.away[i] = MetricVector2D(-10.f + 3.f * i, 18.f - 3.f * i); }
        const MetricVector2D B(-10.f, 2.f);
        h.away[5] = B + MetricVector2D(0.4f, 0.f);
        const float dist = std::sqrt(10.f * 10.f * 0.f + 0.f) ;
        (void)dist;
        MetricVector2D toG(-52.f - B.x, 0.f - B.y);
        const float tl = std::sqrt(toG.x * toG.x + toG.y * toG.y);
        MetricVector2D ctrl = B + MetricVector2D(toG.x / tl * 1.75f, toG.y / tl * 1.75f);
        h.home[0] = ctrl;
        bool sawStick = false, sawTackle = false;
        for (int f = 0; f < 30; f++) {
            h.e.ctrlSeen_ = true; h.e.ctrlPos_ = ctrl; h.e.ctrlSeenMs_ = h.t;
            GestureOutput g = h.step(B, 2);
            if (g.valid && g.actionType == ACT_TACKLE) sawTackle = true;
            if (g.valid && g.actionType == ACT_PRESS && g.r > 0.1f) sawStick = true;
        }
        CHECK(!sawTackle, "on the containment buffer: no lunge");
        CHECK(!sawStick, "on the containment buffer: the defender holds position");
    }
    // 8. further rules
    {
        // ray-cast: a defender within 0.6 m of the pass line removes the target
        Scene s;
        s.home[1] = MetricVector2D(0.f, 0.f);
        s.home[2] = MetricVector2D(14.f, 0.f);
        for (int i = 3; i < 11; i++) s.home[i] = MetricVector2D(-25.f + 2.f * i, -28.f);
        for (int i = 0; i < 11; i++) s.away[i] = MetricVector2D(35.f + i, -30.f + 6.f * i);
        s.away[0] = MetricVector2D(7.f, 0.4f);
        int hit = 0;
        for (int f = 0; f < 6; f++) {
            s.step(MetricVector2D(0.2f, 0.f));
            for (int i = 0; i < s.e.candidateCount(); i++)
                if (s.e.candidates()[i].type == ACT_PASS && s.e.candidates()[i].target.x > 10.f && std::fabs(s.e.candidates()[i].target.y) < 2.f) hit++;
        }
        CHECK(hit == 0, "a defender 0.4 m from the pass line disqualifies that target");
        // sprinting shot: roll touch first, shorter hold than a standing shot
        auto shotRun = [&](float vx, int* carries, int* hold) {
            Scene r;
            for (int i = 0; i < 10; i++) r.away[i] = MetricVector2D(-20.f + 2.f * i, 20.f - 4.f * i);
            r.away[10] = MetricVector2D(51.f, 0.5f);
            float x = vx > 0.f ? 8.f : 23.f;
            *carries = 0; *hold = 0;
            for (int f = 0; f < 40; f++) {
                r.home[5] = MetricVector2D(x, 0.f);
                GestureOutput g = r.step(MetricVector2D(x + 0.2f, 0.f));
                if (g.valid && g.actionType == ACT_CARRY && g.durationMs == 100) (*carries)++;
                if (g.valid && g.actionType == ACT_SHOT) { *hold = g.durationMs; break; }
                x += vx * 0.08f;
            }
        };
        int c1, h1, c2, h2;
        shotRun(9.f, &c1, &h1);
        shotRun(0.f, &c2, &h2);
        std::printf("   sprint: roll nudges %d, hold %d ms; standing: nudges %d, hold %d ms\n", c1, h1, c2, h2);
        CHECK(c1 >= 1 && c2 == 0, "sprinting with room: a 100 ms nudge precedes the shot; standing: none");
        CHECK(h1 > 0 && h2 > 0 && h1 < h2, "momentum compensation shortens the power hold of a sprinting shot");
        // goalkeeper lead: a keeper moving toward +y makes the bot aim at the other post
        Scene g;
        for (int i = 0; i < 10; i++) g.away[i] = MetricVector2D(-20.f + 2.f * i, 20.f - 4.f * i);
        g.home[5] = MetricVector2D(28.f, 0.f);
        float ky = -1.f; int shots = 0; float th = 0;
        for (int f = 0; f < 14; f++) {
            g.away[10] = MetricVector2D(51.f, ky);
            ky += 0.4f;
            GestureOutput x = g.step(MetricVector2D(28.2f, 0.f));
            if (x.valid && x.actionType == ACT_SHOT) { shots++; th = x.theta; break; }
        }
        CHECK(shots == 1 && th > 180.f, "keeper moving to +y (at -1 m now, ahead of it later): the shot goes to the -y post");
        // dense central midfield: lofted through ball
        Scene m;
        m.home[1] = MetricVector2D(-5.f, 0.f);
        m.home[2] = MetricVector2D(3.f, -7.f);
        m.home[3] = MetricVector2D(5.f, 8.f);
        for (int i = 4; i < 11; i++) m.home[i] = MetricVector2D(-30.f + 2.f * i, -30.f);
        m.home[8] = MetricVector2D(22.f, 4.f);   // a runner well beyond the press
        for (int i = 0; i < 11; i++) m.away[i] = MetricVector2D(40.f, -30.f + 6.f * i);
        const float ox[5] = {-4.f, -3.f, -6.f, -7.f, -1.f}, oy[5] = {4.f, -5.f, 9.f, -9.f, 10.f};
        for (int i = 0; i < 5; i++) m.away[i] = MetricVector2D(ox[i], oy[i]);
        bool lofted = false;
        for (int f = 0; f < 8; f++) { GestureOutput x = m.step(MetricVector2D(-4.8f, 0.f)); if (x.valid && x.actionType == ACT_LOFTED) lofted = true; }
        CHECK(lofted, "five defenders in the central midfield box: a lofted ball is forced");
    }
    std::printf("\n%s (%d failures)\n", g_fail ? "SOME RESCUE CHECKS FAILED" : "ALL RESCUE CHECKS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
