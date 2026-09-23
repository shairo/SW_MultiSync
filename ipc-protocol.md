# swhook control-plane (IPC) protocol — v1

`swhook.dll` listens on **127.0.0.1:`ipcPort`** (default 28215, `swhook.ini`). Transport is plain TCP,
**one JSON object per line** in each direction. Any language that can open a socket can drive the
tool; the bundled GUI / `swctl` use exactly this and nothing else.

```
→ {"cmd":"status"}
← {"ok":true,"version":"0.2.0",...}
```

- A request is a flat object with `cmd` plus command-specific fields. Optional `id` (string) is
  echoed back unchanged so a client may pipeline requests.
- Every response has `ok` (bool). On failure `error` (string) says why.
- Numbers that are counters are **cumulative** since DLL load; `nowMs` (GetTickCount64) is included
  so clients compute rates by differencing two samples.
- Connection is persistent; blank lines are ignored; a line over 64 KB closes the socket.
- A connection starting with `GET ` / `POST ` gets an HTTP `501` — reserved for a future HTTP shim
  (Stormworks addon `server.httpGet`).

## Commands

| cmd | request fields | response (besides `ok`) |
|---|---|---|
| `ping` / `version` | – | `version`, `protocol`, `pid`, `hooked` |
| `status` | – | see *status fields* + `config{}` |
| `stats.get` | – | *status fields* + `peers[]` |
| `peers.get` | – | `nowMs`, `peers[]` |
| `vehicles.get` | – | `nowMs`, `vehicles[]` |
| `relay.set` | `enabled` bool | `relay`, `mutate` |
| `mutate.set` | `enabled` bool | (debug: X+50 offset demo) |
| `capture.start` | – | `captureFile` (rolls a new .swcap) |
| `capture.stop` | – | – |
| `capture.mark` | – | `mark` ordinal (error if not capturing) |
| `config.get` | – | `config{}`, `fields[{name,type,startupOnly,help}]` |
| `config.set` | `key`, `value` | `key`, `value` (after clamping), `startupOnly` |
| `config.reload` | – | `keys` applied, `config{}` |
| `config.save` | – | `path` (writes swhook.ini, comments preserved) |
| `debug.hold` | `peer` (SteamID64), `ms` (0..60000, default 5000) | `peer`, `ms`, `extended` — DROP every send to that peer (the game is told it was sent) until `ms` elapse or `debug.release`; nothing is queued or replayed, so the client gets a real hole in its tick stream. (An earlier queue-and-flush variant only showed the client waiting for ticks and fast-forwarding.) Calling it again extends the deadline; the window ends lazily on the next send after it |
| `debug.release` | `peer` | `peer`, `dropped` — end a hold now |
| `debug.nudge` | `peer`, `kind` (`tp` \| `tile` \| `unload` \| `reload` \| `raw`), optional `x` `y` `z` / `tileX` `tileZ` / `hex` + `count` | `peer`, `kind`, `records`, `bytes`[, `pos[3]`] — append records to the peer's NEXT fully-decoded tick message (one-shot). `tp` = 0x5E fast-travel to x,y,z (default: the player's last pose) + bare 0x09; `tile` = 0x45 load handshake for the tile at x,z (default: the player's own tile); `unload` = 0x46 unload of that tile (the server does not know, so nothing reloads it — a client-side "tile gone" state on demand); `reload` = the 0x46 now and a 0x45 for the same tile 500 ms later (forced tile reload; see the 2026-09-22 experiment in `tools/swcap/README.md`); `raw` = your own records as hex, refused unless they walk with the server-side length rules as exactly `count` records. Freeze-mitigation probes, see `tools/swcap/README.md` |
| `unload` | – | `hooked` — restores the vtable, drains in-flight calls, closes IPC/files and unmaps the DLL (~1 s later). Development aid: `swctl unload && build && swctl inject` iterates without a server restart. All counters/caches reset. |

`startupOnly` keys (`relay`, `capture`, `ipcPort`) change the stored value only; the running
behaviour of those is controlled live via `relay.set` / `capture.*`, and the port needs a restart.

### status fields
`version protocol pid uptimeMs nowMs hooked hookError relay capturing mutate captureFile
captureBytes logFile walkFailFile ipcPort ipcClients ipcRequests peerCount vehicleCount seq marks`
`freezeEvents` — freeze-detector onsets this session (all peers).
`walk{type8 full partial appendOk appendBad dumped}` — record-walker understanding gate
(`full/type8` = walk rate). `inject{sends appended rewritten bytes}` — relay totals.

### peers[] entry
`steamId name connected firstSeenMs lastSeenMs lastTick sendCount sendBytes recvCount recvBytes
injSends injBytes injRecords type8 walkFull walkPartial vehicles
type3 rwalkFull rwalkPartial [sync{…}] [pos[3] posMs]`
`name` is the in-game player name (UTF-8) from the client's join request (RECV msgType=1, see
`protocol/transport.md`); for players who joined before the DLL was injected it is filled from their
first chat line (the server's 0x01 echo carries the sender name, `protocol/ui-chat.md`); `""` until then.
`type3`/`rwalkFull`/`rwalkPartial` are the client→server (msgType=3) counterparts of the walk KPI;
`pos` is the player's last world position from client 0x2F (absent until one is seen), `posMs` its
GetTickCount64 stamp (compare with `nowMs`).
`freeze{frozenSinceMs reason events poseVeh poseStaticMs defReqs defAcks unackedMs pendingDefs[]}` — the
client-freeze detector (`src/hook/freeze.h`, thresholds `freezePoseMs` / `freezeDefMs` in config).
`frozenSinceMs` is 0 while healthy; `reason` is `pose` (the player's 0x2F pose has not changed for
`poseStaticMs` while the vehicle it sits in, `poseVeh`, moved ≥ 20 m on the server), `defs` (a vehicle
definition the server pushed in answer to the client's 0x1B request has had no 0x29 loaded-ack for
`unackedMs`), or both. `pendingDefs` lists the unacked vehicle ids. Each onset also writes a
`FREEZE?` line to the session .log and a capture marker. Present only for peers that sent a pose.
`sync{clientTick clientTps framesBehind tickLag hbMs}` — the client's own sync report from its ~1 Hz
0x34 heartbeat (`protocol/client.md`; the server relays the same clientTps / framesBehind in its 0x05
player-list row, which is what the in-game player list shows as TPS / "+N f"). `clientTick` is the last
world tick the client simulated (0 while it is still loading after a join); `tickLag` = `lastTick` −
`clientTick` when the heartbeat arrived; `hbMs` its GetTickCount64 stamp — treat the block as stale if
it is more than a couple of seconds old. Absent until the first heartbeat.
`sendBytes` = what the game itself sent this peer (native); `injBytes` = extra bytes we added on
top. `connected` is false after the ch15 close byte or 5 s without any traffic to that peer; peers silent
for over 10 minutes are dropped from the list (and from the DLL's table). Counters are per DLL lifetime.

### vehicles[] entry
`id tick pos[3] rot[4] srcGap [group] feeds[{steamId,gap}]`
`group` is the spawn group id (a multi-body spawn shares one; equals the first vehicle's id), present
only for vehicles whose 0x2B placement was seen while hooked.
`srcGap` is I_src: the EMA of ticks between fresh position samples across all native feeds
(0 = unknown yet).
`feeds` lists which recipients receive this vehicle natively and their native gap in ticks
(smallest gap ≈ the peer sitting in / nearest to it).

## Example (Python)

```python
import socket, json
s = socket.create_connection(("127.0.0.1", 28215)); f = s.makefile("rw")
def call(**req):
    f.write(json.dumps(req) + "\n"); f.flush(); return json.loads(f.readline())
print(call(cmd="status")["walk"])
call(cmd="relay.set", enabled=True)
for v in call(cmd="vehicles.get")["vehicles"]: print(v["id"], v["pos"])
```
