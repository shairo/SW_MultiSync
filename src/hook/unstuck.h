// unstuck.h — EXPERIMENTAL, investigation-only: `?unstuck` / `?unstuck2` chat commands. No guarantee
// they help; kept out of the main flow so the whole feature can be dropped in one go. Included by
// dllmain.cpp in its middle (uses Nudge / g_nudge / put_chat / tile helpers / write_marker / logf).
#pragma once

// ---- player self-help: `?unstuck` / `?unstuck2` ----
// For the "ghost chunk" stall: the client stops simulating the world on entering a tile while its
// network thread keeps running (tools/swcap/README.md). `?` lines go to addons and never reach the
// chat, so the game ignores these and the DLL — which only watches the client 0x02 — queues, on the
// same g_nudge path as debug.nudge, for the 5x5 tiles around the player's last pose (the ring a teleport streams): `?unstuck` a 0x45 load,
// `?unstuck2` a 0x46 unload then the 0x45 kReloadGapMs later (forced reload); plus a 0x01 chat line to
// that player alone so they see it was taken. Each 0x45 carries the tile's last server-sent u8 (1 if never seen): with 0 the client re-created the
// tile as unpurchased (workbench unusable) on both variants — the server only ever sends 1, for tiles it
// keeps as purchased (the ones with a workbench, it seems); a teleport away and back restored it. Always on (players help themselves, no admin needed);
// per-peer cooldown; every use is logged and marked. Both variants stay until one proves enough.
// Before re-sending, each use reports (log + the player's chat) what the server had last sent that player
// for each ring tile (tiles.h) — to tell "server never sent the load" from "client lost / dropped it".
constexpr uint64_t kUnstuckCooldownMs = 5000;
constexpr int kUnstuckRadius = 2;                             // 5x5 = the ring the server itself streams on a teleport
static std::map<uint64_t, uint64_t> g_unstuckMs;              // sid -> last accepted use
// 1 = "?unstuck", 2 = "?unstuck2", 0 = anything else (exact match: case and blanks count)
static int unstuck_cmd(const uint8_t* s, int n) {
    auto is = [&](const char* c) { return n == (int)strlen(c) && !memcmp(s, c, n); };
    return is("?unstuck") ? 1 : is("?unstuck2") ? 2 : 0;
}
// What the server itself told this player about the 5x5 ring, from the tiles.h ledger, BEFORE our re-send:
// one log line per tile, and a short summary to the player's chat. The question it answers: was the
// tile the client is waiting on never loaded by the server (last word UNLOAD / nothing), loaded but
// never acked (lost on the way), or loaded+acked (the client had it and dropped it)? A tile with no
// ledger entry means the DLL was not watching when it was sent (injected / re-injected after) — then
// the verdict is INSUFFICIENT, whatever the other tiles say.
static void unstuck_report(uint64_t sid, int32_t tx, int32_t tz, uint64_t now, std::vector<std::string>& chat) {
    const tiles::Peer& tp = tiles::at(sid, now);
    int nAck = 0, nNoAck = 0, nUnl = 0, nUnseen = 0;
    logf("== unstuck tiles: peer=%llu ledger=%.1fs gaps=%llu (S=last server record, f=purchased flag, ack=client 0x28 after the load) ==\n",
         (unsigned long long)sid, (now - tp.sinceMs) / 1000.0, (unsigned long long)tp.gaps);
    for (int32_t z = tz + kUnstuckRadius; z >= tz - kUnstuckRadius; --z)
        for (int32_t x = tx - kUnstuckRadius; x <= tx + kUnstuckRadius; ++x) {
            auto it = tp.t.find({ x, z });
            if (it == tp.t.end()) { ++nUnseen; logf("   (%d,%d) UNSEEN\n", x, z); continue; }
            const tiles::Tile& t = it->second;
            const char* st;
            if (t.last == 0x46) { st = "UNLOAD"; ++nUnl; }
            else if (t.ackMs)   { st = "LOAD+ack"; ++nAck; }
            else                { st = "LOAD/no-ack"; ++nNoAck; }
            char ack[32] = ""; if (t.last == 0x45 && t.ackMs) _snprintf_s(ack, _TRUNCATE, " ack=+%llums", (unsigned long long)(t.ackMs - t.ms));
            logf("   (%d,%d) S=%s f=%u tick=%u age=%.1fs loads=%u unloads=%u%s%s\n", x, z, st, (unsigned)t.flag, t.tick,
                 (now - t.ms) / 1000.0, t.loads, t.unloads, ack, t.injMs ? " (re-sent by us before)" : "");
        }
    const char* verdict =
        nUnseen ? "INSUFFICIENT: some tiles were sent before the DLL was watching" :
        nUnl    ? "server's last word for some tiles is UNLOAD" :
        nNoAck  ? "server sent LOAD for all, some never acked by the client" :
                  "server sent LOAD for all and the client acked all";
    logf("== unstuck verdict: peer=%llu load+ack=%d load/no-ack=%d unload=%d unseen=%d -> %s ==\n", (unsigned long long)sid,
         nAck, nNoAck, nUnl, nUnseen, verdict);
    // chat: one line of counts + a one-word verdict (details stay in the log)
    const char* v = nUnseen ? "INSUFFICIENT" : nUnl ? "SERVER-UNLOADED" : nNoAck ? "NOT-ACKED" : "ALL-ACKED";
    char b[160];
    _snprintf_s(b, _TRUNCATE, "tiles: ack %d / noack %d / unload %d / unseen %d%s -> %s", nAck, nNoAck, nUnl, nUnseen,
                tp.gaps ? " / gaps" : "", v);
    chat.push_back(b);
}
static void on_unstuck(uint64_t sid, const stats::Peer& ps, int mode, uint64_t now) {
    uint64_t& last = g_unstuckMs[sid];
    if (last && now - last < kUnstuckCooldownMs) { logf("== unstuck ignored (cooldown): peer=%llu ==\n", (unsigned long long)sid); return; }
    if (!ps.posMs) { logf("== unstuck ignored (no pose yet): peer=%llu ==\n", (unsigned long long)sid); return; }
    last = now;
    int32_t tx = tile_of(ps.px), tz = tile_of(ps.pz);
    bool reload = mode == 2;
    Nudge nd; nd.kind = reload ? "unstuck/0x46" : "unstuck/0x45";
    for (int32_t x = tx - kUnstuckRadius; x <= tx + kUnstuckRadius; ++x)
        for (int32_t z = tz - kUnstuckRadius; z <= tz + kUnstuckRadius; ++z) {
            if (reload) { put_tile_unload(nd.recs, x, z); put_tile_load(nd.follow, x, z); ++nd.followCount; }
            else put_tile_load(nd.recs, x, z);
            ++nd.count;
        }
    uint64_t mk = write_marker();
    logf("== unstuck: peer=%llu name=\"%s\" pos=(%.1f,%.1f,%.1f) tile=(%d,%d) r=%d mode=%d mark=%llu ==\n", (unsigned long long)sid,
         ps.name.c_str(), ps.px, ps.py, ps.pz, tx, tz, kUnstuckRadius, mode, (unsigned long long)mk);
    std::vector<std::string> report;
    unstuck_report(sid, tx, tz, now, report);                 // before on_inject: the ledger as the server left it
    for (int32_t x = tx - kUnstuckRadius; x <= tx + kUnstuckRadius; ++x)
        for (int32_t z = tz - kUnstuckRadius; z <= tz + kUnstuckRadius; ++z) tiles::on_inject(sid, x, z, now);
    for (auto& line : report) { put_chat(nd.recs, line, "[SW_MultiSync]"); ++nd.count; }
    char msg[96]; _snprintf_s(msg, _TRUNCATE, "reloading tiles around (%d, %d)", tx, tz);
    put_chat(nd.recs, msg, "[SW_MultiSync]"); ++nd.count;
    g_nudge[sid] = std::move(nd);
    logflush();
}
