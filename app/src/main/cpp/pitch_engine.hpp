#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace pe {

constexpr float PITCH_HALF_X = 52.0f;
constexpr float PITCH_HALF_Y = 34.0f;
constexpr int XT_COLS = 12;
constexpr int XT_ROWS = 8;
constexpr int TEAM_SIZE = 11;
constexpr int MAX_DET = 24;
constexpr int MAX_CAND = 40;
constexpr int MAX_PIXELS = 1 << 18;

enum Team : int { TEAM_HOME = 0, TEAM_AWAY = 1, TEAM_BALL = 2 };

enum ActionType : int {
    ACT_NONE = 0, ACT_CARRY = 1, ACT_PASS = 2, ACT_THROUGH = 3, ACT_LOFTED = 4, ACT_SHOOT = 5, ACT_SHOT = 6,
    ACT_TACKLE = 7, ACT_SWITCH = 8, ACT_MATCHUP = 9, ACT_PRESS = 10,
    ACT_STEPUP = 11
};

enum Status : int { ST_NOT_LIVE = 0, ST_LOOSE = 1, ST_TEAM = 2, ST_OPP = 3 };

// Player role inferred live from position (see PitchEngine::updateRoles)
enum Role : int { ROLE_UNK = 0, ROLE_GK, ROLE_CB, ROLE_FB, ROLE_DM, ROLE_CM, ROLE_AM, ROLE_WG, ROLE_ST };

struct MetricVector2D {
    float x = 0.f, y = 0.f;
    MetricVector2D() = default;
    MetricVector2D(float ax, float ay) : x(ax), y(ay) {}
    MetricVector2D operator+(const MetricVector2D& o) const { return {x + o.x, y + o.y}; }
    MetricVector2D operator-(const MetricVector2D& o) const { return {x - o.x, y - o.y}; }
    MetricVector2D operator*(float k) const { return {x * k, y * k}; }
    float dot(const MetricVector2D& o) const { return x * o.x + y * o.y; }
    float length() const;
};

float distance(const MetricVector2D& a, const MetricVector2D& b);
float segmentDistance(const MetricVector2D& a, const MetricVector2D& b, const MetricVector2D& p);

struct PlayerState {
    int id = 0;
    int team = TEAM_HOME;
    MetricVector2D pos;
    MetricVector2D vel;
    MetricVector2D future;
    MetricVector2D acc;
    MetricVector2D vel5;          // 5-frame moving average of vel (alignment checks)
    float vh[5][2] = {{0,0},{0,0},{0,0},{0,0},{0,0}};
    int vhN = 0;
    double lastSeenMs = 0;
    int missed = 0;
    bool active = false;
    bool visible = false;
    bool kInit = false;
    float kP[2][3] = {{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
    float kRVar = 0.f;
};

struct xTGrid {
    float xt[XT_ROWS][XT_COLS];
    float xg[XT_ROWS][XT_COLS];
    xTGrid();
    void build();
    void setValues(const float* rowMajor96);
    static int colOf(float attackX);
    static int rowOf(float y);
    float valueAt(float attackX, float y) const;
    float xgAt(float attackX, float y) const;
    static float shotValue(float attackX, float y);
};

#ifndef PB_RESCUE_DEFAULT
#define PB_RESCUE_DEFAULT true      // build with -DPB_RESCUE_DEFAULT=false for the rule set without the rescue rules
#endif

#ifndef PB_DEFAULT_PASS_LEN
#define PB_DEFAULT_PASS_LEN 28.0f   // BEAST: allow longer progressive / through balls
#endif

struct TacticalConfig {
    float wPress = 0.05f;
    float wOffside = 0.30f;
    float utilityScale = 12.0f;
    float minDeltaXT = -0.01f;
    float tempCounter = 0.2f;
    float tempHold = 0.8f;
    float tempMin = 0.02f;
    int counterWindowMs = 4000;
    int counterHoldMs = 2500;
    float horizonSec = 0.2f;
    float pressRadius = 8.0f;
    float ownRadius = 3.5f;
    float offsideMargin = 0.3f;
    float minPassLen = 2.5f;
    float maxPassLen = PB_DEFAULT_PASS_LEN;     // tiki-taka: short build-up passes only
    float carryDist = 8.0f;
    float shootRange = 35.0f;     // max distance considered for any shot candidate (extended for finishing pack)
    float clearHalf = 2.6f;
    float clearSlope = 1.35f;     // sharper lane discrimination → near-zero interception on selected passes
    float loftTriggerClear = 3.0f;
    float loftMinLen = 16.0f;
    float throughLead = 5.0f;     // shorter lead so throughs arrive
    float trackGate = 10.0f;
    float gateGrowth = 1.2f;
    float gateMax = 30.0f;
    float maxSpeed = 10.0f;
    float velAlpha = 0.4f;
    float posAlpha = 0.8f;
    int maxCoastFrames = 12;
    bool kalman = false;
    float kalQ = 5.0f;
    float kalR = 0.6f;
    float kalRMin = 0.25f;
    float kalRMax = 2.0f;
    float kalRAlpha = 0.05f;
    float kalV0 = 3.0f;
    float kalLatencyMs = 100.0f;
    float kalProjGain = 1.0f;
    int decisionIntervalMs = 95;  // faster decision loop for high-tempo touches
    int actionCooldownMs = 260;
    int minPassHoldMs = 35;
    int maxPassHoldMs = 220;
    int loftExtraMs = 90;
    int carryHoldMs = 120;
    bool dashWhenFree = true;
    int attackDir = -1;
    bool invertX = false;
    bool invertY = false;
    int minBlobPx = 3;
    int maxBlobPx = 600;
    bool erode = true;
    int ballMinPx = 4;
    float ballMaxFrac = 0.25f;
    float ballMinHoleRatio = 0.08f;
    float ballMinRingCoverage = 0.45f;
    float ballMaxInnerFill = 0.40f;
    float ballGlowHoleRatio = 0.10f;
    float ballGlowCoverage = 0.55f;
    float ballDiscCoverage = 0.90f;
    float ballDiscMaxInner = 0.85f;
    float glowMinTimes = 3.6f;
    float glowMaxAspect = 1.6f;
    float glowMinFill = 0.40f;
    float splitMinElong = 1.3f;
    float ballDiscMaxFill = 0.60f;
    int ballDiscMinPx = 24;
    float ballInnerFrac = 0.35f;
    bool ballAllowSolidFallback = true;
    uint32_t seed = 0x1234ABCDu;
    bool shotAlert = true;
    bool shotHold = true;
    bool autoShoot = true;  // FINAL: fire when window is real, no hesitation
    int shootHoldMs = PB_RESCUE_DEFAULT ? 200 : 520;   // upper limit of a normal shot's hold
    bool legendary = false;
    bool defence = true;          // play without the ball (press, tackle, switch, match-up)
    // time-aware interception model: a defender can reach a point on the pass lane before the ball does
    float defSpeed = 6.4f;        // m/s a defender closes down a lane (slightly more pessimistic → only high-margin passes)
    float defReact = 0.18f;       // s reaction time of the defender
    float defBody = 0.85f;        // m foot / body reach
    float marginHalf = 1.35f;     // metres of spare reach at which a pass is 50 % safe (raised for near-zero interception)
    float minPassXp = 0.82f;      // passes below this are only played when nothing safer exists (stricter)
    bool longPassPenalty = true;  // passes beyond longPassFrom lose utility in proportion to how unsafe they are
    float longPassFrom = 18.0f;
    float wLongPass = 0.10f;
    bool cutBack = true;          // in the final third, prefer the pass that gives a team-mate a clearly better shot than the carrier has
    float cutBackRange = 32.0f;   // carrier within this distance of the goal
    float cutBackBoxHalfY = 14.0f;
    float cutBackMinFree = 2.0f;  // receiver must have at least this much room
    float wCutBack = 1.70f;       // cut-back to free man in box wins
    float wTiki = 0.12f;          // bonus for short, safe, open one-twos (retain the ball)

    // ---- Goals pack: conversion-first finishing, chance creation, second ball, score urgency ----
    bool conversionFirst = true;  // prefer high-xG looks; force 1v1 / open-goal finishes
    float highXgMargin = 0.04f;   // when two shot options exist, require this xG gap to switch aim
    float openGoalOpp = 2.4f;     // 1v1 / open look fires sooner
    float convMinLane = 1.0f;     // allow forced finish with 1 m lane
    float boxMustProgressSec = 0.45f; // release or shoot — no camping in the box
    float finalThirdDepth = 22.0f;   // metres from the opponent goal line
    bool secondBall = true;       // after a shot, hunt the rebound for this window
    int secondBallMs = 1500;
    float secondBallRadius = 18.0f;
    bool dynamicThroughLead = true; // through lead scales with runner speed toward goal
    float throughLeadPerMs = 0.25f; // lead per ms of flight; kept short so passes do not arrive behind the runner
    bool boxOccupy = true;        // reward deliveries into the central shooting zone
    float wBoxOccupy = 1.15f;
    float boxOccupyHalfY = 10.0f;
    float boxOccupyDepth = 16.0f;
    bool flankCross = true;       // early delivery from the flanks into the box
    float flankCrossMinY = 16.0f;
    float flankCrossMaxLen = 30.0f;
    float wFlankCross = 0.42f;
    bool chipGk = true;           // loft / chip when the opponent keeper is advanced
    float chipGkDepth = 9.0f;     // GK more than this many metres from his goal line
    float chipMinDist = 12.0f;
    float chipMaxDist = 34.0f;
    float wChip = 0.35f;
    // ---- safe possession, box entry, counter break ----
    bool safeFirst = true;        // with a clean pass on, doubtful passes and dribbles into pressure lose out
    float safeFirstXp = 0.80f;    // a pass at least this likely counts as a clean option
    float safeFirstMargin = 2.8f; // ... with at least this much lane clearance (m)
    float wUnsafeDemote = 0.60f;
    float recvContestR = 2.6f;    // an opponent this close to the receiver contests the ball on arrival
    float wRecvContest = 0.22f;
    bool turnoverCost = true;     // a pass that is likely to be cut out costs more the nearer it is to our own goal
    float wTurnover = 0.40f;
    float turnoverFullAx = -8.0f; // full cost at or behind this attacking x, none from +30 m on
    bool boxEntry = true;         // reward the pass that takes the ball from outside the box to a free man inside it
    float wBoxEntry = 0.65f;
    float boxEntryDepth = 18.0f;
    float boxEntryHalfY = 17.0f;
    bool counterBreak = true;     // right after winning the ball, the clean forward ball into space beats slow build-up
    int counterBreakMs = 4200;
    float wCounterBreak = 0.32f;
    bool scoreUrgency = true;     // extra finishing urgency when behind or late
    float scoreBehindBoost = 0.22f;
    float lateGameFrac = 0.72f;
    float lateUrgencyBoost = 0.18f;
    bool firstTimeFinish = true;  // prefer first-time / short-hold finishes on cut-backs and loose balls in the box
    float firstTimeHoldMs = 85.0f;  // FINAL
    // ---- Scoring Machine — clinical conversion, pressure-smart carry, final-third overload, post-loss compact ----
    bool scoringMachine = true;   // master switch for the scoring pack
    bool clinicalShot = true;     // suppress low-xG contested shots; prefer cut-back / progress when xG is weak
    float clinicalMinXg = 0.12f;  // still kill junk range; box handled separately
    float clinicalBoxXg = 0.02f;  // box looks fire — possession without goals loses matches
    float wClinicalDemote = 0.85f;  // less demote so real chances are taken
    bool pressureCarry = true;    // when pressed, steer carry toward nearest support + away from nearest defender
    float pressCarryR = 3.8f;     // opponent inside this distance triggers pressure-aware carry
    float wSupportSeek = 0.28f;   // bonus for carries that close on a free team-mate
    bool finalOverload = true;    // when we outnumber them in the final third / box, force progressive options
    float overloadDepth = 24.0f;  // metres from opponent goal line
    float overloadHalfY = 20.0f;
    float wOverload = 0.72f;
    int overloadMinNet = 0;       // need at least this many more attackers than defenders in the zone
    bool postLossCompact = true;  // after a turnover, prioritise recovery runs and shape over speculative counters for a short window
    int postLossMs = 2800;
    float wPostLossSafe = 0.25f;  // boost safe short options and demote speculative long balls right after a loss
    bool highTempoTrail = true;   // when behind or late, raise urgency and lower pass-safety bar slightly
    float trailUrgency = 0.55f;  // hungrier for goals
    float trailXpRelax = 0.04f;   // allow slightly riskier progressive passes when trailing
    bool forceReleasePress = true;// under sustained close press, force the safest short option (triangle / recycle) over a contested carry
    float forcePressDist = 2.2f;
    int forcePressMs = 420;
    float smNetCap = 0.95f;       // more room for conversion bonuses to win
    // clearing in our own box, long power shots
    float boxDepth = 17.0f;       // own penalty box depth from the goal line (m)
    float boxHalfY = 21.0f;
    float clearPressDist = 9.0f;  // clear when an opponent is this close and no safe pass exists
    float clearSafeXp = 0.62f;
    int clearHoldMs = 380;
    float longShotRange = 32.0f;  // outside-the-box power shot allowed up to this distance to goal
    float longShotMin = 14.0f;
    float longShotXp = 0.78f;     // higher lane quality — 0 xG stun spam was the problem
    float longShotFree = 5.0f;    // need real space for the wind-up
    float longShotHalfY = 18.0f;  // lateral limit (m from the centre line) for long-range efforts
    // Stunning (power) shot from outside the box: 20-30 m, needs room for the long wind-up, shooter roughly facing the corner
    bool stunEnabled = true;      // long power shots enabled for finishing pack (backup data needed more attempts from range)
    float stunMin = 18.0f;
    float stunMax = 32.0f;
    float stunSafe = PB_RESCUE_DEFAULT ? 3.6f : 4.0f;        // more wind-up room
    float stunCos = 0.70f;        // tighter facing (~45 deg) so stun shots stay on frame
    float stunPostInset = PB_RESCUE_DEFAULT ? 0.9f : 0.7f;   // aim further inside the post
    float stunMaxMs = 850.0f;     // slightly shorter power gauge — less overhit
    float longShotMinXg = 0.04f;  // outside the box a stun shot needs at least this much xG
    float stunLaneMin = 1.8f;     // no defender near the shot line
    float goalHalfWidth = 3.66f;  // 7.32 m goal
    float safePassLen = PB_DEFAULT_PASS_LEN;    // validation: any pass longer than this is dropped from the matrix
    float backPassBox = 42.0f;    // never pass back toward our goal
    float noTackleDepth = 26.0f;  // no tackle while the ball carrier is closer than this to our own goal line: contain and jockey instead
    bool gkPassBan = true;        // the goalkeeper is never a pass target for a field player
    float gkBanDepth = 22.0f;     // deeper ban on goal-hugging targets
    bool faceBeforePass = true;   // rotate to face the receiver before the pass is triggered
    float faceTolDeg = 50.0f;     // heading error (deg) accepted as 'facing' the target
    float faceMinSpeed = 1.0f;    // below this speed the heading is unknown: one turn tick first
    int turnHoldMs = 90;         // length of one turning stick tick
    int turnMaxMs = 300;          // never wait longer than this to face the target
    float turnSkipOppDist = 4.0f; // an opponent this close forces a one-touch pass without turning
    float urgency = 1.00f;        // FINAL: max finishing urgency
    float urgencyAfterSec = 0.5f; // FINAL: urgency immediate
    int restartGraceMs = 2500;    // after a stoppage (goal, replay) play safe, quick short passes only
    int restartMinGapMs = 1500;   // radar gap this long counts as a stoppage
    float wChance = 3.20f;        // hard reward passes that create a better shot
    float aimInset = PB_RESCUE_DEFAULT ? 2.3f : 2.0f;       // shots snap to this offset from the goal centre (post 3.66 minus 0.4 m): the corner away from the keeper
    float shotFaceTolDeg = PB_RESCUE_DEFAULT ? 42.0f : 45.0f; // heading error accepted before a shot (more lenient → more shots fire)
    float longShotHoldBase = PB_RESCUE_DEFAULT ? 110.0f : 300.0f; // long-shot hold = base + perM * distance, capped by shootHoldMs
    float longShotHoldPerM = PB_RESCUE_DEFAULT ? 2.0f : 5.0f;
    float tempScale = 0.75f;      // tiki-taka: pick the best pass more consistently (lower softmax temperature)
    int fastCooldownMs = 130;     // fast-paced build-up: the cooldown after a pass never exceeds this
    float noPassZone = 18.0f;     // never pass to a player this close to our own goal line
    float boxShotRange = 34.0f;   // finish zone
    float boxShotXp = 0.10f;      // take the box chance when lane is real
    int boxShotHoldMs = PB_RESCUE_DEFAULT ? 140 : 320;   // short press inside the box
    float commitDist = 2.2f;      // tackle only inside this distance (otherwise contain / jockey) ...
    float looseTouchDist = 1.3f;  // ... or when the carrier's touch runs this far from him
    float tackleMaxDist = 3.4f;   // metres
    int tackleGapMs = 900;
    float cueRangeFactor = 1.12f;
    float cueLaneXp = 0.28f;      // lower lane quality needed for a cue shot (still only 2–3 attempts)
    float cuePrimeXp = 0.40f;
    float cuePrimeXg = 0.03f;
    float cueMinXg = 0.015f;
    int cueConfirmFrames = 1;     // FINAL: zero hesitation
    int cueHoldMs = 1200;
    int cueMinGapMs = 1200;
    int cueRearmMs = 400;

    bool colorOnly = true;
    float minDotFill = 0.30f;
    float maxDotAspect = 4.0f;
    float maxMeanScore = 0.75f;
    int maxPerTeam = TEAM_SIZE;
    bool rejectNonDots = true;
    float dotMinFill = 0.52f;
    float dotSurroundMin = 0.55f;
    bool ballGlowTint = true;
    float ballCarryHoldMs = 1800.f;
    float signProbationMs = 5000.f;
    int signProbationRun = 8;
    int looseBelow = 11;
    int looseMinPx = 4;
    float looseMinFill = 0.45f;
    float looseMaxAspect = 2.5f;

    bool keepBall = true;
    float keepBias = 0.60f;
    float crowdRadius = 7.0f;
    float crowdTight = 3.5f;
    int crowdMinOpp = 2;
    float safeXp = 0.65f;
    bool virtualKeeper = false;   // never pass back to a made-up keeper spot (risk of own goals)
    float wKeep = 0.12f;
    float wOpen = 0.10f;
    float openRadius = 9.0f;
    float carryCrowdPenalty = 0.14f;
    float riskyPassPenalty = 0.18f;
    float crowdTempScale = 0.50f;
    int crowdMaxHoldMs = 300;
    float ownerTieMargin = 0.80f;
    float occludedPassFactor = 0.90f;
    int rescueMinEdge = 2;

    bool playmaker = true;
    float passSpeed = 16.0f;
    float leadMinSpeed = 2.0f;
    float leadVelFactor = 0.6f;
    float leadMaxSec = 0.8f;
    float leadMaxM = 3.0f;
    float accAlpha = 0.25f;
    float accMin = 0.8f;
    float accMax = 4.5f;
    float releaseTtc = 1.2f;
    float contactRadius = 1.6f;
    float closeMin = 1.2f;
    float wRelease = 0.14f;
    float baitRadius = 15.0f;
    float baitCommitSpeed = 1.4f;
    float wBait = 0.18f;
    float wingY = 14.f;
    float wingBonus = 0.30f;
    float tempoTarget = 0.62f;
    float tempoWindowSec = 90.f;
    float tempoMinSec = 30.f;
    float tempoLateralFwd = 4.0f;
    float wTempo = 0.12f;
    float diagMinDeg = 20.f;
    float diagMaxDeg = 75.f;
    float wDiag = 0.10f;
    float oneTouchRadius = 4.2f;  // wider window for fast-paced one-touch
    float oneTouchTtc = 0.75f;    // accept slightly longer time-to-contact
    float oneTouchMaxLen = 30.f;
    float wOneTouch = 0.48f;      // stronger preference for one-touch tempo
    float zoneAhead = 32.f;
    float zoneHalfWidth = 15.f;
    int zoneFree = 3;
    int zoneFull = 7;
    float zoneR = 10.f;
    float wZone = 0.16f;
    float wClutch = 0.22f;
    float clutchSafeXp = 0.80f;
    float wChase = 0.14f;

    bool ballCoast = true;
    float ballCoastDecay = 2.5f;
    bool occlusionHold = true;
    float occRadius = 2.4f;
    int occHoldFrames = 10;
    float occVelKeep = 0.985f;

    bool context = true;
    float wFacing = 0.05f;
    float wSupport = 0.06f;
    float wCompact = 0.06f;
    float supportRadius = 9.0f;
    float supportDepth = 5.0f;
    float compactRef = 18.0f;

    bool antiAdapt = true;
    float adaptDrop = 0.12f;
    int adaptMinEvents = 8;
    float noiseTemp = 0.35f;
    float noiseJitter = 0.40f;
    float noiseRiseSec = 6.0f;
    float noiseFallSec = 20.0f;

    bool styles = true;
    float styleGain = 0.6f;
    float styleTauSec = 25.0f;
    float styleWarmSec = 30.0f;

    // ---- Rescue rule set: emergency clearance, forward-only build-up, one-touch tempo, zero-hesitation shooting,
    //      penalty-box lockout, line step-up, interception cones, wing switch, containment ----
    bool rescueRules = PB_RESCUE_DEFAULT;       // master switch for everything in this block
    // emergency clearance: carrier inside panicZone of our goal line with an opponent inside panicRadius
    float panicZone = 14.0f;
    float panicRadius = 3.0f;
    float panicAngleDeg = 45.0f;   // angle of the clearance from the attack axis, toward the nearer touchline
    float panicReach = 40.0f;
    int panicHoldMs = 420;
    // forward-only build-up: no backward ground passes; sideways only as an emergency release
    bool forwardOnly = true;
    float forwardMin = 1.5f;       // progressive minimum
    float crowdSideBand = 2.5f;    // recycle depth allowed under press in our own third
    float crowdSideBandMid = 8.0f; // recycle depth allowed under press from midfield forward (never toward our box)
    float sideBand = 0.5f;         // sideways almost banned
    float sidewaysPress = 0.8f;    // localized pressure (sum of 1 - d/pressRadius over nearby opponents) needed for a sideways release
    float forwardSafeMargin = 1.5f;// a forward lane with at least this much clearance (m) counts as open
    // one-touch tempo: carry toward the next receiver, trigger the pass shortly before the ball arrives
    bool preAlign = true;
    float preAlignMaxDeg = 95.0f;  // more aggressive pre-alignment for fast touches
    bool anticipate = true;
    int oneTouchPreMs = 85;        // fire the pass earlier so the touch is already prepared (fast-paced)
    int oneTouchSlackMs = 55;      // wider tolerance so more one-touch windows are caught
    float anticipateMinSpeed = 3.2f; // trigger on slightly slower balls too
    // zero-hesitation shooting (aggressive finishing pack for Legendary)
    bool shotEveryFrame = true;  // FINAL
    float shotSkipTurnOpp = 2.8f;  // nearest opponent at least this far: fire off the current stride, no turning
    int shotRearmMs = 700;         // after a shot (or a cancelled attempt) the next look is allowed again (faster rearm)
    float shotPriorityAx = 8.0f;  // prefer shot earlier in the final third
    bool shotLog = true;
    // defence
    // no tackle: the defender shadows with Match-up and containment instead of going to ground
    bool tackleEnabled = !PB_RESCUE_DEFAULT;
    // shooting is limited to this distance from goal (m) — extended so more chances are taken
    float shotMaxDist = 30.0f;
    int shotSpacingMs = 900;      // after a shot, no second shot for this long
    // release rules: pressed by an opponent inside pressReleaseDist for pressReleaseMs, or holding too long, forces a pass
    float pressReleaseDist = 2.5f;
    int pressReleaseMs = 300;
    int holdCapPressMs = 800;       // hold cap with an opponent within 4 m
    int holdCapFreeMs = 1800;       // hold cap with room
    // final third: an unmarked team-mate inside the box is worth a bonus when there is no shot
    float wBoxPass = 1.25f;       // deliver into the box
    float boxPassFree = 2.2f;
    bool boxLockout = true;        // no tackle / press-call while the defender or the ball is inside our own box
    float defLockDepth = 18.0f;    // depth from our goal line (m) that disables tackles and press calls
    bool lineStep = true;          // step the back line up when the opposition passes sideways or backward in midfield
    float lineStepM = 2.0f;
    int lineStepMs = 1200;
    float lineStepMid = 22.0f;     // |x| below this counts as midfield
    float coneRadius = 3.0f;       // controlled player within this distance of the carrier-receiver lane snaps onto it
    bool containment = true;       // hold a buffer between the defender and an attacker outside the box
    float containMin = 1.5f;
    float containMax = 2.0f;
    // wing switch: crowded around the carrier, hit the far touchline
    bool wingSwitch = true;
    float switchRadius = 10.0f;
    int switchMinOpp = 5;
    float switchMinLat = 18.0f;
    float switchOpenMin = 4.0f;
    float switchPower = 0.95f;
    float switchMaxLen = 62.0f;
    // further rescue rules
    float gkLeadSec = 0.76f;          // keeper position is extrapolated this far (wind-up time) before choosing the corner
    float momentumGamma = PB_RESCUE_DEFAULT ? 0.08f : 0.18f;      // sprinting adds momentum: shorten the power hold by gamma * v / vmax
    bool rollTouch = true;            // sprinting shot with room: 0.1 s nudge toward goal first
    float rollMinSpeed = 8.0f;
    float rollMinOpp = 3.5f;
    int rollMs = 100;
    bool triangle = true;             // bonus for passes that form a 45-75 degree triangle with a supporting runner
    float triMinDeg = 40.f, triMaxDeg = 80.f, triLane = 1.8f, wTriangle = 0.22f; // strong triangle preference for short tempo
    float passAlpha = 0.03f, passBeta = 0.12f;   // pass power = base * (1 + alpha * length + beta * defenders on the ray)
    float rayMin = 0.85f;             // stricter pass-lane clearance
    bool blindSide = true;            // first touch away from the pressure centroid
    float densityLoftN = 5;           // central midfield defenders that force a lofted through ball
    float stickLambda = 0.25f;        // per-frame blend of the pre-aligned stick toward the receiver
    float stepMinGap = 12.0f;         // line step-up only when the opposition ball is this far from our last defender
    bool velAvg5 = true;

    // ---- Upgrade pack: live opponent line, role inference, two-step lookahead, loose-ball interception,
    //      counter-press, stuck / radar-glitch recovery, in-match pass-safety learning. Master switch + one switch each. ----
    bool upgrades = PB_RESCUE_DEFAULT;
    bool oppLine = true;          // continuous, confidence-weighted estimate of the opponent's back-line height and width
    float oppLineAlpha = 0.12f;
    float highLineDepth = 30.f;   // back line this far (m) from their goal or more leaves space behind it
    float deepLineDepth = 18.f;   // back line this close to their goal or less: no room for balls in behind
    float wLine = 0.20f;
    bool roles = true;            // GK / CB / FB / DM / CM / AM / WG / ST from position ranks
    float wRole = 0.05f;
    bool lookahead = true;        // value of the receiver's next touch (shoot or progress again), 2 touches deep
    float wLook = 0.80f;
    float lookMinXp = 0.50f;
    bool ballFlight = true;
    float ballFric = 0.80f;       // 1/s exponential rolling friction used to predict the loose ball
    bool looseChase = true;       // loose ball: the controlled player goes to the predicted intercept point when he wins the race
    float chaseSpeed = 6.5f;      // m/s the controlled player covers (dash)
    float chaseMargin = 0.25f;    // s he must beat the nearest opponent by
    float chaseTeamMargin = 0.10f;// and the nearest team-mate (who would otherwise be the one to take it)
    float chaseMaxDist = 22.f;
    bool counterPress = true;     // swarm the ball for a few seconds after a turnover, then drop back into shape
    int counterPressMs = 5800;    // ELITE: longer swarm after turnover
    float counterPressMinAx = -20.f;   // not while the ball is deeper than this in our own half (attack axis): shape first
    float counterPressSwitchGap = 0.7f;// switch to a team-mate who is this much closer (more aggressive)
    bool recovery = true;         // stuck-decision and radar-glitch detection with an automatic soft reset
    int stuckMs = 6000;
    float stuckBallM = 3.0f;
    float jumpM = 8.f;            // median distance (m) from each radar dot to the nearest dot of the previous frame: a real read stays under ~2 m, a garbage read is far above
    int jumpRuns = 3;
    bool online = true;           // in-match, strongly regularised adjustment of the pass-safety threshold
    float onlineTarget = 0.78f;   // pass completion the threshold steers toward
    float onlineMaxUp = 0.08f;
    float onlineMaxDown = 0.0f;   // never loosen: lost passes are under-counted, so a high completion rate is not trustworthy
    float onlineGain = 0.60f;
    float onlineAlpha = 0.15f;

    // ---- Match intelligence pack: score / clock aware phases, opponent-intent filter, in-match possession value,
    //      short sequence search, defensive cover and interception races. Everything here lives inside one match only. ----
    bool plus = true;             // master switch for the whole pack
    bool plusField = true;        // potential-field carry direction
    bool plusEpv = true;          // role-aware expected possession value, updated in-match with TD(lambda)
    bool plusIntent = true;       // Bayesian filter over the opponent's press height, flank, compactness, long-ball bias, recovery
    bool plusSeq = true;          // four-pass build-up dynamic program with cumulative interception risk
    bool plusTree = true;         // depth-limited Monte-Carlo action tree
    bool plusThrough = true;      // multi-hypothesis through-ball aim and score
    bool plusMotif = true;        // sequence mining over the last 40 sequences
    bool plusStyle = true;        // TD controlled patience / directness / width / risk
    bool plusRace = true;         // interception race assignment
    bool plusCover = true;        // min-conflicts defensive cover
    bool plusPhase = true;        // early press, lead protection, possession loop, chase
    bool plusOneTwo = true;       // give-and-go return passes
    bool plusRebound = true;      // rebound hunting after our own shot
    bool plusFinalThird = true;   // no dribbling in the final third unless the way is clear
    float plusTreeMs = 5.0f;      // time budget of the action tree per decision
    int plusTreeRollouts = 14;    // rollouts per root action
    float plusMatchSec = 600.f;   // match length used when the host does not supply the played fraction
    float plusLambda = 0.7f;      // TD(lambda) trace decay of the possession value
    float plusAlpha = 0.12f;      // TD step size
    float wEpv = 0.9f;
    float wSeq = 0.55f;
    float wTree = 0.60f;
    float wThrough = 0.65f;  // less weight on long throughs that miss
    float wMotif = 0.35f;
    float wIntent = 0.20f;
    float wStyle = 0.20f;
    float wOneTwo = 0.32f;
    float wNoDribble = 0.30f;
    float wLoop = 0.28f;
    float plusFinalAx = 17.0f;    // attack-axis position where the final third starts
    int legendHoldMs = 700;       // longest a recycling carrier may keep the ball when nobody presses him

    // ---- Meta layer: reaction model, deception, 1v1 defence, set pieces, late game (pitch_meta.inc). Per match only. ----
    bool meta = true;
    bool metaNoise = true;        // anti-pattern residuals on repeated passes
    bool metaFeint = true;        // shot / pass fakes
    bool metaCommit = true;       // delayed decision window + late commitment among the top options
    bool metaStupid = true;       // rare low-ranked plays in zones where the opposition seldom intercepts
    bool metaReact = true;        // measured opposition reaction time replaces the fixed defender reaction
    bool metaLock = true;         // animation-aware lock of opposing players
    bool metaMicro = true;        // small stick residual on carries
    bool metaIntent1v1 = true;    // lunge / jockey / contain / step-in / drop-off classifier
    bool metaContain = true;      // adaptive jockey distance and weak-side bias
    bool metaBait = true;         // leave a low-success lane open and run it down
    bool metaSecondMan = true;    // second defender cuts the escape lane
    bool metaRecovery = true;     // recovery-run assignment after a turnover
    bool metaTrackFreeze = true;  // freeze risky actions after a radar identity jump
    bool metaOverload = true;     // half-space overload and the sudden switch
    bool metaThirdMan = true;     // pass to the player who can find a runner
    bool metaThroughTiming = true;
    bool metaSecurity = true;     // possession-security circuit breaker
    bool metaField = true;        // interception-probability field
    bool metaMultiScale = true;   // 3-second territory term
    bool metaFsm = true;          // Clinical / Patient / Chaotic / Desperado
    bool metaSetPiece = true;     // restart rules
    bool metaAntiBait = true;     // each deceptive system fades when it stops working
    bool metaLate = true;         // late-game tempo
    float metaLateStart = 0.84f;  // played fraction where the late-game rules start
    float metaJumpM = 7.0f;       // one-frame displacement that counts as a radar jump
    float wLock = 0.06f, wField = 0.30f, wMS = 0.40f, wThird = 0.40f, wOver = 0.12f, wSet = 0.45f;

    // ---- Planning layer: lane-cut model, offside trap, combination streak, keeper model, role shape, conditional counter-press,
    //      tempo phases, ball hypotheses, free-man ranking, second ball, directional clears, local overloads, opponent
    //      channels, restart build-up, pressure field, late-game risk, adaptive cadence (pitch_plan.inc). Per match only. ----
    bool plan = true;
    bool planCache = true;        // keep the best option for a few frames so play looks planned
    bool planLane = true;         // dynamic lane value with press bias and acceleration
    bool planTrap = true;         // offside-trap detector
    bool planCombo = true;        // one-two / triangle streak
    bool planKeeper = true;       // keeper dive bias from observed keeper moves after our shots
    bool planShape = true;        // role-aware shape, cover shadow, switch only when the shape improves
    bool planCounterPress = true; // numbers condition and a foul-risk style cut-off
    bool planTempo = true;        // build / progress / finish / protect
    bool planOppModel = true;     // press trigger distance and panic depth
    bool planBall = true;         // ball hypotheses while the ring is lost
    bool planFreeMan = true;      // danger and free-man ranking
    bool planSecondBall = true;   // second-ball window after shots and clearances
    bool planDirClear = true;     // clear to the flank with room and a team-mate
    bool planSidestep = true;     // step away from a closing opponent, then play
    bool planOverload = true;     // 2v1 / 3v2 near the ball
    bool planChannel = true;      // per-channel pass success against this opponent
    bool planRestart = true;      // build-up after a restart
    bool planPressure = true;     // Gaussian pressure field forces the release
    bool planLateRisk = true;     // late-game risk and the chaos noise on a draw
    bool planCadence = true;      // decision interval follows the pressure (faster under pressure and in the final third)
    bool planHoldMargin = false;   // a calm carrier needs a margin to prefer a pass over the carry; the margin fades with time on the ball
    bool planCalmSlow = false;    // stretch the interval to 200 ms when calm (off: measured to delay the first release)
    float wLane = 0.30f, wFree = 0.12f, wCache = 0.05f;
    float planPressHigh = 1.10f;  // pressure-field value that forces a release
    float planPressCalm = 0.30f;  // below this the cadence relaxes

    // ---- layer: roll-out, learned heads, deception, survival, set pieces, stamina, latency (pitch_v617.inc). ----
    bool v7 = true;               // master switch
    bool v7Rollout = true;        // depth-3 radar roll-out + planned-sequence buffer
    bool v7Policy = true;         // PolicyNet blended with the hand-made utilities
    bool v7OppIntent = true;      // build / progress / direct / switch / protect classifier
    bool v7Morph = true;          // numerical overload / underload shape morph
    bool v7Embed = true;          // per-opponent latent vector conditioning the policy
    bool v7PreAlign = true;       // stick lined up with the receiving angle 100-180 ms before the pass
    bool v7SeqNet = true;         // sequence value network
    bool v7MetaNet = true;        // meta controller: style weights and risk offsets
    bool v7DefHead = true;        // learned defensive mode (tight / normal / passive)
    bool v7Risk = true;           // score- and clock-aware risk layer
    bool v7Timing = true;         // learned pass / through-ball timing residual
    bool v7Landing = true;        // ball landing zone from the last 6 radar positions
    bool v7Survival = true;       // escape mode under extreme pressure
    bool v7Escape = true;         // learned escape head used inside survival mode
    bool v7Style = true;          // Control / Vertical / Chaos blend
    bool v7Recurrent = true;      // recurrent opponent movement predictor
    bool v7SetPiece = true;       // adversarial set-piece routines
    bool v7Kickoff = true;        // compact kick-off lock
    bool v7GkShadow = true;       // keeper shadow model for the covering defender
    bool v7Latency = true;        // measured latency replaces the fixed Kalman lead
    bool v7Stamina = true;        // stamina estimate from radar movement
    bool v7TempoLock = true;      // possession recycling lock after erratic play / many stoppages
    bool v7DualPress = true;      // reward the zone a second presser leaves
    bool v7Learn = true;          // end-of-match training of every learned head
    bool v7Ewc = true;            // elastic weight consolidation
    bool v7Diversity = true;      // concentration penalty on recent behaviour
    bool v7League = true;         // offline league step on the replay
    bool v7Anomaly = true;        // drop low-quality matches before training
    float wPolicy = 0.22f, wRoll = 0.30f, wBuf = 0.35f, wSeqNet = 0.12f, wIntent7 = 0.05f, wVacated = 0.45f, wMorph = 0.10f;
    float v7SurvPress = 1.7f;     // pressure-field value that starts survival mode
    float v7StaminaCap = 700.f;   // weighted high-intensity metres that empty one player

    // ---- layer (pitch_v618.inc): threat prediction, last-man protection, defender assignment, press gating, plan validity,
    //      counter exposure, tactical state machine, opponent profiles, shot prevention, per-kind latency, radar guard, benchmarking. ----
    bool v8 = true;               // master switch (off = exactly the behaviour)
    bool v8Threat = true;         // 1  per-opponent threat score, run types, dangerous lane, confidence
    bool v8Shape = true;          // 2  last-man protection, beaten-line prediction, depth by runner speed, de-conflicted points
    bool v8Assign = true;         // 3  multi-factor defender assignment with a commitment period
    bool v8Press = true;          // 4  recovery probability gates the counter-press; safe double-team exploitation
    bool v8PlanValid = true;      // 5  roll-out plan cancelled when its assumptions fail
    bool v8Exposure = true;       // 6  counter-attack exposure and risk limits per tactical state
    bool v8Fsm = true;            // 7  seven-state tactical machine
    bool v8Kickoff = true;        // 7  kick-off protection with suitability based slots
    bool v8OppModel = true;       // 8  opponent pattern profiles + evidence-weighted confidence
    bool v8ShotGuard = true;      // 9  shot / cutback / central pass detection and lane blocking
    bool v8Latency = true;        // 10 per-kind latency statistics and bounded compensation
    bool v8Guard = true;          // 11 radar validation, safe mode, command sanitiser
    bool v8Bench = true;          // 12 per-match metrics, train / eval split, regression gate
    bool v8Record = false;        // 12 frame recorder for replay (host switch)
    bool v8GkControl = false;     // 9  goalkeeper intents only when the host has confirmed a reliable mapping
    float v8ThreatTrig = 0.52f;   // confidence-weighted threat that triggers pre-emptive defence
    float v8ConfMin = 0.45f;      // lowest dot confidence that may trigger it
    int v8CommitMs = 700;         // an assignment is kept this long ...
    float v8OverrideGain = 1.4f;  // ... unless a new threat is this much more dangerous
    float v8PressRecMin = 0.42f;  // recovery probability needed to counter-press
    int v8PressMinCover = 2;      // defenders that must stay goal-side of the ball while pressing
    int v8KickoffMs = 1800;       // length of the kick-off protection
    float v8SafeEnter = 0.50f, v8SafeExit = 0.64f;   // tracking confidence thresholds of safe mode
    float wExpo = 0.30f;          // weight of the counter-exposure penalty
    int v8GateMinEval = 4;        // evaluation matches needed before the regression gate decides
    float v8LatMaxComp = 120.f;   // bounded latency compensation (ms)
};

struct GestureOutput {
    float theta = 0.f;
    float r = 0.f;
    int durationMs = 0;
    int actionType = ACT_NONE;
    bool dash = false;
    bool valid = false;
    int status = ST_NOT_LIVE;
    int flick = 0;                // stunning shot: 0 none, 1 flick the shoot button left, 2 right

    int64_t pack() const;
    static GestureOutput unpack(int64_t v);
};

struct PassCandidate {
    int targetId = 0;
    int type = ACT_PASS;
    MetricVector2D target;
    float length = 0.f;
    float clearance = 0.f;
    float xP = 0.f;
    float xPRaw = 0.f;
    float deltaXT = 0.f;
    float pressure = 0.f;
    float offside = 0.f;
    float utility = 0.f;
    float prob = 0.f;
    float diag = 0.f;
};

struct MatchState {
    PlayerState team[TEAM_SIZE];
    PlayerState opp[TEAM_SIZE];
    MetricVector2D ball;
    MetricVector2D ballVel;
    bool ballSeen = false;
    int status = ST_NOT_LIVE;
    int carrier = -1;
    int attackSign = 1;
    bool counter = false;
    float temperature = 0.8f;
    double timeMs = 0;
    float crowd = 0.f;
    int crowdOpp = 0;
    float nearestOpp = 1e3f;
    bool crowded = false;
    float pressTtc = 1e3f;
    int committed = 0;
    float pressDist = 1e3f;
    bool oneTouch = false;
    float congestion = 0.f;
    float oppSpread = 18.f;
    float oppDepth = 0.5f;
};

constexpr int HUNGARIAN_MAX = 32;
double hungarianSolve(const float* cost, int n, int* rowToCol);

struct HsvScalar {
    double v[4] = {0, 0, 0, 0};
    HsvScalar() = default;
    HsvScalar(double h, double s, double val, double a = 0) { v[0] = h; v[1] = s; v[2] = val; v[3] = a; }
};

constexpr int HSV_TOL_H = 10;
constexpr int HSV_TOL_S = 40;
constexpr int HSV_TOL_V = 40;
constexpr int HSV_PACK_INTS = 11;

struct HsvRange {
    bool set = false;
    bool chroma = true;
    bool wrapHue = false;
    int h0 = 0, s0 = 0, v0 = 0;
    HsvScalar lower;
    HsvScalar upper;
    bool contains(int h, int s, int v) const;
    void pack(int* out11) const;
    bool unpack(const int* in11);
};

struct Homography {
    double h[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool fromQuad(const float* src8);
    MetricVector2D apply(float px, float py) const;
};


// Per-match state of the match intelligence pack (see TacticalConfig::plus). Cleared at every new match; nothing is carried between matches.
constexpr int PLUS_EPV_ROLES = 5, PLUS_EPV_DENS = 3, PLUS_EPV_VEL = 2;
constexpr int PLUS_EPV_N = PLUS_EPV_ROLES * XT_COLS * XT_ROWS * PLUS_EPV_DENS * PLUS_EPV_VEL;
constexpr int PLUS_TRACE = 16, PLUS_SEQ = 40, PLUS_STEPS = 8, PLUS_KEYS = 243, PLUS_PAIRS = 512;

struct PlusSeq {
    unsigned short key[PLUS_STEPS] = {0};
    int n = 0;
    int outcome = 0;       // +1 ended in a shot, -1 ended in a turnover, 0 interrupted
};

struct PlusState {
    // clock and estimated score
    double liveMs = 0;
    float phase = -1.f;                 // played fraction of the match supplied by the host, -1 = use the live clock
    int goalsFor = 0, goalsAgainst = 0;
    double lastGoalMs = -1e9;
    float lastBallAx = 0.f, lastBallY = 0.f;
    double lastBallMs = -1e9;
    bool lastBallValid = false;
    int stableStatus = 0, candStatus = 0, candRun = 0;
    double lastTeamMs = -1e9;
    // opponent intent filter: press height, flank overload, compactness, long-ball bias, recovery speed
    float im[5] = {0.5f, 0.5f, 0.5f, 0.3f, 0.5f};
    float ip[5] = {0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    bool longFlag = false;
    int longEvents = 0;
    double oppPossMs = 0;
    // possession value table and the trace of the running sequence
    float epv[PLUS_EPV_N];
    bool epvInit = false;
    int trace[PLUS_TRACE];
    int nTrace = 0;
    // sequence mining
    PlusSeq seq[PLUS_SEQ];
    int nSeq = 0, seqHead = 0;
    PlusSeq cur;
    float keyLift[PLUS_KEYS];
    float keyN[PLUS_KEYS];
    unsigned short pairKey[PLUS_PAIRS];
    float pairLift[PLUS_PAIRS];
    unsigned char pairUsed[PLUS_PAIRS];
    // TD style controller
    float theta[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float eps[4] = {0.f, 0.f, 0.f, 0.f};
    double winStartMs = -1;
    float winValueSum = 0.f;
    int winValueN = 0;
    int winTurnovers = 0;
    float baseline = 0.f;
    // give-and-go
    int lastPasser = -1;
    MetricVector2D lastPassFrom;
    double lastPassMs = -1e9;
    // per-candidate extras prepared before the utilities are built
    float mh[MAX_CAND];
    // defence
    double coverAt = -1e9;
    int clutchOverride = -1;
    PlusState() { clear(); }
    void clear();
};

// Per-match state of the meta layer (see TacticalConfig::meta). Cleared at every new match.
constexpr int META_ACT = 48, META_SYS = 7, META_ZONES = 12;
struct MetaAct { float len = 0, ang = 0, hold = 0, sx = 0, sy = 0; };
struct MetaWatch { double t0 = 0; int opp = -1; MetricVector2D v0; MetricVector2D lane; float sp0 = 0; double tDec = -1; bool active = false; };

struct MetaState {
    MetaAct acts[META_ACT];
    int nActs = 0, actHead = 0;
    // opposition reaction model: [0] decision latency, [1] execution latency (seconds), mean and mean absolute deviation
    float reMean[2] = {0.f, 0.f}, reDev[2] = {0.f, 0.f};
    int reN[2] = {0, 0};
    MetaWatch watch[2];
    // radar jump and animation proxy
    MetricVector2D prevPos[2][TEAM_SIZE];
    bool prevOk[2][TEAM_SIZE];
    MetricVector2D prevVel[TEAM_SIZE];
    MetricVector2D acc[TEAM_SIZE];
    bool animOk[TEAM_SIZE];
    double lastBig[TEAM_SIZE];
    float dirVar[TEAM_SIZE];
    unsigned char anim[TEAM_SIZE];            // 0 recoverable, 1 locked, 2 committed
    double freezeUntil = -1e9;
    // 1v1 intent classifier: jockey, lunge, contain, step-in, drop-off
    float W[5][6];
    float prob[5];
    float feat[6];
    float featStart[6];
    float probStart[5];
    float iPrevD = 0.f, iPrevClose = 0.f;
    double iPrevT = -1e9;
    double commitDoneAt = -1e9;
    float readEma = 0.f;
    double recAt = -1e9, secAt = -1e9, baitAt = -1e9;
    double iT0 = -1;
    float iD0 = 0.f;
    int iOpp = -1;
    // opposition carrier bursts and engagement side
    float burst[4][4];
    float burstTot = 0.f;
    float sideCnt[2] = {0.f, 0.f};
    float contain = 0.f;
    float prevCarSpeed = 0.f;
    double prevCarAt = -1e9, lastBurstAt = -1e9;
    float oppProg = 0.f;
    float prevBallX = 0.f;
    bool prevBallOk = false;
    // opposition pass history by lane kind: completed / intercepted
    float oppPassOk[3] = {1.f, 1.f, 1.f}, oppPassLost[3] = {1.f, 1.f, 1.f};
    int oppPassKind = -1;
    double oppPassAt = -1e9;
    double baitUntil = -1e9;
    int baitKind = -1;
    int prevStable = 0;
    // interception-probability field
    float field[XT_COLS * XT_ROWS];
    double fieldAt = -1e9;
    // possession security
    double safeUntil = -1e9;
    int tZone[3] = {-1, -1, -1};
    // overload and switch
    int overSide = 0;
    double overUntil = -1e9, overAt = -1e9;
    float oppCenY = 0.f;
    // zones the opposition does not intercept
    float spAtt[META_ZONES][3][3];
    float spLost[META_ZONES][3][3];
    int curZone = -1, curPress = 0, curKind = 0;
    // feints and the commit gate
    double feintUntil = -1e9, feintLastAt = -1e9;
    MetricVector2D feintDir;
    int feintKind = 0;
    float oppJump = 0.f;
    double jumpAt = -1e9;
    double commitStart = -1.0;
    int commitType = -1, commitTarget = -1;
    float smU[4] = {0, 0, 0, 0};
    int smType[4] = {-1, -1, -1, -1}, smTarget[4] = {-1, -1, -1, -1};
    // deceptive systems: 0 noise, 1 shot fake, 2 pass fake, 3 bait, 4 odd play, 5 through timing, 6 commit gate
    float sysSucc[META_SYS];
    int sysN[META_SYS];
    int lastSys = -1;
    double lastSysAt = -1e9;
    int recentInter = 0;
    double lastInterAt = -1e9;
    float thrRate = 0.f;
    int thrN = 0;
    float thrOffsetMs = 0.f;
    // four-mode machine
    int mode = 0;
    double modeSince = -1e9;
    float possEma = 0.5f;
    double modeAt = -1e9;
    float threatEma = 0.f;
    // restarts
    int restartKind = 0;                       // 1 corner, 2 goal kick, 3 throw-in, 4 free kick, 5 their set piece
    double stillSince = -1e9;
    MetaState() { clear(); }
    void clear();
};

// Per-match state of the planning layer (see TacticalConfig::plan). Cleared at every new match.
constexpr int PLAN_CH = 3;
struct PlanState {
    double cacheAt = -1e9;
    int cacheType = -1, cacheTarget = -1;
    float linePrev = 0.f, lineRate = 0.f;
    bool linePrevOk = false;
    double trapUntil = -1e9;
    int streak = 0;
    double streakAt = -1e9, comboForceAt = -1e9;
    float keeperBias = 0.f;
    int keeperN = 0;
    bool keeperWatch = false;
    double keeperWatchAt = -1e9;
    float keeperY0 = 0.f;
    bool cpOk = true, cpUsed = false;
    double cpCheckAt = -1e9, cpBlockedUntil = -1e9, midBlockUntil = -1e9;
    double cpFailAt[2] = {-1e9, -1e9};
    float pressTrig = 6.f;
    int pressTrigN = 0;
    float prevClose = 0.f;
    float lineLevel = 0.f, lineBehind = 0.f, lineLead = 0.f;
    bool haveLevel = false, haveLead = false;
    double lineSampleAt = -1e9;
    double winAt = -1e9;
    float seenEma = 1.f;
    int ballHyp = 0;
    int attachSide = -1, attachIdx = -1;
    MetricVector2D attachOff;
    float danger[TEAM_SIZE];
    float freeR[TEAM_SIZE];
    int topFree[2] = {-1, -1};
    double rankAt = -1e9;
    double sideUntil = -1e9, sideLast = -1e9;
    MetricVector2D sideTo;
    float chOk[PLAN_CH][3], chN[PLAN_CH][3];
    int curChan = 1;
    double buildUntil = -1e9;
    int buildCount = 0;
    int lastRestart = 0;
    double shadowAt = -1e9;
    PlanState() { clear(); }
    void clear();
};

// ---------------------------------------------------------------------------------------------------------------------
// layer (pitch_v617.inc): small learned heads, planning buffers, deception, survival, set pieces, stamina, latency.
// V7Mem is long-term memory (kept between matches, saved by the host); V7State is per match.
// ---------------------------------------------------------------------------------------------------------------------
constexpr int V7_NIN = 12, V7_NH = 8, V7_NCLS = 5;
constexpr int V7_REPLAY = 20, V7_TUP_MATCH = 120, V7_EMB_N = 16, V7_EMB_D = 4, V7_CKPT = 4, V7_FP = 8, V7_FPN = 5;
constexpr int V7_HIST = 14, V7_SEQ_CAP = 600, V7_ESC_CAP = 300, V7_TUP_CAP = 900;
enum PlayStyle : int { PS_CONTROL = 0, PS_VERTICAL = 1, PS_CHAOS = 2 };

// One-hidden-layer network with a flat parameter vector, so that EWC anchors, Fisher estimates and checkpoints are plain arrays.
template <int IN, int H, int OUT>
struct V7Mlp {
    static constexpr int NP = H * IN + H + OUT * H + OUT;
    float p[NP], anchor[NP], fisher[NP];
    int trained = 0;
    V7Mlp() { init(1u); }
    void init(uint32_t seed) {
        uint32_t s = seed ? seed : 1u;
        for (int k = 0; k < NP; k++) { p[k] = 0.f; anchor[k] = 0.f; fisher[k] = 0.f; }
        for (int k = 0; k < H * IN; k++) {                       // small random first layer; output layer starts at zero (neutral)
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            p[k] = ((float)(s & 0xFFFFu) / 65535.f - 0.5f) * 0.8f / std::sqrt((float)IN);
        }
        trained = 0;
    }
    void forward(const float* x, float* out, float* hid = nullptr) const {
        float h[H];
        for (int j = 0; j < H; j++) {
            float a = p[H * IN + j];
            for (int i = 0; i < IN; i++) a += p[j * IN + i] * x[i];
            h[j] = std::tanh(a);
            if (hid) hid[j] = h[j];
        }
        const int o2 = H * IN + H;
        for (int o = 0; o < OUT; o++) {
            float a = p[o2 + OUT * H + o];
            for (int j = 0; j < H; j++) a += p[o2 + o * H + j] * h[j];
            out[o] = a;
        }
    }
    // squared sensitivity of output idx to every weight (accumulated into g2): the Fisher-style importance used by EWC
    void sens(const float* x, int idx, float* g2) const {
        float h[H], out[OUT];
        forward(x, out, h);
        const int o2 = H * IN + H;
        for (int j = 0; j < H; j++) {
            const float dh = p[o2 + idx * H + j] * (1.f - h[j] * h[j]);
            for (int i = 0; i < IN; i++) g2[j * IN + i] += dh * x[i] * dh * x[i];
            g2[H * IN + j] += dh * dh;
            g2[o2 + idx * H + j] += h[j] * h[j];
        }
        g2[o2 + OUT * H + idx] += 1.f;
    }
    // one SGD step on output idx toward target; EWC pulls every weight toward its anchor in proportion to its Fisher value
    float step(const float* x, int idx, float target, float lr, float ewc, float* g2) {
        float h[H], out[OUT];
        forward(x, out, h);
        float err = out[idx] - target;
        err = err < -2.f ? -2.f : (err > 2.f ? 2.f : err);
        const int o2 = H * IN + H;
        float g[NP];
        for (int k = 0; k < NP; k++) g[k] = 0.f;
        for (int j = 0; j < H; j++) {
            const float dh = err * p[o2 + idx * H + j] * (1.f - h[j] * h[j]);
            for (int i = 0; i < IN; i++) g[j * IN + i] = dh * x[i];
            g[H * IN + j] = dh;
            g[o2 + idx * H + j] = err * h[j];
        }
        g[o2 + OUT * H + idx] = err;
        for (int k = 0; k < NP; k++) {
            if (g2) g2[k] += g[k] * g[k];
            float pull = lr * ewc * fisher[k]; if (pull > 0.5f) pull = 0.5f;          // bounded, so a strong consolidation cannot oscillate
            p[k] -= lr * g[k] + pull * (p[k] - anchor[k]);
        }
        trained++;
        return err * err;
    }
};
typedef V7Mlp<V7_NIN, V7_NH, V7_NCLS> V7Policy;

struct V7Tup { float f[V7_NIN] = {0}; int cls = 0; float r = 0.f; };
struct V7SeqSample { float f[8] = {0}; float y = 0.f; };
struct V7EscSample { float f[7] = {0}; float y = 0.f; };
struct V7MatchRec { std::vector<V7Tup> tup; };
struct V7Embed { uint32_t key = 0; float e[V7_EMB_D] = {0, 0, 0, 0}; int n = 0; };
struct V7Hop { float len = 0.f, fwd = 0.f, cosA = 0.f; };
struct V7Plan { int type = 0, targetId = 0; };

struct V7Mem {
    V7Policy policy;
    V7Mlp<8, 6, 1> seqNet;
    V7Mlp<6, 6, 3> defNet;
    V7Mlp<8, 8, 5> metaNet;
    float esc[7] = {0, 0, 0, 0, 0, 0, 0};            // escape head: logistic weights, index 0 is the bias
    float timingQ[3][3][5];                           // [kind pass/through/shot][pressure low/mid/high][offset arm]
    int timingN[3][3][5];
    float spW[6][3];                                  // set-piece routine weights per restart kind (EXP3 mixing)
    int spN[6][3];
    V7Embed emb[V7_EMB_N];
    int nEmb = 0;
    uint32_t curSig = 0;                              // opponent kit signature of the running match
    std::vector<V7MatchRec> replay;
    std::vector<V7SeqSample> seqRep;
    std::vector<V7EscSample> escRep;
    std::vector<V7Tup> defRep;                        // defence-head tuples reuse V7Tup (f[0..5], cls = mode)
    float ckpt[V7_CKPT][V7Policy::NP];
    int nCkpt = 0, ckptHead = 0;
    float fp[V7_FPN][V7_FP];
    int nFp = 0, fpHead = 0;
    int matches = 0, matchesRejected = 0, trainedMatches = 0;
    std::string report;                               // plain-language result of the last end-of-match step
    V7Mem() { clear(); }
    void clear();
};

struct V7State {
    // match quality (anomaly filter)
    long frames = 0, noBall = 0, jumpFrames = 0, dtN = 0;
    double dtMean = 0, dtM2 = 0, lastFrameMs = -1;
    // experience of this match
    std::vector<V7Tup> tup;
    std::vector<V7SeqSample> seqS;
    std::vector<V7EscSample> escS;
    std::vector<V7Tup> defS;
    int clsCount[V7_NCLS] = {0, 0, 0, 0, 0};
    double lenSum = 0; int lenN = 0, fwdN = 0;
    double axSum = 0; int axN = 0;
    int passKept = 0, passLost = 0;
    bool tupPending = false; V7Tup tupP; double tupT = 0;
    // planned sequence (rollout + buffer)
    V7Plan plan[3]; int nPlan = 0; double planUntil = -1e9, planAt = -1e9, rollAt = -1e9;
    V7Hop hops[3]; int nHops = 0;
    // opponent intent: build, progress, direct, switch, protect
    float oi[5] = {0.2f, 0.2f, 0.2f, 0.2f, 0.2f}; int oiCount = 0, oiTop = 0; double flipT[10] = {0}; int nFlip = 0;
    // shape morph
    MetricVector2D morphTo[2]; int morphIdx[2] = {-1, -1}; float morphStr = 0.f; double morphUntil = -1e9;
    // opponent embedding in use
    float emb[V7_EMB_D] = {0, 0, 0, 0}; bool embLoaded = false;
    // timing residual
    int tKind = -1, tPr = -1, tArm = -1; bool tPend = false; double tHoldUntil = -1e9; int tHoldType = -1, tHoldTarget = -1;
    // pre-emptive stick alignment
    int paType = -1, paTarget = -1; double paDoneAt = -1e9;
    // ball history and measured flight friction
    double bhT[6] = {0}; MetricVector2D bhP[6]; int bhN = 0, bhHead = 0; float fric = 3.0f; int fricN = 0;
    // survival and escape
    bool surv = false; double survUntil = -1e9, survLock = -1e9;
    bool escPend = false; float escF[7] = {0}; double escT = 0; float escP0 = 0.f;
    // risk layer
    float rMinAdd = 0.f, rThrMul = 1.f, rShotMul = 1.f; int lastDiff = -99, lastMin = -1;
    // play style and meta controller
    float sw[3] = {1.f, 0.f, 0.f}, swT[3] = {1.f, 0.f, 0.f};
    int styleCur = 0; double styleLockUntil = -1e9, styleNextAt = 0;
    float metaX[8] = {0}; int metaStyle = -1; float metaRisk[2] = {0, 0}; double metaWinStart = -1;
    int metaKept0 = 0, metaLost0 = 0, metaGoals0 = 0; float metaPoss0 = 0.5f; double metaPossSum = 0; int metaPossN = 0;
    float metaMinAdd = 0.f, metaShotMul = 1.f;
    // defence head
    int defMode = 0; float defX[6] = {0}; bool defPend = false; double defAt = -1e9, defStart = -1e9; int defGa0 = 0;
    // recurrent opponent predictor (fixed random gates, readout trained online)
    float gh[TEAM_SIZE][6] = {}; float gro[3][7] = {}; float gphi[TEAM_SIZE][5][7] = {}; double gphiT[TEAM_SIZE][5] = {}; MetricVector2D gphiV[TEAM_SIZE][5]; int gphiN[TEAM_SIZE] = {};
    MetricVector2D gprev[TEAM_SIZE]; float gprevSp[TEAM_SIZE] = {}; float gprevAng[TEAM_SIZE] = {}; bool gprevOk[TEAM_SIZE] = {};
    MetricVector2D predDv[TEAM_SIZE]; float pSharp[TEAM_SIZE] = {}; int gSeen = 0;
    // restarts, set pieces, kick-off lock
    double lastRestartUntil = -1e9; int spKind = 0, spRoutine = -1, spStep = 0; double spStart = -1e9; bool spOurs = false; float spProb[3] = {0.f, 0.f, 0.f}; bool spShot = false, spLost = false; bool restartPend = false; double restartPendAt = -1e9;
    double koUntil = -1e9; bool koOurs = false;
    // stamina
    float hi[TEAM_SIZE] = {}; float stamAvg = 1.f;
    // tempo lock
    double tlUntil = -1e9; double stopT[8] = {0}; int nStop = 0;
    // dual press
    double hT[V7_HIST] = {0}; MetricVector2D hP[V7_HIST][TEAM_SIZE]; int hN = 0, hHead = 0;
    bool dual = false; double dualAt = -1e9; MetricVector2D vacated;
    // keeper shadow (last computed)
    float gkGap = 0.f;
    int nRoll = 0, rollType[8] = {0}, rollTgt[8] = {0}; float rollV[8] = {0};
    int prevStable = 0;
    V7State() { tup.reserve(V7_TUP_CAP); seqS.reserve(V7_SEQ_CAP); escS.reserve(400); defS.reserve(300); }
    void clear() {
        std::vector<V7Tup> a; a.swap(tup); std::vector<V7SeqSample> b; b.swap(seqS); std::vector<V7EscSample> c; c.swap(escS); std::vector<V7Tup> d; d.swap(defS);
        *this = V7State();
        a.clear(); b.clear(); c.clear(); d.clear();
        if (a.capacity() >= (size_t)V7_TUP_CAP) { tup.swap(a); seqS.swap(b); escS.swap(c); defS.swap(d); }
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// layer (pitch_v618.inc). V8Mem is long-term (saved with the memory file), V8State is per match.
// ---------------------------------------------------------------------------------------------------------------------
constexpr int V8_PAT = 5, V8_HIST = 40, V8_PROF = 16, V8_EXP = 240, V8_RESP = 3, V8_REL = 6, V8_DASH_STRIDE = 24;
enum TacState : int { TS_DEFEND = 0, TS_PRESS, TS_RECOVER, TS_COUNTER, TS_CONTROL, TS_PROTECT, TS_KICKOFF, TS_N };
enum RunKind : int { RK_NONE = 0, RK_FORWARD, RK_OVERLAP, RK_THROUGH, RK_CROSS, RK_SHOT };
enum DefJob : int { JOB_PRESS = 0, JOB_LANE, JOB_RUNNER, JOB_LINE, JOB_N };
enum ShotKind : int { SK_NONE = 0, SK_SHOT, SK_CUTBACK, SK_CENTRAL, SK_CLOSE };
enum ShotResp : int { SR_NONE = 0, SR_BLOCK, SR_INTERCEPT, SR_PRESSURE, SR_RECOVER };
enum InKind : int { IK_MOVE = 0, IK_PASS, IK_SHOT, IK_SWITCH, IK_OTHER, IK_N };
enum GateVerdict : int { GV_INSUFFICIENT = 0, GV_PROMOTE, GV_HOLD, GV_ROLLBACK };
enum OppPattern : int { OP_PRESS = 0, OP_THROUGH, OP_SWITCH, OP_WINGCUT, OP_LONG };

struct V8LatKind {
    int n = 0;
    float mean = 0.f, m2 = 0.f, ema = 0.f;         // total time from command submission to the end of the injected gesture (ms)
    float qMean = 0.f, oMean = 0.f;                // of which queue wait and dispatch overhead
    int drift = 0, late = 0, missed = 0, timeouts = 0, recal = 0;
    float effMean = 0.f; int effN = 0, effMiss = 0;   // decision to observed ball release: includes game animation, not only input
    float sd() const { return m2 > 0.f ? std::sqrt(m2) : 0.f; }   // m2 is the exponentially weighted variance
};

struct V8MatchRec {
    int gen = 0, split = 0, valid = 1;             // brain generation, 0 training / 1 evaluation, 1 usable
    int gf = 0, ga = 0, shotsFor = 0, shotsAgainst = 0, dangerAgainst = 0, turnovers = 0;
    float liveSec = 0.f, trackConf = 0.f, safeFrac = 0.f, poss = 0.5f;
    float patN[V8_PAT] = {0, 0, 0, 0, 0}, patOk[V8_PAT] = {0, 0, 0, 0, 0};
    float conf = 0.f, suff = 0.f, held = 0.f, perf = 0.5f;
};
struct V8Agg { int n = 0; float cs = 0.f, gd = 0.f, danger = 0.f, shots = 0.f, win = 0.f, perf = 0.f; };

struct V8OppProfile {
    uint32_t key = 0;
    int n = 0;                                     // matches observed
    float rate[V8_PAT] = {0, 0, 0, 0, 0};          // events per minute (running mean over matches)
    float rate2[V8_PAT] = {0, 0, 0, 0, 0};         // running mean of squares, for consistency
    float succ[V8_PAT] = {0, 0, 0, 0, 0};          // evidence-weighted share of events that gained ground
    float wsum[V8_PAT] = {0, 0, 0, 0, 0};          // total evidence weight behind succ
    float lineDepth = 0.f, lineSd = 0.f, throughDelay = 0.f;
    int throughN = 0;
    float route[3] = {0.33f, 0.34f, 0.33f};
    float resp[V8_PAT][V8_RESP];                   // our defensive response (drop / hold / step) -> share of events that did not gain ground
    float respN[V8_PAT][V8_RESP];
    float conf = 0.f;
    V8OppProfile() { for (int p = 0; p < V8_PAT; p++) for (int r = 0; r < V8_RESP; r++) { resp[p][r] = 0.5f; respN[p][r] = 0.f; } }
};

struct V8Mem {
    V8OppProfile prof[V8_PROF];
    int nProf = 0;
    V8MatchRec hist[V8_HIST];
    int nHist = 0, hHead = 0;
    int seen = 0, gen = 0;                         // matches seen (valid or not), brain generation (valid training matches)
    V8Agg good, cand;                              // known-good aggregate, evaluation aggregate since the last promotion
    bool haveGood = false;
    int sinceGood = 0;
    int verdict = GV_INSUFFICIENT;
    bool verdictPending = false;
    std::string reason;
    std::string lastReplay;                        // recording of the match that just ended (not persisted)
    int promotions = 0, rollbacks = 0;
    V8Mem() { clear(); }
    void clear();
};

struct V8Rel { bool on = false; int pat = -1, resp = 1; double t = 0; MetricVector2D from; float w = 1.f; float gd0 = 0.f; };
struct V8Exp { float f[V7_NIN] = {0}; int cls = 0, alt = -1, tov0 = 0, shots0 = 0; float margin = 0.f, r = 0.f; bool done = false; double t = 0; };

struct V8State {
    long frames = 0, liveFrames = 0, safeFrames = 0;
    double liveMs = 0, confSum = 0;
    // 1 threat
    float thr[TEAM_SIZE] = {0}, cfOpp[TEAM_SIZE] = {0}, cfOwn[TEAM_SIZE] = {0};
    int run[TEAM_SIZE] = {0};
    int top = -1;
    float topScore = 0.f;
    MetricVector2D laneFrom, laneTo, dest;
    float laneScore = 0.f;
    int laneRecv = -1;
    bool preempt = false;
    double preemptUntil = -1e9;
    int preemptCount = 0;
    // 2 shape
    int lastMan = -1, runnerIdx = -1;
    bool lmCover = true, beatable = false, holdOk = false;
    MetricVector2D beatPt, holdPt;
    float holdDepth = 0.f, lineScale = 1.f;
    // 3 assignment
    int jobHolder[JOB_N] = {-1, -1, -1, -1};
    int jobOf[TEAM_SIZE];
    bool jobOn[JOB_N] = {false, false, false, false};
    double jobSince[JOB_N] = {-1e9, -1e9, -1e9, -1e9};
    float jobThreat[JOB_N] = {0, 0, 0, 0};
    MetricVector2D jobPt[JOB_N];
    double assignAt = -1e9, switchLock = -1e9;
    int assignCount = 0, overrideCount = 0, prevOppCarrier = -1, prevRunner = -1;
    // 4 press
    float recProb = 0.f;
    int coverCount = 0;
    bool pressOk = true;
    double pressStart = -1e9, pressFailAt = -1e9;
    MetricVector2D lossBall;
    int pressDecisions = 0, retreats = 0;
    // 5 plan validity
    int planCancels = 0;
    // 7 tactical state machine
    int st = TS_CONTROL, prevSt = TS_CONTROL;
    double stSince = 0, stHold = -1e9, gainAt = -1e9, lossAt = -1e9, koUntil = -1e9;
    float riskMul = 1.f, minAdd = 0.f, shotMul = 1.f, lineAdd = 0.f;
    bool pressAllowed = false, emergency = false, koSeen = false;
    int trans[TS_N][TS_N];
    double dwell[TS_N];
    double koSeenAt = -1e9;
    // 8 opponent model (this match)
    float pat[V8_PAT] = {0, 0, 0, 0, 0}, patOk[V8_PAT] = {0, 0, 0, 0, 0}, patW[V8_PAT] = {0, 0, 0, 0, 0};
    V8Rel rel[V8_REL];
    double lastPatAt[V8_PAT] = {-1e9, -1e9, -1e9, -1e9, -1e9};
    float routeCnt[3] = {0, 0, 0};
    double lineN = 0, lineMean = 0, lineM2 = 0;
    double oppGainAt = -1e9;
    float throughDelaySum = 0.f; int throughDelayN = 0;
    float respSum[V8_PAT][V8_RESP] = {{0}}, respW[V8_PAT][V8_RESP] = {{0}};
    int fsmPrev = 0, hudMismatch = 0;
    float prevBallSp = 0.f;
    int lineMode = 1;                              // 0 drop, 1 hold, 2 step (our response mode in force)
    float oppLineBias = 0.f, oppPressMul = 1.f;
    V8Exp exp[V8_EXP];
    int nExp = 0, expHead = 0, expPend = -1, regret = 0;
    double expAt = -1e9;
    // 9 shot guard
    int sk = SK_NONE, resp = SR_NONE;
    MetricVector2D blockPt, shotFrom, shotTo;
    float tReact = 9.f, laneCover = 0.f, shotSpeed = 0.f;
    double skSince = -1e9, lastShotAgainst = -1e9, lastDanger = -1e9;
    int shotsAgainst = 0, dangerAgainst = 0, shotsFor = 0, turnovers = 0;
    MetricVector2D prevBall, prevOppCarPos;
    double prevOppCarAt = -1e9;
    bool prevBallOk = false;
    // 10 latency
    V8LatKind lat[IK_N];
    int pendKind = -1;
    double pendAt = -1e9;
    int pendType = -1;
    // 11 guard
    float trackConf = 1.f, visHome = 1.f, visAway = 1.f, ballSeenEma = 1.f, jumpEma = 0.f, overlapEma = 0.f;
    bool safe = false, dirSuspect = false, ownConfirmed = true, scaleBad = false;
    int dirBad = 0, scaleBadRun = 0, cmdDrops = 0, cmdConflicts = 0, overlapPairs = 0, implausible = 0;
    double safeSince = -1e9, lastBallSeenAt = -1e9;
    // 12 bench / recorder
    bool evalMatch = false;
    std::string rec;
    long recFrames = 0;
    bool recTrunc = false, recHeader = false;
    int hudNow = 0;
    bool ctrlSeen = false;
    MetricVector2D ctrlPos;
    V8State() {
        for (int i = 0; i < TEAM_SIZE; i++) jobOf[i] = -1;
        for (int a = 0; a < TS_N; a++) { dwell[a] = 0; for (int b = 0; b < TS_N; b++) trans[a][b] = 0; }
    }
    // a new match clears everything except the latency statistics (a property of the phone, not of the match)
    void clear() { std::string keep; keep.swap(rec); V8LatKind keepLat[IK_N]; for (int k = 0; k < IK_N; k++) keepLat[k] = lat[k]; *this = V8State(); keep.clear(); rec.swap(keep); for (int k = 0; k < IK_N; k++) lat[k] = keepLat[k]; }
};

class PitchEngine {
public:
    PitchEngine();

    void setConfig(const TacticalConfig& c);
    const TacticalConfig& config() const { return cfg_; }
    bool setTeamColors(int homeRGB, int awayRGB);
    void setBallColor(int ballRGB);
    void clearBallColor();
    static HsvRange defaultBallRange();
    bool ballIsRing() const { return ballRing_; }
    bool yellowKitInPlay() const { return yellowKit_; }
    void setColors(int teamRGB, int oppRGB, int ballRGB);
    static HsvRange hsvRangeFromRgb(int rgb, int dH = HSV_TOL_H, int dS = HSV_TOL_S, int dV = HSV_TOL_V);
    void kitBounds(int* out3x11) const;
    bool setKitBounds(const int* in3x11);
    bool haveColors() const { return haveColors_; }
    bool setHomographyQuad(const float* quad8);
    void clearHomographyQuad();
    void setBufferLayout(int rowStride, int pixelStride, bool bgr = false);
    void setFrameTimeMs(double t) { frameTimeMs_ = t; }
    void setGrid(const float* rowMajor96) { grid_.setValues(rowMajor96); }
    void reset();

    int processFrame(unsigned char* buffer, int width, int height);

    int ingestDetections(const MetricVector2D* home, int nHome, const MetricVector2D* away, int nAway,
                         const MetricVector2D* ball, bool ballSeen);

    void extrapolateKinematics(float deltaTimeSec);
    float computeExpectedThreat(const PlayerState& player, const MetricVector2D& target) const;
    int calculatePassProbabilities(const MatchState& state);
    GestureOutput generateGestureVector(const PlayerState& targetPlayer, float passPower);

    int consumeShotCue();
    bool isKeeperIdx(int j) const;
    float urgencyLevel() const;
    // Stunning-shot geometry: aim corner (away from the keeper), power ratio, touch time
    bool stunBand(float dg) const { return cfg_.stunEnabled && dg >= cfg_.stunMin && dg <= cfg_.stunMax; }
    float stunAimY(const MetricVector2D& from) const;
    bool stunConditions(const MetricVector2D& from, float* aimYOut) const;
    float stunPowerRatio(float d) const;
    int shotWindowLevel(const MetricVector2D& from, float* clearXp = nullptr, float* xg = nullptr,
                        float* aimY = nullptr) const;
    bool holding() const { return holding_; }
    float possessionShare() const { return possShare_; }
    // honest per-match numbers for the host's graph: out[0..7] = passes kept, passes lost, turnovers, shots fired live,
    // estimated goals for, estimated goals against, our share of the ball, seconds of live play
    void matchStats(float* out) const {
        out[0] = (float)statPassKept_; out[1] = (float)statPassLost_; out[2] = (float)statTurnovers_; out[3] = (float)shotsLive_;
        out[4] = (float)plus_.goalsFor; out[5] = (float)plus_.goalsAgainst; out[6] = possShare_; out[7] = (float)(plus_.liveMs / 1000.0);
    }
    void setClutchMode(int m) { clutch_ = m < 0 ? 0 : (m > 2 ? 2 : m); }
    int clutchMode() const { return clutch_; }
    void setHumanControl(bool on);
    // 0 = unknown, 1 = we attack (HUD shows Pass/Through/Shoot), 2 = we defend (HUD shows Switch/Match-up/Tackle)
    void setHudPossession(int v) { hudPoss_ = v < 0 || v > 2 ? 0 : v; hudPossMs_ = frameTimeMs_; }
    bool controlledSeen() const { return ctrlSeen_ && frameTimeMs_ - ctrlSeenMs_ < 700.0; }
    MetricVector2D controlledPos() const { return ctrlPos_; }

    void takeTrackerStats(double* out5);
    bool humanControl() const { return human_; }

    static constexpr int ATT_NF = 6;
    std::string exportAttackLog() const;
    std::string learnAttack(const std::string& logs);
    std::string exportAttackWeights() const;
    bool importAttackWeights(const std::string& s);
    void resetAttackWeights();
    void clearAttackLog();
    void attackStats(int& attempts, int& kept) const { attempts = attAtt_; kept = attOk_; }
    bool attackLearned() const { return attLearned_; }
    std::string attackCalibration(const std::string& logs) const;
    void setShootConfig(bool autoShoot, int holdMs, bool live);
    void shotStats(int& live, int& dry) const { live = shotsLive_; dry = shotsDry_; }
    float shotXgOffset() const { return shotXgOff_; }
    bool shotPending() const { return shotPending_; }
    struct Strategy { float dKeep = 0.f, dTempo = 0.f, fwd = 1.f, tempMul = 1.f; };
    const Strategy& strategy() const { return strat_; }
    Strategy effectiveStrategy() const { return effStrat(); }
    static constexpr int N_STYLES = 4;
    const float* styleWeights() const { return opp_.w; }
    float noiseLevel() const { return opp_.noise; }
    bool opponentAdapting() const { return opp_.adapting; }
    void noteOutcome(bool kept);
    void setAdaptive(bool ballCoast, bool occHold, bool context, bool styles, bool antiAdapt);
    int strategySamples() const { return (int)sh_.size(); }
    const float* attackWeights() const { return attW_; }

    std::string exportShotLog() const { return shotLogText_; }
    void clearShotLog() { shotLogText_.clear(); nBlkPress_ = nBlkLos_ = nBlkCool_ = 0; }
    void shotBlockCounts(int& pressure, int& los, int& cooldown) const { pressure = nBlkPress_; los = nBlkLos_; cooldown = nBlkCool_; }
    float localPressure(const MetricVector2D& p) const;

    // ---- Upgrade pack API ----
    int roleOf(int teamIdx) const { return teamIdx >= 0 && teamIdx < TEAM_SIZE ? role_[teamIdx] : (int)ROLE_UNK; }
    static const char* roleName(int r);
    float oppLineDepth() const { return oppLineDepth_; }     // metres from their goal line (EMA)
    float oppLineVel() const { return oppLineVel_; }         // m/s, positive = the line is stepping up
    float oppLineWidth() const { return oppLineWidth_; }
    float oppLineConf() const { return oppLineConf_; }       // 0..1
    bool predictBall(float tSec, MetricVector2D* pos, MetricVector2D* vel, bool* leavesPitch) const;
    int counterPressCount() const { return cpCount_; }
    int recoveryCount() const { return recoveries_; }
    float onlinePassAdj() const { return onlineAdj_; }
    std::string healthLog() const { return healthLog_; }
    std::string matchSummary() const;
    void clearMatchStats();
    void softResetTracking();

    const GestureOutput& lastGesture() const { return gesture_; }
    // host supplies the played fraction of the match (0..1); a negative value falls back to the engine's own live clock
    void setMatchPhase(float frac) { plus_.phase = frac; }
    // goals read from the radar at stoppages (estimates: a goal is counted when the ball was last seen in the goal mouth)
    void plusScore(int* gf, int* ga) const { if (gf) *gf = plus_.goalsFor; if (ga) *ga = plus_.goalsAgainst; }
    // 0 neutral, 1 early press, 2 protect the lead, 3 chase the game
    int plusMode() const { return plusModeNow(); }
    const MatchState& state() const { return state_; }
    const PassCandidate* candidates() const { return cand_; }
    int candidateCount() const { return nCand_; }
    int chosenCandidate() const { return chosen_; }
    const xTGrid& grid() const { return grid_; }
    float temperature() const { return state_.temperature; }
    float targetTemperature(const MatchState& s) const;
    float lastFrameMicros() const { return lastMicros_; }
    int debugDots(float* out, int maxFloats) const;

    // ---- API ----
    void setPlayStyle(int style, double lockMs = 60000.0);       // 0 Control, 1 Vertical, 2 Chaos; blended over about 25 s
    int playStyle() const { return v7_.styleCur; }
    const float* playStyleWeights() const { return v7_.sw; }
    const float* oppIntent() const { return v7_.oi; }             // build, progress, direct, switch, protect
    bool survivalActive() const { return v7_.surv; }
    bool tempoLocked() const { return state_.timeMs < v7_.tlUntil; }
    bool kickoffLockActive() const { return state_.timeMs < v7_.koUntil; }
    int plannedCount() const { return state_.timeMs < v7_.planUntil ? v7_.nPlan : 0; }
    float staminaOf(int teamIdx) const;
    float staminaAvg() const { return v7_.stamAvg; }
    bool predictBallLanding(MetricVector2D* land, float* tLand) const;
    void setMeasuredLatencyMs(float ms, int samples) { v7LatMs_ = ms; v7LatN_ = samples; }
    float effLatencyMs() const;
    std::string v7Export() const;                                 // long-term memory as text (nets, replay, embeddings, bandits)
    bool v7Import(const std::string& s);
    std::string v7EndMatch();                                     // anomaly filter + end-of-match training; clears the per-match state
    void v7ResetMemory() { v7m_.clear(); }
    const V7Mem& v7Memory() const { return v7m_; }
    const V7State& v7State() const { return v7_; }

    // ---- API ----
    int tacticalState() const { return v8_.st; }
    static const char* tacticalName(int s);
    float trackConfidence() const { return v8_.trackConf; }
    bool safeMode() const { return v8_.safe; }
    bool preemptActive() const { return v8_.preempt; }
    float threatOf(int oppIdx) const { return oppIdx >= 0 && oppIdx < TEAM_SIZE ? v8_.thr[oppIdx] : 0.f; }
    int runKindOf(int oppIdx) const { return oppIdx >= 0 && oppIdx < TEAM_SIZE ? v8_.run[oppIdx] : (int)RK_NONE; }
    int shotThreat() const { return v8_.sk; }
    int shotResponse() const { return v8_.resp; }
    bool lastManCovered() const { return v8_.lmCover; }
    int jobOfPlayer(int teamIdx) const { return teamIdx >= 0 && teamIdx < TEAM_SIZE ? v8_.jobOf[teamIdx] : -1; }
    float recoveryProbability() const { return v8_.recProb; }
    bool pressPermitted() const { return v8_.pressOk; }
    void noteInputLatency(int kind, float queueMs, float overheadMs, int flags);   // flags: 1 late, 2 missed / dropped, 4 timeout
    std::string inputLatencyReport() const;
    float latencyLeadMs(int kind) const;
    bool gkIntent(float* dirY, bool* rush) const;                                 // false unless the host confirmed a reliable keeper mapping
    void setGkControlReliable(bool on) { cfg_.v8GkControl = on; }
    void setRecording(bool on) { cfg_.v8Record = on; }
    std::string exportReplay() const;
    void setControlledForReplay(bool seen, const MetricVector2D& p) { ctrlSeen_ = seen; ctrlPos_ = p; ctrlSeenMs_ = seen ? frameTimeMs_ : -1e9; }
    std::string v8Export() const;
    bool v8Import(const std::string& s);
    int gateVerdict() const { return v8m_.verdict; }
    bool gatePending() const { return v8m_.verdictPending; }
    std::string gateReason() const { return v8m_.reason; }
    void gateAck() { v8m_.verdictPending = false; }
    int dashSeries(float* out, int maxFloats) const;                              // rows of V8_DASH_STRIDE floats, oldest first
    int liveSeries(float* out, int maxFloats) const;
    bool evalMatchNow() const { return v8_.evalMatch; }
    const V8Mem& v8Memory() const { return v8m_; }
    const V8State& v8State() const { return v8_; }
    const V8LatKind& latencyKind(int k) const { return v8_.lat[k < 0 ? 0 : (k >= IK_N ? IK_N - 1 : k)]; }
    void v8ResetMemory() { v8m_.clear(); }

private:
    // ---- planning layer (pitch_plan.inc) ----
    PlanState plan_;
    // ---- layer (pitch_v617.inc) ----
    struct V7Roll;
    friend struct V7Roll;
    V7State v7_;
    V7Mem v7m_;
    float v7LatMs_ = 0.f;
    int v7LatN_ = 0;
    // ---- layer (pitch_v618.inc) ----
    V8State v8_;
    V8Mem v8m_;
    void v8Frame(double dtSec);
    void v8GuardUpdate(double dtSec);
    void v8ThreatUpdate();
    void v8ShapeUpdate();
    void v8AssignUpdate();
    void v8PressUpdate();
    void v8FsmUpdate();
    void v8OppObserve();
    void v8ShotUpdate();
    void v8PlanValidate();
    void v8NoteDecision();
    void v8Apply(const PlayerState& car);
    void v8EndMatch(std::string& rep, bool valid);
    void v8BeginMatch();
    bool v8Suspect(std::string* why) const;
    void v8Gate(std::string& rep);
    void v8UpdateProfile(const V8MatchRec& rec);
    void v8Record(const MetricVector2D* home, int nHome, const MetricVector2D* away, int nAway, const MetricVector2D* ball, bool ballSeen);
    void v8RecordEvent(const char* what, float value);
    GestureOutput v8Sanitize(const GestureOutput& g);
    bool v8DefHook(GestureOutput* out, double now, bool noPress, float dC);
    bool v8DefKickoff(GestureOutput* out, double now);
    bool v8DefShot(GestureOutput* out, double now, bool noPress, float dC);
    bool v8DefSafe(GestureOutput* out, double now, bool noPress);
    bool v8DefRetreat(GestureOutput* out, double now, bool noPress);
    bool v8HoldSwitch(int nearIdx) const;
    bool v8PressGate() const;
    float v8LineScale() const;
    MetricVector2D v8Deconflict(const MetricVector2D& p, int self) const;
    float v8LaneOpen(const MetricVector2D& from, const MetricVector2D& to) const;
    float v8Sigmoid(float x) const { return 1.f / (1.f + std::exp(-x)); }
    float v8LatencyComp(int kind) const;
    int v8ProfileSlot(bool create);
    const V8OppProfile* v8Profile() const;
    void v7Frame(double dtSec);
    void v7Apply(const PlayerState& car);
    float v7Phase() const;
    void v7StateFeat(const PlayerState& car, float* f) const;
    static int v7ClassOf(const PassCandidate& c);
    float v7EmbedVal(int d) const;
    void v7LoadEmbed();
    void v7NoteAction(const PassCandidate& c, const GestureOutput& g, const PlayerState& car);
    void v7PassResult(bool kept);
    void v7ResolveTup(float r);
    void v7SeqEnd(int outcome);
    void v7SeqFeat(const PlayerState& car, const PassCandidate* c, float* f) const;
    void v7Rollout(const PlayerState& car);
    float v7RollVal(int type, int targetId) const;
    void v7IntentUpdate();
    void v7MorphUpdate();
    void v7HistoryUpdate();
    void v7DualUpdate();
    void v7BallHistory();
    void v7StaminaUpdate(double dtSec);
    void v7RestartUpdate();
    void v7RiskUpdate();
    void v7StyleUpdate(double dtSec);
    void v7MetaWindow();
    void v7TempoUpdate();
    void v7SurvivalUpdate();
    void v7RecurrentUpdate(double dtSec);
    void v7DefenceOutcome();
    bool v7SurvivalPick(int* pick, const PlayerState& car);
    bool v7PreAlignStep(const PassCandidate& c, const PlayerState& car, bool forced, GestureOutput* out);
    bool v7TimingHold(const PassCandidate& c, const PlayerState& car, bool forced);
    void v7TimingLead(PassCandidate& c) const;
    float v7TempScale() const;
    bool v7TempoLockedNow() const { return cfg_.v7 && cfg_.v7TempoLock && state_.timeMs < v7_.tlUntil; }
    bool v7DefEarly(GestureOutput* out, double now);
    bool v7DefMid(GestureOutput* out, double now, bool noPress, float dC);
    bool v7GkShadowPoint(MetricVector2D* pt);
    void v7DefModeChoose(float dC);
    float v7DefBufMul() const;
    GestureOutput v7PressTo(const MetricVector2D& to, int ms, bool dash, float r);
    bool v7DefLanding(GestureOutput* out, double now, bool noPress);
    MetricVector2D v7PredCarrierNear(const MetricVector2D& B, int* idx) const;
    MetricVector2D v7PredCarrier(int oppIdx) const;
    float v7RewardModel(const float* f, int cls, bool* ok) const;
    void v7TrainPolicy(const std::vector<V7Tup>& cur, float divLambda, std::string& rep);
    void v7League(std::string& rep);
    float v7QualityScore(std::string& why) const;
    float planPressure(const MetricVector2D& p) const;
    void planFrame(double dtSec);
    void planPrepare(const PlayerState& car);
    void planApply(const PlayerState& car);
    float planLaneCut(const PlayerState& car, const PassCandidate& c) const;
    bool planForcePick(int* pick, const PlayerState& car);
    bool planSidestep(int pick, const PlayerState& car, bool forced, GestureOutput* out);
    void planNoteAction(const PassCandidate& c, const PlayerState& car);
    void planPassResult(bool kept);
    void planOnTurnover();
    void planOnWin();
    void planOnShot(const PlayerState& car);
    bool planBallHypothesis(double dtSec);
    bool planCounterPressOk() const;
    float planKeeperAim(float aim) const;
    float planClearSide(const PlayerState& car, float side) const;
    bool planShadowPoint(MetricVector2D* pt);
    int planCadenceMs(int base) const;
    double ballHoldMs() const;
    // ---- meta layer (pitch_meta.inc) ----
    MetaState meta_;
    GestureOutput defGesture_;
    int trackJumps_ = 0;
    float defReactEff() const;
    float metaReactP80() const;
    float metaSysScale(int sys) const;
    int metaModeNow() const;
    void metaFrame(double dtSec);
    void metaEdge(int prev, int cur);
    void metaTurnover();
    void metaPassResult(bool kept);
    void metaPrepare(const PlayerState& car);
    void metaApply(const PlayerState& car);
    void metaDecorrelate(int pick, const PlayerState& car);
    bool metaPreCommit(int* pick, const PlayerState& car, bool forced, GestureOutput* out);
    void metaNoteAction(const PassCandidate& c, const GestureOutput& g, const PlayerState& car);
    float metaMicroAngle(float pressure) const;
    float metaContainBuf() const;
    float metaShapeScale() const;
    float metaSideBias() const;
    bool metaSecondMan(MetricVector2D* pt);
    bool metaBaitPoint(MetricVector2D* pt);
    bool metaRecoveryPoint(MetricVector2D* pt);
    int metaZone(const MetricVector2D& p) const;
    void metaIntentUpdate(double now);
    // ---- match intelligence pack (pitch_plus.inc) ----
    PlusState plus_;
    int plusModeNow() const;
    int effClutch() const;
    float plusLeadEst() const;
    void plusFrame(double dtSec);
    void plusStoppage();
    int epvCell(int role, float ax, float y, int dens, int vbin) const;
    float epvValue(int role, float ax, float y, int dens, int vbin) const;
    int epvRoleBin(int teamIdx) const;
    int oppDensityAt(const MetricVector2D& p, float r) const;
    void epvInit();
    void plusPrepare(const PlayerState& car);
    void plusApply(const PlayerState& car);
    float plusIntentBonus(const PlayerState& car, const PassCandidate& c) const;
    void plusIntentUpdate(double dtSec);
    void plusStyleUpdate(double dtSec);
    MetricVector2D plusFieldDir(const MetricVector2D& from) const;
    void plusNoteAction(const PassCandidate& c, const PlayerState& car);
    void plusEndSequence(int outcome, const MetricVector2D& at);
    void plusMineSequences();
    float plusMotifBonus(const PlayerState& car, const PassCandidate& c) const;
    float plusSeqValue(const PlayerState& car, float* q, int n) const;
    void plusTreeScores(const PlayerState& car, float* q, bool* have) const;
    bool plusRaceAssign(int* bestIdx, MetricVector2D* where, float* lead, float* ctrlTta) const;
    bool plusCoverPoint(MetricVector2D* pt, bool* needsLine);
    GestureOutput plusReboundGesture();
    float plusTta(const PlayerState& p, const MetricVector2D& to, float vmax) const;
    struct Det { float x = 0, y = 0, area = 0, score = 0, ax = 1.f, ay = 0.f, len = 0.f, elong = 9.f; };

    void segment(const unsigned char* buf, int w, int h);
    void detectBall(int w, int h);
#ifdef PITCHBOT_WITH_OPENCV
    void detectBallCv(int w, int h);
#endif
    void updateKitOverlap();
    float windowWorstScore(const HsvRange& r) const;
    void rescueHiddenDots(const unsigned char* buf, int w, int h);
    void rejectNonDots(const unsigned char* buf, int w, int h);
    bool ballUsable() const;
    void rebuildHomography(int w, int h);
    void emitDetections(Det* d, int n, MetricVector2D* out, int& nOut);

    void kalmanUpdate(PlayerState& p, const MetricVector2D& z, float dt);
    void kalmanCoast(PlayerState& p, float dt);
    void trackTeam(PlayerState* tr, const MetricVector2D* det, int nDet, double dt, int idBase, int team);
    void updateAttackSign();
    void updatePossession(double dtSec);

    float lanePressure(const MetricVector2D& p) const;
    float nearestOppDistance(const MetricVector2D& p) const;
    float offsideAmount(const MetricVector2D& receiverFuture) const;
    float interceptMargin(const MetricVector2D& from, const MetricVector2D& to) const;
    bool inOwnBox(const MetricVector2D& p) const;
    float passProbability(const MetricVector2D& from, const MetricVector2D& to, float& clearance, bool through,
                          bool lofted) const;
    void softmaxAll(float temperature);
    int sampleCandidate();
    uint32_t nextRand();
    float rand01();
    GestureOutput makeGesture(int type, const MetricVector2D& from, const MetricVector2D& to, float power,
                              bool dash) const;
    GestureOutput decide();
    GestureOutput decideDefence();
    void detectControlled(const unsigned char* buf, int w, int h);
    void updateShotCue(const PlayerState& car);
    bool tryPanicClear(const PlayerState& car, GestureOutput& out);
    void filterForwardOnly(const PlayerState& car);
    void addWingSwitch(const PlayerState& car);
    bool preAlignedCarry(const PlayerState& car, int bestIdx, GestureOutput& g);
    void addLoftedThrough(const PlayerState& car);
    bool anticipatedPass(GestureOutput& out);
    void updateOppPass(int oppIdx);
    int shotBlockReason(const PlayerState& car, int lvl) const;
    void logShotBlock(int reason, const PlayerState& car);
    bool panicNeeded(const PlayerState& car) const;
    int controlledIdx() const;
    float goalDistance(const MetricVector2D& p) const;
    int effDecisionMs() const;
    int effCooldownMs() const;
    void updateCrowd();
    float opennessOf(const MetricVector2D& p) const;
    float receiverReliability(const PlayerState& r) const;
    void applyKeepBall(const PlayerState& car);
    MetricVector2D runnerLead(const PlayerState& r, float passLen) const;
    float nearestOppExcluding(const MetricVector2D& p, int skip) const;
    float zoneDensityAt(const MetricVector2D& p) const;
    void updatePlaymaker();
    void applyPlaymaker(const PlayerState& car);
    void updateOppShape();
    void applyContext(const PlayerState& car);
    void updateStyle(double dtSec);
    void coastBall(double dtSec);
    bool overlapNear(const PlayerState* self, const MetricVector2D& p) const;
    Strategy effStrat() const;
    int jitteredDecisionMs();

    // goals-pack helpers (five conversion / chance-creation functions)
    float conversionShotBonus(const PlayerState& car, const PassCandidate& c) const;
    float boxOccupationBonus(const PlayerState& car, const PassCandidate& c) const;
    float flankCrossBonus(const PlayerState& car, const PassCandidate& c) const;
    float chipGkBonus(const PlayerState& car, const PassCandidate& c) const;
    float boxEntryBonus(const PlayerState& car, const PassCandidate& c) const;
    float counterBreakBonus(const PlayerState& car, const PassCandidate& c) const;
    float receiverContestPenalty(const PassCandidate& c) const;
    float turnoverCostPenalty(const PlayerState& car, const PassCandidate& c) const;
    // scoring-machine helpers
    float clinicalShotPenalty(const PlayerState& car, const PassCandidate& c) const;
    float overloadBonus(const PlayerState& car, const PassCandidate& c) const;
    float supportSeekCarryBonus(const PlayerState& car, const PassCandidate& c) const;
    float postLossAdjust(const PlayerState& car, const PassCandidate& c) const;
    int countZoneNet(float depthMax, float halfY) const;  // attackers - defenders in zone (attack-signed)
    void armPostLoss();
    void armSecondBall(const MetricVector2D& from);
    float scoreClockUrgency() const;
    bool finalThirdMustProgress(const PlayerState& car, double now) const;
    int pickBestProgressive(int n, const PlayerState& car) const;
    float oppGkDepth() const;

    // upgrade pack
    void updateUpgrades(double dtSec);
    void updateRoles();
    void updateOppLine(double dtSec);
    void monitorHealth(double nowMs, double dtSec);
    float upgradeBonus(const PlayerState& car, const PassCandidate& c) const;
    float adaptiveAttackBonus(const PlayerState& car, const PassCandidate& c) const;
    float coordinatedRiskAdjustment(const PlayerState& car, const PassCandidate& c) const;
    void onlineLearn(bool kept);
    GestureOutput decideLoose();
    bool counterPressActive() const;

    struct OppModel {
        float noise = 0.f, keptFast = 0.7f, keptSlow = 0.7f, press = 0.f;
        int events = 0;
        bool adapting = false;
        float w[4] = {0.f, 0.f, 0.f, 0.f};
    };
    OppModel opp_;

    float possShare_ = 0.5f;
    float possOwnedSec_ = 0.f;
    int commitIdx_[TEAM_SIZE] = {0};
    int nCommit_ = 0;
    int clutch_ = 0;

    TacticalConfig cfg_;
    xTGrid grid_;
    Homography hom_;
    bool customQuad_ = false;
    float quad_[8] = {0};
    int homW_ = 0, homH_ = 0;

    HsvRange spec_[3];
    bool ballCustom_ = false;
    bool yellowKit_ = false;
    bool ballRing_ = false;
    bool ballGlow_ = false;
    bool ringSeen_ = false;
    float ballScore_ = 0.f;
    bool haveColors_ = false;

    int rowStride_ = 0, pixelStride_ = 4;
    bool bgr_ = false;
    double frameTimeMs_ = 0, prevFrameMs_ = 0;
    float lastMicros_ = 0;

    std::vector<uint16_t> stamp_;
    uint16_t stampId_ = 0;
    std::vector<uint8_t> tint_, ymu_;
    bool ballAssumed_ = false;
    double signLockMs_ = -1e9;
    std::vector<uint8_t> cls_, ero_, ym_, ym2_;
    std::vector<int32_t> stack_;
    Det dets_[3][MAX_DET];
    int nDets_[3] = {0, 0, 0};
    MetricVector2D mHome_[MAX_DET], mAway_[MAX_DET];
    int nMHome_ = 0, nMAway_ = 0;
    MetricVector2D mBall_;
    bool mBallSeen_ = false;
    float hCost_[HUNGARIAN_MAX * HUNGARIAN_MAX];
    int hAssign_[HUNGARIAN_MAX];
    int detToTrack_[MAX_DET];

    MatchState state_;
    bool signInit_ = false;
    double notLiveSinceMs_ = -1;
    double restartUntilMs_ = -1e9;
    double turnStartMs_ = -1;
    int turnTargetId_ = -1;
    double dirEma_ = 0.0;
    int signCand_ = 1, signRun_ = 0;
    int owner_ = 0, candOwner_ = 0, candRun_ = 0;
    double ownerSinceMs_ = 0, lastBallSeenMs_ = -1e9, counterUntilMs_ = -1e9;
    bool ballWasKnown_ = false;
    int lastOwnerForCounter_ = 0;
    double nextDecisionAt_ = 0, cooldownUntil_ = 0;
    int liveFrames_ = 0, notLiveFrames_ = 0;

    struct Pending { bool on = false; double due = 0, t0 = 0; MetricVector2D pos0, vel0; };
    Pending pend_[TEAM_SIZE];
    double calAD_ = 0, calDD_ = 0, calN_ = 0, calR_ = 0, calFrames_ = 0;
    void updateCalibration(double nowMs);

    PassCandidate cand_[MAX_CAND];
    int nCand_ = 0;
    int chosen_ = -1;
    GestureOutput gesture_;
    uint32_t rng_ = 1;

    int pendingCue_ = 0;
    bool cueArmed_ = true;
    int cueRun_ = 0;
    int cueLevelNow_ = 0;
    double lastCueMs_ = -1e9, cueHoldUntil_ = 0, lostSinceMs_ = -1;
    float cueAimY_ = 0.f;
    bool holding_ = false;
    bool human_ = false;
    int hudPoss_ = 0;
    double hudPossMs_ = -1e9;
    bool ctrlSeen_ = false;
    MetricVector2D ctrlPos_;
    double ctrlSeenMs_ = -1e9;
    double defNextAt_ = 0, defSwitchAt_ = -1e9, defTackleAt_ = -1e9, defMatchAt_ = -1e9, oppSinceMs_ = -1;
    std::vector<int32_t> ctrlStack_;
    std::vector<uint8_t> ctrlMask_;
    double ballHoldStartMs_ = -1;
    int holdCarrier_ = -1;
    double holdStartAt_ = -1, holdLastAt_ = -1, pressStartAt_ = -1;
    double crowdSinceMs_ = -1;

    struct SnapCand { int id = 0, type = 0; MetricVector2D target; float xpRaw = 0, pressure = 0, len = 0, fwd = 0; };
    struct AttRec { double t = 0; int type = 0; float xp = 0, pressure = 0, crowd = 0, len = 0, fwd = 0, ax = 0, ay = 0; int outcome = 0; };
    static constexpr int ATT_LOG_MAX = 3000;
    void attackSnapshot();
    void attackTrack(int prevOwner);
    void attackEvent(bool kept, int recvIdx);
    float attackProb(const float* f) const;
    static void attackFeatures(float xpRaw, float pressure, float crowd, float len, float fwd, float* f);
    struct StratRec { float k = 0, t = 0, f = 1, r = 0; };
    std::string learnStrategy(const std::string& logs);
    std::string learnShooting(const std::string& logs);
    Strategy strat_;
    std::vector<StratRec> sh_;
    SnapCand snap_[MAX_CAND];
    int nSnap_ = 0;
    bool snapOk_ = false, snapShotWindow_ = false;
    int snapCarrier_ = -1;
    MetricVector2D snapFrom_;
    float snapCrowd_ = 0.f;
    double snapT_ = -1e9, flightT_ = -1e9;
    bool flight_ = false;
    int lastCarrierIdx_ = -1;
    bool shotLive_ = true;
    bool shotArmed_ = true;
    int shotRun_ = 0;
    bool shotPending_ = false;
    double shotPendingUntil_ = 0;
    float shotAimY_ = 0.f;
    int shotsLive_ = 0, shotsDry_ = 0;
    float shotXgOff_ = 0.f;
    bool shotLearned_ = false;
    std::vector<AttRec> attLog_;
    int attAtt_ = 0, attOk_ = 0;
    float attW_[ATT_NF] = {0.f, 1.f, 0.f, 0.f, 0.f, 0.f};
    bool attLearned_ = false;
    int attLearnedN_ = 0;

    // rescue rules state
    int switchIdx_ = -1;
    double lastActMs_ = -1e9;
    double defConeAt_ = -1e9, defStepAt_ = -1e9;
    MetricVector2D aimDir_;
    bool aimDirOn_ = false;
    double rollAtMs_ = -1e9;
    long frameNo_ = 0;
    int shotSeq_ = 0;               // unique id per fired shot (SHOT_FIRED EVT) so backups can dedupe
    int statPassOut_ = 0, statPassKept_ = 0, statPassLost_ = 0, statTurnovers_ = 0;
    float lastRayD_ = 99.f;
    int oppCarrier_ = -1, prevOppCarrier_ = -1;
    MetricVector2D oppCarrierPos_, prevOppCarrierPos_;
    double oppPassMs_ = -1e9;
    double lineStepUntil_ = -1e9;
    double shotFiredMs_ = -1e9;
    double secondBallUntil_ = -1e9;
    double postLossUntil_ = -1e9;   // window after a turnover for compact recovery
    double pressCloseSince_ = -1e9; // how long the carrier has been tightly pressed
    double ballWonMs_ = -1e9, lastOwnCallMs_ = -1e9;
    MetricVector2D secondBallAt_;
    double finalThirdSince_ = -1e9;
    int finalThirdCarrier_ = -1;
    double lastAntMs_ = -1e9;
    int antLastRecv_ = -1;
    bool antBusy_ = false;
    std::string shotLogText_;
    int nBlkPress_ = 0, nBlkLos_ = 0, nBlkCool_ = 0;
    int lastBlkReason_ = 0;
    double lastBlkMs_ = -1e9;
    bool wasInShotBand_ = false;

    // upgrade pack state
    int role_[TEAM_SIZE] = {0};
    float rankEma_[TEAM_SIZE] = {0};
    bool roleInit_ = false;
    float oppLineDepth_ = 26.f, oppLineVel_ = 0.f, oppLineWidth_ = 20.f, oppLineConf_ = 0.f;
    bool oppLineInit_ = false;
    double lostBallMs_ = -1e9;
    bool cpCounted_ = false;
    double cpSwitchAt_ = -1e9;
    double looseNextAt_ = 0;
    int cpCount_ = 0, recoveries_ = 0, chaseCount_ = 0;
    // health monitor
    bool haveCentroid_ = false;
    MetricVector2D rawPts_[TEAM_SIZE], prevRawPts_[TEAM_SIZE];
    int rawN_ = 0, prevRawN_ = 0;
    int jumpRun_ = 0;
    double jumpFirstMs_ = -1e9;
    MetricVector2D stuckBall_;
    double stuckSinceMs_ = -1;
    int stuckAction_ = -1;
    std::string healthLog_;
    // online pass-safety learning
    float onlineKept_ = 0.75f, onlineAdj_ = 0.f;
    int onlineN_ = 0;
    // match summary accumulators
    double sumTeamSec_ = 0, sumOppSec_ = 0, sumLooseSec_ = 0;
    double thirdSec_[3] = {0, 0, 0};       // our possession time with the ball in our / middle / final third
    double flankSec_[3] = {0, 0, 0};       // ... top side / centre / bottom side of the radar
    double lineDepthSum_ = 0, lineDepthW_ = 0;
    int turnoverCount_ = 0, wonBackCount_ = 0;
};

}
