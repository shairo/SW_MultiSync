// Distance vs native 0x81 spacing. For every recipient peer, the client→server 0x2F player pose
// (RECV msgType=3) gives that peer's world position; for every SEND 0x81 to the same peer we take
// body0's position, compute |veh − player| and dETA = record.time − msg.tick (the interval the
// server chose at THIS send, so it pairs with this distance). Buckets dETA by distance so the game's
// distance LOD can be read off a capture. (--gap uses the tick gap since the previous 0x81 instead.)
//   test_dist <file.swcap> [--csv out.csv] [--peer S] [--gap]
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <map>
#include <algorithm>
#include <string>
#include "../../src/hook/rec.h"

static double rdD(const uint8_t* p){ double v; memcpy(&v,p,8); return v; }
struct Pose { uint64_t ms; double x,y,z; };
struct Samp { double dist, speed; uint32_t gap, deta; uint64_t peer; uint32_t veh, tick; int bodies; uint64_t ms; double vx,vy,vz, px,py,pz; };

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_dist <file.swcap> [--csv out] [--peer S]\n"); return 1; }
    const char* csv = nullptr; uint64_t onlyPeer = 0; bool useGap = false;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--gap")) useGap = true;
    for (int i = 2; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--csv")) csv = argv[i+1];
        if (!strcmp(argv[i], "--peer")) onlyPeer = strtoull(argv[i+1], nullptr, 10);
    }
    FILE* f = fopen(argv[1], "rb"); if (!f) { printf("cannot open\n"); return 1; }
    char hdr[7] = {0}; fread(hdr, 1, 6, f);
    if (hdr[0] != 'S' || hdr[5] != 0x02) { printf("not SWCAP v2\n"); return 1; }

    std::map<uint64_t, std::vector<Pose>> poses;                       // peer -> pose series (ms order)
    struct Last { uint32_t tick; double x,y,z; };
    std::map<std::pair<uint64_t,uint32_t>, Last> last;                 // (peer,veh) -> last 0x81
    std::vector<Samp> samps;
    uint64_t recvMsgs = 0, recvFull = 0, poseRecs = 0, sends81 = 0, noPose = 0;
    std::vector<uint8_t> buf;
    for (;;) {
        uint8_t dir; if (fread(&dir, 1, 1, f) != 1) break;
        uint64_t seq, ms, sid; int32_t ch, flags; uint32_t orig, stored;
        fread(&seq,8,1,f); fread(&ms,8,1,f); fread(&sid,8,1,f);
        fread(&ch,4,1,f); fread(&flags,4,1,f); fread(&orig,4,1,f); fread(&stored,4,1,f);
        buf.resize(stored); if (stored) fread(buf.data(),1,stored,f);
        if (ch != 0 || stored != orig || stored < 22) continue;
        if (onlyPeer && sid != onlyPeer) continue;
        const uint8_t* b = buf.data();
        if (*reinterpret_cast<const uint32_t*>(b) != 0) continue;
        const uint8_t* body = b + 8; int blen = (int)stored - 8;
        uint32_t mt = *reinterpret_cast<const uint32_t*>(body + 4);
        if (dir == 1 && mt == 3) {
            recvMsgs++;
            rec::Walk w = rec::walk_client(body, blen);
            if (!w.full) continue;
            recvFull++;
            int cnt = (int)rec::U16(body, blen, 10), off = 14;
            for (int i = 0; i < cnt; i++) {
                int L = rec::decode_client_len(body, blen, off);
                if (body[off] == 0x2F) {
                    poses[sid].push_back({ ms, rdD(body+off+28), rdD(body+off+36), rdD(body+off+44) });
                    poseRecs++;
                }
                off += L;
            }
        } else if (dir == 0 && mt == 8) {
            rec::Walk w = rec::walk(body, blen);
            if (!w.full) continue;
            int cnt = (int)rec::U32(body, blen, 16), off = 20;
            uint32_t tick = rec::U32(body, blen, 8);
            for (int i = 0; i < cnt; i++) {
                uint32_t tag = rec::U32(body, blen, off); int L = rec::decode_len(body, blen, off);
                if (tag == 0x81 && body[off+14] == 1) {
                    sends81++;
                    uint32_t veh = rec::U32(body, blen, off+4);
                    uint32_t eta = rec::U32(body, blen, off+8);
                    long deta = (long)eta - (long)tick;
                    int bodies = (int)rec::U16(body, blen, off+12);
                    double x = rdD(body+off+31), y = rdD(body+off+39), z = rdD(body+off+47);
                    auto key = std::make_pair(sid, veh);
                    auto it = last.find(key);
                    auto pit = poses.find(sid);
                    if (it != last.end() && pit != poses.end() && !pit->second.empty()) {
                        const Pose& p = pit->second.back();     // latest pose at capture time (stream order)
                        double d = std::sqrt((x-p.x)*(x-p.x)+(y-p.y)*(y-p.y)+(z-p.z)*(z-p.z));
                        uint32_t gap = tick - it->second.tick;
                        double mv = std::sqrt((x-it->second.x)*(x-it->second.x)+(y-it->second.y)*(y-it->second.y)+(z-it->second.z)*(z-it->second.z));
                        if (gap > 0 && gap < 2000 && deta > 0 && deta < 2000 && ms - p.ms < 5000)
                            samps.push_back({ d, mv / gap * 60.0, gap, (uint32_t)deta, sid, veh, tick, bodies, ms, x,y,z, p.x,p.y,p.z });
                    } else if (pit == poses.end()) noPose++;
                    last[key] = { tick, x, y, z };
                }
                off += L;
            }
        }
    }
    printf("recv msgType3: %llu (full %llu), 0x2F poses %llu over %zu peers; 0x81 body0 sends %llu (no pose for peer: %llu); samples %zu\n",
        (unsigned long long)recvMsgs, (unsigned long long)recvFull, (unsigned long long)poseRecs, poses.size(),
        (unsigned long long)sends81, (unsigned long long)noPose, samps.size());
    if (csv) {
        FILE* o = fopen(csv, "w");
        fprintf(o, "peer,veh,tick,ms,dist,gap,deta,speed,bodies,vx,vy,vz,px,py,pz\n");
        for (auto& s : samps)
            fprintf(o, "%llu,%u,%u,%llu,%.1f,%u,%u,%.2f,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                (unsigned long long)s.peer, s.veh, s.tick, (unsigned long long)s.ms, s.dist, s.gap, s.deta,
                s.speed, s.bodies, s.vx, s.vy, s.vz, s.px, s.py, s.pz);
        fclose(o);
        // <csv>.poses: every 0x2F pose (peer, capture ms, world pos) for tile-crossing analysis
        std::string pp = std::string(csv) + ".poses";
        FILE* q = fopen(pp.c_str(), "w");
        fprintf(q, "peer,ms,x,y,z\n");
        for (auto& kv : poses) for (auto& p : kv.second)
            fprintf(q, "%llu,%llu,%.1f,%.1f,%.1f\n", (unsigned long long)kv.first, (unsigned long long)p.ms, p.x, p.y, p.z);
        fclose(q);
    }
    // distance buckets
    const double edges[] = { 0, 25, 50, 100, 150, 200, 300, 400, 500, 750, 1000, 1500, 2000, 3000, 5000, 1e9 };
    int nb = sizeof(edges)/sizeof(edges[0]) - 1;
    printf("\n%-12s %7s %6s %6s %6s %6s %6s\n", "dist[m]", "n", "p10", "p50", "p90", "mean", "spd");
    for (int i = 0; i < nb; i++) {
        std::vector<uint32_t> g; double sum = 0, spd = 0;
        for (auto& s : samps) if (s.dist >= edges[i] && s.dist < edges[i+1]) { uint32_t v = useGap ? s.gap : s.deta; g.push_back(v); sum += v; spd += s.speed; }
        if (g.empty()) continue;
        std::sort(g.begin(), g.end());
        char lab[32]; if (edges[i+1] >= 1e9) snprintf(lab, 32, ">=%.0f", edges[i]); else snprintf(lab, 32, "%.0f-%.0f", edges[i], edges[i+1]);
        printf("%-12s %7zu %6u %6u %6u %6.1f %6.1f\n", lab, g.size(), g[g.size()/10], g[g.size()/2], g[g.size()*9/10], sum/g.size(), spd/g.size());
    }
    return 0;
}
