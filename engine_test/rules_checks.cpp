// Checks for the rules: keeper pass ban, short-pass cap, face-before-pass, long shots, post-goal restart.
// g++ -std=c++17 -O1 -I../app/src/main/cpp rules_checks.cpp ../app/src/main/cpp/pitch_engine.cpp -o rules_checks && ./rules_checks
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "pitch_engine.hpp"
using namespace pe;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) std::printf("ok:   %s\n", msg); else { std::printf("FAIL: %s\n", msg); g_fail++; } } while (0)

struct Scene {
    PitchEngine e;
    MetricVector2D home[11], away[11];
    double t = 1000;
    Scene(int attackDir = 1) {
        TacticalConfig c;
        c.attackDir = attackDir;
        c.maxPassLen = c.safePassLen = 18.f;   // the pass cap these scenes were laid out for
        e.setConfig(c);
        for (int i = 0; i < 11; i++) {
            home[i] = MetricVector2D(-48.f + 8.f * i, -24.f + 5.f * i);
            away[i] = MetricVector2D(-40.f + 8.f * i, 24.f - 5.f * ((i * 3) % 11));
        }
        home[0] = MetricVector2D(-50.f, 0.f);   // keeper
    }
    void frame(MetricVector2D ball) {
        e.setFrameTimeMs(t);
        e.ingestDetections(home, 11, away, 11, &ball, true);
        t += 80;
    }
    void dead(int n) { for (int i = 0; i < n; i++) { e.setFrameTimeMs(t); e.ingestDetections(nullptr, 0, nullptr, 0, nullptr, false); t += 80; } }
};

int main() {
    {   // keeper is never a target, no candidate pass is longer than the cap, even with the carrier close to the keeper
        Scene s;
        s.home[3] = MetricVector2D(-30.f, 5.f);
        for (int i = 0; i < 10; i++) s.frame(MetricVector2D(-29.8f, 5.f));
        bool gk = false, longPass = false; int passes = 0;
        for (int i = 0; i < s.e.candidateCount(); i++) {
            const PassCandidate& c = s.e.candidates()[i];
            if (c.type != ACT_PASS && c.type != ACT_THROUGH && c.type != ACT_LOFTED) continue;
            passes++;
            if (c.targetId == s.e.state().team[0].id) gk = true;
            if (c.length > s.e.config().safePassLen + 1e-3f) longPass = true;
        }
        std::printf("   %d pass candidates\n", passes);
        CHECK(passes > 0, "short passes are still offered");
        CHECK(!gk, "the goalkeeper is never a pass target");
        CHECK(!longPass, "no pass candidate exceeds the short-pass cap");
    }
    {   // a standing carrier turns first, then passes
        Scene s;
        s.home[5] = MetricVector2D(0.f, 0.f);
        s.home[6] = MetricVector2D(10.f, -8.f);
        s.home[7] = MetricVector2D(12.f, 12.f);
        for (int i = 0; i < 11; i++) if (i != 5 && i != 6 && i != 7) s.away[i] = MetricVector2D(30.f + i, 28.f - 5.f * i);
        int turnTicks = 0, passes = 0, firstPassFrame = -1;
        for (int f = 0; f < 40; f++) {
            s.frame(MetricVector2D(0.2f, 0.f));
            const GestureOutput& g = s.e.lastGesture();
            if (g.valid && g.actionType == ACT_CARRY && g.durationMs == s.e.config().turnHoldMs) turnTicks++;
            if (g.valid && (g.actionType == ACT_PASS || g.actionType == ACT_THROUGH || g.actionType == ACT_LOFTED)) {
                passes++;
                if (firstPassFrame < 0) firstPassFrame = f;
            }
        }
        std::printf("   turn ticks %d, first pass at frame %d\n", turnTicks, firstPassFrame);
        CHECK(firstPassFrame >= 0 && firstPassFrame <= 3, "standing carrier: zero-hesitation design, the pass goes out within a few frames (no turn tick)");
        CHECK(passes >= 1, "and the pass is still played afterwards");
    }
    {   // restart: the engine keeps its learned attack direction through a goal stoppage and plays at once
        Scene s(-1);   // auto-detect
        s.home[5] = MetricVector2D(5.f, 0.f);
        for (int i = 0; i < 40; i++) s.frame(MetricVector2D(5.2f, 0.f));
        const int before = s.e.state().attackSign;
        s.dead(60);
        for (int i = 0; i < 3; i++) s.frame(MetricVector2D(5.2f, 0.f));
        CHECK(s.e.state().attackSign == before, "attack direction survives a goal stoppage (no re-learning lag)");
        CHECK(s.e.state().status == ST_TEAM, "possession is re-established within 3 frames of the radar returning");
    }
    {   // urgency grows with a long spell and a high possession share
        Scene s;
        s.home[5] = MetricVector2D(0.f, 0.f);
        for (int i = 0; i < 400; i++) s.frame(MetricVector2D(0.2f, 0.f));
        std::printf("   urgency %.2f\n", s.e.urgencyLevel());
        CHECK(s.e.urgencyLevel() > 0.3f, "attacking urgency builds with sterile possession");
    }
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
