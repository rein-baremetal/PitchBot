// Checks for hidden-ball continuity, overlap tracking, pass context, opponent-adaptive noise and the style blend.
// Build: g++ -std=c++17 -O1 -I../app/src/main/cpp edge_checks.cpp ../app/src/main/cpp/pitch_engine.cpp -o edge_checks && ./edge_checks
#include <cmath>
#include <cstdio>
#include <vector>

#include "pitch_engine.hpp"

using namespace pe;

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); fails++; } else std::printf("ok:   %s\n", msg); } while (0)

static void scene(MetricVector2D* h, MetricVector2D* a) {
    h[0] = {-48.f, 0.f};
    h[1] = {-30.f, -22.f}; h[2] = {-30.f, -8.f}; h[3] = {-30.f, 8.f}; h[4] = {-30.f, 22.f};
    h[5] = {-12.f, -26.f}; h[6] = {-10.f, 26.f}; h[7] = {-8.f, 8.f};
    h[8] = {4.f, -12.f};  h[9] = {12.f, 14.f};  h[10] = {0.f, 0.f};
    a[0] = {48.f, 0.f};
    a[1] = {35.f, -22.f}; a[2] = {35.f, -8.f}; a[3] = {35.f, 8.f}; a[4] = {35.f, 22.f};
    a[5] = {30.f, -14.f}; a[6] = {30.f, 14.f}; a[7] = {26.f, 0.f};
    a[8] = {22.f, -24.f}; a[9] = {22.f, 24.f}; a[10] = {40.f, 0.f};
}

static TacticalConfig allOff() {
    TacticalConfig c;
    c.attackDir = 1;
    c.seed = 99;
    c.ballCoast = c.occlusionHold = c.context = c.antiAdapt = c.styles = false;
    return c;
}

static int findCand(const PitchEngine& e, MetricVector2D p, float tol) {
    int bi = -1;
    float bd = tol;
    for (int i = 0; i < e.candidateCount(); i++) {
        const PassCandidate& c = e.candidates()[i];
        if (c.type != ACT_PASS && c.type != ACT_LOFTED) continue;
        const float d = distance(c.target, p);
        if (d < bd) { bd = d; bi = i; }
    }
    return bi;
}

int main() {
    {
        auto run = [&](bool coast, int* teamFrames, float* maxGap) {
            TacticalConfig c = allOff();
            c.ballCoast = coast;
            PitchEngine e;
            e.setConfig(c);
            MetricVector2D h[11], a[11];
            scene(h, a);
            double t = 1000;
            *teamFrames = 0;
            *maxGap = 0.f;
            for (int f = 0; f < 30; f++) {
                h[10] = {8.f * 0.08f * f, 0.f};
                const MetricVector2D ball = {h[10].x + 0.4f, 0.f};
                const bool seen = f < 12 || f >= 20;
                e.setFrameTimeMs(t);
                e.ingestDetections(h, 11, a, 11, &ball, seen);
                if (f >= 12 && f < 20) {
                    if (e.state().status == ST_TEAM) (*teamFrames)++;
                    *maxGap = std::max(*maxGap, distance(e.state().ball, h[10]));
                }
                t += 80;
            }
        };
        int tOn, tOff;
        float gOn, gOff;
        run(true, &tOn, &gOn);
        run(false, &tOff, &gOff);
        std::printf("   hidden dribble: team-owned frames on=%d off=%d | ball-to-carrier gap on=%.2f off=%.2f m\n", tOn, tOff, gOn, gOff);
        CHECK(tOn == 8, "hidden ball: possession is kept for the whole 0.64 s gap while dribbling");
        CHECK(tOff < tOn, "hidden ball: without continuity the frozen ball loses possession mid-dribble");
        CHECK(gOn < 1.5f && gOff > gOn, "hidden ball: the estimate stays on the carrier instead of lagging behind");
    }

    {
        auto run = [&](bool coast) {
            TacticalConfig c = allOff();
            c.ballCoast = coast;
            PitchEngine e;
            e.setConfig(c);
            MetricVector2D h[11], a[11];
            scene(h, a);
            double t = 1000;
            float bx = -20.f;
            float x = 0.f;
            for (int f = 0; f < 16; f++) {
                const MetricVector2D ball = {bx, 15.f};
                e.setFrameTimeMs(t);
                e.ingestDetections(h, 11, a, 11, &ball, f < 8);
                if (f < 8) bx += 12.f * 0.08f;
                x = e.state().ball.x;
                t += 80;
            }
            return x;
        };
        const float on = run(true), off = run(false);
        std::printf("   loose ball hidden 8 frames: x on=%.2f off=%.2f (last seen %.2f)\n", on, off, -20.f + 7 * 12.f * 0.08f);
        CHECK(on > off + 1.f, "hidden loose ball keeps travelling along its last velocity");
        CHECK(on < off + 12.f * 0.08f * 8.f, "hidden loose ball slows down instead of running at full speed");
    }

    {
        auto run = [&](bool hold) {
            TacticalConfig c = allOff();
            c.occlusionHold = hold;
            PitchEngine e;
            e.setConfig(c);
            MetricVector2D h[12], a[11];
            scene(h, a);
            const MetricVector2D ball = {-10.f, 30.f};
            double t = 1000;
            float worst = 0.f;
            for (int f = 0; f < 40; f++) {
                const float x = -20.f + 6.f * f * 0.08f;
                const bool merged = f >= 10 && f < 24;
                int n = 0;
                for (int i = 0; i < 9; i++) h[n++] = {-40.f + i * 3.f, -30.f + i * 6.f};
                if (merged) h[n++] = {x, 0.f};
                else { h[n++] = {x, 0.6f}; h[n++] = {x, -0.6f}; }
                for (int i = 0; i < 11; i++) a[i] = {40.f, -25.f + i * 5.f};
                e.setFrameTimeMs(t);
                e.ingestDetections(h, n, a, 11, &ball, true);
                if (f == 23) {
                    for (int i = 0; i < 11; i++) {
                        const PlayerState& p = e.state().team[i];
                        if (p.active && std::fabs(p.pos.y) < 2.f && p.pos.x > -25.f && p.pos.x < 0.f)
                            worst = std::max(worst, std::fabs(p.pos.x - x));
                    }
                }
                t += 80;
            }
            return worst;
        };
        const float on = run(true), off = run(false);
        std::printf("   overlap 1.1 s: worst position error of an overlapped player on=%.2f off=%.2f m\n", on, off);
        CHECK(on < off * 0.6f, "overlap: a hidden player is tracked much closer to the truth while merged with another dot");
    }

    {
        auto run = [&](float wF, float wS, float wC, float runY, float* uUp, float* uBack, bool ctx) {
            TacticalConfig c = allOff();
            c.context = ctx;
            c.wFacing = wF; c.wSupport = wS; c.wCompact = wC;
            PitchEngine e;
            e.setConfig(c);
            MetricVector2D h[11], a[11];
            scene(h, a);
            const MetricVector2D ball = {0.2f, 0.f};
            double t = 1000;
            for (int i = 0; i < 14; i++) {
                h[8] = {4.f + 6.f * 0.08f * i, -12.f};
                h[9] = {12.f - 6.f * 0.08f * i, 14.f};
                (void)runY;
                e.setFrameTimeMs(t);
                e.ingestDetections(h, 11, a, 11, &ball, true);
                t += 80;
            }
            const int iu = findCand(e, h[8], 8.f), ib = findCand(e, h[9], 8.f);
            *uUp = iu >= 0 ? e.candidates()[iu].utility : -9.f;
            *uBack = ib >= 0 ? e.candidates()[ib].utility : -9.f;
        };
        float u0, b0, u1, b1, uz, bz;
        run(0.f, 0.f, 0.f, 0.f, &u0, &b0, false);
        run(0.05f, 0.f, 0.f, 0.f, &u1, &b1, true);
        run(0.f, 0.f, 0.f, 0.f, &uz, &bz, true);
        std::printf("   context facing: upfield runner off=%.4f on=%.4f | retreating runner off=%.4f on=%.4f\n", u0, u1, b0, b1);
        CHECK(std::fabs(uz - u0) < 1e-6f && std::fabs(bz - b0) < 1e-6f, "context with all weights zero equals context off");
        CHECK((u1 - u0) > (b1 - b0) + 1e-5f, "context: a receiver running upfield gains more than one retreating");
    }

    {
        auto run = [&](bool support, float* u) {
            TacticalConfig c = allOff();
            c.context = true;
            c.wFacing = 0.f; c.wCompact = 0.f;
            c.wSupport = support ? 0.2f : 0.f;
            PitchEngine e;
            e.setConfig(c);
            MetricVector2D h[11], a[11];
            scene(h, a);
            h[7] = {5.f, 12.f};
            h[5] = {6.f, 18.f};
            const MetricVector2D ball = {0.2f, 0.f};
            double t = 1000;
            for (int i = 0; i < 14; i++) {
                e.setFrameTimeMs(t);
                e.ingestDetections(h, 11, a, 11, &ball, true);
                t += 80;
            }
            const int ib = findCand(e, h[9], 6.f);
            *u = ib >= 0 ? e.candidates()[ib].utility : -9.f;
        };
        float a1, a0;
        run(true, &a1);
        run(false, &a0);
        std::printf("   support behind receiver: utility with=%.4f without=%.4f\n", a1, a0);
        CHECK(a1 > a0 + 1e-4f, "context: team-mates behind the receiver raise that pass");
    }

    {
        PitchEngine e;
        TacticalConfig c = allOff();
        c.antiAdapt = true;
        e.setConfig(c);
        for (int i = 0; i < 30; i++) e.noteOutcome(true);
        CHECK(!e.opponentAdapting(), "adaptation: a steady keep rate is not flagged");
        for (int i = 0; i < 7; i++) e.noteOutcome(false);
        CHECK(e.opponentAdapting(), "adaptation: a sudden drop in kept passes is flagged");
        MetricVector2D h[11], a[11];
        scene(h, a);
        const MetricVector2D ball = {0.2f, 0.f};
        double t = 1000;
        for (int i = 0; i < 120; i++) {
            e.setFrameTimeMs(t);
            e.ingestDetections(h, 11, a, 11, &ball, true);
            t += 80;
        }
        std::printf("   noise after 9.6 s of adaptation: %.2f\n", e.noiseLevel());
        CHECK(e.noiseLevel() > 0.9f, "adaptation: noise ramps up to full while the opponent keeps adjusting");
        for (int i = 0; i < 40; i++) e.noteOutcome(true);
        for (int i = 0; i < 400; i++) {
            e.setFrameTimeMs(t);
            e.ingestDetections(h, 11, a, 11, &ball, true);
            t += 80;
        }
        CHECK(!e.opponentAdapting() && e.noiseLevel() < 0.05f, "adaptation: noise fades again once passes land");
    }

    {
        auto trace = [&](bool anti, bool noisy) {
            TacticalConfig c = allOff();
            c.antiAdapt = anti;
            PitchEngine e;
            e.setConfig(c);
            if (noisy) { for (int i = 0; i < 30; i++) e.noteOutcome(true); for (int i = 0; i < 8; i++) e.noteOutcome(false); }
            MetricVector2D h[11], a[11];
            scene(h, a);
            const MetricVector2D ball = {0.2f, 0.f};
            double t = 1000;
            double sum = 0;
            for (int i = 0; i < 400; i++) {
                a[5] = {30.f - 0.02f * i, -14.f};
                e.setFrameTimeMs(t);
                e.ingestDetections(h, 11, a, 11, &ball, true);
                sum += e.lastGesture().actionType * 1.0 + e.lastGesture().theta * 0.37 + e.lastGesture().durationMs * 0.001;
                t += 80;
            }
            return sum;
        };
        const double off = trace(false, false), idle = trace(true, false), r1 = trace(true, true), r2 = trace(true, true);
        CHECK(std::fabs(off - idle) < 1e-9, "noise is inert until adaptation is detected (output identical to off)");
        CHECK(std::fabs(r1 - r2) < 1e-9, "noise is reproducible for a given seed");
        CHECK(std::fabs(r1 - off) > 1e-6, "noise changes the play once adaptation is detected");
    }

    {
        TacticalConfig c = allOff();
        c.styles = true;
        PitchEngine e;
        e.setConfig(c);
        MetricVector2D h[11], a[11];
        MetricVector2D ball = {0.2f, 0.f};
        auto play = [&](bool deepCompact) {
            double t = 1000;
            for (int i = 0; i < 700; i++) {
                scene(h, a);
                if (deepCompact) {
                    for (int k = 1; k < 11; k++) a[k] = {36.f + (k % 3) * 2.f, -8.f + (k % 5) * 4.f};
                } else {
                    for (int k = 1; k < 11; k++) a[k] = {-10.f + k * 4.f, -30.f + (k % 5) * 15.f};
                }
                e.setFrameTimeMs(t);
                e.ingestDetections(h, 11, a, 11, &ball, true);
                t += 80;
            }
        };
        const PitchEngine::Strategy before = e.effectiveStrategy();
        CHECK(std::fabs(before.fwd - 1.f) < 1e-6f && std::fabs(before.tempMul - 1.f) < 1e-6f, "styles: neutral before any evidence is gathered");
        play(true);
        const float* w = e.styleWeights();
        float sum = 0.f;
        for (int i = 0; i < PitchEngine::N_STYLES; i++) sum += w[i];
        std::printf("   deep compact block: patient=%.2f direct=%.2f counter=%.2f chaos=%.2f\n", w[0], w[1], w[2], w[3]);
        CHECK(sum <= 1.0001f && sum > 0.5f, "styles: weights form a bounded blend");
        CHECK(w[0] > w[1] && w[0] > w[2], "styles: a deep compact block draws the Patient profile");
        const PitchEngine::Strategy deep = e.effectiveStrategy();
        play(false);
        std::printf("   stretched high line: patient=%.2f direct=%.2f counter=%.2f chaos=%.2f\n", w[0], w[1], w[2], w[3]);
        CHECK(w[1] + w[2] > w[0], "styles: a stretched high line shifts the blend to Direct/Counter");
        const PitchEngine::Strategy high = e.effectiveStrategy();
        CHECK(high.fwd > deep.fwd && high.dKeep < deep.dKeep, "styles: the blend moves forward-pass weight and keep-ball bias the right way");
        CHECK(high.dKeep >= -0.4f && high.fwd <= 1.35f && high.fwd >= 0.75f && high.tempMul >= 0.7f && high.tempMul <= 1.5f, "styles: blended knobs stay in their safe ranges");
        TacticalConfig c2 = c;
        c2.styles = false;
        e.setConfig(c2);
        CHECK(std::fabs(e.effectiveStrategy().fwd - e.strategy().fwd) < 1e-9f, "styles off: the learned strategy is used unchanged");
    }

    std::printf("\n%s (%d failures)\n", fails ? "SOME CHECKS FAILED" : "ALL EDGE CHECKS PASSED", fails);
    return fails ? 1 : 0;
}
