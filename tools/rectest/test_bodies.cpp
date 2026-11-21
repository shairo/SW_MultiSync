// Unit check of the turret / gun prototype in src/hook/relay.h (body_sample / body_native / body_fill /
// body_build). A synthetic vehicle: body0 (type 1) and body 1 (a turret) whose voxel sits away from its
// centre of mass. The native record carries the true type-2 value; the addon reports the voxel pose relative to body0.
//   cl /nologo /std:c++17 /EHsc /utf-8 /D_USE_MATH_DEFINES tools\rectest\test_bodies.cpp /Fe:build\test_bodies.exe
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include "../../src/hook/rec.h"
#include "../../src/hook/relay.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static void put_f(std::vector<uint8_t>& b, float v) { uint8_t t[4]; memcpy(t, &v, 4); b.insert(b.end(), t, t + 4); }
static void put_d(std::vector<uint8_t>& b, double v) { uint8_t t[8]; memcpy(t, &v, 8); b.insert(b.end(), t, t + 8); }
static void put_u32(std::vector<uint8_t>& b, uint32_t v) { uint8_t t[4]; memcpy(t, &v, 4); b.insert(b.end(), t, t + 4); }
static float rd_f(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }

// yaw rotation (about y) as a quaternion
static void yawq(double a, double q[4]) { q[0] = 0; q[1] = std::sin(a / 2); q[2] = 0; q[3] = std::cos(a / 2); }

// A 0x81 with body0 type 1 (P1, q1), body 1 as `t1` (2 = rel q / rel p, 0 = no change), body 2 type 0.
static std::vector<uint8_t> record(uint32_t V, const double P1[3], const double q1[4], int t1, const double rq[4], const double rp[3]) {
    std::vector<uint8_t> r;
    put_u32(r, 0x81); put_u32(r, V); put_u32(r, 100);
    r.push_back(3); r.push_back(0);                                  // bodyCount 3
    r.push_back(1); for (int i = 0; i < 4; i++) put_f(r, (float)q1[i]); for (int i = 0; i < 3; i++) put_d(r, P1[i]);
    r.push_back((uint8_t)t1);
    if (t1 == 2) { for (int i = 0; i < 4; i++) put_f(r, (float)rq[i]); for (int i = 0; i < 3; i++) put_f(r, (float)rp[i]); }
    r.push_back(0);
    return r;
}

int main() {
    using namespace relay;
    g_cfg.bodySync = 1;
    const uint32_t V = 77;
    // world: hull at P1 yawed 30°, turret yawed a further 50° (world yaw 80°). Turret CoM relative to the
    // hull CoM in hull axes = (0, 1.5, -2). Voxel→CoM offset in turret axes = (3, 0.25, -1).
    double q1[4]; yawq(30 * M_PI / 180, q1);
    double P1[3] = { 1000, 20, -500 };
    double rqTrue[4]; yawq(50 * M_PI / 180, rqTrue);
    double rpTrue[3] = { 0, 1.5, -2 };
    // The addon reports the voxel relative to body0: position in hull axes, rotation R0ᵀR (= rq here).
    double qT[4] = { rqTrue[0], rqTrue[1], rqTrue[2], rqTrue[3] };
    double com[3] = { rpTrue[0], rpTrue[1], rpTrue[2] };
    double offTrue[3] = { 3, 0.25, -1 }, w[3]; qrot(qT, offTrue, w);
    double vox[3] = { com[0] - w[0], com[1] - w[1], com[2] - w[2] };  // where the addon's voxel is (hull axes)

    // addon samples at ticks 97 and 100 (turret still), native record at 99 → measured offset
    body_sample(V, 1, 97, vox[0], vox[1], vox[2], qT);
    body_sample(V, 1, 100, vox[0], vox[1], vox[2], qT);
    auto nat = record(V, P1, q1, 2, rqTrue, rpTrue);
    body_native(nat.data(), (int)nat.size(), V, 99);
    const BodyTrack& b = g_bodies[std::make_pair(V, 1)];
    CHECK(b.offKnown && b.measured == 1, "offset measured");
    for (int i = 0; i < 3; i++) CHECK(std::fabs(b.off[i] - offTrue[i]) < 1e-3, "off[%d] = %f (want %f)", i, b.off[i], offTrue[i]);
    body_native(nat.data(), (int)nat.size(), V, 99);                    // same tick again (another recipient): no re-measure
    CHECK(b.measured == 1, "one measurement per native tick");

    // second native record at 100: the check against the known offset is ~0
    body_native(nat.data(), (int)nat.size(), V, 100);
    CHECK(b.measured == 2 && g_bodyErrN == 1 && b.errPos < 1e-3 && b.errAng < 0.05, "check errPos %f errAng %f", b.errPos, b.errAng);

    // the turret turns to 90° relative; the addon reports it. body_fill must write the new rel q / rel p.
    double rq2[4]; yawq(90 * M_PI / 180, rq2);
    double qT2[4] = { rq2[0], rq2[1], rq2[2], rq2[3] };
    double w2[3]; qrot(qT2, offTrue, w2);
    double vox2[3] = { com[0] - w2[0], com[1] - w2[1], com[2] - w2[2] };   // CoM stays (it turns about its CoM here)
    body_sample(V, 1, 103, vox2[0], vox2[1], vox2[2], qT2);
    body_sample(V, 1, 106, vox2[0], vox2[1], vox2[2], qT2);
    auto out = record(V, P1, q1, 2, rqTrue, rpTrue);                  // a record still holding the old value
    int n = body_fill(out.data(), (int)out.size(), V, 106, 106);
    CHECK(n == 1, "one body written (n=%d)", n);
    const uint8_t* s1 = out.data() + 14 + 41;
    double got[4] = { rd_f(s1 + 1), rd_f(s1 + 5), rd_f(s1 + 9), rd_f(s1 + 13) };
    CHECK(qangle_deg(got, rq2) < 0.05, "rel q angle %f", qangle_deg(got, rq2));
    for (int i = 0; i < 3; i++) CHECK(std::fabs(rd_f(s1 + 17 + i * 4) - rpTrue[i]) < 1e-3, "rel p[%d] %f", i, rd_f(s1 + 17 + i * 4));
    // the hull moved and turned (another body0 pose in the record): the relative value is unchanged
    {
        double q1b[4]; yawq(-70 * M_PI / 180, q1b);
        double P1b[3] = { 1200, 25, -300 };
        auto out2 = record(V, P1b, q1b, 2, rqTrue, rpTrue);
        CHECK(body_fill(out2.data(), (int)out2.size(), V, 106, 106) == 1, "filled with another hull pose");
        const uint8_t* s2 = out2.data() + 14 + 41;
        double g2[4] = { rd_f(s2 + 1), rd_f(s2 + 5), rd_f(s2 + 9), rd_f(s2 + 13) };
        CHECK(qangle_deg(g2, rq2) < 0.05, "rel q independent of the hull %f", qangle_deg(g2, rq2));
        for (int i = 0; i < 3; i++) CHECK(std::fabs(rd_f(s2 + 17 + i * 4) - rpTrue[i]) < 1e-3, "rel p[%d] independent of the hull", i);
    }

    // body_build: a template whose body 1 is type 0 grows by 28 bytes (type 2); body 2 (unknown) stays 0
    auto tmpl = record(V, P1, q1, 0, nullptr, nullptr);
    uint8_t buf[512];
    int len = body_build(buf, sizeof buf, tmpl.data(), (int)tmpl.size(), V, 106);
    CHECK(len == (int)tmpl.size() + 28, "built len %d (template %d)", len, (int)tmpl.size());
    CHECK(buf[14 + 41] == 2 && buf[14 + 41 + 29] == 0, "slot types %d %d", buf[14 + 41], buf[14 + 41 + 29]);
    CHECK(body_fill(buf, len, V, 106, 106) == 1, "filled built record");
    CHECK(rec::decode_len(buf, len, 0) == len, "decode_len of the built record %d", rec::decode_len(buf, len, 0));
    // too small a buffer: refused
    CHECK(body_build(buf, (int)tmpl.size() + 10, tmpl.data(), (int)tmpl.size(), V, 106) == 0, "cap respected");

    // off: nothing written, template copied as is
    g_cfg.bodySync = 0;
    CHECK(body_fill(out.data(), (int)out.size(), V, 106, 106) == 0, "bodySync=0 writes nothing");
    CHECK(body_build(buf, sizeof buf, tmpl.data(), (int)tmpl.size(), V, 106) == (int)tmpl.size(), "bodySync=0 copies");
    g_cfg.bodySync = 1;

    // stale source: older than `stale` ticks → not used
    CHECK(body_fill(out.data(), (int)out.size(), V, 106 + 400, 106 + 400) == 0, "stale body not written");

    // a native record that arrives before the addon sample of its tick waits and is measured later
    {
        const uint32_t W = 78;
        body_sample(W, 1, 200, vox[0], vox[1], vox[2], qT);
        auto n2 = record(W, P1, q1, 2, rqTrue, rpTrue);
        body_native(n2.data(), (int)n2.size(), W, 202);
        CHECK(g_bodyPending.size() == 1, "pending %d", (int)g_bodyPending.size());
        body_sample(W, 1, 203, vox[0], vox[1], vox[2], qT);
        bodies_retry(204);
        CHECK(g_bodyPending.empty() && g_bodies[std::make_pair(W, 1)].offKnown, "measured after the sample came");
        // the body stands still: samples 60 ticks apart still bracket the native tick
        body_sample(W, 1, 263, vox[0], vox[1], vox[2], qT);
        body_native(n2.data(), (int)n2.size(), W, 240);
        CHECK(g_bodies[std::make_pair(W, 1)].measured == 2, "still body measured across a long gap");
        // no addon sample ever comes: dropped after 90 ticks
        body_native(n2.data(), (int)n2.size(), W, 300);
        bodies_retry(391);
        CHECK(g_bodyPending.empty(), "dropped after the wait");
    }

    // guard: a native record 2 m off (taken while the turret slews, or the CoM really moved) is discarded,
    // the body is not written while the last check missed, and three misses in a row start over
    {
        BodyTrack& g = g_bodies[std::make_pair(V, 1)];
        double off0[3] = { g.off[0], g.off[1], g.off[2] };
        uint64_t m0 = g.measured;
        double rpFar[3] = { rpTrue[0], rpTrue[1], rpTrue[2] + 2.0 };
        auto bad = record(V, P1, q1, 2, rq2, rpFar);
        body_native(bad.data(), (int)bad.size(), V, 104);
        CHECK(g.measured == m0 && g.rejected == 1 && g.missRun == 1, "miss discarded (measured %llu rejected %llu)",
              (unsigned long long)g.measured, (unsigned long long)g.rejected);
        for (int i = 0; i < 3; i++) CHECK(std::fabs(g.off[i] - off0[i]) < 1e-9, "offset kept after a miss");
        auto o3 = record(V, P1, q1, 2, rqTrue, rpTrue);
        CHECK(body_fill(o3.data(), (int)o3.size(), V, 106, 106) == 0, "not written while the last check missed");
        body_native(bad.data(), (int)bad.size(), V, 105);
        body_native(bad.data(), (int)bad.size(), V, 106);
        CHECK(g.resets == 1 && g.missRun == 0 && g.okRun == 0, "three misses: start over (resets %llu)", (unsigned long long)g.resets);
        double dd = std::sqrt((g.off[0] - off0[0]) * (g.off[0] - off0[0]) + (g.off[1] - off0[1]) * (g.off[1] - off0[1]) + (g.off[2] - off0[2]) * (g.off[2] - off0[2]));
        CHECK(std::fabs(dd - 2.0) < 1e-3, "offset now the new measurement (moved %f m, body axes)", dd);
        CHECK(body_fill(o3.data(), (int)o3.size(), V, 106, 106) == 0, "not written before a check confirms the new offset");
        body_sample(V, 1, 109, vox2[0], vox2[1], vox2[2], qT2);
        body_native(bad.data(), (int)bad.size(), V, 108);                   // agrees with the new offset
        CHECK(g.okRun == 1, "confirmed (okRun %d)", g.okRun);
        CHECK(body_fill(o3.data(), (int)o3.size(), V, 109, 109) == 1, "written again once confirmed");
    }

    // near client: the server sends a nudged gun as type 0 ("no change"); bodynear_pass turns it into type 2
    // with the addon pose when it differs from what that client was last shown by more than 0.01°
    {
        const uint32_t X = 79; const uint64_t peer = 5;
        body_sample(X, 1, 400, vox[0], vox[1], vox[2], qT);
        body_sample(X, 1, 403, vox[0], vox[1], vox[2], qT);
        auto n2 = record(X, P1, q1, 2, rqTrue, rpTrue);
        body_native(n2.data(), (int)n2.size(), X, 401);
        body_native(n2.data(), (int)n2.size(), X, 402);                    // second check confirms (okRun 1)
        // a message: 8-byte envelope, 20-byte body header (count at +16), the 0x81, then a fixed 0x39 record
        auto msg = [&](const std::vector<uint8_t>& r) {
            std::vector<uint8_t> m(8 + 20, 0);
            m.insert(m.end(), r.begin(), r.end());
            std::vector<uint8_t> tail(36, 0); uint32_t tg = 0x39; memcpy(tail.data(), &tg, 4); tail[35] = 0xAB;
            m.insert(m.end(), tail.begin(), tail.end());
            uint32_t two = 2; memcpy(m.data() + 8 + 16, &two, 4);
            return m;
        };
        std::unordered_set<uint32_t> none, mine{ X };
        uint8_t buf2[1024];
        auto run = [&](const std::vector<uint8_t>& m, const std::unordered_set<uint32_t>& h, int& wpos) {
            memcpy(buf2, m.data(), m.size()); wpos = (int)m.size();
            return bodynear_pass(peer, buf2, wpos, sizeof buf2, 2, 2, 404, h);
        };
        int wp = 0;
        g_bodyPeers = { peer, 6 };                                          // both players typed ?bs on
        auto shown = msg(record(X, P1, q1, 2, rqTrue, rpTrue));
        CHECK(run(shown, none, wp) == 0 && wp == (int)shown.size(), "a type-2 record is left alone (it sets what is shown)");
        auto zero = msg(record(X, P1, q1, 0, nullptr, nullptr));
        CHECK(run(zero, none, wp) == 0, "no change since shown: nothing written");
        double rqN[4]; yawq((50 + 0.05) * M_PI / 180, rqN);                   // nudged by 0.05°
        double qN[4] = { rqN[0], rqN[1], rqN[2], rqN[3] }, wN[3]; qrot(qN, offTrue, wN);
        body_sample(X, 1, 404, com[0] - wN[0], com[1] - wN[1], com[2] - wN[2], qN);

        g_cfg.bodySync = 0; CHECK(run(zero, none, wp) == 0, "bodySync=0 writes nothing"); g_cfg.bodySync = 1;
        g_bodyPeers.erase(peer);                                            // ?bs off: this player gets the server as is
        CHECK(!body_sync_for(peer) && run(zero, none, wp) == 0 && wp == (int)zero.size(), "player off (?bs off): nothing written");
        g_bodyPeers.insert(peer);
        CHECK(body_sync_for(peer), "?bs on again");
        g_cfg.bodySync = 0; CHECK(!body_sync_for(peer), "bodySync=0 is off for everyone"); g_cfg.bodySync = 1;
        int n3 = run(zero, none, wp);
        CHECK(n3 == 1 && wp == (int)zero.size() + 28, "nudge written (n=%d, grew %d)", n3, wp - (int)zero.size());
        const uint8_t* s3 = buf2 + 8 + 20 + 14 + 41;
        double g3[4] = { rd_f(s3 + 1), rd_f(s3 + 5), rd_f(s3 + 9), rd_f(s3 + 13) };
        CHECK(s3[0] == 2 && qangle_deg(g3, rqN) < 0.005, "type 2 with the nudged angle (off by %f°)", qangle_deg(g3, rqN));
        for (int i = 0; i < 3; i++) CHECK(std::fabs(rd_f(s3 + 17 + i * 4) - rpTrue[i]) < 1e-3, "near rel p[%d] %f", i, rd_f(s3 + 17 + i * 4));
        int recLen = rec::decode_len(buf2 + 8, wp - 8, 20);
        CHECK(recLen == (int)record(X, P1, q1, 0, nullptr, nullptr).size() + 28, "grown record decodes (%d)", recLen);
        uint32_t tg; memcpy(&tg, buf2 + 8 + 20 + recLen, 4);
        CHECK(tg == 0x39 && buf2[8 + 20 + recLen + 35] == 0xAB, "the next record moved intact");
        CHECK(run(zero, none, wp) == 0, "sent once: the next no-change record is left alone");
        // a far recipient: pass 1 rewrote the record (hull, type-2 bodies) but left the type-0 gun
        {
            const uint64_t farPeer = 6; auto& shF = g_bodyShown[std::make_tuple(X, 1, farPeer)];
            shF.have = true; for (int k = 0; k < 4; k++) shF.rq[k] = rqTrue[k];
            memcpy(buf2, zero.data(), zero.size()); int wf = (int)zero.size();
            CHECK(bodynear_pass(farPeer, buf2, wf, sizeof buf2, 2, 2, 404, mine) == 1 && wf == (int)zero.size() + 28, "far (rewritten) record: nudge written too");
        }
        double rqS[4]; yawq((50 + 0.055) * M_PI / 180, rqS);                  // a further 0.005°: below the threshold
        double qS[4] = { rqS[0], rqS[1], rqS[2], rqS[3] }, wS[3]; qrot(qS, offTrue, wS);
        body_sample(X, 1, 405, com[0] - wS[0], com[1] - wS[1], com[2] - wS[2], qS);
        CHECK(run(zero, none, wp) == 0, "0.005° more: not sent");
        CHECK(run(zero, none, wp) == 0 && g_bodyShown[std::make_tuple(X, 1, peer)].have, "shown state kept");
        bodies_forget(X);
        CHECK(!g_bodyShown.count(std::make_tuple(X, 1, peer)), "shown forgotten with the vehicle");
    }

    // seat hotkey 6 (keyMask bit 20) adds stepDeg to the gun offset once per press and forces a resend
    {
        g_bodyOff = BodyOff{}; g_bodyOff.on = true; g_bodyOff.veh = V; g_bodyOff.body = 1; g_bodyOff.stepDeg = 0.05;
        auto& sh = g_bodyShown[std::make_tuple(V, 1, (uint64_t)9)]; sh.have = true;
        CHECK(!key_step(9, 1u << 3), "another key: nothing");
        CHECK(key_step(9, (1u << 20) | (1u << 3)), "hotkey 6 pressed");
        CHECK(!key_step(9, 1u << 20), "held: once per press");
        CHECK(std::fabs(g_bodyOff.yawDeg - 0.05) < 1e-12 && !g_bodyShown[std::make_tuple(V, 1, (uint64_t)9)].have, "offset 0.05 and resend forced");
        key_step(9, 0); key_step(9, 1u << 20);
        double ang = 2.0 * std::atan2(g_bodyOff.ry, g_bodyOff.rw) * 180.0 / M_PI;
        CHECK(std::fabs(g_bodyOff.yawDeg - 0.10) < 1e-12 && std::fabs(ang - 0.10) < 1e-9, "second press: 0.10 (rotation %f)", ang);
        g_bodyOff.on = false; CHECK(!key_step(9, 0) && !key_step(9, 1u << 20), "off: nothing");
        g_bodyOff = BodyOff{};
    }

    // despawn forgets the tracks
    bodies_forget(V);
    CHECK(!has_bodies(V), "forgotten");

    if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
    printf("test_bodies: all checks passed\n");
    return 0;
}
