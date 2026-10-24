// Checks: (1) pass learner refuses success-only data and keeps sane weight signs, (2) a degenerate saved pass model is
// dropped, (3) no back pass to box players (built with -DPB_RESCUE_DEFAULT=false so the forward-only rule cannot hide a failure), (4) mapping.
// Build: g++ -std=c++17 -O1 -DPB_RESCUE_DEFAULT=false -I../app/src/main/cpp fix_checks.cpp ../app/src/main/cpp/pitch_engine.cpp -o fix_checks && ./fix_checks
#include "pitch_engine.hpp"
#include <cstdio>
#include <cmath>
#include <string>
using namespace pe;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); fails++; } else std::printf("ok:   %s\n", msg); } while (0)

static std::string attackLines(int okN, int badN) {
    std::string s;
    char b[200];
    for (int i = 0; i < okN + badN; i++) {
        const int y = i < okN ? 1 : 0;
        const float xp = y ? 0.6f + 0.02f * (i % 10) : 0.1f + 0.02f * (i % 5);
        std::snprintf(b, sizeof b, "A %d 2 %.4f %.4f %.4f %.3f %.3f 0.5 0.5 %d\n", 1000 + i * 700, xp, 0.2f + 0.05f * (i % 4), 0.3f, 9.f + (i % 7), 4.f, y);
        s += b;
    }
    return s;
}

int main() {
    // 1) success-only data: the pass model must stay at its prior
    {
        PitchEngine e;
        e.learnAttack(attackLines(49, 0));
        const float* w = e.attackWeights();
        std::printf("   weights after 49/49 kept: %.2f %.2f %.2f %.2f %.2f %.2f\n", w[0], w[1], w[2], w[3], w[4], w[5]);
        CHECK(std::fabs(w[0]) < 1e-4f && std::fabs(w[1] - 1.f) < 1e-4f, "49 completed passes and no lost one: pass model unchanged");
    }
    // 2) mixed data: learns, and pressure / crowd / length weights cannot turn positive, bias cannot make everything safe
    {
        PitchEngine e;
        e.learnAttack(attackLines(30, 18));
        const float* w = e.attackWeights();
        std::printf("   weights after 30 kept / 18 lost: %.2f %.2f %.2f %.2f %.2f %.2f\n", w[0], w[1], w[2], w[3], w[4], w[5]);
        CHECK(w[2] <= 0.f && w[3] <= 0.f && w[4] <= 0.3f, "pressure and crowd weights never positive");
        CHECK(w[0] <= 0.8f && w[0] >= -1.f, "bias stays in its safe range");
        CHECK(w[1] > 0.3f, "lane-quality weight is learned above its floor from mixed data");
    }
    // 3) a damaged model from the 0-7 / 0-0 matches is dropped on import
    {
        PitchEngine e;
        const std::string m = "v 1\nml 1\npa 1.94212 0.30000 0.61421 0.58520 0.47731 0.26941\npan 49\n";
        e.importAttackWeights(m);
        const float* w = e.attackWeights();
        CHECK(std::fabs(w[0]) < 1e-4f && std::fabs(w[1] - 1.f) < 1e-4f, "damaged saved pass model (bias 1.94, lane weight at floor) is discarded");
    }
    // 4) no back pass towards our goal: a team-mate 21 m from our goal line behind the carrier is not offered, one ahead still is
    {
        PitchEngine e; TacticalConfig c = e.config(); c.attackDir = 1; e.setConfig(c);
        MetricVector2D h[11], a[11];
        for (int i = 0; i < 11; i++) { h[i] = MetricVector2D(10.f + 3.f * i, -30.f + 6.f * i); a[i] = MetricVector2D(20.f + 2.f * i, -30.f + 6.f * i); }
        h[10] = MetricVector2D(-20.f, 0.f);       // carrier, 32.5 m from our goal line
        h[0]  = MetricVector2D(-51.f, 0.f);       // keeper
        h[1]  = MetricVector2D(-31.f, 8.f);       // team-mate behind the carrier, 21.5 m from our goal line, free
        h[2]  = MetricVector2D(-10.f, 8.f);       // team-mate ahead, free
        MetricVector2D ball(-19.7f, 0.f);
        bool back = false, ahead = false;
        for (int f = 0; f < 40; f++) {
            e.setFrameTimeMs(1000.0 + f * 60.0); e.setHudPossession(1);
            e.ingestDetections(h, 11, a, 11, &ball, true);
            for (int i = 0; i < e.candidateCount(); i++) {
                const PassCandidate& pc = e.candidates()[i];
                if (pc.type != ACT_PASS && pc.type != ACT_THROUGH && pc.type != ACT_LOFTED) continue;
                if (pc.target.x < -26.f && pc.target.y > 4.f) back = true;
                if (pc.target.x > -14.f && pc.target.x < -6.f) ahead = true;
            }
        }
        CHECK(!back, "no pass to a team-mate behind the ball near our own goal");
        CHECK(ahead, "a forward team-mate is still offered");
    }
    // 5) the radar-to-pitch mapping puts the centre of the radar quad at the centre of the pitch
    {
        Homography H;
        const float q[8] = {8.f, 8.f, 184.f, 8.f, 184.f, 111.f, 8.f, 111.f};
        CHECK(H.fromQuad(q), "homography from the outline quad");
        const MetricVector2D c = H.apply(96.f, 59.5f);
        CHECK(std::fabs(c.x) < 0.01f && std::fabs(c.y) < 0.01f, "radar centre maps to the pitch centre");
    }
    std::printf(fails ? "fix_checks: %d FAILED\n" : "fix_checks: all passed\n", fails);
    return fails ? 1 : 0;
}
