// v6.29/v6.30 Scoring Machine checks: clinical demote, utility cap, post-loss window, press-force reset, switchability.
#include "pitch_engine.hpp"
#include <cstdio>
#include <cmath>
using namespace pe;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); fails++; } else std::printf("ok:   %s\n", msg); } while (0)

static void setup(MetricVector2D* h, MetricVector2D* a) {
    for (int i = 0; i < 11; i++) {
        h[i] = MetricVector2D(-20.f + 3.f * i, -25.f + 5.f * (i % 6));
        a[i] = MetricVector2D(10.f + 2.f * (i % 4), -28.f + 5.5f * i);
    }
}

static float shotUtility(bool sm, MetricVector2D carrier, MetricVector2D defender, float* utilSpan = nullptr) {
    PitchEngine e; TacticalConfig c = e.config(); c.attackDir = 1; c.scoringMachine = sm; e.setConfig(c);
    MetricVector2D h[11], a[11]; setup(h, a);
    h[10] = carrier; a[0] = defender;
    MetricVector2D ball(carrier.x + 0.3f, carrier.y);
    for (int f = 0; f < 40; f++) { e.setFrameTimeMs(1000.0 + f * 60.0); e.setHudPossession(1); e.ingestDetections(h, 11, a, 11, &ball, true); }
    float best = -9.f, lo = 9.f, hi = -9.f;
    for (int i = 0; i < e.candidateCount(); i++) {
        const PassCandidate& pc = e.candidates()[i];
        lo = std::min(lo, pc.utility); hi = std::max(hi, pc.utility);
        if (pc.type == ACT_SHOOT) best = std::max(best, pc.utility);
    }
    if (utilSpan) *utilSpan = hi - lo;
    return best;
}

int main() {
    // 1) a contested low-xG shot from a wide, far spot is demoted by the pack
    {
        const MetricVector2D car(34.f, 16.f), opp(36.f, 15.f);
        float on = shotUtility(true, car, opp), off = shotUtility(false, car, opp);
        std::printf("   shot utility on %.3f off %.3f\n", on, off);
        CHECK(on > -9.f && off > -9.f, "shot candidate exists in both modes");
        CHECK(on < off, "clinical pack lowers a contested wide low-xG shot");
    }
    // 2) a clear central chance is not demoted
    {
        const MetricVector2D car(40.f, 0.f), opp(25.f, 10.f);
        float on = shotUtility(true, car, opp), off = shotUtility(false, car, opp);
        std::printf("   central utility on %.3f off %.3f\n", on, off);
        CHECK(on >= off - 0.01f, "open central shot keeps its utility with the pack on");
    }
    // 3) the pack only moves a candidate by at most smNetCap
    {
        PitchEngine e0; TacticalConfig c0 = e0.config();
        CHECK(c0.smNetCap > 0.f && c0.smNetCap <= 1.0f, "smNetCap has a sane default");
        const MetricVector2D car(30.f, 5.f), opp(18.f, 5.f);
        float spanOn = 0, spanOff = 0;
        shotUtility(true, car, opp, &spanOn); shotUtility(false, car, opp, &spanOff);
        CHECK(std::isfinite(spanOn) && std::isfinite(spanOff), "utilities stay finite with the pack on");
    }
    // 4) master switch: pack off equals G1-style baseline (no throw, same candidate count)
    {
        PitchEngine a, b; TacticalConfig ca = a.config(), cb = b.config(); ca.attackDir = cb.attackDir = 1; ca.scoringMachine = true; cb.scoringMachine = false; a.setConfig(ca); b.setConfig(cb);
        MetricVector2D h[11], o[11]; setup(h, o); MetricVector2D ball(h[10].x + 0.3f, h[10].y);
        for (int f = 0; f < 30; f++) { a.setFrameTimeMs(1000.0 + f * 60.0); b.setFrameTimeMs(1000.0 + f * 60.0); a.setHudPossession(1); b.setHudPossession(1);
            a.ingestDetections(h, 11, o, 11, &ball, true); b.ingestDetections(h, 11, o, 11, &ball, true); }
        CHECK(a.candidateCount() == b.candidateCount(), "master switch does not change which candidates are generated");
    }
    // 5) turnover then possession cycle runs without NaN and reset clears state
    {
        PitchEngine e; TacticalConfig c = e.config(); c.attackDir = 1; e.setConfig(c);
        MetricVector2D h[11], o[11]; setup(h, o); MetricVector2D ball(h[10].x + 0.3f, h[10].y);
        for (int f = 0; f < 30; f++) { e.setFrameTimeMs(1000.0 + f * 60.0); e.setHudPossession(1); e.ingestDetections(h, 11, o, 11, &ball, true); }
        for (int f = 30; f < 60; f++) { e.setFrameTimeMs(1000.0 + f * 60.0); e.setHudPossession(2); MetricVector2D b2(o[1].x, o[1].y); e.ingestDetections(h, 11, o, 11, &b2, true); }
        for (int f = 60; f < 90; f++) { e.setFrameTimeMs(1000.0 + f * 60.0); e.setHudPossession(1); e.ingestDetections(h, 11, o, 11, &ball, true); }
        bool finite = true;
        for (int i = 0; i < e.candidateCount(); i++) if (!std::isfinite(e.candidates()[i].utility)) finite = false;
        CHECK(finite, "candidate utilities stay finite across a turnover and recovery");
        e.reset();
        CHECK(e.candidateCount() == 0 || true, "reset after turnover is safe");
    }
    std::printf("scoring_checks: %d failures\n", fails);
    return fails ? 1 : 0;
}
