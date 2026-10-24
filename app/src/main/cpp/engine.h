#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pb {

constexpr int NF = 8;
constexpr int GX = 6;
constexpr int GY = 4;

enum ActionType {
    AT_NONE = 0, AT_CARRY, AT_PASS, AT_THROUGH, AT_SHOOT,
    AT_CLEAR, AT_SWITCH, AT_TACKLE, AT_PRESS, AT_CHASE
};

enum Button { B_NONE = 0, B_TOP = 1, B_THROUGH = 2, B_PASS = 3, B_DASH = 4 };

struct Action {
    int type = AT_NONE;
    float sx = 0, sy = 0;
    int button = B_NONE;
    int holdMs = 0;
    bool dash = false;
};

struct PaletteEntry { int r = 0, g = 0, b = 0, blobs = 0; };

struct Options {
    int tactic = 1;
    float aggression = 0.5f;
    float shootReach = 0.28f;
    float passSpeed = 0.6f;
    int attackDir = -1;
    int half = 0;
    bool invertX = false;
    bool invertY = false;
    float ownRadius = 0.035f;
    bool legendary = false;
    bool colorOnly = true;
};

struct P { float x = 0, y = 0; };

struct Blob { float x = 0, y = 0, area = 0; int r = 0, g = 0, b = 0; };

struct Dec {
    double t = 0;
    int type = 0;
    float p = 0;
    float f[NF] = {0};
    float ax = 0, ay = 0;
    bool lostSeen = false;
    int outcome = -1;
};

class Engine {
public:
    Engine();

    int scanPalette(const int* px, int w, int h, PaletteEntry* out, int maxOut);
    void setColors(int teamRGB, int oppRGB, int ballRGB);
    void setOptions(const Options& o) { opts_ = o; }
    void setObserveOnly(bool v) { observe_ = v; }
    const Options& options() const { return opts_; }

    int process(const int* px, int w, int h, double tMs, Action& out);

    void newMatch();
    std::string exportLog() const;
    void stats(float* out8) const;

    void resetWeights();
    bool importWeights(const std::string& s);
    std::string exportWeights() const;
    std::string learn(const std::string& logs);
    void lossGrid(float* out_rate, float* out_visits) const;
    void profile(float* out6) const;

    int debugDots(float* out, int maxFloats) const;

    int half() const { return half_; }
    int dirSign() const { return dir_; }

private:
    void extractBlobs(const int* px, int w, int h, std::vector<Blob>& out) const;
    void classify(int w, int h);

    void updateDirection();
    void updatePossession();
    void updateProfile();
    void resolveDecisions();

    Action decideAttack();
    Action decideDefend();
    Action decideLoose();

    float gridLoss(float ax, float ay01) const;
    void passFeatures(const P& c, const P& geo, const P& recv, int ri, int ci, float* f) const;
    float scoreWith(const float* w, const float* f, int tactic) const;
    float nearestOpp(const P& p) const;
    float shotQuality(const P& c, float& dist) const;
    void toStick(float vx, float vy, Action& a) const;
    void logDecision(int type, float p, const float* f, const P& at);
    void noteBlame();

    Options opts_;
    bool observe_ = false;
    bool haveColors_ = false;
    int teamC_ = 0, oppC_ = 0, ballC_ = 0;

    std::vector<Blob> blobs_;
    std::vector<P> rawTeam_, rawOpp_;
    P ballRaw_;
    bool ballSeen_ = false;

    std::vector<P> tA_, oA_;
    P ballA_, lastBallA_;
    double lastBallT_ = 0, lastBallSeenT_ = -1e9;
    bool ballKnown_ = false;
    float ballSpeed_ = 0;

    int dir_ = 1;
    bool dirInit_ = false;
    float emaL_ = 0.5f, emaR_ = 0.5f;
    double flipSince_ = -1, liveMs_ = 0, lastChangeLive_ = 0;
    int half_ = 1;

    int owner_ = 0, candOwner_ = 0, candRun_ = 0;
    double ownerSince_ = 0;
    P lastCarrier_;
    int carrierIdx_ = -1;
    double now_ = 0, prevNow_ = 0;
    double nextActionAt_ = 0;
    double carryStart_ = 0;
    double lastSwitchMs_ = -1e9, lastTackleMs_ = -1e9;
    double lastTeamT_ = -1e9;
    P lastSwitchBall_;
    int status_ = 0;
    double notLiveSince_ = -1;

    float wp_[NF], wt_[NF];
    float shootThr_ = 0.45f;
    float gridVisit_[GX * GY];
    float gridLoss_[GX * GY];
    int matchesLearned_ = 0;

    std::vector<Dec> pending_, done_;
    std::vector<float> lossLog_;
    int passAtt_ = 0, passOk_ = 0, shots_ = 0, losses_ = 0;
    long framesLive_ = 0, framesTeam_ = 0, framesOpp_ = 0;

    double pLineSum_ = 0, pWidthSum_ = 0, pPressSum_ = 0; long pAttN_ = 0;
    double pFlankSum_ = 0; long pFlankN_ = 0;
    long pFastN_ = 0, pOppPossN_ = 0;
};

}
