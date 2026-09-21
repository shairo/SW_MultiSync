// Offline pre-flight for the stage-C relay (src/hook/relay.h). Replays a .swcap in order, feeding
// every single-frame SEND type=8 ch0 message to relay::observe, then (as if F6 were ON) builds the
// injected copy and re-walks it. The critical safety property: every injected message must STILL be
// rec::walk-full (recordCount and lengths consistent) — otherwise we'd corrupt a real client's
// stream. Also reports injected record/byte volume so it can be checked against `swcap bw`/`shadow`.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <map>
#include <array>
#include <algorithm>
#include "../../src/hook/rec.h"
#include "../../src/hook/relay.h"

static double rdD(const uint8_t* p){ double v; memcpy(&v,p,8); return v; }
static float  rdF(const uint8_t* p){ float v; memcpy(&v,p,4); return v; }

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_relay <file.swcap> [config.ini]\n"); return 1; }
    if (argc >= 3) {
        int n = relay::load_config_file(argv[2]);
        if (n < 0) { printf("cannot open config %s\n", argv[2]); return 1; }
        const relay::Cfg& c = relay::g_cfg;
        printf("config %s: %d keys -> intervalMin=%d intervalMax=%d lodDenser=%d lag=%d horizonMax=%d "
               "relayMinGap=%d srcRatio=%.2f stale=%d gapOutlier=%.0f maxPerSend=%d budgetWindow=%d "
               "capKbpsPerPeer=%d\n",
               argv[2], n, c.intervalMin, c.intervalMax, c.lodDenser, c.lag, c.horizonMax,
               c.relayMinGap, c.srcRatio, c.stale, c.gapOutlier, c.maxPerSend, c.budgetWindow, c.capKbpsPerPeer);
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open\n"); return 1; }
    char hdr[7] = {0}; fread(hdr, 1, 6, f);
    if (hdr[0] != 'S' || hdr[5] != 0x02) { printf("not SWCAP v2\n"); return 1; }

    uint64_t msgs = 0, injected = 0, walkFail = 0, rcFail = 0;
    uint32_t tMin = 0xFFFFFFFF, tMax = 0;
    // --- pose sanity diagnostics on the INJECTED stream ---
    uint64_t nonFinite = 0, absurdPos = 0, badQuat = 0, bigJump = 0;
    int samples = 0;
    // per-vehicle: last NATIVE body0 pos seen (to measure the jump our injection introduces)
    std::map<uint32_t, std::array<double,3>> lastNativePos;
    // ETA-monotonicity poison detector: if a client gates per-vehicle updates on non-decreasing time,
    // then a NATIVE record whose ETA is below the max ETA we already INJECTED for that (peer,veh) would
    // be rejected -> the vehicle sticks at our predicted pose = the freeze. Track max injected ETA and
    // count native updates that fall below it.
    std::map<std::pair<uint64_t,uint32_t>, uint32_t> maxInjEta;
    uint64_t poison = 0; int poisonSamp = 0; long maxPoisonGap = 0;
    std::vector<int> appendHorizon;   // (ETA - tick) of each appended record
    std::vector<uint8_t> buf, out;
    for (;;) {
        uint8_t dir;
        if (fread(&dir, 1, 1, f) != 1) break;
        uint64_t seq, tickMs, sid; int32_t ch, flags; uint32_t orig, stored;
        fread(&seq, 8, 1, f); fread(&tickMs, 8, 1, f); fread(&sid, 8, 1, f);
        fread(&ch, 4, 1, f); fread(&flags, 4, 1, f); fread(&orig, 4, 1, f); fread(&stored, 4, 1, f);
        buf.resize(stored);
        if (stored) fread(buf.data(), 1, stored, f);
        if (ch != 0 || stored != orig || stored < 22) continue;
        const uint8_t* b = buf.data();
        if (*reinterpret_cast<const uint32_t*>(b) != 0) continue;              // frag==0
        const uint8_t* body = b + 8; int blen = (int)stored - 8;
        if (dir == 1) {                                                        // client stream: feed 0x2F poses (distance LOD)
            if (*reinterpret_cast<const uint32_t*>(body + 4) == 3) {
                rec::Walk cw = rec::walk_client(body, blen);
                if (cw.full && cw.first81 >= 0 && cw.first81 + 52 <= blen)
                    relay::observe_pose(sid, rdD(body + cw.first81 + 28), rdD(body + cw.first81 + 36), rdD(body + cw.first81 + 44));
            }
            continue;
        }
        if (stored < 28) continue;
        if (*reinterpret_cast<const uint32_t*>(body + 4) != 8) continue;       // msgType==8
        if (!rec::walk(body, blen).full) continue;                            // inject only on full
        uint32_t tick = *reinterpret_cast<const uint32_t*>(body + 8);
        if (tick < tMin) tMin = tick; if (tick > tMax) tMax = tick;
        msgs++;

        relay::observe(sid, body, blen, tick);                                // same order as live
        int cap = (int)stored + relay::kMaxPerSend * relay::kMaxRecLen;
        out.resize(cap);
        int newcub = relay::build_inject(sid, b, (int)stored, out.data(), cap, tick);
        // record native body0 poses + eta from the ORIGINAL message (truth we start from), and run the
        // ETA-poison check: a native update whose ETA is below our already-injected max for (sid,veh).
        std::map<uint32_t,uint32_t> nativeVehEta;   // veh -> native ETA in THIS message
        {
            int c = (int)rec::U32(body, blen, 16), o = 20;
            for (int i = 0; i < c && o < blen; i++) {
                uint32_t tag = rec::U32(body, blen, o); int L = rec::decode_len(body, blen, o);
                if (L <= 0) break;
                if (tag == 0x81) {
                    uint32_t V = rec::U32(body, blen, o + 4);
                    uint32_t E = rec::U32(body, blen, o + 8);
                    nativeVehEta[V] = E;
                    auto pk = std::make_pair(sid, V);
                    auto mit = maxInjEta.find(pk);
                    if (mit != maxInjEta.end() && E < mit->second) {
                        poison++;
                        if ((long)mit->second - E > maxPoisonGap) maxPoisonGap = (long)mit->second - E;
                        if (poisonSamp < 8) { poisonSamp++;
                            printf("  [POISON] sid=%llu veh=%u nativeETA=%u < injectedMaxETA=%u (tick=%u)\n",
                                   (unsigned long long)sid, V, E, mit->second, tick);
                        }
                    }
                    if (o + 14 < blen && body[o+14] == 1 && o + 31 + 24 <= blen)
                        lastNativePos[V] = { rdD(body+o+31), rdD(body+o+39), rdD(body+o+47) };
                }
                o += L;
            }
        }
        if (newcub > 0) {
            injected++;
            const uint8_t* nbody = out.data() + 8; int nblen = newcub - 8;
            rec::Walk w = rec::walk(nbody, nblen);
            if (!w.full) walkFail++;
            if (w.walked != w.recordCount) rcFail++;
            // scan every 0x81 body0 pose in the INJECTED message for pathologies
            int c = (int)rec::U32(nbody, nblen, 16), o = 20;
            for (int i = 0; i < c && o < nblen; i++) {
                uint32_t tag = rec::U32(nbody, nblen, o); int L = rec::decode_len(nbody, nblen, o);
                if (L <= 0) break;
                if (tag == 0x81) {
                    uint32_t Ve = rec::U32(nbody, nblen, o + 4);
                    uint32_t Ee = rec::U32(nbody, nblen, o + 8);
                    // an APPENDED record = a 0x81 for a veh not present natively in this message
                    if (nativeVehEta.find(Ve) == nativeVehEta.end()) {
                        auto pk = std::make_pair(sid, Ve);
                        auto mit = maxInjEta.find(pk);
                        if (mit == maxInjEta.end() || Ee > mit->second) maxInjEta[pk] = Ee;
                        appendHorizon.push_back((int)((int64_t)Ee - (int64_t)tick));
                    }
                }
                if (tag == 0x81 && o + 14 < nblen && nbody[o+14] == 1 && o + 31 + 24 <= nblen) {
                    uint32_t V = rec::U32(nbody, nblen, o + 4);
                    double x = rdD(nbody+o+31), y = rdD(nbody+o+39), z = rdD(nbody+o+47);
                    float q[4]; for (int k=0;k<4;k++) q[k]=rdF(nbody+o+15+k*4);
                    double qn = 0; for (int k=0;k<4;k++) qn += (double)q[k]*q[k];
                    bool nf = !std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)
                              ||!std::isfinite(q[0])||!std::isfinite(q[1])||!std::isfinite(q[2])||!std::isfinite(q[3]);
                    bool ab = std::fabs(x)>1e6||std::fabs(y)>1e6||std::fabs(z)>1e6;
                    bool bq = std::isfinite(qn) && (qn < 0.5 || qn > 2.0);   // renormalized quats should be ~1
                    double jump = 0; auto it = lastNativePos.find(V);
                    if (it != lastNativePos.end()) {
                        double dx=x-it->second[0], dy=y-it->second[1], dz=z-it->second[2];
                        jump = std::sqrt(dx*dx+dy*dy+dz*dz);
                    }
                    bool bj = std::isfinite(jump) && jump > 5000;    // injection moved it >5km from native truth
                    if (nf) nonFinite++;
                    if (ab) absurdPos++;
                    if (bq) badQuat++;
                    if (bj) bigJump++;
                    if ((nf||ab||bq||bj) && samples < 25) {
                        samples++;
                        printf("  [!] veh=%u tick=%u pos=(%.0f,%.0f,%.0f) |q|^2=%.3f jumpVsNative=%.0f%s%s%s%s\n",
                               V, tick, x, y, z, qn, jump,
                               nf?" NONFINITE":"", ab?" ABSURD":"", bq?" BADQUAT":"", bj?" BIGJUMP":"");
                    }
                }
                o += L;
            }
        }
    }
    fclose(f);
    double secs = tMax > tMin ? (tMax - tMin) / 60.0 : 1;
    printf("full type8 messages replayed: %llu\n", (unsigned long long)msgs);
    printf("  injected messages : %llu\n", (unsigned long long)injected);
    printf("  WALK FAILURES     : %llu   <-- must be 0\n", (unsigned long long)walkFail);
    printf("  recCount mismatch : %llu   <-- must be 0\n", (unsigned long long)rcFail);
    printf("  appended records  : %llu  rewritten native: %llu\n",
           (unsigned long long)relay::g_injRecords, (unsigned long long)relay::g_rewrites);
    printf("  appended bytes    : %llu  (~%.3f Mbps over %.0fs)\n",
           (unsigned long long)relay::g_injBytes, relay::g_injBytes * 8 / 1e6 / secs, secs);
    printf("-- injected-pose sanity --\n");
    printf("  non-finite (NaN/Inf) : %llu\n", (unsigned long long)nonFinite);
    printf("  absurd |pos|>1e6     : %llu\n", (unsigned long long)absurdPos);
    printf("  bad quat |q|^2 off 1 : %llu\n", (unsigned long long)badQuat);
    printf("  big jump >5km vs nat : %llu\n", (unsigned long long)bigJump);
    printf("  ETA-POISON (native ETA < our injected ETA) : %llu  maxGap=%ld ticks\n",
           (unsigned long long)poison, maxPoisonGap);
    if (!appendHorizon.empty()) {
        std::sort(appendHorizon.begin(), appendHorizon.end());
        auto q=[&](double f){ return appendHorizon[(size_t)(f*(appendHorizon.size()-1))]; };
        printf("  appended ETA horizon (ETA-tick) ticks: p50=%d p90=%d p99=%d min=%d max=%d\n",
               q(.5), q(.9), q(.99), appendHorizon.front(), appendHorizon.back());
    }
    return 0;
}
