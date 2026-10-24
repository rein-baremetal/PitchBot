// Open-loop replay of recorded radar tracks through the engine. The recorded game keeps going whatever the engine decides,
// so this measures decisions, not results: for every spell where we had the ball and then lost it to the opponent, did the
// engine release the ball (pass / through / lofted / shot / clear) before the steal, and how long did it hold on.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "pitch_engine.hpp"
using namespace pe;
struct Row { double t; int st; int nh, na; int bs; float bx, by; std::vector<MetricVector2D> h, a; };
static std::vector<Row> load(const char* path) {
    std::vector<Row> rows; std::ifstream f(path); std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line[0] == 't') continue;
        std::stringstream ss(line); std::string tok; std::vector<double> v;
        while (std::getline(ss, tok, ',')) v.push_back(atof(tok.c_str()));
        if (v.size() < 7) continue;
        Row r; r.t = v[0]; r.st = (int)v[1]; r.nh = (int)v[2]; r.na = (int)v[3]; r.bs = (int)v[4];
        auto X = [](double a) { return (float)((a / 1000.0 - 0.5) * 2.0 * PITCH_HALF_X); };
        auto Y = [](double a) { return (float)((a / 1000.0 - 0.5) * 2.0 * PITCH_HALF_Y); };
        r.bx = X(v[5]); r.by = Y(v[6]);
        size_t k = 7;
        if (v.size() < k + 2 * (size_t)(r.nh + r.na)) continue;
        for (int i = 0; i < r.nh; i++, k += 2) r.h.push_back(MetricVector2D(X(v[k]), Y(v[k + 1])));
        for (int i = 0; i < r.na; i++, k += 2) r.a.push_back(MetricVector2D(X(v[k]), Y(v[k + 1])));
        rows.push_back(r);
    }
    return rows;
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const int planOn = argc > 2 ? atoi(argv[2]) : 1;
    std::vector<Row> rows = load(argv[1]);
    PitchEngine e;
    TacticalConfig c;
    c.legendary = true;
    c.attackDir = 1;
    (void)planOn;
    if (argc > 2) {
        std::string list = argv[2];
        struct F { const char* n; bool* p; } fl[] = {
            {"plan", &c.plan}, {"planCache", &c.planCache}, {"planLane", &c.planLane}, {"planTrap", &c.planTrap}, {"planCombo", &c.planCombo},
            {"planKeeper", &c.planKeeper}, {"planShape", &c.planShape}, {"planCounterPress", &c.planCounterPress}, {"planTempo", &c.planTempo},
            {"planOppModel", &c.planOppModel}, {"planBall", &c.planBall}, {"planFreeMan", &c.planFreeMan}, {"planSecondBall", &c.planSecondBall},
            {"planDirClear", &c.planDirClear}, {"planSidestep", &c.planSidestep}, {"planOverload", &c.planOverload}, {"planChannel", &c.planChannel},
            {"planRestart", &c.planRestart}, {"planPressure", &c.planPressure}, {"planLateRisk", &c.planLateRisk}, {"planCadence", &c.planCadence}, {"planHoldMargin", &c.planHoldMargin}, {"planCalmSlow", &c.planCalmSlow},
            {"meta", &c.meta}, {"plus", &c.plus}, {"metaCommit", &c.metaCommit}, {"metaFeint", &c.metaFeint}, {"metaNoise", &c.metaNoise}, {"metaStupid", &c.metaStupid}, {"metaReact", &c.metaReact}, {"metaLock", &c.metaLock}, {"metaMicro", &c.metaMicro}, {"metaIntent1v1", &c.metaIntent1v1}, {"metaContain", &c.metaContain}, {"metaBait", &c.metaBait}, {"metaSecondMan", &c.metaSecondMan}, {"metaRecovery", &c.metaRecovery}, {"metaTrackFreeze", &c.metaTrackFreeze}, {"metaOverload", &c.metaOverload}, {"metaThirdMan", &c.metaThirdMan}, {"metaThroughTiming", &c.metaThroughTiming}, {"metaSecurity", &c.metaSecurity}, {"metaField", &c.metaField}, {"metaMultiScale", &c.metaMultiScale}, {"metaFsm", &c.metaFsm}, {"metaSetPiece", &c.metaSetPiece}, {"metaAntiBait", &c.metaAntiBait}, {"metaLate", &c.metaLate},};
        std::stringstream ss(list); std::string tok;
        while (std::getline(ss, tok, ',')) for (auto& f : fl) if (tok == f.n) *f.p = false;
    }
    e.setConfig(c);
    e.setShootConfig(true, 520, true);
    int spells = 0, steals = 0, releasedBefore = 0, nonfinite = 0;
    double holdSum = 0; int holdN = 0;
    int prevSt = 0; double spellStart = -1, firstRelease = -1;
    long frames = 0;
    std::vector<double> lat; int noRelease = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& r = rows[i];
        MetricVector2D ball(r.bx, r.by);
        e.setFrameTimeMs(1000.0 + r.t);
        e.setHudPossession(r.st == 2 ? 1 : (r.st == 3 ? 2 : 0));
        e.ingestDetections(r.h.data(), r.nh, r.a.data(), r.na, r.bs ? &ball : nullptr, r.st != 0);
        const GestureOutput& g = e.lastGesture();
        frames++;
        if (g.valid && (!std::isfinite(g.r) || !std::isfinite(g.theta))) nonfinite++;
        const bool release = g.valid && g.status == ST_TEAM && (g.actionType == ACT_PASS || g.actionType == ACT_THROUGH || g.actionType == ACT_LOFTED || g.actionType == ACT_SHOT || g.actionType == ACT_SHOOT);
        if (r.st == 2 && prevSt != 2) { spellStart = r.t; firstRelease = -1; spells++; }
        if (r.st == 2 && release && firstRelease < 0) firstRelease = r.t;
        if (prevSt == 2 && r.st != 2 && spellStart >= 0) { if (firstRelease >= 0) lat.push_back((firstRelease - spellStart) / 1000.0); else noRelease++; }
        if (prevSt == 2 && r.st == 3) {              // steal
            steals++;
            if (firstRelease >= 0 && r.t - firstRelease >= 0 ) releasedBefore++;
            if (spellStart >= 0) { holdSum += (r.t - spellStart) / 1000.0; holdN++; }
        }
        prevSt = r.st;
    }
    std::printf("frames %ld  our-ball spells %d  steals %d  engine released before the steal %d (%.0f%%)  mean spell before a steal %.2f s  non-finite %d\n",
                frames, spells, steals, releasedBefore, steals ? 100.0 * releasedBefore / steals : 0.0, holdN ? holdSum / holdN : 0.0, nonfinite);
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double q) { return lat.empty() ? 0.0 : lat[(size_t)(q * (lat.size() - 1))]; };
    int within = 0; for (double v : lat) if (v <= 1.5) within++;
    std::printf("   time from winning the ball to the engine's first release: median %.2f s, p90 %.2f s, released within 1.5 s in %.0f%% of %zu spells, never released in %d\n",
                pct(0.5), pct(0.9), lat.empty() ? 0.0 : 100.0 * within / (double)(lat.size() + noRelease), lat.size() + noRelease, noRelease);
    return 0;
}
