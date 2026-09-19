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
| `unload` | – | `hooked` — restores the vtable, drains in-flight calls, closes IPC/files and unmaps the DLL (~1 s later). Development aid: `swctl unload && build && swctl inject` iterates without a server restart. All counters/caches reset. |

`startupOnly` keys (`relay`, `capture`, `ipcPort`) change the stored value only; the running
behaviour of those is controlled live via `relay.set` / `capture.*`, and the port needs a restart.

### status fields
`version protocol pid uptimeMs nowMs hooked hookError relay capturing mutate captureFile
captureBytes logFile walkFailFile ipcPort ipcClients ipcRequests peerCount vehicleCount seq marks`
`walk{type8 full partial appendOk appendBad dumped}` — record-walker understanding gate
(`full/type8` = walk rate). `inject{sends appended rewritten bytes}` — relay totals.

### peers[] entry
`steamId connected firstSeenMs lastSeenMs lastTick sendCount sendBytes recvCount recvBytes
injSends injBytes injRecords type8 walkFull walkPartial vehicles`
`sendBytes` = what the game itself sent this peer (native); `injBytes` = extra bytes we added on
top. `connected` is false after the ch15 close byte or 5 s without any traffic to that peer; peers silent
for over 10 minutes are dropped from the list (and from the DLL's table). Counters are per DLL lifetime.

### vehicles[] entry
`id tick pos[3] rot[4] srcGap feeds[{steamId,gap}]`
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
