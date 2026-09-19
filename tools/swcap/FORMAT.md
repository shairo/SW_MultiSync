# .swcap capture container

Binary, one file per capture session, written by `swhook.dll` (`src/hook/dllmain.cpp`) and read by
`tools/swcap/Container.cs` (also `tools/rectest`). A capture is the application-layer payload handed
to Steam by each `SendMessageToUser` call — pre-transport, so what the client actually receives.

```
file header : "SWCAP" u8 version          (6 bytes; version 1 or 2)
per event   : u8  dir        0 = send, 1 = recv, 2 = marker
              u64 seq
              u64 tickMs     GetTickCount64 at capture
              u64 steamId    remote peer (recipient for sends)
              i32 channel
              i32 flags      nSendFlags
              u32 origCub    the frame's true size
              u32 storedCub  bytes actually stored   (version 2 only; v1: = origCub)
              bytes[storedCub]
```

Metadata overhead is ~41 B/event — negligible against payload. Steam likely compresses on the wire,
so actual network bytes are fewer than the file suggests.

## Bulk thinning (version 2)

Frames larger than `kBulkThreshold = 65536` B are stored header-only: the first `kBulkKeepHead = 256`
B (transport header + message header + any filename prefix), payload dropped, `origCub` still
recording the true size. Gameplay `msgType=8` (≤ ~16 KB) is never thinned; map tiles (523,272 B)
and big mod files are. Multi-GB tile-sync sessions become small files while every gameplay frame
stays intact and all sizing stays accurate.

Thinned frames read back as `Frame.Truncated` and are always treated as `Bulk` (never a complete
single-frame message). Frame layout above the container level:
[protocol/transport.md](../../protocol/transport.md).

A file whose last event is cut short (DLL killed mid-write) is read up to the last complete event.
