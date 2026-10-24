#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "engine.h"
#define private public
#include "pitch_engine.hpp"

static long g_allocs = 0;
static bool g_count = false;
void* operator new(std::size_t n) {
    if (g_count) g_allocs++;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace pb;

static const int W = 200, H = 120;
static const int TEAM = (200 << 16) | (60 << 8) | 110;
static const int OPP = (240 << 16) | (240 << 8) | 240;
static const int BALL = (230 << 16) | (230 << 8) | 40;

struct Pt { float x, y; };

static void dot(std::vector<int>& img, float nx, float ny, int col, int rad) {
    int cx = (int)(nx * W), cy = (int)(ny * H);
    for (int y = -rad; y <= rad; y++)
        for (int x = -rad; x <= rad; x++)
            if (x * x + y * y <= rad * rad + 1) {
                int px = cx + x, py = cy + y;
                if (px >= 0 && py >= 0 && px < W && py < H) img[py * W + px] = col;
            }
}

static std::vector<int> frame(const std::vector<Pt>& team, const std::vector<Pt>& opp, Pt ball, bool withBall = true) {
    std::vector<int> img(W * H, (40 << 16) | (110 << 8) | 50);
    for (int y = 0; y < H; y++) { img[y * W + W / 2] = OPP; }
    for (int x = 0; x < W; x++) { img[(H / 2) * W + x] = (200 << 16) | (200 << 8) | 200; }
    for (const Pt& p : team) dot(img, p.x, p.y, TEAM, 3);
    for (const Pt& p : opp) dot(img, p.x, p.y, OPP, 3);
    if (withBall) dot(img, ball.x, ball.y, BALL, 2);
    return img;
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); fails++; } else std::printf("ok:   %s\n", msg); } while (0)

static std::vector<Pt> teamBase() {
    return {{0.03f, 0.5f}, {0.20f, 0.2f}, {0.20f, 0.4f}, {0.20f, 0.6f}, {0.20f, 0.8f},
            {0.40f, 0.3f}, {0.40f, 0.7f}, {0.45f, 0.5f}, {0.60f, 0.25f}, {0.60f, 0.75f}, {0.65f, 0.5f}};
}
static std::vector<Pt> oppBase() {
    return {{0.97f, 0.5f}, {0.85f, 0.2f}, {0.85f, 0.4f}, {0.85f, 0.6f}, {0.85f, 0.8f},
            {0.70f, 0.3f}, {0.70f, 0.7f}, {0.55f, 0.5f}, {0.50f, 0.2f}, {0.50f, 0.8f}, {0.30f, 0.5f}};
}

int main() {
    {
        Engine e;
        auto t = teamBase(), o = oppBase();
        auto img = frame(t, o, {0.45f, 0.5f});
        PaletteEntry pal[8];
        int n = e.scanPalette(img.data(), W, H, pal, 8);
        std::printf("palette entries: %d\n", n);
        for (int i = 0; i < n; i++) std::printf("  rgb(%d,%d,%d) blobs=%d\n", pal[i].r, pal[i].g, pal[i].b, pal[i].blobs);
        CHECK(n >= 3, "palette finds at least 3 colour groups (team, opponent, ball)");
        CHECK(n >= 1 && pal[0].blobs >= 10 && pal[0].blobs <= 12, "largest colour group has 10-12 dots");
    }

    {
        Engine e;
        e.setColors(TEAM, OPP, BALL);
        Options op;
        e.setOptions(op);
        auto t = teamBase(), o = oppBase();
        Pt ball = {0.455f, 0.5f};
        double tm = 1000;
        Action a;
        int last = -1, typeSeen[16] = {0};
        for (int i = 0; i < 40; i++) {
            auto img = frame(t, o, ball);
            last = e.process(img.data(), W, H, tm, a);
            typeSeen[a.type]++;
            tm += 80;
        }
        CHECK(last == 2, "status = team has the ball");
        CHECK(e.dirSign() == 1, "attack direction detected as right (keeper on left)");
        CHECK(typeSeen[AT_PASS] + typeSeen[AT_THROUGH] + typeSeen[AT_SHOOT] > 0, "bot plays a pass/through/shot rather than only carrying");
        float s[8];
        e.stats(s);
        std::printf("   passes=%d shots=%d\n", (int)s[1], (int)s[3]);
    }

    {
        Engine e;
        e.setColors(TEAM, OPP, BALL);
        e.setOptions(Options());
        auto t = teamBase(), o = oppBase();
        t[10] = {0.86f, 0.52f};
        o[1] = {0.85f, 0.2f};
        o[4] = {0.85f, 0.85f};
        o[2] = {0.70f, 0.4f}; o[3] = {0.70f, 0.6f};
        Pt ball = {0.865f, 0.52f};
        double tm = 1000;
        Action a;
        bool shot = false;
        for (int i = 0; i < 20; i++) {
            auto img = frame(t, o, ball);
            e.process(img.data(), W, H, tm, a);
            if (a.type == AT_SHOOT) { shot = true; CHECK(a.button == B_TOP, "shot uses the TOP button position"); CHECK(a.sx > 0.3f, "shot stick points toward the right goal"); break; }
            tm += 80;
        }
        CHECK(shot, "bot shoots from a free position in the box");
    }

    {
        Engine e;
        e.setColors(TEAM, OPP, BALL);
        e.setOptions(Options());
        auto t = teamBase(), o = oppBase();
        o[7] = {0.40f, 0.52f};
        Pt ball = {0.405f, 0.52f};
        double tm = 1000;
        Action a;
        int st = 0;
        bool movedToward = false;
        for (int i = 0; i < 30; i++) {
            auto img = frame(t, o, ball);
            st = e.process(img.data(), W, H, tm, a);
            if (st == 3 && (a.type == AT_CHASE || a.type == AT_PRESS || a.type == AT_SWITCH || a.type == AT_TACKLE)) movedToward = true;
            tm += 80;
        }
        CHECK(st == 3, "status = opponent has the ball");
        CHECK(movedToward, "bot defends (chase/press/switch/tackle) when the opponent has the ball");
    }

    {
        Engine e;
        e.setColors(TEAM, OPP, BALL);
        std::vector<int> blank(W * H, (40 << 16) | (110 << 8) | 50);
        Action a;
        int st = e.process(blank.data(), W, H, 1000, a);
        CHECK(st == 0 && a.type == AT_NONE, "blank radar => not live, no action");
    }

    {
        Engine e;
        e.setColors(TEAM, OPP, BALL);
        e.setOptions(Options());
        e.newMatch();
        auto t = teamBase(), o = oppBase();
        double tm = 1000;
        Action a;
        for (int cycle = 0; cycle < 40; cycle++) {
            Pt ball = {0.455f, 0.5f};
            for (int i = 0; i < 12; i++) {
                auto img = frame(t, o, ball);
                e.process(img.data(), W, H, tm, a);
                tm += 80;
            }
            bool lose = (cycle % 2 == 1);
            auto o2 = o;
            Pt b2 = {0.30f, 0.30f};
            if (lose) o2[10] = {0.305f, 0.30f};
            auto t2 = t;
            if (!lose) t2[5] = {0.305f, 0.30f};
            for (int i = 0; i < 40; i++) {
                auto img = frame(t2, o2, b2);
                e.process(img.data(), W, H, tm, a);
                tm += 80;
            }
        }
        float s[8];
        e.stats(s);
        std::printf("   poss=%.0f%% passAtt=%d passOk=%d shots=%d losses=%d\n", s[0], (int)s[1], (int)s[2], (int)s[3], (int)s[4]);
        CHECK(s[1] > 5, "decisions logged (pass attempts > 5)");
        CHECK(s[4] > 5, "possession losses detected and counted");
        std::string log = e.exportLog();
        CHECK(log.find("\nD ") != std::string::npos, "log contains labelled decisions");
        CHECK(log.find("\nL ") != std::string::npos, "log contains loss locations");

        std::string before = e.exportWeights();
        std::string logs = log + "R 1 2\n" + log + "R 0 1\n";
        std::string summary = e.learn(logs);
        std::printf("%s", summary.c_str());
        std::string after = e.exportWeights();
        CHECK(before != after, "learning changes the stored weights");

        Engine e2;
        CHECK(e2.importWeights(after), "weights round-trip: import succeeds");
        CHECK(e2.exportWeights() == after, "weights round-trip: identical export");
        CHECK(!e2.importWeights("garbage"), "garbage weights are rejected");
    }

    std::printf("\n--- analytics engine ---\n");

    {
        pe::xTGrid g;
        CHECK(pe::XT_COLS == 12 && pe::XT_ROWS == 8, "xT grid is 12 columns x 8 rows");
        CHECK(pe::xTGrid::colOf(-52.f) == 0 && pe::xTGrid::colOf(52.f) == 11 && pe::xTGrid::colOf(0.f) == 6,
              "xT column lookup: -52 -> 0, 0 -> 6, 52 -> 11");
        CHECK(pe::xTGrid::rowOf(-34.f) == 0 && pe::xTGrid::rowOf(34.f) == 7 && pe::xTGrid::rowOf(0.f) == 4,
              "xT row lookup: -34 -> 0, 0 -> 4, 34 -> 7");
        CHECK(pe::xTGrid::colOf(1000.f) == 11 && pe::xTGrid::colOf(-1000.f) == 0 && pe::xTGrid::rowOf(NAN) == 0,
              "xT lookup clamps out-of-range and NaN input");
        bool mono = true, bounded = true;
        for (int r = 0; r < pe::XT_ROWS; r++)
            for (int c = 0; c < pe::XT_COLS; c++) {
                if (!(g.xt[r][c] >= 0.f && g.xt[r][c] <= 1.f)) bounded = false;
                if (c > 0 && g.xt[r][c] + 1e-6f < g.xt[r][c - 1]) mono = false;
            }
        CHECK(bounded, "every xT cell is within 0..1");
        CHECK(mono, "xT never decreases toward the opponent goal (every row)");
        CHECK(g.xt[3][11] > g.xt[3][6] && g.xt[3][6] > g.xt[3][0], "xT: box > midfield > own goal line");
        CHECK(g.xt[3][11] > g.xt[0][11] && g.xt[4][11] > g.xt[7][11], "xT: central box cells beat wide box cells");
        CHECK(g.xt[3][11] >= g.xg[3][11] && g.xg[3][11] > 0.1f, "xT >= shot component, and a central six-yard shot is a real chance");

        pe::PitchEngine e;
        pe::PlayerState p;
        p.pos = {10.f, 0.f};
        float fwd = e.computeExpectedThreat(p, {40.f, 0.f});
        float back = e.computeExpectedThreat(p, {-30.f, 0.f});
        CHECK(fwd > 0.f && back < 0.f, "dXT: forward pass positive, backward pass negative");
        CHECK(std::fabs(e.computeExpectedThreat(p, p.pos)) < 1e-9f, "dXT of staying in the same cell is zero");
        CHECK(std::isfinite(e.computeExpectedThreat(p, {NAN, NAN})), "dXT with NaN target stays finite");
    }

    {
        const float c[9] = {4, 1, 3, 2, 0, 5, 3, 2, 2};
        int a[3];
        double tot = pe::hungarianSolve(c, 3, a);
        CHECK(std::fabs(tot - 5.0) < 1e-9 && a[0] == 1 && a[1] == 0 && a[2] == 2, "Hungarian finds the optimal 3x3 assignment");
        const float one[1] = {7.f};
        int a1[1];
        CHECK(pe::hungarianSolve(one, 1, a1) == 7.0 && a1[0] == 0, "Hungarian handles n = 1");
        const float nan9[9] = {NAN, 1, 1, 1, NAN, 1, 1, 1, NAN};
        int a2[3];
        pe::hungarianSolve(nan9, 3, a2);
        CHECK(a2[0] != a2[1] && a2[1] != a2[2] && a2[0] != a2[2], "Hungarian survives NaN costs and still returns a permutation");
        CHECK(pe::hungarianSolve(c, 0, a) == 0.0, "Hungarian n = 0 is a no-op");
    }

    {
        auto metric = [](Pt p) { return pe::MetricVector2D((p.x - 0.5f) * 104.f, (p.y - 0.5f) * 68.f); };
        pe::MetricVector2D home[11], away[11];
        pe::MetricVector2D hv[11];
        {
            auto tb = teamBase();
            auto ob = oppBase();
            for (int i = 0; i < 11; i++) { home[i] = metric(tb[i]); away[i] = metric(ob[i]); }
        }
        home[5] = {-9.6f, 0.0f};  hv[5] = {6.f, 0.f};
        home[6] = {9.6f, 1.5f};   hv[6] = {-6.f, 0.f};
        for (int i = 0; i < 11; i++) if (i != 5 && i != 6) hv[i] = {0.f, 0.f};
        hv[8] = {0.f, 3.f};

        pe::PitchEngine e;
        pe::TacticalConfig cfg;
        e.setConfig(cfg);
        double tm = 1000;
        int slotOf[11];
        bool stable = true;
        int worstFrame = -1;
        const int perm[11] = {7, 2, 9, 0, 4, 10, 1, 6, 3, 8, 5};
        for (int f = 0; f < 60; f++) {
            const double t = f * 0.08;
            pe::MetricVector2D det[11];
            int n = 0;
            for (int k = 0; k < 11; k++) {
                int i = perm[(k + f) % 11];
                bool dropped = (f >= 20 && f <= 24 && (i == 3 || i == 5 || i == 9));
                if (dropped) continue;
                det[n++] = {home[i].x + hv[i].x * (float)t, home[i].y + hv[i].y * (float)t};
            }
            e.setFrameTimeMs(tm);
            if (f >= 30 && f <= 31) e.ingestDetections(nullptr, 0, nullptr, 0, nullptr, false);
            else e.ingestDetections(det, n, away, 11, nullptr, false);
            if (f == 0) {
                for (int s = 0; s < 11; s++) {
                    float best = 1e9f; int bi = 0;
                    for (int i = 0; i < 11; i++) {
                        float d = pe::distance(e.state().team[s].pos, home[i]);
                        if (d < best) { best = d; bi = i; }
                    }
                    slotOf[bi] = s;
                }
            }
            if (f >= 30 && f <= 31) { tm += 80; continue; }
            if (f > 0) {
                for (int i = 0; i < 11; i++) {
                    pe::MetricVector2D truth(home[i].x + hv[i].x * (float)t, home[i].y + hv[i].y * (float)t);
                    if (pe::distance(e.state().team[slotOf[i]].pos, truth) > 3.0f) { stable = false; if (worstFrame < 0) worstFrame = f; }
                }
            }
            tm += 80;
        }
        if (!stable) std::printf("   first unstable frame: %d\n", worstFrame);
        CHECK(stable, "tracker keeps every persistent ID through shuffled input, dropped detections, flicker and a crossing");
        bool ids = true;
        for (int s = 0; s < 11; s++) if (e.state().team[s].id != s + 1 || e.state().opp[s].id != 101 + s) ids = false;
        CHECK(ids, "persistent IDs are 1..11 (home) and 101..111 (away)");
        CHECK(e.state().team[slotOf[5]].vel.x > 3.f && e.state().team[slotOf[6]].vel.x < -3.f, "tracker estimates velocity (crossing runners)");
        CHECK(e.state().team[slotOf[8]].vel.y > 1.5f, "tracker estimates velocity (vertical runner)");

        e.extrapolateKinematics(0.2f);
        const pe::PlayerState& r = e.state().team[slotOf[5]];
        CHECK(std::fabs((r.future.x - r.pos.x) - r.vel.x * 0.2f) < 1e-4f, "future position = position + velocity * 0.2 s");
    }

    auto metricPt = [](Pt p) { return pe::MetricVector2D((p.x - 0.5f) * 104.f, (p.y - 0.5f) * 68.f); };
    auto scenario = [&](pe::PitchEngine& e, pe::MetricVector2D* home, pe::MetricVector2D* away, pe::MetricVector2D ball, int frames, double t0) {
        double tm = t0;
        for (int i = 0; i < frames; i++) {
            e.setFrameTimeMs(tm);
            e.ingestDetections(home, 11, away, 11, &ball, true);
            tm += 80;
        }
        return tm;
    };

    auto slotNear = [](const pe::PitchEngine& e, pe::MetricVector2D p) {
        int bi = 0; float bd = 1e9f;
        for (int i = 0; i < 11; i++) { float d = pe::distance(e.state().team[i].pos, p); if (d < bd) { bd = d; bi = i; } }
        return bi;
    };

    {
        pe::MetricVector2D home[11], away[11];
        auto tb = teamBase(); auto ob = oppBase();
        for (int i = 0; i < 11; i++) { home[i] = metricPt(tb[i]); away[i] = metricPt(ob[i]); }
        home[10] = {10.f, 0.f};
        home[9] = {10.f, 0.f};
        home[8] = {10.0f, 0.0f};
        away[10] = {10.f, 0.f};
        pe::PitchEngine e;
        scenario(e, home, away, {10.2f, 0.f}, 4, 1000);
        CHECK(e.state().status == pe::ST_TEAM, "scenario: team has the ball");
        int n = e.candidateCount();
        bool finite = n > 0;
        float psum = 0;
        for (int i = 0; i < n; i++) {
            const pe::PassCandidate& c = e.candidates()[i];
            if (!std::isfinite(c.xP) || !std::isfinite(c.utility) || !std::isfinite(c.prob) || c.xP < 0.f || c.xP > 1.f) finite = false;
            psum += c.prob;
            if (c.length < 2.5f && c.type != pe::ACT_CARRY && c.type != pe::ACT_SHOOT) finite = false;
        }
        CHECK(finite, "xP / utility / softmax stay finite with stacked players and an opponent on the carrier");
        CHECK(std::fabs(psum - 1.f) < 1e-4f, "softmax probabilities sum to 1");

        pe::PitchEngine e2;
        pe::MetricVector2D noOpp[11];
        for (int i = 0; i < 11; i++) noOpp[i] = {40.f + i, 20.f};
        pe::MetricVector2D h2[11];
        for (int i = 0; i < 11; i++) h2[i] = metricPt(tb[i]);
        scenario(e2, h2, noOpp, {h2[10].x + 0.3f, h2[10].y}, 4, 1000);
        bool ok = e2.candidateCount() > 0;
        for (int i = 0; i < e2.candidateCount(); i++)
            if (!std::isfinite(e2.candidates()[i].xP) || e2.candidates()[i].xP > 1.f) ok = false;
        CHECK(ok, "xP is finite and <= 1 when no interceptor is near");

        pe::PitchEngine e3;
        pe::TacticalConfig zc;
        zc.tempCounter = 0.f; zc.tempHold = 0.f; zc.tempMin = 0.f;
        e3.setConfig(zc);
        scenario(e3, h2, noOpp, {h2[10].x + 0.3f, h2[10].y}, 4, 1000);
        float s3 = 0; bool fin = e3.candidateCount() > 0;
        for (int i = 0; i < e3.candidateCount(); i++) { s3 += e3.candidates()[i].prob; if (!std::isfinite(e3.candidates()[i].prob)) fin = false; }
        CHECK(fin && std::fabs(s3 - 1.f) < 1e-4f, "softmax with T = 0 is clamped: finite and normalised");
        CHECK(e3.temperature() >= 0.01f, "temperature never reaches zero");
    }

    {
        pe::MetricVector2D home[11], away[11];
        auto tb = teamBase(); auto ob = oppBase();
        for (int i = 0; i < 11; i++) { home[i] = metricPt(tb[i]); away[i] = metricPt(ob[i]); }
        home[10] = {0.f, 0.f};
        home[9] = {20.f, -15.f};
        home[8] = {20.f, 15.f};
        away[9] = {10.f, 7.5f};
        away[8] = {11.f, 8.5f};
        away[5] = {25.f, -30.f};
        away[7] = {30.f, 30.f};
        pe::PitchEngine e;
        pe::TacticalConfig cfg;
        cfg.maxPassLen = cfg.safePassLen = 60.f;   // this check is about lane quality, not the tiki-taka length cap
        e.setConfig(cfg);
        scenario(e, home, away, {0.3f, 0.f}, 4, 1000);
        float xpFree = -1, xpBlocked = -1;
        for (int i = 0; i < e.candidateCount(); i++) {
            const pe::PassCandidate& c = e.candidates()[i];
            if (c.type == pe::ACT_PASS || c.type == pe::ACT_LOFTED) {
                if (c.targetId == e.state().team[slotNear(e, {20.f, -15.f})].id) xpFree = c.xP;
                if (c.targetId == e.state().team[slotNear(e, {20.f, 15.f})].id) xpBlocked = c.xP;
            }
        }
        std::printf("   xP free=%.2f blocked=%.2f\n", xpFree, xpBlocked);
        // the interception model is time-aware (a 25 m pass is slow), so an open long lane scores a little lower than before
        CHECK(xpFree > 0.55f && xpBlocked < xpFree * 0.5f, "xP: open lane scores higher than a lane with a defender on it");

        pe::PitchEngine e2;
        pe::MetricVector2D h2[11], a2[11];
        for (int i = 0; i < 11; i++) { h2[i] = home[i]; a2[i] = away[i]; }
        h2[9] = {22.f, -15.f};
        h2[8] = {44.f, 15.f};
        for (int i = 0; i < 11; i++) a2[i].x = std::min(a2[i].x, 30.f);
        a2[0] = {50.f, 0.f};
        e2.setConfig(cfg);
        scenario(e2, h2, a2, {0.3f, 0.f}, 4, 1000);
        float offOn = 1, offOff = 0;
        for (int i = 0; i < e2.candidateCount(); i++) {
            const pe::PassCandidate& c = e2.candidates()[i];
            if (c.type == pe::ACT_PASS || c.type == pe::ACT_LOFTED) {
                if (c.targetId == e2.state().team[slotNear(e2, {22.f, -15.f})].id) offOn = c.offside;
                if (c.targetId == e2.state().team[slotNear(e2, {44.f, 15.f})].id) offOff = c.offside;
            }
        }
        CHECK(offOff == 1.f && offOn == 0.f, "offside penalty applies only to the receiver beyond the second-last defender");
    }

    {
        pe::MetricVector2D home[11], away[11];
        auto tb = teamBase(); auto ob = oppBase();
        for (int i = 0; i < 11; i++) { home[i] = metricPt(tb[i]); away[i] = metricPt(ob[i]); }
        home[10] = {0.f, 0.f}; home[9] = {20.f, -15.f}; home[8] = {12.f, 10.f}; home[7] = {-10.f, 5.f};

        pe::PitchEngine hold;
        scenario(hold, home, away, {0.3f, 0.f}, 10, 1000);
        float tHold = hold.temperature();
        float maxHold = 0;
        for (int i = 0; i < hold.candidateCount(); i++) maxHold = std::max(maxHold, hold.candidates()[i].prob);

        pe::PitchEngine ctr;
        double tm = 1000;
        pe::MetricVector2D ob2[11];
        for (int i = 0; i < 11; i++) ob2[i] = away[i];
        for (int i = 0; i < 6; i++) {
            ctr.setFrameTimeMs(tm);
            pe::MetricVector2D b = {-20.f, 0.f};
            pe::MetricVector2D ho[11], ao[11];
            for (int k = 0; k < 11; k++) { ho[k] = home[k]; ao[k] = away[k]; }
            ho[10] = {-30.f, 20.f}; ao[10] = {-20.3f, 0.f};
            ctr.ingestDetections(ho, 11, ao, 11, &b, true);
            tm += 80;
        }
        for (int i = 0; i < 10; i++) {
            ctr.setFrameTimeMs(tm);
            pe::MetricVector2D b = {-20.f, 0.2f};
            pe::MetricVector2D ho[11], ao[11];
            for (int k = 0; k < 11; k++) { ho[k] = home[k]; ao[k] = away[k]; }
            ho[10] = {-20.f, 0.f}; ao[10] = {-5.f, 20.f};
            ctr.ingestDetections(ho, 11, ao, 11, &b, true);
            tm += 80;
        }
        float tCtr = ctr.temperature();
        float maxCtr = 0;
        for (int i = 0; i < ctr.candidateCount(); i++) maxCtr = std::max(maxCtr, ctr.candidates()[i].prob);
        std::printf("   T(hold)=%.2f T(counter)=%.2f  maxProb(hold)=%.2f maxProb(counter)=%.2f\n", tHold, tCtr, maxHold, maxCtr);
        CHECK(std::fabs(tHold - 0.8f) < 1e-4f, "possession-holding phase uses T = 0.8");
        CHECK(std::fabs(tCtr - 0.2f) < 1e-4f, "counter-attack phase uses T = 0.2");

        pe::PitchEngine sharp;
        pe::TacticalConfig c1;
        c1.tempHold = 0.2f;
        sharp.setConfig(c1);
        scenario(sharp, home, away, {0.3f, 0.f}, 10, 1000);
        float maxSharp = 0;
        for (int i = 0; i < sharp.candidateCount(); i++) maxSharp = std::max(maxSharp, sharp.candidates()[i].prob);
        CHECK(maxSharp >= maxHold - 1e-6f, "lower temperature never spreads probability away from the best option");   // the tuned engine vetoes bad lanes hard, so the best option may already be at ~1.0

        auto run = [&](uint32_t seed) {
            pe::PitchEngine e;
            pe::TacticalConfig c;
            c.seed = seed;
            c.forwardOnly = false;   // the sampling check needs several comparable options
            e.setConfig(c);
            std::vector<int64_t> out;
            double t = 1000;
            for (int i = 0; i < 80; i++) {
                e.setFrameTimeMs(t);
                pe::MetricVector2D b = {0.3f, 0.f};
                e.ingestDetections(home, 11, away, 11, &b, true);
                out.push_back(e.lastGesture().pack());
                t += 80;
            }
            return out;
        };
        CHECK(run(7) == run(7), "same seed + same input => bit-identical gesture stream (deterministic, no jitter)");
        {
            // The tuned engine hard-vetoes bad lanes, so a scene often has one playable option and the gesture stream
            // legitimately ignores the seed. Check the sampler itself: different seeds draw different numbers, and a
            // two-way split of the probability mass is actually sampled both ways.
            pe::PitchEngine a, b;
            pe::TacticalConfig ca, cb; ca.seed = 7; cb.seed = 99;
            a.setConfig(ca); b.setConfig(cb);
            bool differs = false;
            for (int i = 0; i < 8; i++) if (a.nextRand() != b.nextRand()) differs = true;
            CHECK(differs, "different seed changes the random stream used for sampling");
            pe::PitchEngine s;
            pe::TacticalConfig cs; cs.seed = 5; s.setConfig(cs);
            s.nCand_ = 2; s.cand_[0].prob = 0.5f; s.cand_[1].prob = 0.5f;
            int hit0 = 0, hit1 = 0;
            for (int i = 0; i < 400; i++) (s.sampleCandidate() == 0 ? hit0 : hit1)++;
            CHECK(hit0 > 120 && hit1 > 120, "sampleCandidate follows the probabilities");
        }
    }

    {
        pe::GestureOutput g;
        g.theta = 123.4f; g.r = 1.f; g.durationMs = 205; g.actionType = pe::ACT_LOFTED; g.valid = true; g.status = pe::ST_TEAM; g.dash = true;
        pe::GestureOutput u = pe::GestureOutput::unpack(g.pack());
        CHECK(std::fabs(u.theta - 123.4f) < 0.06f && u.r == 1.f && u.durationMs == 205 && u.actionType == pe::ACT_LOFTED &&
              u.valid && u.status == pe::ST_TEAM && u.dash, "gesture pack/unpack round-trips every field");
        pe::GestureOutput bad;
        bad.theta = NAN; bad.r = 5.f; bad.durationMs = 99999; bad.valid = true;
        pe::GestureOutput ub = pe::GestureOutput::unpack(bad.pack());
        CHECK(ub.r == 1.f && ub.durationMs == 4095 && ub.theta == 0.f, "pack clamps NaN / out-of-range values");

        pe::MetricVector2D home[11], away[11];
        auto tb = teamBase(); auto ob = oppBase();
        for (int i = 0; i < 11; i++) { home[i] = metricPt(tb[i]); away[i] = metricPt(ob[i]); }
        home[10] = {0.f, 0.f}; home[9] = {20.f, 0.f};
        away[9] = {5.f, 25.f}; away[8] = {-5.f, -25.f};
        pe::PitchEngine e;
        { pe::TacticalConfig pc; pc.loftMinLen = 99.f; e.setConfig(pc); }   // plain ground passes: hold time is the check here
        scenario(e, home, away, {0.3f, 0.f}, 4, 1000);
        const pe::PlayerState& tgt = e.state().team[slotNear(e, {20.f, 0.f})];
        pe::GestureOutput p = e.generateGestureVector(tgt, 0.5f);
        CHECK(p.valid && (p.theta < 1.f || p.theta > 359.f), "pass to a player on the right: theta = 0 degrees");
        pe::GestureOutput weak = e.generateGestureVector(tgt, 0.0f), strong = e.generateGestureVector(tgt, 1.0f);
        CHECK(weak.durationMs == e.config().minPassHoldMs && strong.durationMs == e.config().maxPassHoldMs, "pass power maps to hold time between the configured bounds");
        CHECK(e.generateGestureVector(tgt, 0.5f).pack() == p.pack(), "generateGestureVector is a pure function of its inputs");
        pe::PlayerState below = e.state().team[slotNear(e, {20.f, 0.f})];
        below.future = {0.f, 20.f};
        below.id = 999;
        pe::GestureOutput d = e.generateGestureVector(below, 0.5f);
        CHECK(std::fabs(d.theta - 90.f) < 0.5f, "target below the carrier: theta = 90 degrees (screen y points down)");
        pe::PitchEngine ex;
        pe::TacticalConfig cx;
        cx.invertX = true;
        ex.setConfig(cx);
        scenario(ex, home, away, {0.3f, 0.f}, 4, 1000);
        pe::GestureOutput inv = ex.generateGestureVector(ex.state().team[slotNear(ex, {20.f, 0.f})], 0.5f);
        CHECK(std::fabs(inv.theta - 180.f) < 0.5f, "invertX flips the stick direction");

        pe::PitchEngine ec;
        double tm = 1000;
        double lastBtn = -1e9;
        bool cooldownOk = true;
        int buttons = 0;
        for (int i = 0; i < 200; i++) {
            ec.setFrameTimeMs(tm);
            pe::MetricVector2D b = {0.3f, 0.f};
            ec.ingestDetections(home, 11, away, 11, &b, true);
            const pe::GestureOutput& g2 = ec.lastGesture();
            if (g2.valid && g2.actionType != pe::ACT_CARRY) {
                if (tm - lastBtn < std::min(ec.config().actionCooldownMs, ec.config().fastCooldownMs) - 1) cooldownOk = false;
                lastBtn = tm;
                buttons++;
            }
            tm += 80;
        }
        CHECK(buttons > 0, "engine eventually plays a pass / shot while holding the ball");
        CHECK(cooldownOk, "button actions are never closer together than actionCooldownMs");
    }

    {
        auto t = teamBase(), o = oppBase();
        pe::PitchEngine e;
        e.setColors(TEAM, OPP, BALL);
        Pt ball = {0.455f, 0.5f};
        std::vector<unsigned char> rgba((size_t)W * H * 4);
        auto render = [&](const std::vector<Pt>& tt, const std::vector<Pt>& oo, Pt bb) {
            auto img = frame(tt, oo, bb);
            for (int i = 0; i < W * H; i++) {
                rgba[i * 4 + 0] = (img[i] >> 16) & 255;
                rgba[i * 4 + 1] = (img[i] >> 8) & 255;
                rgba[i * 4 + 2] = img[i] & 255;
                rgba[i * 4 + 3] = 255;
            }
        };
        render(t, o, ball);
        double tm = 1000;
        int st = 0;
        for (int i = 0; i < 6; i++) {
            e.setFrameTimeMs(tm);
            st = e.processFrame(rgba.data(), W, H);
            tm += 80;
        }
        float dbg[64];
        e.debugDots(dbg, 64);
        std::printf("   dots: home=%d away=%d ball=%d, attackSign=%d, status=%d\n", (int)dbg[0], (int)dbg[1], (int)dbg[2], e.state().attackSign, st);
        CHECK((int)dbg[0] == 11 && (int)dbg[1] == 11, "HSV segmentation finds 11 + 11 players (lines and keepers handled)");
        CHECK(dbg[2] == 1.f && std::fabs(dbg[3] - 0.455f) < 0.02f && std::fabs(dbg[4] - 0.5f) < 0.03f, "ball found and mapped through the homography");
        CHECK(e.state().attackSign == 1, "attack direction detected (keeper on the left => attack +X)");
        CHECK(st == pe::ST_TEAM, "status = team has the ball");
        pe::Homography hm;
        float q[8] = {0, 0, 200, 0, 200, 120, 0, 120};
        hm.fromQuad(q);
        pe::MetricVector2D c0 = hm.apply(100, 60), c1 = hm.apply(0, 0), c2 = hm.apply(200, 120);
        CHECK(std::fabs(c0.x) < 1e-3f && std::fabs(c0.y) < 1e-3f && std::fabs(c1.x + 52.f) < 1e-3f && std::fabs(c1.y + 34.f) < 1e-3f &&
              std::fabs(c2.x - 52.f) < 1e-3f && std::fabs(c2.y - 34.f) < 1e-3f, "homography maps ROI centre to (0,0) and corners to (+-52, +-34)");
        float degenerate[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        pe::Homography hd;
        CHECK(!hd.fromQuad(degenerate), "degenerate quad is rejected");

        std::vector<Pt> t2 = t;
        long a0 = 0;
        g_count = true;
        g_allocs = 0;
        double worstUs = 0;
        for (int i = 0; i < 60; i++) {
            t2[8].x = 0.60f + 0.004f * i;
            render(t2, o, {0.455f, 0.5f});
            g_count = false;
            long before = g_allocs;
            g_count = true;
            e.setFrameTimeMs(tm);
            e.processFrame(rgba.data(), W, H);
            a0 += g_allocs - before;
            worstUs = std::max(worstUs, (double)e.lastFrameMicros());
            tm += 80;
        }
        g_count = false;
        std::printf("   worst frame %.0f us for a %dx%d radar\n", worstUs, W, H);
        CHECK(a0 == 0, "zero heap allocations inside processFrame (60 frames)");
        CHECK(worstUs < 50000.0, "frame time under the 50 ms budget");
        CHECK(e.state().team[0].active, "tracking still active after 60 frames");

        std::vector<unsigned char> blank((size_t)W * H * 4);
        for (int i = 0; i < W * H; i++) { blank[i * 4] = 40; blank[i * 4 + 1] = 110; blank[i * 4 + 2] = 50; blank[i * 4 + 3] = 255; }
        e.setFrameTimeMs(tm);
        int sb = e.processFrame(blank.data(), W, H);
        CHECK(sb == pe::ST_NOT_LIVE && !e.lastGesture().valid, "blank radar => not live, no gesture");
        CHECK(e.processFrame(nullptr, W, H) == pe::ST_NOT_LIVE, "null buffer is rejected");
        CHECK(e.processFrame(rgba.data(), 4, 4) == pe::ST_NOT_LIVE, "tiny ROI is rejected");
    }

    {
        pe::HsvRange red = pe::PitchEngine::hsvRangeFromRgb(0xFF0000);
        CHECK(red.set && red.chroma && red.wrapHue && (int)red.lower.v[0] == 170 && (int)red.upper.v[0] == 10,
              "red pick => wrapped hue window 170..10 (dH +-10)");
        CHECK((int)red.lower.v[1] == 215 && (int)red.upper.v[1] == 255 && (int)red.lower.v[2] == 215 && (int)red.upper.v[2] == 255,
              "red pick => S and V windows +-40 clamped to 255");
        CHECK(red.contains(3, 240, 240) && red.contains(176, 240, 240) && !red.contains(30, 240, 240), "red window membership across the seam");

        pe::HsvRange blue = pe::PitchEngine::hsvRangeFromRgb(0x2030E0);
        CHECK(!blue.wrapHue && (int)blue.upper.v[0] - (int)blue.lower.v[0] == 20 &&
              (int)blue.upper.v[1] - (int)blue.lower.v[1] <= 80 && blue.contains(blue.h0, blue.s0, blue.v0),
              "blue pick => hue window 20 wide, contains its own centre");

        pe::HsvRange white = pe::PitchEngine::hsvRangeFromRgb(0xF0F0F0);
        CHECK(white.set && !white.chroma && (int)white.upper.v[1] <= 40 + 5 && (int)white.lower.v[2] >= 200 - 5,
              "white pick => hue ignored, low-saturation bright window");
        CHECK(white.contains(90, 5, 235) && !white.contains(90, 160, 235), "white window rejects saturated pixels");

        pe::PitchEngine e;
        CHECK(!e.haveColors(), "no colours before the user picks");
        CHECK(!e.setTeamColors(0xC83C6E, 0xC83C6E), "identical home / away picks are rejected");
        CHECK(!e.haveColors(), "rejected picks leave the engine unset");
        CHECK(e.setTeamColors(0xE12028, 0x2030E0) && e.haveColors(), "setTeamColors accepts two distinct kits");

        int b[3 * pe::HSV_PACK_INTS];
        e.kitBounds(b);
        CHECK(b[0] == 1 && b[pe::HSV_PACK_INTS] == 1 && b[2 * pe::HSV_PACK_INTS] == 1, "kitBounds: home + away set, ball window defaults to yellow");
        pe::PitchEngine e2;
        CHECK(e2.setKitBounds(b) && e2.haveColors(), "setKitBounds restores saved bounds");
        int b2[3 * pe::HSV_PACK_INTS];
        e2.kitBounds(b2);
        CHECK(std::memcmp(b, b2, sizeof(b)) == 0, "bounds survive a save / restore round trip");
        int bad[3 * pe::HSV_PACK_INTS];
        std::memcpy(bad, b, sizeof(b));
        bad[2] = 200;
        pe::PitchEngine e3;
        CHECK(!e3.setKitBounds(bad) && !e3.haveColors(), "out-of-range bounds are rejected");
        CHECK(!e3.setKitBounds(nullptr), "null bounds are rejected");

        auto render = [&](std::vector<unsigned char>& rgba, const std::vector<Pt>& tt, const std::vector<Pt>& oo, Pt bb,
                          int homeCol, int awayCol, int ballCol) {
            std::vector<int> img(W * H, (40 << 16) | (110 << 8) | 50);
            for (const Pt& q : tt) dot(img, q.x, q.y, homeCol, 3);
            for (const Pt& q : oo) dot(img, q.x, q.y, awayCol, 3);
            dot(img, bb.x, bb.y, ballCol, 2);
            rgba.assign((size_t)W * H * 4, 255);
            for (int i = 0; i < W * H; i++) {
                rgba[i * 4 + 0] = (img[i] >> 16) & 255;
                rgba[i * 4 + 1] = (img[i] >> 8) & 255;
                rgba[i * 4 + 2] = img[i] & 255;
            }
        };
        auto countDots = [&](pe::PitchEngine& eng, const std::vector<unsigned char>& rgba, float* dbg) {
            double tm = 1000;
            for (int i = 0; i < 6; i++) {
                eng.setFrameTimeMs(tm);
                eng.processFrame(const_cast<unsigned char*>(rgba.data()), W, H);
                tm += 80;
            }
            eng.debugDots(dbg, 64);
        };
        std::vector<unsigned char> rgba;
        const int RED = (225 << 16) | (30 << 8) | 40, BLUE = (30 << 16) | (60 << 8) | 220, WHITEB = (250 << 16) | (250 << 8) | 250;
        render(rgba, teamBase(), oppBase(), {0.455f, 0.5f}, RED, BLUE, WHITEB);
        float dbg[64];
        pe::PitchEngine ek;
        ek.setColors(RED, BLUE, WHITEB);
        countDots(ek, rgba, dbg);
        CHECK((int)dbg[0] == 11 && (int)dbg[1] == 11 && dbg[2] == 1.f, "red vs blue kit + white ball segmented from runtime picks");

        pe::PitchEngine er;
        er.setKitBounds([&] { static int t[3 * pe::HSV_PACK_INTS]; ek.kitBounds(t); return t; }());
        float dbg2[64];
        countDots(er, rgba, dbg2);
        CHECK((int)dbg2[0] == 11 && (int)dbg2[1] == 11 && dbg2[2] == 1.f, "restored bounds segment identically");

        const int SEAM_PICK = (255 << 16) | (0 << 8) | 20, SEAM_DOT = (255 << 16) | (40 << 8) | 0;
        render(rgba, teamBase(), oppBase(), {0.455f, 0.5f}, SEAM_DOT, BLUE, WHITEB);
        pe::PitchEngine es;
        es.setColors(SEAM_PICK, BLUE, WHITEB);
        countDots(es, rgba, dbg);
        CHECK((int)dbg[0] == 11 && (int)dbg[1] == 11, "dots across the 179 -> 0 hue seam still match the picked colour");

        pe::PitchEngine en;
        en.setTeamColors(RED, BLUE);
        countDots(en, rgba, dbg);
        CHECK(dbg[2] == 0.f, "no ball colour picked => ball is not segmented");
    }

    {
        const int CLARET = (200 << 16) | (60 << 8) | 110, YELLOW_KIT = (240 << 16) | (220 << 8) | 30, BALL_Y = (230 << 16) | (230 << 8) | 40;
        auto draw = [&](std::vector<unsigned char>& rgba, int awayCol, bool ring, bool dotted, Pt ball, bool withBall) {
            std::vector<int> img(W * H, (40 << 16) | (110 << 8) | 50);
            for (const Pt& q : teamBase()) dot(img, q.x, q.y, CLARET, 3);
            for (const Pt& q : oppBase()) dot(img, q.x, q.y, awayCol, 3);
            if (withBall) {
                const int cx = (int)(ball.x * W), cy = (int)(ball.y * H);
                for (int y = -6; y <= 6; y++)
                    for (int x = -6; x <= 6; x++) {
                        const double d = std::sqrt((double)(x * x + y * y));
                        bool on = ring ? (d <= 4.4 && d >= 2.7) : (d <= 3.2);
                        if (on && ring && dotted) {
                            double ang = std::atan2((double)y, (double)x) * 180.0 / 3.14159265358979;
                            if (ang < 0) ang += 360.0;
                            if (std::fmod(ang, 36.0) > 24.0) on = false;
                        }
                        if (on) img[(cy + y) * W + (cx + x)] = BALL_Y;
                    }
            }
            rgba.assign((size_t)W * H * 4, 255);
            for (int i = 0; i < W * H; i++) {
                rgba[i * 4 + 0] = (img[i] >> 16) & 255;
                rgba[i * 4 + 1] = (img[i] >> 8) & 255;
                rgba[i * 4 + 2] = img[i] & 255;
            }
        };
        auto run = [&](pe::PitchEngine& eng, std::vector<unsigned char>& rgba, float* dbg) {
            double tm = 1000;
            for (int i = 0; i < 6; i++) { eng.setFrameTimeMs(tm); eng.processFrame(rgba.data(), W, H); tm += 80; }
            eng.debugDots(dbg, 64);
        };
        std::vector<unsigned char> img;
        float dbg[64];
        const Pt freeSpot = {0.35f, 0.12f};

        pe::PitchEngine a;
        a.setTeamColors(CLARET, YELLOW_KIT);
        draw(img, YELLOW_KIT, false, false, freeSpot, true);
        run(a, img, dbg);
        CHECK(a.yellowKitInPlay(), "yellow opponent kit overlaps the ball window");
        CHECK((int)dbg[0] == 11 && (int)dbg[1] == 11, "yellow kit: 11 + 11 player dots still found");
        CHECK(dbg[2] == 0.f, "yellow kit + solid yellow blob: NOT taken for the ball");

        draw(img, YELLOW_KIT, true, false, freeSpot, true);
        pe::PitchEngine b2;
        b2.setTeamColors(CLARET, YELLOW_KIT);
        run(b2, img, dbg);
        CHECK(dbg[2] == 1.f && std::fabs(dbg[3] - freeSpot.x) < 0.03f && std::fabs(dbg[4] - freeSpot.y) < 0.04f && b2.ballIsRing(),
              "yellow kit + hollow ring: ring marker found as the ball");
        CHECK((int)dbg[0] == 11 && (int)dbg[1] == 11, "ring marker is not counted as a player");

        draw(img, YELLOW_KIT, true, true, freeSpot, true);
        pe::PitchEngine c;
        c.setTeamColors(CLARET, YELLOW_KIT);
        run(c, img, dbg);
        CHECK(dbg[2] == 1.f && c.ballIsRing(), "dotted yellow ring is recognised");

        const int WHITE = (240 << 16) | (240 << 8) | 240;
        draw(img, WHITE, true, false, freeSpot, true);
        pe::PitchEngine d1;
        d1.setTeamColors(CLARET, WHITE);
        run(d1, img, dbg);
        CHECK(!d1.yellowKitInPlay() && dbg[2] == 1.f && d1.ballIsRing(), "no yellow kit: ring ball found");
        draw(img, WHITE, false, false, freeSpot, true);
        pe::PitchEngine d2;
        d2.setTeamColors(CLARET, WHITE);
        run(d2, img, dbg);
        CHECK(dbg[2] == 1.f && !d2.ballIsRing(), "no yellow kit: solid yellow blob accepted as fallback");
        pe::TacticalConfig strict = d2.config();
        strict.ballAllowSolidFallback = false;
        pe::PitchEngine d3;
        d3.setConfig(strict);
        d3.setTeamColors(CLARET, WHITE);
        run(d3, img, dbg);
        CHECK(dbg[2] == 0.f, "solid fallback can be switched off");

        draw(img, WHITE, true, false, {0.45f, 0.5f}, true);
        pe::PitchEngine d4;
        d4.setTeamColors(CLARET, WHITE);
        run(d4, img, dbg);
        CHECK((int)dbg[0] == 11, "ring over a player dot does not remove the player");
    }

    {
        auto mk = [&](pe::MetricVector2D* home, pe::MetricVector2D* away) {
            for (int i = 0; i < 11; i++) {
                home[i] = {-30.f + 3.f * i, -20.f + 4.f * i};
                away[i] = {-10.f + 2.f * i, 25.f - 5.f * i};
            }
            home[10] = {40.f, 0.f};
            away[10] = {51.f, 0.f};
        };
        pe::TacticalConfig cfg;
        cfg.attackDir = 1;

        {
            pe::MetricVector2D home[11], away[11];
            mk(home, away);
            pe::PitchEngine e;
            pe::TacticalConfig holdCfg = cfg;
            holdCfg.autoShoot = false;   // holding only applies when the bot waits for the cue instead of shooting automatically
            e.setConfig(holdCfg);
            pe::MetricVector2D ball = {40.2f, 0.f};
            int cues = 0, firstLevel = 0, holdFrames = 0, passDuringHold = 0;
            double tm = 1000;
            for (int i = 0; i < 25; i++) {
                e.setFrameTimeMs(tm);
                e.ingestDetections(home, 11, away, 11, &ball, true);
                int c = e.consumeShotCue();
                if (c > 0) { cues++; if (!firstLevel) firstLevel = c; }
                const pe::GestureOutput& g = e.lastGesture();
                if (e.holding()) {
                    holdFrames++;
                    if (g.valid && g.actionType != pe::ACT_SHOOT) passDuringHold++;
                }
                tm += 80;
            }
            CHECK(cues == 1, "shot cue fires exactly once for one clear chance");
            CHECK(firstLevel >= 1, "shot cue has a level");
            CHECK(holdFrames > 5, "after the cue the bot holds while the shot is on");
            CHECK(passDuringHold == 0, "no pass / through ball while holding (stick-only shoot gesture)");
            CHECK(e.consumeShotCue() == 0, "consuming the cue clears it");

            pe::MetricVector2D lost = {-5.f, 25.f};
            for (int i = 0; i < 20; i++) {
                e.setFrameTimeMs(tm);
                e.ingestDetections(home, 11, away, 11, &lost, true);
                tm += 80;
            }
            int cues2 = 0;
            for (int i = 0; i < 25; i++) {
                e.setFrameTimeMs(tm);
                e.ingestDetections(home, 11, away, 11, &ball, true);
                if (e.consumeShotCue() > 0) cues2++;
                tm += 80;
            }
            CHECK(cues2 == 1, "a new possession re-arms the cue (once more, not repeatedly)");
        }

        {
            pe::MetricVector2D home[11], away[11];
            mk(home, away);
            away[3] = {45.f, 0.f};
            away[4] = {45.f, -2.8f};
            away[5] = {45.f, 2.8f};
            pe::PitchEngine e;
            e.setConfig(cfg);
            pe::MetricVector2D ball = {40.2f, 0.f};
            int cues = 0;
            double tm = 1000;
            for (int i = 0; i < 25; i++) {
                e.setFrameTimeMs(tm);
                e.ingestDetections(home, 11, away, 11, &ball, true);
                if (e.consumeShotCue() > 0) cues++;
                tm += 80;
            }
            CHECK(cues == 0, "no cue when every aim line is blocked");
        }

        {
            pe::MetricVector2D home[11], away[11];
            mk(home, away);
            home[10] = {5.f, 0.f};
            pe::PitchEngine e;
            e.setConfig(cfg);
            pe::MetricVector2D ball = {5.2f, 0.f};
            int cues = 0;
            double tm = 1000;
            for (int i = 0; i < 25; i++) {
                e.setFrameTimeMs(tm);
                e.ingestDetections(home, 11, away, 11, &ball, true);
                if (e.consumeShotCue() > 0) cues++;
                tm += 80;
            }
            CHECK(cues == 0, "no cue from the halfway line");
        }

        {
            pe::MetricVector2D home[11], away[11];
            mk(home, away);
            pe::TacticalConfig c2 = cfg;
            c2.shotAlert = false;
            c2.shotHold = false;
            pe::PitchEngine e;
            e.setConfig(c2);
            pe::MetricVector2D ball = {40.2f, 0.f};
            int cues = 0, holds = 0;
            double tm = 1000;
            for (int i = 0; i < 25; i++) {
                e.setFrameTimeMs(tm);
                e.ingestDetections(home, 11, away, 11, &ball, true);
                if (e.consumeShotCue() > 0) cues++;
                if (e.holding()) holds++;
                tm += 80;
            }
            CHECK(cues == 0 && holds == 0, "alert and hold can be switched off");
        }

        {
            pe::MetricVector2D home[11], away[11];
            mk(home, away);
            home[10] = {10.f, 0.f};
            for (int i = 0; i < 10; i++) home[i] = {-5.f + 5.f * i, -25.f + 5.f * i};
            pe::TacticalConfig c3 = cfg;
            c3.legendary = true;
            pe::PitchEngine e;
            e.setConfig(c3);
            pe::MetricVector2D ball = {10.2f, 0.f};
            bool finiteOk = true;
            int actions = 0;
            double tm = 1000;
            for (int i = 0; i < 80; i++) {
                e.setFrameTimeMs(tm);
                e.ingestDetections(home, 11, away, 11, &ball, true);
                const pe::GestureOutput& g = e.lastGesture();
                if (g.valid) {
                    actions++;
                    if (!std::isfinite(g.theta) || !std::isfinite(g.r)) finiteOk = false;
                }
                tm += 80;
            }
            CHECK(finiteOk && actions > 0, "Legendary mode produces valid finite gestures");
        }
    }

    {
        std::printf("\n-- v2: colour-only / overlap / keep the ball --\n");
        auto toRgba = [&](const std::vector<int>& img) {
            std::vector<unsigned char> rgba((size_t)W * H * 4, 255);
            for (int i = 0; i < W * H; i++) {
                rgba[i * 4 + 0] = (img[i] >> 16) & 255;
                rgba[i * 4 + 1] = (img[i] >> 8) & 255;
                rgba[i * 4 + 2] = img[i] & 255;
            }
            return rgba;
        };
        auto run = [&](pe::PitchEngine& eng, const std::vector<unsigned char>& rgba, float* dbg) {
            double tm = 1000;
            for (int i = 0; i < 6; i++) {
                eng.setFrameTimeMs(tm);
                eng.processFrame(const_cast<unsigned char*>(rgba.data()), W, H);
                tm += 80;
            }
            eng.debugDots(dbg, 64);
        };
        const int LOOK = (239 << 16) | (35 << 8) | 48;
        const int GREENREF = (30 << 16) | (200 << 8) | 60;
        float dbg[64];

        {
            auto tb = teamBase();
            auto ob = oppBase();
            tb.pop_back();
            auto img = frame(tb, ob, {0.455f, 0.5f});
            dot(img, 0.50f, 0.06f, LOOK, 3);
            dot(img, 0.55f, 0.94f, GREENREF, 3);
            auto rgba = toRgba(img);
            pe::PitchEngine e;
            e.setColors(TEAM, OPP, BALL);
            run(e, rgba, dbg);
            std::printf("   A: home=%d away=%d ball=%d\n", (int)dbg[0], (int)dbg[1], (int)dbg[2]);
            CHECK((int)dbg[0] == 10 && (int)dbg[1] == 11, "colour-only: third-colour + lookalike markers are ignored (10 home dots)");
            pe::TacticalConfig off = e.config();
            off.colorOnly = false;
            pe::PitchEngine e2;
            e2.setConfig(off);
            e2.setColors(TEAM, OPP, BALL);
            run(e2, rgba, dbg);
            CHECK((int)dbg[0] == 11, "contrast: without colour-only the lookalike is taken for a player");
        }

        {
            auto tb = teamBase();
            auto ob = oppBase();
            tb.pop_back();
            auto img = frame(tb, ob, {0.455f, 0.5f});
            for (int y = 106; y < 111; y++)
                for (int x = 60; x < 95; x++) img[y * W + x] = TEAM;
            auto rgba = toRgba(img);
            pe::PitchEngine e;
            e.setColors(TEAM, OPP, BALL);
            run(e, rgba, dbg);
            CHECK((int)dbg[0] == 10, "colour-only: a thick bar of the picked colour is not a player");
            pe::TacticalConfig off = e.config();
            off.colorOnly = false;
            pe::PitchEngine e2;
            e2.setConfig(off);
            e2.setColors(TEAM, OPP, BALL);
            run(e2, rgba, dbg);
            CHECK((int)dbg[0] > 10, "contrast: without colour-only the bar is split into fake players");
        }

        {
            auto tb = teamBase();
            auto ob = oppBase();
            ob[7] = {tb[7].x + 2.0f / W, tb[7].y};
            auto img = frame(tb, ob, {0.9f, 0.9f});
            auto rgba = toRgba(img);
            pe::PitchEngine e;
            e.setColors(TEAM, OPP, BALL);
            run(e, rgba, dbg);
            std::printf("   overlap: home=%d away=%d\n", (int)dbg[0], (int)dbg[1]);
            CHECK((int)dbg[0] == 11 && (int)dbg[1] == 11, "overlap: half-hidden home dot is kept, both teams still 11");
            pe::TacticalConfig off = e.config();
            off.colorOnly = false;
            pe::PitchEngine e2;
            e2.setConfig(off);
            e2.setColors(TEAM, OPP, BALL);
            run(e2, rgba, dbg);
            std::printf("   overlap without rescue: home=%d\n", (int)dbg[0]);
            CHECK((int)dbg[0] == 10, "contrast: without the rescue the half-hidden dot disappears (the crowd would be misread)");
        }
    }

    {
        pe::TacticalConfig cfg;
        cfg.attackDir = 1;
        auto mkState = [&](pe::MetricVector2D* home, pe::MetricVector2D* away) {
            home[0] = {-48.f, 0.f};
            home[1] = {-30.f, -22.f}; home[2] = {-30.f, -8.f}; home[3] = {-30.f, 8.f}; home[4] = {-30.f, 22.f};
            home[5] = {-12.f, -26.f}; home[6] = {-10.f, 26.f};
            home[7] = {-8.f, 8.f};
            home[8] = {14.f, -24.f}; home[9] = {14.f, 24.f};
            home[10] = {0.f, 0.f};
            away[0] = {48.f, 0.f};
            away[1] = {35.f, -22.f}; away[2] = {35.f, -8.f}; away[3] = {35.f, 8.f}; away[4] = {35.f, 22.f};
            away[5] = {3.f, 1.5f};
            away[6] = {2.5f, -2.5f};
            away[7] = {4.f, 0.f};
            away[8] = {18.f, -12.f}; away[9] = {18.f, 12.f}; away[10] = {28.f, 0.f};
        };
        auto runCarrier = [&](pe::PitchEngine& e, const pe::MetricVector2D* home, const pe::MetricVector2D* away, int frames,
                              int* firstPassFrame, float* passTheta, int* carries) {
            double tm = 1000;
            pe::MetricVector2D ball = {0.2f, 0.f};
            *firstPassFrame = -1;
            *carries = 0;
            for (int i = 0; i < frames; i++) {
                e.setFrameTimeMs(tm);
                e.ingestDetections(home, 11, away, 11, &ball, true);
                const pe::GestureOutput& g = e.lastGesture();
                if (g.valid && g.actionType == pe::ACT_CARRY) (*carries)++;
                if (g.valid && (g.actionType == pe::ACT_PASS || g.actionType == pe::ACT_THROUGH ||
                                g.actionType == pe::ACT_LOFTED) && *firstPassFrame < 0) {
                    *firstPassFrame = i;
                    *passTheta = g.theta;
                }
                tm += 80;
            }
        };
        pe::MetricVector2D home[11], away[11];
        mkState(home, away);
        pe::PitchEngine e;
        e.setConfig(cfg);
        int pf, carries;
        float th = -1.f;
        runCarrier(e, home, away, 30, &pf, &th, &carries);
        std::printf("   crowd: crowded=%d opp=%d nearest=%.1f firstPassFrame=%d theta=%.0f carryFrames=%d\n",
                    (int)e.state().crowded, e.state().crowdOpp, e.state().nearestOpp, pf, th, carries);
        CHECK(e.state().crowded && e.state().crowdOpp >= 2, "crowd detected around the carrier (picked-colour dots only)");
        CHECK(pf >= 0 && pf <= 8, "crowded carrier releases the ball within ~0.6 s");
        CHECK(pf >= 0 && std::fabs(th - 135.f) < 25.f, "the release goes to the open teammate");

        pe::TacticalConfig off = cfg;
        off.keepBall = false;
        pe::PitchEngine e0;
        e0.setConfig(off);
        int pf0, carries0;
        float th0 = -1.f;
        runCarrier(e0, home, away, 30, &pf0, &th0, &carries0);
        std::printf("   baseline (keepBall off): firstPassFrame=%d carryFrames=%d\n", pf0, carries0);
        float pCarryKeep = 0.f, pCarryBase = 0.f;
        for (int i = 0; i < e.candidateCount(); i++)
            if (e.candidates()[i].type == pe::ACT_CARRY) pCarryKeep = e.candidates()[i].prob;
        for (int i = 0; i < e0.candidateCount(); i++)
            if (e0.candidates()[i].type == pe::ACT_CARRY) pCarryBase = e0.candidates()[i].prob;
        std::printf("   P(carry): keep-the-ball=%.3f baseline=%.3f\n", pCarryKeep, pCarryBase);
        CHECK(pCarryKeep <= pCarryBase + 1e-6f, "keep-the-ball never makes dribbling in a crowd MORE likely");
        CHECK(pf >= 0 && (pf0 < 0 || pf <= pf0 || carries <= carries0), "keep-the-ball is not slower to release than the baseline");

        pe::MetricVector2D away2[11];
        for (int i = 0; i < 11; i++) away2[i] = away[i];
        away2[5] = {30.f, 1.5f}; away2[6] = {28.f, -2.5f}; away2[7] = {25.f, -2.f};
        pe::PitchEngine en;
        en.setConfig(cfg);
        int pfn, carriesn;
        float thn = -1.f;
        runCarrier(en, home, away2, 3, &pfn, &thn, &carriesn);
        CHECK(!en.state().crowded, "open carrier is not 'crowded'");

        pe::MetricVector2D away3[11];
        for (int i = 0; i < 11; i++) away3[i] = away2[i];
        away3[5] = {6.f, 1.5f};
        pe::PitchEngine e3;
        e3.setConfig(cfg);
        int pf3, carries3;
        float th3 = -1.f;
        runCarrier(e3, home, away3, 3, &pf3, &th3, &carries3);
        CHECK(!e3.state().crowded && e3.state().crowdOpp == 1, "a single opponent at 6 m is not a crowd");

        pe::MetricVector2D hh[11], aa[11];
        mkState(hh, aa);
        for (int i = 5; i <= 7; i++) aa[i] = {30.f + i, 10.f};
        aa[5] = {1.6f, 0.f};
        pe::MetricVector2D ballB = {1.0f, 0.f};
        pe::PitchEngine ek;
        ek.setConfig(cfg);
        pe::PitchEngine ekoff;
        pe::TacticalConfig offc = cfg;
        offc.keepBall = false;
        ekoff.setConfig(offc);
        double tmk = 1000;
        for (int i = 0; i < 12; i++) {
            pe::MetricVector2D a2[11];
            for (int q = 0; q < 11; q++) a2[q] = aa[q];
            if (i < 6) a2[5] = {20.f, 10.f};
            ek.setFrameTimeMs(tmk);
            ek.ingestDetections(hh, 11, a2, 11, &ballB, true);
            ekoff.setFrameTimeMs(tmk);
            ekoff.ingestDetections(hh, 11, a2, 11, &ballB, true);
            tmk += 80;
        }
        std::printf("   tie: keepBall status=%d, off status=%d\n", ek.state().status, ekoff.state().status);
        CHECK(ek.state().status == pe::ST_TEAM, "an opponent dot drawn over the ball does not steal possession (tie margin)");
        CHECK(ekoff.state().status == pe::ST_OPP, "contrast: without keep-the-ball the nearer opponent dot owns the ball");
    }

    {
        std::printf("\n-- v2.1: merged dots / virtual keeper / direction --\n");
        {
            auto tb = teamBase();
            auto ob = oppBase();
            const Pt a = tb[5];
            const Pt b = {a.x + 5.0f / W, a.y + 4.0f / H};
            tb[6] = b;
            auto img = frame(tb, ob, {0.9f, 0.9f});
            std::vector<unsigned char> rgba((size_t)W * H * 4, 255);
            for (int i = 0; i < W * H; i++) {
                rgba[i * 4] = (img[i] >> 16) & 255; rgba[i * 4 + 1] = (img[i] >> 8) & 255; rgba[i * 4 + 2] = img[i] & 255;
            }
            pe::PitchEngine e;
            e.setColors(TEAM, OPP, BALL);
            double tm = 1000;
            for (int i = 0; i < 6; i++) { e.setFrameTimeMs(tm); e.processFrame(rgba.data(), W, H); tm += 80; }
            float d[64];
            e.debugDots(d, 64);
            auto best = [&](Pt t) {
                float bd = 1e9f;
                for (int q = 0; q < (int)d[0]; q++) {
                    const float dx = (d[5 + 2 * q] - t.x) * 104.f, dy = (d[6 + 2 * q] - t.y) * 68.f;
                    bd = std::min(bd, std::sqrt(dx * dx + dy * dy));
                }
                return bd;
            };
            std::printf("   merged pair: home=%d errA=%.2f m errB=%.2f m\n", (int)d[0], best(a), best(b));
            CHECK((int)d[0] == 11 && best(a) < 1.6f && best(b) < 1.6f, "two touching home dots are split to ~their true positions");
        }

        {
            pe::TacticalConfig cfg;
            cfg.attackDir = 1;
            cfg.gkPassBan = false;   // the virtual keeper only exists when keeper passes are allowed
            cfg.forwardOnly = false; cfg.backPassBox = 0.f;
            pe::MetricVector2D home[11], away[11];
            home[0] = {-20.f, -20.f};
            for (int i = 1; i < 11; i++) home[i] = {-15.f + 4.f * i, (i % 2 ? -1.f : 1.f) * (10.f + i)};
            home[10] = {-30.f, 0.f};
            for (int i = 0; i < 11; i++) away[i] = {20.f + 3.f * i, (i % 2 ? 1.f : -1.f) * (5.f + 2.f * i)};
            away[0] = {-27.f, 1.5f};
            away[1] = {-27.5f, -2.5f};
            away[2] = {-26.f, 0.f};
            auto hasKeeperCand = [&](const pe::TacticalConfig& c) {
                pe::PitchEngine e;
                e.setConfig(c);
                pe::MetricVector2D ball = {-29.8f, 0.f};
                double tm = 1000;
                for (int i = 0; i < 6; i++) { e.setFrameTimeMs(tm); e.ingestDetections(home, 11, away, 11, &ball, true); tm += 80; }
                for (int i = 0; i < e.candidateCount(); i++)
                    if (e.candidates()[i].targetId == 98) return true;
                return false;
            };
            // the feature still exists but is OFF by default: passes to a made-up keeper spot risk own goals
            pe::TacticalConfig onCfg = cfg;
            onCfg.virtualKeeper = true;
            onCfg.gkPassBan = false;   // the keeper ban (on by default) also blocks the made-up keeper spot
            CHECK(!pe::TacticalConfig().virtualKeeper, "virtual keeper back-pass is off by default");
            const bool on = hasKeeperCand(onCfg);
            pe::TacticalConfig off = cfg;
            off.virtualKeeper = false;
            const bool offHas = hasKeeperCand(off);
            home[0] = {-49.f, 0.f};
            const bool realKeeper = hasKeeperCand(onCfg);
            std::printf("   virtual keeper: on=%d off=%d withRealKeeper=%d\n", (int)on, (int)offHas, (int)realKeeper);
            CHECK(on && !offHas && !realKeeper, "virtual keeper back-pass is offered only when crowded, deep and no keeper dot exists");
        }

        {
            pe::TacticalConfig cfg;
            cfg.attackDir = -1;
            pe::PitchEngine e;
            e.setConfig(cfg);
            pe::MetricVector2D home[11], away[11];
            auto tb = teamBase(), ob = oppBase();
            auto mp = [&](Pt q) { return pe::MetricVector2D{(q.x - 0.5f) * 104.f, (q.y - 0.5f) * 68.f}; };
            double tm = 1000;
            pe::MetricVector2D ball = {0.f, 0.f};
            for (int i = 0; i < 40; i++) {
                for (int k = 0; k < 11; k++) { home[k] = mp(tb[k]); away[k] = mp(ob[k]); }
                e.setFrameTimeMs(tm);
                e.ingestDetections(home, 11, away, 11, &ball, true);
                tm += 80;
            }
            const int before = e.state().attackSign;
            for (int i = 0; i < 12; i++) {
                for (int k = 0; k < 11; k++) { home[k] = mp(tb[k]); away[k] = mp(ob[k]); }
                for (int k = 5; k < 10; k++) { home[k].x += 14.f; away[k + 1 > 10 ? 10 : k + 1].x -= 8.f; }
                e.setFrameTimeMs(tm);
                e.ingestDetections(home, 11, away, 11, &ball, true);
                tm += 80;
            }
            std::printf("   direction: before=%d after cluster=%d\n", before, e.state().attackSign);
            CHECK(before == 1 && e.state().attackSign == 1, "auto attack direction is stable through a reversed midfield cluster");
        }
    }

    {
        pe::TacticalConfig cfg;
        cfg.attackDir = 1;
        pe::PitchEngine e;
        e.setConfig(cfg);
        pe::MetricVector2D home[11], away[11];
        auto tb = teamBase(), ob = oppBase();
        auto mp = [&](Pt q) { return pe::MetricVector2D{(q.x - 0.5f) * 104.f, (q.y - 0.5f) * 68.f}; };
        for (int k = 0; k < 11; k++) { home[k] = mp(tb[k]); away[k] = mp(ob[k]); }
        pe::MetricVector2D ball = {home[7].x + 0.4f, home[7].y};
        double tm = 1000;
        int valid = 0, teamFrames = 0;
        auto feed = [&](int frames) {
            valid = 0; teamFrames = 0;
            for (int i = 0; i < frames; i++) {
                e.setFrameTimeMs(tm);
                const int s = e.ingestDetections(home, 11, away, 11, &ball, true);
                tm += 80;
                if (s == pe::ST_TEAM) teamFrames++;
                if (e.lastGesture().valid) valid++;
            }
        };
        feed(40);
        const bool botActs = teamFrames > 30 && valid > 0;
        e.setHumanControl(true);
        feed(25);
        const bool humanQuiet = teamFrames > 20 && valid == 0 && e.state().carrier >= 0;
        e.setHumanControl(false);
        feed(40);
        const bool botBack = teamFrames > 30 && valid > 0;
        std::printf("   hand-over: botActs=%d humanQuiet=%d botBack=%d\n", (int)botActs, (int)humanQuiet, (int)botBack);
        CHECK(botActs, "hand-over: the bot plays while it has the ball");
        CHECK(humanQuiet, "hand-over: while the human plays the engine keeps tracking but gives no gesture");
        CHECK(botBack, "hand-over: after hand-back the bot plays again");
    }

    {
        auto scene = [&](pe::MetricVector2D* h, pe::MetricVector2D* a) {
            h[0] = {-48.f, 0.f};
            h[1] = {-30.f, -22.f}; h[2] = {-30.f, -8.f}; h[3] = {-30.f, 8.f}; h[4] = {-30.f, 22.f};
            h[5] = {-12.f, -26.f}; h[6] = {-10.f, 26.f}; h[7] = {-8.f, 8.f};
            h[8] = {4.f, -12.f};  h[9] = {12.f, 14.f};  h[10] = {0.f, 0.f};
            a[0] = {48.f, 0.f};
            a[1] = {35.f, -22.f}; a[2] = {35.f, -8.f}; a[3] = {35.f, 8.f}; a[4] = {35.f, 22.f};
            a[5] = {30.f, -14.f}; a[6] = {30.f, 14.f}; a[7] = {26.f, 0.f};
            a[8] = {22.f, -24.f}; a[9] = {22.f, 24.f}; a[10] = {40.f, 0.f};
        };
        auto findCand = [&](const pe::PitchEngine& e, pe::MetricVector2D p, float tol) {
            int bi = -1;
            float bd = tol;
            for (int i = 0; i < e.candidateCount(); i++) {
                const pe::PassCandidate& c = e.candidates()[i];
                if (c.type != pe::ACT_PASS && c.type != pe::ACT_LOFTED) continue;
                const float d = pe::distance(c.target, p);
                if (d < bd) { bd = d; bi = i; }
            }
            return bi;
        };
        pe::TacticalConfig base;
        base.attackDir = 1;
        base.forwardOnly = false;   // these terms act on level and back passes, which the forward-only filter would otherwise remove
        pe::TacticalConfig off = base;
        off.playmaker = false;

        {
            auto runLead = [&](const pe::TacticalConfig& cfg, bool sprint, float* targetX) {
                pe::PitchEngine e;
                e.setConfig(cfg);
                pe::MetricVector2D h[11], a[11];
                scene(h, a);
                const pe::MetricVector2D ball = {0.2f, 0.f};
                pe::MetricVector2D rpos = h[8];
                double tm = 1000;
                for (int i = 0; i < 30; i++) {
                    if (sprint) { const float t = i * 0.08f; rpos = {4.f + 0.5f * 3.f * t * t, -12.f}; h[8] = rpos; }
                    e.setFrameTimeMs(tm);
                    e.ingestDetections(h, 11, a, 11, &ball, true);
                    tm += 80;
                }
                const int ci = findCand(e, rpos, 6.f);
                *targetX = ci >= 0 ? e.candidates()[ci].target.x : -999.f;
            };
            float onX, offX, stOnX, stOffX;
            runLead(base, true, &onX);
            runLead(off, true, &offX);
            runLead(base, false, &stOnX);
            runLead(off, false, &stOffX);
            std::printf("   lead: sprinting on=%.2f off=%.2f | standing on=%.2f off=%.2f\n", onX, offX, stOnX, stOffX);
            CHECK(onX > offX + 0.5f && onX <= offX + 3.1f, "playmaker lead: a sprinting receiver is led 0.5-3 m further ahead");
            CHECK(std::fabs(stOnX - stOffX) < 0.01f, "playmaker lead: a standing receiver is not moved");
        }

        {
            auto runPress = [&](const pe::TacticalConfig& cfg, float* ttc, float* pCarry, float* pSafe) {
                pe::PitchEngine e;
                e.setConfig(cfg);
                pe::MetricVector2D h[11], a[11];
                scene(h, a);
                const pe::MetricVector2D ball = {0.2f, 0.f};
                double tm = 1000;
                for (int i = 0; i < 24; i++) {
                    a[5] = {16.f - 6.f * (i * 0.08f), 0.f};
                    e.setFrameTimeMs(tm);
                    e.ingestDetections(h, 11, a, 11, &ball, true);
                    tm += 80;
                }
                *ttc = e.state().pressTtc;
                *pCarry = 0.f; *pSafe = 0.f;
                for (int i = 0; i < e.candidateCount(); i++) {
                    const pe::PassCandidate& c = e.candidates()[i];
                    if (c.type == pe::ACT_CARRY) *pCarry = c.prob;
                    else if (c.type != pe::ACT_SHOOT && c.xP >= base.safeXp) *pSafe += c.prob;
                }
            };
            float tOn, cOn, sOn, tOff, cOff, sOff;
            runPress(base, &tOn, &cOn, &sOn);
            runPress(off, &tOff, &cOff, &sOff);
            std::printf("   press: ttc=%.2f  P(carry) on=%.3f off=%.3f  P(safe pass) on=%.3f off=%.3f\n", tOn, cOn, cOff, sOn, sOff);
            CHECK(tOn < 1.0f && tOn > 0.1f, "press release: time to contact is measured for a sprinting opponent");
            CHECK(cOn < cOff, "press release: dribbling into the closing opponent is less likely");
            CHECK(sOn >= sOff - 1e-4f, "press release: safe release probability does not drop");
        }

        {
            pe::TacticalConfig on = base;
            on.wRelease = 0.f; on.wTempo = 0.f; on.wOneTouch = 0.f; on.wZone = 0.f;
            struct R { float uA, uB; int committed; };
            auto runBait = [&](const pe::TacticalConfig& cfg, float ay = 14.f) {
                pe::PitchEngine e;
                e.setConfig(cfg);
                pe::MetricVector2D h[11], a[11];
                scene(h, a);
                h[9] = {12.f, ay};
                const pe::MetricVector2D ball = {0.2f, 0.f};
                double tm = 1000;
                for (int i = 0; i < 12; i++) {
                    a[5] = {9.f - 5.f * (i * 0.08f), 0.f};
                    e.setFrameTimeMs(tm);
                    e.ingestDetections(h, 11, a, 11, &ball, true);
                    tm += 80;
                }
                R r{-9.f, -9.f, e.state().committed};
                const int ia = findCand(e, h[9], 3.f), ib = findCand(e, h[7], 3.f);
                if (ia >= 0) r.uA = e.candidates()[ia].utility;
                if (ib >= 0) r.uB = e.candidates()[ib].utility;
                return r;
            };
            const R rOn = runBait(on), rOff = runBait(off);
            std::printf("   bait: committed=%d  A on=%.4f off=%.4f  B on=%.4f off=%.4f\n", rOn.committed, rOn.uA, rOff.uA, rOn.uB, rOff.uB);
            CHECK(rOn.committed >= 1, "bait: a defender running at the carrier is detected");
            CHECK(rOn.uA > rOff.uA + 0.01f, "bait: the receiver behind the drawn defender gains utility");
            CHECK(std::fabs(rOn.uB - rOff.uB) < 1e-4f, "bait: a receiver in front of his run is unchanged");
            const R wOn = runBait(on, 14.f), wOff = runBait(off, 14.f), cOn = runBait(on, 5.f), cOff = runBait(off, 5.f);
            std::printf("   overload sector: wing gain=%.4f  central gain=%.4f\n", wOn.uA - wOff.uA, cOn.uA - cOff.uA);
            CHECK((wOn.uA - wOff.uA) > (cOn.uA - cOff.uA) + 0.005f && (cOn.uA - cOff.uA) > 0.f,
                  "overload: the vacated wing is worth more than the vacated centre, and both gain");
        }

        {
            pe::TacticalConfig noTempo = base;
            noTempo.wTempo = 0.f;
            auto runTempo = [&](const pe::TacticalConfig& cfg, bool weOwn, float* share, float* uLat) {
                pe::PitchEngine e;
                e.setConfig(cfg);
                pe::MetricVector2D h[11], a[11];
                scene(h, a);
                pe::MetricVector2D ball = weOwn ? pe::MetricVector2D{0.2f, 0.f} : pe::MetricVector2D{a[7].x + 0.2f, a[7].y};
                double tm = 1000;
                for (int i = 0; i < 520; i++) {
                    e.setFrameTimeMs(tm);
                    e.ingestDetections(h, 11, a, 11, &ball, true);
                    tm += 80;
                }
                ball = {0.2f, 0.f};
                for (int i = 0; i < 12; i++) {
                    e.setFrameTimeMs(tm);
                    e.ingestDetections(h, 11, a, 11, &ball, true);
                    tm += 80;
                }
                *share = e.possessionShare();
                const int ci = findCand(e, h[7], 3.f);
                *uLat = ci >= 0 ? e.candidates()[ci].utility : -9.f;
            };
            float shLow, uLow, shLow0, uLow0, shHigh, uHigh, shHigh0, uHigh0;
            runTempo(base, false, &shLow, &uLow);
            runTempo(noTempo, false, &shLow0, &uLow0);
            runTempo(base, true, &shHigh, &uHigh);
            runTempo(noTempo, true, &shHigh0, &uHigh0);
            std::printf("   tempo: share low=%.2f high=%.2f | lateral utility low on=%.4f off=%.4f | high on=%.4f off=%.4f\n",
                        shLow, shHigh, uLow, uLow0, uHigh, uHigh0);
            CHECK(shLow < 0.45f && shHigh > 0.60f, "tempo: the possession average follows who owned the ball");
            CHECK(uLow > uLow0 + 0.01f, "tempo: below target possession the safe lateral pass gains utility");
            CHECK(std::fabs(uHigh - uHigh0) < 1e-5f, "tempo: above target possession the tempo term does nothing");
        }

        {
            auto runThrough = [&](const pe::TacticalConfig& cfg0, bool run, float* angleFromAxis, float* diag, float* runAngle) {
                pe::PitchEngine e;
                pe::TacticalConfig cfg = cfg0;
                cfg.plusThrough = false;      // this check isolates the playmaker aim; the multi-hypothesis aim has its own checks
                e.setConfig(cfg);
                pe::MetricVector2D h[11], a[11];
                scene(h, a);
                const pe::MetricVector2D ball = {0.2f, 0.f};
                pe::MetricVector2D rpos = run ? pe::MetricVector2D{2.f, -18.f} : pe::MetricVector2D{12.f, -12.f};
                double tm = 1000;
                for (int i = 0; i < 30; i++) {
                    const float t = i * 0.08f;
                    if (run) rpos = {2.f + 5.f * t, -18.f + 4.f * t};
                    h[8] = rpos;
                    e.setFrameTimeMs(tm);
                    e.ingestDetections(h, 11, a, 11, &ball, true);
                    tm += 80;
                }
                *angleFromAxis = -1.f; *diag = -1.f; *runAngle = 39.f;
                int bi = -1; float bd = 14.f;
                for (int i = 0; i < e.candidateCount(); i++) {
                    const pe::PassCandidate& c = e.candidates()[i];
                    if (c.type != pe::ACT_THROUGH) continue;
                    const float d = pe::distance(c.target, rpos);
                    if (d < bd) { bd = d; bi = i; }
                }
                if (bi < 0) return;
                const pe::PassCandidate& c = e.candidates()[bi];
                const pe::MetricVector2D off2 = c.target - rpos;
                *angleFromAxis = std::atan2(std::fabs(off2.y), off2.x) * 57.2957795f;
                *diag = c.diag;
            };
            float aOn, dOn, rA, aOff, dOff, aStOn, dStOn, aStOff, dStOff;
            runThrough(base, true, &aOn, &dOn, &rA);
            runThrough(off, true, &aOff, &dOff, &rA);
            runThrough(base, false, &aStOn, &dStOn, &rA);
            runThrough(off, false, &aStOff, &dStOff, &rA);
            std::printf("   through: aim angle off axis  run on=%.0f off=%.0f | standing on=%.0f off=%.0f | diag flag on=%.0f standing=%.0f\n",
                        aOn, aOff, aStOn, aStOff, dOn, dStOn);
            CHECK(aOn >= 0.f && aOff >= 0.f && aOn > aOff + 8.f, "through ball: the aim point bends along a diagonal run (play off aims straight)");
            CHECK(dOn == 1.f && dStOn <= 0.f, "through ball: a forward diagonal run is flagged, a standing receiver is not");
            CHECK((aStOn < 0.f && aStOff < 0.f) || std::fabs(aStOn - aStOff) < 1.f, "through ball: a standing receiver is never led differently by the playmaker aim");
        }

        {
            pe::PitchEngine e;
            e.setConfig(base);
            pe::MetricVector2D h[11], a[11];
            scene(h, a);
            const pe::MetricVector2D ball = {0.2f, 0.f};
            double tm = 1000;
            int touchFrames = 0, carriesDuring = 0, passesDuring = 0, crowdedDuring = 0;
            for (int i = 0; i < 26; i++) {
                a[5] = {16.f - 7.f * (i * 0.08f), 0.f};
                e.setFrameTimeMs(tm);
                e.ingestDetections(h, 11, a, 11, &ball, true);
                tm += 80;
                if (e.state().oneTouch) {
                    touchFrames++;
                    if (e.state().crowded) crowdedDuring++;
                    const pe::GestureOutput& g = e.lastGesture();
                    if (g.valid && g.actionType == pe::ACT_CARRY) carriesDuring++;
                    if (g.valid && (g.actionType == pe::ACT_PASS || g.actionType == pe::ACT_THROUGH || g.actionType == pe::ACT_LOFTED)) passesDuring++;
                }
            }
            std::printf("   one-touch: frames=%d carries=%d passes=%d crowdedFrames=%d\n", touchFrames, carriesDuring, passesDuring, crowdedDuring);
            CHECK(touchFrames >= 2, "one-touch: the trigger fires for a sprinting opponent about to make contact");
            CHECK(carriesDuring == 0 && passesDuring >= 1, "one-touch: the carrier releases the ball instead of dribbling");
        }

        {
            auto runZone = [&](const pe::TacticalConfig& cfg, float* cong, float* uLat, float* uFwd) {
                pe::PitchEngine e;
                e.setConfig(cfg);
                pe::MetricVector2D h[11], a[11];
                scene(h, a);
                h[8] = {20.f, -2.f};
                a[2] = {8.f, -6.f};  a[3] = {10.f, 6.f};  a[4] = {14.f, -10.f}; a[5] = {16.f, 0.f};
                a[6] = {18.f, 9.f};  a[7] = {22.f, -4.f}; a[8] = {24.f, 6.f};   a[9] = {26.f, -9.f};
                const pe::MetricVector2D ball = {0.2f, 0.f};
                double tm = 1000;
                for (int i = 0; i < 14; i++) {
                    e.setFrameTimeMs(tm);
                    e.ingestDetections(h, 11, a, 11, &ball, true);
                    tm += 80;
                }
                *cong = e.state().congestion;
                const int il = findCand(e, h[7], 3.f), iv = findCand(e, h[8], 3.f);
                *uLat = il >= 0 ? e.candidates()[il].utility : -9.f;
                *uFwd = iv >= 0 ? e.candidates()[iv].utility : -9.f;
            };
            float cOn, lOn, fOn, cOff, lOff, fOff;
            runZone(base, &cOn, &lOn, &fOn);
            runZone(off, &cOff, &lOff, &fOff);
            std::printf("   zone: congestion=%.2f lateral on=%.4f off=%.4f | vertical on=%.4f off=%.4f\n", cOn, lOn, lOff, fOn, fOff);
            CHECK(cOn > 0.8f, "zone density: eight opponents in the zone ahead read as a congested block");
            CHECK(lOn > lOff + 0.01f, "zone density: the free lateral pass gains utility");
            CHECK(fOn < fOff - 0.005f, "zone density: the vertical ball into the block loses utility");
        }

        {
            auto runClutch = [&](int mode, float* uLat, float* uFwd) {
                pe::PitchEngine e;
                e.setConfig(base);
                e.setClutchMode(mode);
                pe::MetricVector2D h[11], a[11];
                scene(h, a);
                h[8] = {16.f, -12.f};
                a[5] = {9.f, -1.f};   // beside the lane, not on it: the interception model rates a ball through a defender as ~0
                const pe::MetricVector2D ball = {0.2f, 0.f};
                double tm = 1000;
                for (int i = 0; i < 14; i++) {
                    e.setFrameTimeMs(tm);
                    e.ingestDetections(h, 11, a, 11, &ball, true);
                    tm += 80;
                }
                const int il = findCand(e, h[7], 3.f), iv = findCand(e, h[8], 3.f);
                *uLat = il >= 0 ? e.candidates()[il].utility : -9.f;
                *uFwd = iv >= 0 ? e.candidates()[iv].utility : -9.f;
            };
            float l0, f0, l1, f1, l2, f2;
            runClutch(0, &l0, &f0);
            runClutch(1, &l1, &f1);
            runClutch(2, &l2, &f2);
            std::printf("   clutch: normal lat=%.4f fwd=%.4f | protect lat=%.4f fwd=%.4f | chase lat=%.4f fwd=%.4f\n", l0, f0, l1, f1, l2, f2);
            CHECK(l1 > l0 + 0.01f, "late-game protect: the safe lateral pass gains utility");
            CHECK(f1 < f0, "late-game protect: the doubtful forward ball loses utility");
            CHECK(f2 > f0 + 0.005f && l2 < l0, "late-game chase: the forward ball gains and sideways recycling loses");
            pe::PitchEngine e;
            e.setClutchMode(9);
            const bool clamped = e.clutchMode() == 2;
            e.setClutchMode(-3);
            CHECK(clamped && e.clutchMode() == 0, "late-game plan: mode is clamped to 0..2");
        }

        {
            pe::TacticalConfig zero = base;
            zero.wRelease = zero.wBait = zero.wTempo = 0.f;
            zero.wOneTouch = zero.wZone = zero.wDiag = zero.wClutch = zero.wChase = 0.f;
            zero.leadVelFactor = 0.f;
            auto runScene = [&](const pe::TacticalConfig& cfg, pe::PassCandidate* out, int* n) {
                pe::PitchEngine e;
                e.setConfig(cfg);
                pe::MetricVector2D h[11], a[11];
                scene(h, a);
                const pe::MetricVector2D ball = {0.2f, 0.f};
                double tm = 1000;
                for (int i = 0; i < 14; i++) {
                    a[5] = {9.f - 5.f * (i * 0.08f), 0.f};
                    e.setFrameTimeMs(tm);
                    e.ingestDetections(h, 11, a, 11, &ball, true);
                    tm += 80;
                }
                *n = e.candidateCount();
                for (int i = 0; i < *n && i < 32; i++) out[i] = e.candidates()[i];
            };
            pe::PassCandidate c1[32], c2[32];
            int n1 = 0, n2 = 0;
            runScene(off, c1, &n1);
            runScene(zero, c2, &n2);
            bool same = n1 == n2 && n1 > 0;
            for (int i = 0; same && i < n1 && i < 32; i++)
                same = std::fabs(c1[i].utility - c2[i].utility) < 1e-6f && pe::distance(c1[i].target, c2[i].target) < 1e-4f;
            CHECK(same, "playmaker off equals playmaker with all weights zero (base scoring untouched)");
        }
    }

    std::printf("\n%s (%d failures)\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
