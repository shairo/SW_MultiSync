// Offline validation of the DLL's C++ record walker (src/hook/rec.h) against a .swcap. Reads every
// single-frame SEND type=8 ch0 message, runs rec::walk, and reports full%. Compare to
// `swcap validate <file>` — if the full% matches, the in-DLL walker matches the C# decoder.
#include <cstdio>
#include <cstdint>
#include <vector>
#include "../../src/hook/rec.h"

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_rec <file.swcap>\n"); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open\n"); return 1; }
    char hdr[7] = {0}; fread(hdr, 1, 6, f);
    if (hdr[0] != 'S' || hdr[5] != 0x02) { printf("not SWCAP v2\n"); return 1; }

    uint64_t type8 = 0, full = 0, partial = 0, append_ok = 0, append_bad = 0;
    std::vector<uint8_t> buf, scratch;
    for (;;) {
        uint8_t dir;
        if (fread(&dir, 1, 1, f) != 1) break;
        uint64_t seq, tickMs, sid; int32_t ch, flags; uint32_t orig, stored;
        fread(&seq, 8, 1, f); fread(&tickMs, 8, 1, f); fread(&sid, 8, 1, f);
        fread(&ch, 4, 1, f); fread(&flags, 4, 1, f); fread(&orig, 4, 1, f); fread(&stored, 4, 1, f);
        buf.resize(stored);
        if (stored) fread(buf.data(), 1, stored, f);
        if (dir != 0 || ch != 0) continue;             // SEND, ch0
        if (stored != orig || stored < 28) continue;    // fully-stored single frame only
        const uint8_t* b = buf.data();
        if (*reinterpret_cast<const uint32_t*>(b) != 0) continue;   // frag==0 head
        const uint8_t* body = b + 8; int blen = (int)stored - 8;
        if (*reinterpret_cast<const uint32_t*>(body + 4) != 8) continue;   // msgType==8
        type8++;
        rec::Walk w = rec::walk(body, blen);
        if (w.full) full++; else partial++;
        if (w.full && w.first81 >= 0) {                 // mirror the DLL append-sim
            int rl = rec::decode_len(body, blen, w.first81);
            if (rl > 0) {
                scratch.assign(body, body + blen);
                scratch.insert(scratch.end(), body + w.first81, body + w.first81 + rl);
                uint32_t nc = rec::U32(scratch.data(), (int)scratch.size(), 16) + 1;
                memcpy(scratch.data() + 16, &nc, 4);
                rec::Walk w2 = rec::walk(scratch.data(), (int)scratch.size());
                if (w2.full && w2.walked == (int)nc) append_ok++; else append_bad++;
            }
        }
    }
    fclose(f);
    printf("type8 ch0 SEND single-frame: %llu\n", (unsigned long long)type8);
    printf("  full   : %llu (%.2f%%)\n", (unsigned long long)full, type8 ? 100.0 * full / type8 : 0);
    printf("  partial: %llu\n", (unsigned long long)partial);
    printf("  append-sim ok=%llu bad=%llu\n", (unsigned long long)append_ok, (unsigned long long)append_bad);
    return 0;
}
