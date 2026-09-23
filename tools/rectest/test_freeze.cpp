// Offline replay of the client-freeze detector (src/hook/freeze.h) over a .swcap, fed exactly the
// way dllmain.cpp feeds it live: SEND type=8 (full walk) -> relay::observe + 0x2C removals, SEND ch1
// type=12 -> definition pushed, RECV type=3 -> pose 0x2F / 0x1B request / 0x29 ack, then
// freeze::check on the recv clock. Prints every verdict transition on the capture's t+ms clock.
// Expected: session_20260913_214246_776_freeze fires for peer ...8477 only, a few seconds after
// t+42.3 s (pose static from 42.31 s, first unacked definition pushed at 43.69 s); healthy
// captures print no onset at all.
//   test_freeze <file.swcap> [--pose MS] [--def MS]
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include "../../src/hook/rec.h"
#include "../../src/hook/relay.h"
#include "../../src/hook/freeze.h"

static double rdD(const uint8_t* p){ double v; memcpy(&v,p,8); return v; }

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_freeze <file.swcap> [--pose MS] [--def MS]\n"); return 1; }
    int poseMs = relay::g_cfg.freezePoseMs, defMs = relay::g_cfg.freezeDefMs;
    for (int i = 2; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--pose")) poseMs = atoi(argv[i+1]);
        if (!strcmp(argv[i], "--def"))  defMs  = atoi(argv[i+1]);
    }
    FILE* f = fopen(argv[1], "rb"); if (!f) { printf("cannot open\n"); return 1; }
    char hdr[7] = {0}; fread(hdr, 1, 6, f);
    if (hdr[0] != 'S' || hdr[5] != 0x02) { printf("not SWCAP v2\n"); return 1; }

    uint64_t t0 = 0, onsets = 0, recoveries = 0, sends8 = 0, recv3 = 0, pushes = 0;
    std::map<uint64_t, uint64_t> onsetAt;
    std::vector<uint8_t> buf;
    for (;;) {
        uint8_t dir; if (fread(&dir, 1, 1, f) != 1) break;
        uint64_t seq, ms, sid; int32_t ch, flags; uint32_t orig, stored;
        fread(&seq,8,1,f); fread(&ms,8,1,f); fread(&sid,8,1,f);
        fread(&ch,4,1,f); fread(&flags,4,1,f); fread(&orig,4,1,f); fread(&stored,4,1,f);
        buf.resize(stored); if (stored) fread(buf.data(),1,stored,f);
        if (!t0) t0 = ms;
        if (dir > 1 || stored != orig || stored < 22) continue;
        const uint8_t* b = buf.data();
        if (*reinterpret_cast<const uint32_t*>(b) != 0) continue;              // single-frame head only
        const uint8_t* body = b + 8; int blen = (int)stored - 8;
        uint32_t mt = rec::U32(body, blen, 4);
        if (dir == 0 && ch == 1 && mt == 12 && blen >= 12) { pushes++; freeze::on_defpush(sid, rec::U32(body, blen, 8), ms); continue; }
        if (ch != 0) continue;
        if (dir == 0 && mt == 8) {
            sends8++;
            rec::Walk w = rec::walk(body, blen);
            if (!w.full) continue;
            uint32_t tick = rec::U32(body, blen, 8);
            relay::observe(sid, body, blen, tick);
            for (int i = 0, off = 20, cnt = (int)rec::U32(body, blen, 16); i < cnt && off < blen; i++) {
                int L = rec::decode_len(body, blen, off); if (L <= 0) break;
                if (rec::U32(body, blen, off) == 0x2C) freeze::on_remove(sid, rec::U32(body, blen, off + 4));
                off += L;
            }
        } else if (dir == 1 && mt == 3) {
            recv3++;
            rec::Walk w = rec::walk_client(body, blen);
            if (w.first81 >= 0 && w.first81 + 56 <= blen) {
                double x = rdD(body + w.first81 + 28), y = rdD(body + w.first81 + 36), z = rdD(body + w.first81 + 44);
                relay::observe_pose(sid, x, y, z);
                freeze::on_pose(sid, x, y, z, rec::U32(body, blen, w.first81 + 52), ms);
            }
            for (int i = 0, off = 14; i < w.walked && off < blen; i++) {
                int L = rec::decode_client_len(body, blen, off); if (L <= 0) break;
                if (body[off] == 0x1B) freeze::on_defreq(sid, rec::U32(body, blen, off + 4), ms);
                else if (body[off] == 0x29) freeze::on_defack(sid, rec::U32(body, blen, off + 4), ms);
                off += L;
            }
            int v = freeze::check(sid, ms, poseMs, defMs);
            if (v) {
                const freeze::Peer& p = freeze::at(sid);
                if (v > 0) {
                    onsets++; onsetAt[sid] = ms;
                    printf("t+%7.3f  FREEZE?  peer=%llu reason=%s pose=(%.1f,%.1f,%.1f) veh=%u static=%llums unacked=%llums pending=",
                           (ms - t0) / 1000.0, (unsigned long long)sid, p.reason, p.sx, p.sy, p.sz, (unsigned)p.poseVeh,
                           (unsigned long long)(ms - p.staticSinceMs), (unsigned long long)freeze::oldest_unacked_ms(p, ms));
                    for (auto& kv : p.pending) printf("%u,", (unsigned)kv.first);
                    printf("\n");
                } else {
                    recoveries++;
                    printf("t+%7.3f  over     peer=%llu after %llums pose=(%.1f,%.1f,%.1f)\n", (ms - t0) / 1000.0,
                           (unsigned long long)sid, (unsigned long long)(ms - onsetAt[sid]), p.sx, p.sy, p.sz);
                }
            }
        }
    }
    fclose(f);
    printf("\nsends8=%llu recv3=%llu defPushes=%llu  onsets=%llu recoveries=%llu  (pose>=%d ms, def>=%d ms)\n",
           (unsigned long long)sends8, (unsigned long long)recv3, (unsigned long long)pushes,
           (unsigned long long)onsets, (unsigned long long)recoveries, poseMs, defMs);
    for (auto& kv : freeze::g_peers) {
        const freeze::Peer& p = kv.second;
        printf("  peer %llu: defReqs=%llu defAcks=%llu pendingAtEnd=%zu events=%llu%s\n", (unsigned long long)kv.first,
               (unsigned long long)p.defReqs, (unsigned long long)p.defAcks, p.pending.size(), (unsigned long long)p.events,
               p.frozenSinceMs ? "  STILL FROZEN at end" : "");
    }
    return 0;
}
