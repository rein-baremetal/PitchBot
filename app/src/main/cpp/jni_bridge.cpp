#include <jni.h>

#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"
#include "pitch_engine.hpp"

namespace {
pb::Engine* g_engine = nullptr;
std::mutex g_mu;

pb::Engine& E() {
    if (!g_engine) g_engine = new pb::Engine();
    return *g_engine;
}

pe::PitchEngine* g_pe = nullptr;
std::mutex g_pe_mu;

pe::PitchEngine& PE() {
    if (!g_pe) g_pe = new pe::PitchEngine();
    return *g_pe;
}

// Latency measurement : capture-to-decision time of the last 32 frames plus the tap dispatch overhead reported by the
// accessibility service. The engine uses the moving average as its prediction lead instead of a fixed number.
double bootMs() {
    timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
float g_latRing[32] = {0.f};
int g_latHead = 0, g_latCount = 0;
float g_tapEma = 0.f;
int g_tapN = 0;

jfloatArray makeFloats(JNIEnv* env, const float* data, int n) {
    if (n < 0) n = 0;
    jfloatArray r = env->NewFloatArray((jsize)n);
    if (!r) return nullptr;
    if (n > 0 && data) env->SetFloatArrayRegion(r, 0, (jsize)n, data);
    return r;
}

jintArray makeInts(JNIEnv* env, const jint* data, int n) {
    if (n < 0) n = 0;
    jintArray r = env->NewIntArray((jsize)n);
    if (!r) return nullptr;
    if (n > 0 && data) env->SetIntArrayRegion(r, 0, (jsize)n, data);
    return r;
}

bool pixelsOk(JNIEnv* env, jintArray arr, jint w, jint h) {
    if (!arr || w <= 0 || h <= 0) return false;
    const jlong need = (jlong)w * (jlong)h;
    return (jlong)env->GetArrayLength(arr) >= need;
}

std::string toStd(JNIEnv* env, jstring s) {
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}
}

extern "C" {

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_nInit(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> l(g_mu);
    E();
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_nNewMatch(JNIEnv*, jobject) {
    {
        std::lock_guard<std::mutex> l(g_mu);
        E().newMatch();
    }
    std::lock_guard<std::mutex> l2(g_pe_mu);
    PE().reset();
    PE().clearMatchStats();
    PE().clearAttackLog();
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_nSetObserveOnly(JNIEnv*, jobject, jboolean v) {
    std::lock_guard<std::mutex> l(g_mu);
    E().setObserveOnly(v == JNI_TRUE);
}

JNIEXPORT jintArray JNICALL Java_com_pitchbot_app_Native_nScan(JNIEnv* env, jobject, jintArray pixels, jint w, jint h) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!pixelsOk(env, pixels, w, h)) {
        const jint none[1] = {0};
        return makeInts(env, none, 1);
    }
    jint* px = env->GetIntArrayElements(pixels, nullptr);
    if (!px) {
        const jint none[1] = {0};
        return makeInts(env, none, 1);
    }
    pb::PaletteEntry pal[8];
    int n = E().scanPalette(px, w, h, pal, 8);
    env->ReleaseIntArrayElements(pixels, px, JNI_ABORT);
    std::vector<jint> out(1 + n * 4);
    out[0] = n;
    for (int i = 0; i < n; i++) {
        out[1 + i * 4] = pal[i].r;
        out[2 + i * 4] = pal[i].g;
        out[3 + i * 4] = pal[i].b;
        out[4 + i * 4] = pal[i].blobs;
    }
    return makeInts(env, out.data(), (int)out.size());
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_nSetColors(JNIEnv*, jobject, jint team, jint opp, jint ball) {
    std::lock_guard<std::mutex> l(g_mu);
    E().setColors(team, opp, ball);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_nSetOptions(JNIEnv*, jobject, jint tactic, jfloat aggression,
                                                                 jfloat shootReach, jfloat passSpeed, jint attackDir,
                                                                 jint half, jboolean invX, jboolean invY) {
    std::lock_guard<std::mutex> l(g_mu);
    pb::Options o = E().options();
    o.tactic = tactic;
    o.aggression = aggression;
    o.shootReach = shootReach;
    o.passSpeed = passSpeed;
    o.attackDir = attackDir;
    o.half = half;
    o.invertX = invX;
    o.invertY = invY;
    E().setOptions(o);
}

JNIEXPORT jfloatArray JNICALL Java_com_pitchbot_app_Native_nProcess(JNIEnv* env, jobject, jintArray pixels, jint w,
                                                                    jint h, jlong tMs) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!pixelsOk(env, pixels, w, h)) {
        const float idle[12] = {0.f};
        return makeFloats(env, idle, 12);
    }
    jint* px = env->GetIntArrayElements(pixels, nullptr);
    if (!px) {
        const float idle[12] = {0.f};
        return makeFloats(env, idle, 12);
    }
    pb::Action a;
    int status = E().process(px, w, h, (double)tMs, a);
    env->ReleaseIntArrayElements(pixels, px, JNI_ABORT);
    float dbg[16] = {0.f};
    E().debugDots(dbg, 16);
    const float out[12] = {(float)status, a.sx,        a.sy,         (float)a.button, (float)a.holdMs,
                           a.dash ? 1.f : 0.f, (float)a.type, dbg[0], dbg[1], dbg[2],
                           (float)E().dirSign(), (float)E().half()};
    return makeFloats(env, out, 12);
}

JNIEXPORT jfloatArray JNICALL Java_com_pitchbot_app_Native_nDebugDots(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_mu);
    float buf[64] = {0.f};
    int n = E().debugDots(buf, 64);
    return makeFloats(env, buf, n);
}

JNIEXPORT jfloatArray JNICALL Java_com_pitchbot_app_Native_nStats(JNIEnv* env, jobject) {
    float s[8] = {0.f};
    {
        std::lock_guard<std::mutex> l(g_mu);
        E().stats(s);
    }
    {
        std::lock_guard<std::mutex> l2(g_pe_mu);
        int att = 0, ok = 0;
        PE().attackStats(att, ok);
        if (att > 0) {   // the engine that plays counts its own passes, so the detector's attempts are not added
            s[1] = (float)att;
            s[2] = (float)ok;
        }
        int shotsLive = 0, shotsDry = 0;
        PE().shotStats(shotsLive, shotsDry);
        s[3] += (float)shotsLive;
    }
    return makeFloats(env, s, 8);
}

JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_nExportLog(JNIEnv* env, jobject) {
    std::string log;
    {
        std::lock_guard<std::mutex> l(g_mu);
        log = E().exportLog();
    }
    {
        std::lock_guard<std::mutex> l2(g_pe_mu);
        log += PE().exportAttackLog();
    }
    return env->NewStringUTF(log.c_str());
}

JNIEXPORT jboolean JNICALL Java_com_pitchbot_app_Native_nSetWeights(JNIEnv* env, jobject, jstring s) {
    std::string str = toStd(env, s);
    {
        std::lock_guard<std::mutex> l2(g_pe_mu);
        PE().importAttackWeights(str);
    }
    std::lock_guard<std::mutex> l(g_mu);
    if (str.empty()) {
        E().resetWeights();
        return JNI_TRUE;
    }
    return E().importWeights(str) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_nExportWeights(JNIEnv* env, jobject) {
    std::string w;
    {
        std::lock_guard<std::mutex> l(g_mu);
        w = E().exportWeights();
    }
    {
        std::lock_guard<std::mutex> l2(g_pe_mu);
        w += PE().exportAttackWeights();
    }
    return env->NewStringUTF(w.c_str());
}

JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_nLearn(JNIEnv* env, jobject, jstring logs) {
    const std::string in = toStd(env, logs);
    std::string r;
    {
        std::lock_guard<std::mutex> l(g_mu);
        r = E().learn(in);
    }
    {
        std::lock_guard<std::mutex> l2(g_pe_mu);
        r += PE().learnAttack(in);
    }
    return env->NewStringUTF(r.c_str());
}

JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_attackCalibration(JNIEnv* env, jobject, jstring logs) {
    const std::string in = toStd(env, logs);
    std::string r;
    {
        std::lock_guard<std::mutex> l(g_pe_mu);
        r = PE().attackCalibration(in);
    }
    return env->NewStringUTF(r.c_str());
}

JNIEXPORT jfloatArray JNICALL Java_com_pitchbot_app_Native_nLossGrid(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_mu);
    float rate[pb::GX * pb::GY], vis[pb::GX * pb::GY];
    E().lossGrid(rate, vis);
    std::vector<float> out;
    for (float v : rate) out.push_back(v);
    for (float v : vis) out.push_back(v);
    return makeFloats(env, out.data(), (int)out.size());
}

JNIEXPORT jfloatArray JNICALL Java_com_pitchbot_app_Native_nProfile(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_mu);
    float p[6] = {0.f};
    E().profile(p);
    return makeFloats(env, p, 6);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_initPitchEngine(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE();
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_resetPitchEngine(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().reset();
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_configurePitchEngine(JNIEnv*, jobject, jfloat wPress, jfloat wOffside,
                                                                         jfloat tempCounter, jfloat tempHold, jint attackDir,
                                                                         jboolean invX, jboolean invY, jint decisionMs,
                                                                         jint cooldownMs, jint seed) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    pe::TacticalConfig c = PE().config();
    c.wPress = wPress;
    c.wOffside = wOffside;
    c.tempCounter = tempCounter;
    c.tempHold = tempHold;
    c.attackDir = attackDir;
    c.invertX = invX == JNI_TRUE;
    c.invertY = invY == JNI_TRUE;
    c.decisionIntervalMs = decisionMs;
    c.actionCooldownMs = cooldownMs;
    c.seed = (uint32_t)seed;
    PE().setConfig(c);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setPitchExtras(JNIEnv*, jobject, jboolean legendary, jboolean shotAlert,
                                                                   jboolean shotHold, jfloat shootReach) {
    {
        std::lock_guard<std::mutex> l(g_pe_mu);
        pe::TacticalConfig c = PE().config();
        c.legendary = legendary == JNI_TRUE;
        c.shotAlert = shotAlert == JNI_TRUE;
        c.shotHold = shotHold == JNI_TRUE;
        float r = shootReach * 2.f * pe::PITCH_HALF_X;
        if (!(r == r)) r = 30.f;
        c.shootRange = r < 26.f ? 26.f : (r > 34.f ? 34.f : r);   // accurate range: the box and just outside it
        PE().setConfig(c);
    }
    {
        std::lock_guard<std::mutex> l(g_mu);
        pb::Options o = E().options();
        o.legendary = legendary == JNI_TRUE;
        E().setOptions(o);
    }
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setStateEstimator(JNIEnv*, jobject, jboolean enabled, jfloat latencyMs,
                                                                      jfloat projGain, jfloat r0) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    pe::TacticalConfig c = PE().config();
    c.kalman = enabled == JNI_TRUE;
    float m = latencyMs;
    if (!(m == m)) m = 100.f;
    c.kalLatencyMs = m < 0.f ? 0.f : (m > 500.f ? 500.f : m);
    float g = projGain;
    if (!(g == g)) g = 1.f;
    c.kalProjGain = g < 0.3f ? 0.3f : (g > 1.3f ? 1.3f : g);
    float r = r0;
    if (!(r == r)) r = 0.6f;
    c.kalR = r < 0.25f ? 0.25f : (r > 2.f ? 2.f : r);
    PE().setConfig(c);
}

JNIEXPORT jdoubleArray JNICALL Java_com_pitchbot_app_Native_takeTrackerStats(JNIEnv* env, jobject) {
    double v[5];
    {
        std::lock_guard<std::mutex> l(g_pe_mu);
        PE().takeTrackerStats(v);
    }
    jdoubleArray out = env->NewDoubleArray(5);
    if (out) env->SetDoubleArrayRegion(out, 0, 5, v);
    return out;
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setKeepBall(JNIEnv*, jobject, jboolean keepBall, jfloat keepBias,
                                                                jboolean colorOnly, jboolean virtualKeeper) {
    {
        std::lock_guard<std::mutex> l(g_pe_mu);
        pe::TacticalConfig c = PE().config();
        c.keepBall = keepBall == JNI_TRUE;
        float b = keepBias;
        if (!(b == b)) b = 0.6f;
        c.keepBias = b < 0.f ? 0.f : (b > 1.f ? 1.f : b);
        c.colorOnly = colorOnly == JNI_TRUE;
        c.virtualKeeper = virtualKeeper == JNI_TRUE;
        PE().setConfig(c);
    }
    {
        std::lock_guard<std::mutex> l(g_mu);
        pb::Options o = E().options();
        o.colorOnly = colorOnly == JNI_TRUE;
        E().setOptions(o);
    }
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setAdaptive(JNIEnv*, jobject, jboolean ballCoast, jboolean occHold,
                                                                jboolean context, jboolean styles, jboolean antiAdapt) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().setAdaptive(ballCoast == JNI_TRUE, occHold == JNI_TRUE, context == JNI_TRUE, styles == JNI_TRUE,
                     antiAdapt == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setClutchMode(JNIEnv*, jobject, jint mode) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().setClutchMode((int)mode);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setMatchPhase(JNIEnv*, jobject, jfloat frac) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().setMatchPhase((float)frac);
}

JNIEXPORT jfloatArray JNICALL Java_com_pitchbot_app_Native_nMatchStats(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    float o[8];
    PE().matchStats(o);
    return makeFloats(env, o, 8);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setHudPossession(JNIEnv*, jobject, jint v) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().setHudPossession((int)v);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setHumanControl(JNIEnv*, jobject, jboolean on) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().setHumanControl(on == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setShootConfig(JNIEnv*, jobject, jboolean autoShoot, jint holdMs, jboolean live) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().setShootConfig(autoShoot == JNI_TRUE, (int)holdMs, live == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setRescueRules(JNIEnv*, jobject, jboolean on) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    pe::TacticalConfig c = PE().config();
    c.rescueRules = on == JNI_TRUE;
    PE().setConfig(c);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setUpgradePack(JNIEnv*, jobject, jboolean on) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    pe::TacticalConfig c = PE().config();
    c.upgrades = on == JNI_TRUE;
    PE().setConfig(c);
}

JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_matchSummary(JNIEnv* env, jobject) {
    std::string out;
    {
        std::lock_guard<std::mutex> l(g_pe_mu);
        out = PE().matchSummary();
    }
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_exportShotLog(JNIEnv* env, jobject) {
    std::string out;
    {
        std::lock_guard<std::mutex> l(g_pe_mu);
        out = PE().exportShotLog();
    }
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_clearShotLog(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().clearShotLog();
}

JNIEXPORT jint JNICALL Java_com_pitchbot_app_Native_consumeShotCue(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    return (jint)PE().consumeShotCue();
}

JNIEXPORT jboolean JNICALL Java_com_pitchbot_app_Native_setPitchQuad(JNIEnv* env, jobject, jfloatArray quad) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    if (!quad || env->GetArrayLength(quad) < 8) {
        PE().clearHomographyQuad();
        return JNI_TRUE;
    }
    float q[8];
    env->GetFloatArrayRegion(quad, 0, 8, q);
    return PE().setHomographyQuad(q) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_pitchbot_app_Native_setTeamColors(JNIEnv*, jobject, jint homeRgb, jint awayRgb) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    return PE().setTeamColors((int)homeRgb, (int)awayRgb) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_setBallColor(JNIEnv*, jobject, jint ballRgb) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().setBallColor((int)ballRgb);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_clearBallColor(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().clearBallColor();
}

JNIEXPORT jintArray JNICALL Java_com_pitchbot_app_Native_getKitBounds(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    jint out[3 * pe::HSV_PACK_INTS];
    int tmp[3 * pe::HSV_PACK_INTS];
    PE().kitBounds(tmp);
    for (int i = 0; i < 3 * pe::HSV_PACK_INTS; i++) out[i] = tmp[i];
    return makeInts(env, out, 3 * pe::HSV_PACK_INTS);
}

JNIEXPORT jboolean JNICALL Java_com_pitchbot_app_Native_setKitBounds(JNIEnv* env, jobject, jintArray bounds) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    if (!bounds || env->GetArrayLength(bounds) < 3 * pe::HSV_PACK_INTS) return JNI_FALSE;
    jint in[3 * pe::HSV_PACK_INTS];
    env->GetIntArrayRegion(bounds, 0, 3 * pe::HSV_PACK_INTS, in);
    int tmp[3 * pe::HSV_PACK_INTS];
    for (int i = 0; i < 3 * pe::HSV_PACK_INTS; i++) tmp[i] = (int)in[i];
    return PE().setKitBounds(tmp) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_pitchbot_app_Native_processPitchFrame(JNIEnv* env, jobject, jobject buf, jint rowStride,
                                                                       jint pixelStride, jint roiL, jint roiT, jint roiW,
                                                                       jint roiH, jlong tMs) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    pe::PitchEngine& e = PE();
    unsigned char* base = buf ? static_cast<unsigned char*>(env->GetDirectBufferAddress(buf)) : nullptr;
    const jlong cap = buf ? env->GetDirectBufferCapacity(buf) : 0;
    bool ok = base && cap > 0 && rowStride > 0 && pixelStride >= 3 && roiL >= 0 && roiT >= 0 && roiW > 0 && roiH > 0;
    if (ok) {
        const jlong lastByte = (jlong)(roiT + roiH - 1) * rowStride + (jlong)(roiL + roiW) * pixelStride;
        ok = lastByte <= cap && (jlong)(roiL + roiW) * pixelStride <= rowStride;
    }
    e.setFrameTimeMs((double)tMs);
    const double t0 = bootMs();
    const double age = t0 - (double)tMs;                       // time the frame waited before it reached the engine
    if (!ok) {
        e.processFrame(nullptr, 0, 0);
        return (jlong)e.lastGesture().pack();
    }
    e.setBufferLayout(rowStride, pixelStride, false);
    e.processFrame(base + (size_t)roiT * rowStride + (size_t)roiL * pixelStride, roiW, roiH);
    const double total = age + (bootMs() - t0);
    if (age >= 0.0 && age < 800.0 && total < 1200.0) {
        g_latRing[g_latHead] = (float)total;
        g_latHead = (g_latHead + 1) % 32;
        if (g_latCount < 1000000) g_latCount++;
        const int n = g_latCount < 32 ? g_latCount : 32;
        float sum = 0.f;
        for (int i = 0; i < n; i++) sum += g_latRing[i];
        e.setMeasuredLatencyMs(sum / (float)n + (g_tapN >= 5 ? g_tapEma : 0.f), g_latCount);
    }
    return (jlong)e.lastGesture().pack();
}

JNIEXPORT jfloatArray JNICALL Java_com_pitchbot_app_Native_pitchDebugDots(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    float buf[64] = {0.f};
    int n = PE().debugDots(buf, 64);
    return makeFloats(env, buf, n);
}

JNIEXPORT jfloatArray JNICALL Java_com_pitchbot_app_Native_pitchInfo(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    pe::PitchEngine& e = PE();
    int ct = e.chosenCandidate();
    float out[8] = {e.temperature(), e.lastFrameMicros(), (float)e.candidateCount(),
                    ct >= 0 ? (float)e.candidates()[ct].type : -1.f, (float)e.state().attackSign,
                    e.state().counter ? 1.f : 0.f, e.ballIsRing() ? 1.f : 0.f, e.yellowKitInPlay() ? 1.f : 0.f};
    return makeFloats(env, out, 8);
}


JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_noteTapLatency(JNIEnv*, jobject, jfloat ms) {
    if (!(ms >= 0.f && ms < 600.f)) return;
    std::lock_guard<std::mutex> l(g_pe_mu);
    g_tapEma = g_tapN == 0 ? ms : g_tapEma * 0.9f + ms * 0.1f;
    if (g_tapN < 1000000) g_tapN++;
}

JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_v7Export(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    return env->NewStringUTF(PE().v7Export().c_str());
}

JNIEXPORT jboolean JNICALL Java_com_pitchbot_app_Native_v7Import(JNIEnv* env, jobject, jstring s) {
    const std::string in = toStd(env, s);
    std::lock_guard<std::mutex> l(g_pe_mu);
    return PE().v7Import(in) ? JNI_TRUE : JNI_FALSE;
}

// Trains every learned head on the match that just ended and clears the per-match state; returns a one-line report.
JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_v7EndMatch(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    return env->NewStringUTF(PE().v7EndMatch().c_str());
}

// ---- latency per command kind, regression gate, dashboards, replay ----
JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_v8Latency(JNIEnv*, jobject, jint kind, jfloat queueMs, jfloat overheadMs, jint flags) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().noteInputLatency((int)kind, queueMs, overheadMs, (int)flags);
}

JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_v8LatencyReport(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    return env->NewStringUTF(PE().inputLatencyReport().c_str());
}

JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_v8Export(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    return env->NewStringUTF(PE().v8Export().c_str());
}

JNIEXPORT jboolean JNICALL Java_com_pitchbot_app_Native_v8Import(JNIEnv* env, jobject, jstring s) {
    const std::string in = toStd(env, s);
    std::lock_guard<std::mutex> l(g_pe_mu);
    return PE().v8Import(in) ? JNI_TRUE : JNI_FALSE;
}

// "verdict|pending|reason": 0 insufficient data, 1 promote, 2 hold, 3 rollback
JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_v8Gate(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    std::string r = std::to_string(PE().gateVerdict()) + "|" + (PE().gatePending() ? "1" : "0") + "|" + PE().gateReason();
    return env->NewStringUTF(r.c_str());
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_v8GateAck(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().gateAck();
}

JNIEXPORT jfloatArray JNICALL Java_com_pitchbot_app_Native_v8DashSeries(JNIEnv* env, jobject) {
    float buf[pe::V8_DASH_STRIDE * 30];
    int rows;
    { std::lock_guard<std::mutex> l(g_pe_mu); rows = PE().dashSeries(buf, pe::V8_DASH_STRIDE * 30); }
    jfloatArray a = env->NewFloatArray(rows * pe::V8_DASH_STRIDE);
    if (rows > 0) env->SetFloatArrayRegion(a, 0, rows * pe::V8_DASH_STRIDE, buf);
    return a;
}

JNIEXPORT jfloatArray JNICALL Java_com_pitchbot_app_Native_v8LiveSeries(JNIEnv* env, jobject) {
    float buf[pe::V8_DASH_STRIDE];
    int n;
    { std::lock_guard<std::mutex> l(g_pe_mu); n = PE().liveSeries(buf, pe::V8_DASH_STRIDE); }
    jfloatArray a = env->NewFloatArray(n);
    if (n > 0) env->SetFloatArrayRegion(a, 0, n, buf);
    return a;
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_v8SetRecording(JNIEnv*, jobject, jboolean on) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().setRecording(on == JNI_TRUE);
}

JNIEXPORT jstring JNICALL Java_com_pitchbot_app_Native_v8Replay(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    return env->NewStringUTF(PE().exportReplay().c_str());
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_v8GkReliable(JNIEnv*, jobject, jboolean on) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().setGkControlReliable(on == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_pitchbot_app_Native_v7SetStyle(JNIEnv*, jobject, jint style, jint lockSeconds) {
    std::lock_guard<std::mutex> l(g_pe_mu);
    PE().setPlayStyle((int)style, (double)lockSeconds * 1000.0);
}

}
