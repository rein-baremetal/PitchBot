// Replays a slice of a real recording (normalised radar coordinates) through the engine.
// The radar sees only some of the players and a different set from frame to frame; that must never read as a glitch.
// Build: g++ -std=c++17 -O1 -I../app/src/main/cpp track_checks.cpp ../app/src/main/cpp/pitch_engine.cpp
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#define private public
#include "pitch_engine.hpp"
using namespace pe;
static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) std::printf("ok:   %s\n", msg); else { std::printf("FAIL: %s\n", msg); g_fail++; } } while (0)

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "real_frames/track_excerpt.csv";
    std::ifstream in(path);
    CHECK(in.good(), "excerpt opens");
    PitchEngine e;
    TacticalConfig c;
    c.attackDir = -1;
    e.setConfig(c);
    std::string line;
    long frames = 0, shortReads = 0;
    auto mx = [](double q) { return (float)((q / 1000.0 - 0.5) * 105.0); };
    auto my = [](double q) { return (float)((q / 1000.0 - 0.5) * 68.0); };
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == 't') continue;
        std::vector<double> v;
        std::stringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, ',')) if (!tok.empty()) v.push_back(atof(tok.c_str()));
        if (v.size() < 7) continue;
        const int nh = (int)v[2], na = (int)v[3];
        MetricVector2D h[11], a[11], ball(mx(v[5]), my(v[6]));
        size_t k = 7;
        for (int i = 0; i < nh && i < 11; i++, k += 2) h[i] = MetricVector2D(mx(v[k]), my(v[k + 1]));
        for (int i = 0; i < na && i < 11; i++, k += 2) a[i] = MetricVector2D(mx(v[k]), my(v[k + 1]));
        if (nh < 11) shortReads++;
        e.setFrameTimeMs(100000.0 + v[0]);
        e.ingestDetections(h, nh, a, na, v[4] > 0 ? &ball : nullptr, v[4] > 0);
        frames++;
    }
    CHECK(frames > 500, "excerpt has real frames");
    CHECK(shortReads > frames / 2, "most frames show fewer than 11 home dots (the real radar condition)");
    CHECK(e.healthLog().find("radar inconsistent") == std::string::npos, "a real read with changing dot sets never triggers the radar-glitch recovery");
    CHECK(e.oppLineConf() >= 0.f && e.oppLineDepth() > 0.f && e.oppLineDepth() < 105.f, "opponent line estimate stays within the pitch on real data");
    std::printf("\n%s (%d failures)\n", g_fail ? "SOME TRACK CHECKS FAILED" : "ALL TRACK CHECKS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
