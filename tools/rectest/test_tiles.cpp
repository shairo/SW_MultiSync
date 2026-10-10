// Offline replay of the per-peer tile ledger (src/hook/tiles.h) over a .swcap, fed the way
// dllmain.cpp feeds it live: SEND type=8 (full walk) 0x45 load / 0x46 unload, non-full walk = gap,
// RECV type=1 join = reset, RECV type=3 0x28 = ack, ch15 0x02 = reset. At the end, for each peer, prints
// the 5x5 ring around its last pose the way `?unstuck` would report it (L = load+ack, l = load no ack,
// U = unload, ? = never seen), row z+2 on top.
//   test_tiles <file.swcap>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <map>
#include "../../src/hook/rec.h"
#include "../../src/hook/tiles.h"

static double rdD(const uint8_t* p){ double v; memcpy(&v,p,8); return v; }
static int32_t tile_of(double c) { return (int32_t)floor((c + 500.0) / 1000.0); }

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_tiles <file.swcap>\n"); return 1; }
    FILE* f = fopen(argv[1], "rb"); if (!f) { printf("cannot open\n"); return 1; }
    char hdr[7] = {0}; fread(hdr, 1, 6, f);
    if (hdr[0] != 'S' || hdr[5] != 0x02) { printf("not SWCAP v2\n"); return 1; }
    struct Pose { double x = 0, z = 0; bool have = false; };
    std::map<uint64_t, Pose> pose;
    uint64_t loads = 0, unloads = 0, acks = 0, resets = 0, lastMs = 0;
    std::vector<uint8_t> buf;
    for (;;) {
        uint8_t dir; if (fread(&dir, 1, 1, f) != 1) break;
        uint64_t seq, ms, sid; int32_t ch, flags; uint32_t orig, stored;
        fread(&seq,8,1,f); fread(&ms,8,1,f); fread(&sid,8,1,f);
        fread(&ch,4,1,f); fread(&flags,4,1,f); fread(&orig,4,1,f); fread(&stored,4,1,f);
        buf.resize(stored); if (stored) fread(buf.data(),1,stored,f);
        lastMs = ms;
        if (dir > 1 || stored != orig) continue;
        const uint8_t* b = buf.data();
        if (dir == 0 && ch == 15 && stored == 1 && b[0] == 0x02) { tiles::reset(sid, ms); resets++; continue; }
        if (ch != 0 || stored < 22 || *reinterpret_cast<const uint32_t*>(b) != 0) continue;
        const uint8_t* body = b + 8; int blen = (int)stored - 8;
        uint32_t mt = rec::U32(body, blen, 4);
        if (dir == 0 && mt == 8) {
            rec::Walk w = rec::walk(body, blen);
            if (!w.full) { tiles::on_gap(sid, ms); continue; }
            uint32_t tick = rec::U32(body, blen, 8);
            for (int i = 0, off = 20, cnt = (int)rec::U32(body, blen, 16); i < cnt && off < blen; i++) {
                int L = rec::decode_len(body, blen, off); if (L <= 0) break;
                uint32_t tag = rec::U32(body, blen, off);
                int32_t x = (int32_t)rec::U32(body, blen, off + 4), z = (int32_t)rec::U32(body, blen, off + 8);
                if (tag == 0x45) { tiles::on_load(sid, x, z, body[off + 12], tick, ms); loads++; }
                else if (tag == 0x46) { tiles::on_unload(sid, x, z, tick, ms); unloads++; }
                off += L;
            }
        } else if (dir == 1 && mt == 1) {
            tiles::reset(sid, ms); resets++;
        } else if (dir == 1 && mt == 3) {
            rec::Walk w = rec::walk_client(body, blen);
            if (w.first81 >= 0 && w.first81 + 56 <= blen) {
                Pose& p = pose[sid]; p.x = rdD(body + w.first81 + 28); p.z = rdD(body + w.first81 + 44); p.have = true;
            }
            for (int i = 0, off = 14; i < w.walked && off < blen; i++) {
                int L = rec::decode_client_len(body, blen, off); if (L <= 0) break;
                if (body[off] == 0x28) { tiles::on_ack(sid, (int32_t)rec::U32(body, blen, off + 4), (int32_t)rec::U32(body, blen, off + 8), ms); acks++; }
                off += L;
            }
        }
    }
    fclose(f);
    printf("loads=%llu unloads=%llu acks(0x28)=%llu resets=%llu\n", (unsigned long long)loads, (unsigned long long)unloads,
           (unsigned long long)acks, (unsigned long long)resets);
    for (auto& kv : tiles::g_peers) {
        const tiles::Peer& tp = kv.second;
        auto pit = pose.find(kv.first);
        printf("\npeer %llu: %zu tiles in ledger, gaps=%llu, ledger %.1fs", (unsigned long long)kv.first, tp.t.size(),
               (unsigned long long)tp.gaps, (lastMs - tp.sinceMs) / 1000.0);
        if (pit == pose.end() || !pit->second.have) { printf(", no pose\n"); continue; }
        int32_t tx = tile_of(pit->second.x), tz = tile_of(pit->second.z);
        printf(", last pose tile (%d,%d)\n", tx, tz);
        for (int32_t z = tz + 2; z >= tz - 2; --z) {
            printf("  z=%4d ", z);
            for (int32_t x = tx - 2; x <= tx + 2; ++x) {
                auto it = tp.t.find({ x, z });
                char c = it == tp.t.end() ? '?' : it->second.last == 0x46 ? 'U' : it->second.ackMs ? 'L' : 'l';
                printf(" %c", c);
            }
            printf("\n");
        }
    }
    return 0;
}
