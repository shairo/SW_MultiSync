using SwCap;

if (args.Length < 2) { Usage(); return 1; }
string cmd = args[0], path = args[1];
string[] rest = args[2..];

switch (cmd)
{
    case "stats": Stats(path); break;
    case "frames": Frames(path, rest); break;
    case "msgs": Msgs(path, rest); break;
    case "hex": Hex(path, rest); break;
    case "frag": Frag.Report(path); break;
    case "validate": Validate(path, rest); break;
    case "walk": Walk(path, rest); break;
    case "census": Census(path, rest); break;
    case "when": When(path, rest); break;
    case "marks": Marks(path); break;
    case "lens": Lens(path, rest); break;
    case "solve": Solve(path, rest); break;
    case "bodytypes": BodyTypes(path); break;
    case "eta": Eta(path, rest); break;
    case "vel": Vel(path, rest); break;
    case "bw": Bw(path, rest); break;
    case "shadow": Shadow.Run(path, rest); break;
    case "resend": Resend(path, rest); break;
    case "frozen": Frozen(path, rest); break;
    case "vehrec": VehRec(path, rest); break;
    case "vehscan": VehScan(path, rest); break;
    case "findid": FindId(path, rest); break;
    case "lifespan": Lifespan(path, rest); break;
    case "unz": Unz(path, rest); break;
    case "attick": AtTick(path, rest); break;
    case "rec": Track.Rec(path, int.TryParse(Opt(rest, "--take"), out var rt) ? rt : 60); break;
    case "track": Track.TrackId(path, uint.Parse(Opt(rest, "--id")),
                    int.TryParse(Opt(rest, "--take"), out var kt) ? kt : 200); break;
    case "diff":
        Diff.DiffTwo(path, ulong.Parse(Opt(rest, "--a")), ulong.Parse(Opt(rest, "--b")));
        break;
    case "churn":
        Diff.Churn(path,
            Opt(rest, "--dir", "S") == "R" ? Dir.Recv : Dir.Send,
            int.TryParse(Opt(rest, "--ch"), out var cc) ? cc : 0,
            uint.TryParse(Opt(rest, "--type"), out var tt) ? tt : 8,
            int.TryParse(Opt(rest, "--top"), out var tp) ? tp : 20);
        break;
    default: Usage(); return 1;
}
return 0;

static void Usage()
{
    Console.WriteLine("""
    swcap — Stormworks .swcap capture analyzer

      swcap stats  <file>                       channel/type/direction breakdown
      swcap frames <file> [--dir S|R] [--ch N] [--take N]
                                                raw transport frames (pre-reassembly)
      swcap msgs   <file> [--dir S|R] [--ch N] [--type N] [--take N]
                                                reassembled logical messages + header fields
      swcap hex    <file> --seq N [--body] [--max N]
                                                hex dump of a frame (or its reassembled body)
      swcap frag   <file>                        single-frame vs game-level fragmentation
      swcap validate <file> [--peer SteamID]     walk records with known length rules (% decoded)
      swcap walk   <file> --seq N | --tag N       record breakdown of one message + undecoded tail
      swcap census <file>                         per-tag count + timing (start/end/spread)
      swcap when   <file> --tag N                 every occurrence of a tag (tick + t+ms) for repro
      swcap marks  <file>                         F8 markers (t+ms) dropped live during capture
      swcap bodytypes <file>                      0x81 body-type histogram (unknown-type check)
      swcap eta    <file> [--peer S]              0x81 ETA delta (time - tick) + real sync spacing
      swcap vel    <file> [--id N] [--peer S]     dead-reckoning viability: velocity + 1-interval
                                                extrapolation error vs hold (no --id lists vehicles)
      swcap bw     <file> [--interval N] [--dense N] [--tps N]
                                                relay bandwidth budget: native vs projected relay
                                                Mbps per recipient at a target interval
      swcap shadow <file> [--lod] [--interval N] [--lag N] [--dense N] [--stale N]
                          [--maxspeed N] [--target-ms N] [--top N]
                                                stage-A causal sim: native vs relayed position
                                                error / time-lag / %under-target / relay Mbps.
                                                --lod: per-vehicle I=gap/4[5,60], Λ=I/2
      swcap resend <file> [--peer S]             resend/rewind traces: non-monotonic world tick
                                                per send stream + 0x81 same-tick/ETA-back reappear
      swcap frozen <file> [--eps F] [--tail N]   vehicles whose pose stops updating at session end;
                                                distinguishes "still syncing" vs "went silent"
      swcap vehrec <file> --id N [--peer S] [--last K]
                                                one vehicle's 0x81 records in tick order (tick, dETA,
                                                bodyCount, b0type, move, pos). --peer = one recipient
      swcap vehscan <file> --id N [--peer S] [--jump F]
                                                pose-sanity scan of every body (type1/2): non-finite,
                                                zero/bad quat, absurd pos, per-tick move spikes
      swcap findid <file> --id N [--tags] [--near]
                                                find a u32 id at any record offset (offset histogram)
      swcap lifespan <file> [--near T]           per-vehicle 0x81 first/last tick + sample count
      swcap unz    <file> --id N                 inflate zlib payload of a 0x2F record + hex dump
      swcap attick <file> --at T [--w N]         records in a tick window (flood tags filtered)
      swcap diff   <file> --a SEQ --b SEQ        byte diff of two message bodies
      swcap churn  <file> [--dir S|R] [--ch N] [--type N] [--top N]
                                                per-offset change frequency across a stream
                                                (static vs live fields)
    """);
}

static string Opt(string[] a, string key, string def = "")
{
    for (int i = 0; i < a.Length - 1; i++) if (a[i] == key) return a[i + 1];
    return def;
}
static bool Flag(string[] a, string key) => Array.IndexOf(a, key) >= 0;
// --peer <SteamID>: restrict to one recipient (multiplayer captures interleave per-peer streams).
static ulong? PeerOpt(string[] a) => ulong.TryParse(Opt(a, "--peer"), out var p) ? p : null;

// Solve an undecoded tag's length WITHOUT guessing its fields: for each message blocked at tag N,
// try every candidate end offset e; accept e only if a known record starts at e AND the remaining
// records walk with known rules to EXACTLY the body end. That "the rest closes perfectly" check is
// self-validating (a wrong e almost never closes), so the collected lengths are ground truth even
// when N is followed by more records (not just last). Prints a length histogram + the header of the
// shortest sample and, when a u32 length field is present, tries to spot it.
static void Solve(string path, string[] a)
{
    if (!uint.TryParse(Opt(a, "--tag"), out var tag)) { Console.WriteLine("need --tag N"); return; }
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0);

    // walk known records from `start`; true iff every record is known-Ok and we land exactly at end.
    static bool ClosesFrom(byte[] body, int start, int end)
    {
        int off = start;
        while (off < end)
        {
            var r = Records.Decode(body, off);
            if (r.Status != RecStatus.Ok || r.Length <= 0) return false;
            off += r.Length;
        }
        return off == end;
    }

    var hist = new SortedDictionary<int, int>();
    var byLen = new SortedDictionary<int, byte[]>();       // one representative sample per distinct length
    int solved = 0, blocked = 0;
    foreach (var m in msgs)
    {
        int off = 20; bool reached = false;
        while (off < m.Body.Length)                        // walk known records up to first unknown
        {
            var r = Records.Decode(m.Body, off);
            if (r.Tag == tag) { reached = true; break; }
            if (r.Status != RecStatus.Ok || r.Length <= 0) break;
            off += r.Length;
        }
        if (!reached) continue;
        blocked++;
        for (int e = off + 5; e <= m.Body.Length; e++)     // smallest end that closes the message
        {
            if (e < m.Body.Length && Records.Decode(m.Body, e).Status != RecStatus.Ok) continue;
            if (ClosesFrom(m.Body, e, m.Body.Length))
            {
                int len = e - off; hist[len] = hist.GetValueOrDefault(len) + 1; solved++;
                if (!byLen.ContainsKey(len)) byLen[len] = m.Body.AsSpan(off, len).ToArray();
                break;
            }
        }
    }
    Console.WriteLine($"tag 0x{tag:X2} ({tag}): blocked in {blocked} msgs, solved {solved}\nlength histogram:");
    foreach (var kv in hist) Console.WriteLine($"  len {kv.Key,-5} : {kv.Value}");

    // Auto length-field finder: an offset o where (len - fieldValue@o) is constant across ALL distinct
    // lengths reveals length = C + field. Try u32 and u16 at every offset present in every sample.
    var samples = byLen.Values.ToList();
    if (samples.Count >= 2)
    {
        int minLen = byLen.Keys.First();
        Console.WriteLine("\n-- length-field candidates (len = C + field@o) --");
        bool found = false;
        for (int o = 4; o + 4 <= minLen; o++)
        {
            var c32 = samples.Select(s => s.Length - (long)BitConverter.ToUInt32(s, o)).Distinct().ToList();
            if (c32.Count == 1) { Console.WriteLine($"  len = {c32[0]} + u32@{o}   ✓"); found = true; }
        }
        for (int o = 4; o + 2 <= minLen; o++)
        {
            var c16 = samples.Select(s => s.Length - (long)BitConverter.ToUInt16(s, o)).Distinct().ToList();
            if (c16.Count == 1) { Console.WriteLine($"  len = {c16[0]} + u16@{o}   ✓"); found = true; }
        }
        if (!found) Console.WriteLine("  (no single u16/u32 header field predicts length — nested/multi-field)");
    }
    if (Flag(a, "--dump"))
        foreach (var s in samples.Take(4))
        { Console.WriteLine($"\nsample len={s.Length}:"); Dump(s, Math.Min(s.Length, 96)); }
    else if (samples.Count > 0) { Console.WriteLine($"\nshortest sample ({samples[0].Length} B):"); Dump(samples[0], 96); }
}

// Harvest GROUND-TRUTH lengths for an undecoded tag: find messages where the walk reaches tag N
// (so its start is pinned by known records before it) AND N is the last record (so its end is the
// body end) → exact length = bodyLen - start. Prints a length histogram + the leading bytes of the
// shortest sample, so you can spot a fixed length or the field that encodes a variable one.
static void Lens(string path, string[] a)
{
    if (!uint.TryParse(Opt(a, "--tag"), out var tag)) { Console.WriteLine("need --tag N"); return; }
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0);
    var hist = new SortedDictionary<int, int>();
    byte[]? shortest = null; int shortestLen = int.MaxValue;
    int pinned = 0;
    foreach (var m in msgs)
    {
        // walk known records manually; stop at first unknown. If that unknown is tag N and it's the
        // LAST record slot, its byte range is fully pinned.
        int off = 20, i = 0; bool ok = true;
        for (; i < m.RecordCount; i++)
        {
            if (off >= m.Body.Length) { ok = false; break; }
            var r = Records.Decode(m.Body, off);
            if (r.Tag == tag) break;                       // reached our tag with a pinned start
            if (r.Status != RecStatus.Ok || r.Length <= 0) { ok = false; break; }
            off += r.Length;
        }
        if (!ok || i != m.RecordCount - 1) continue;       // must be the last record slot
        if (off >= m.Body.Length || Records.Decode(m.Body, off).Tag != tag) continue;
        int len = m.Body.Length - off;
        hist[len] = hist.GetValueOrDefault(len) + 1; pinned++;
        if (len < shortestLen) { shortestLen = len; shortest = m.Body.AsSpan(off, len).ToArray(); }
    }
    Console.WriteLine($"tag 0x{tag:X2} ({tag}): {pinned} pinned samples (last-record cases)\nlength histogram:");
    foreach (var kv in hist) Console.WriteLine($"  len {kv.Key,-5} : {kv.Value}");
    if (shortest != null) { Console.WriteLine($"\nshortest sample ({shortestLen} B):"); Dump(shortest, 128); }
}

// List F8 markers (dir=2 events) on the same t+ms clock as `when`, so an action you marked live can
// be lined up against the record traffic around it. SteamId field carries the marker ordinal.
static void Marks(string path)
{
    var frames = Container.Read(path).ToList();
    ulong t0 = frames.Count > 0 ? frames.Min(f => f.TickMs) : 0;
    var marks = frames.Where(f => f.Dir == Dir.Marker).ToList();
    // map each marker's global seq to the world tick of the nearest preceding send message
    var mm = Message.Reassemble(frames)
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8)
        .Select(m => (m.StartSeq, m.Tick)).OrderBy(x => x.StartSeq).ToList();
    uint TickAt(ulong seq) { uint best = 0; foreach (var x in mm) { if (x.StartSeq <= seq) best = x.Tick; else break; } return best; }
    Console.WriteLine($"{"mark",-5} {"t+ms",-8} {"seq",-8} {"~tick",-8}");
    foreach (var f in marks)
        Console.WriteLine($"#{f.SteamId,-4} {f.TickMs - t0,-8} {f.Seq,-8} {TickAt(f.Seq),-8}");
    Console.WriteLine($"\n{marks.Count} markers");
}

// List every occurrence of one tag: tick + wall-clock ms + the containing message's seq + record
// start offset. Run a targeted scenario capture, note when you did each action, then match your
// action times against these rows to confirm which tag an action produces.
static void When(string path, string[] a)
{
    if (!uint.TryParse(Opt(a, "--tag"), out var tag)) { Console.WriteLine("need --tag N"); return; }
    var peer = PeerOpt(a);
    var frames = Container.Read(path).ToList();
    ulong t0 = frames.Count > 0 ? frames.Min(f => f.TickMs) : 0;
    var msgs = Message.Reassemble(frames)
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0
                    && (peer is null || m.SteamId == peer));
    Console.WriteLine($"{"tick",-8} {"t+ms",-8} {"seq",-8} {"recStart",-8} note");
    int n = 0;
    foreach (var m in msgs)
    {
        // need the containing frame's TickMs; StartSeq maps to the head frame
        ulong ms = frames.First(f => f.Seq == m.StartSeq).TickMs - t0;
        foreach (var r in Records.Walk(m).recs)
            if (r.Tag == tag)
            { Console.WriteLine($"{m.Tick,-8} {ms,-8} {m.StartSeq,-8} @{r.Start,-7} {r.Note}"); n++; }
    }
    Console.WriteLine($"\n{n} occurrences of tag 0x{tag:X2} ({tag})");
}

// Per-tag timing census: for every record tag, count + first/last tick + how many distinct ticks
// it spans, and whether it clusters at the start, end, or is spread through the session. Reveals
// which rare tags are spawn/join events (early), teardown/leave events (late), or periodic.
static void Census(string path, string[] a)
{
    var peer = PeerOpt(a);
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0
                    && (peer is null || m.SteamId == peer))
        .ToList();
    uint tMin = msgs.Min(m => m.Tick), tMax = msgs.Max(m => m.Tick);
    double span = Math.Max(1, tMax - tMin);

    var ticks = new Dictionary<uint, List<uint>>();     // tag -> ticks it appeared at
    foreach (var m in msgs)
        foreach (var r in Records.Walk(m).recs)
            (ticks.TryGetValue(r.Tag, out var l) ? l : ticks[r.Tag] = new()).Add(m.Tick);

    Console.WriteLine($"session tick range: {tMin}..{tMax}  ({tMax - tMin} ticks)\n");
    Console.WriteLine($"{"tag",-10} {"count",6} {"firstT",8} {"lastT",8} {"span%",6}  where");
    foreach (var kv in ticks.OrderByDescending(k => k.Value.Count))
    {
        var ts = kv.Value; uint f = ts.Min(), l = ts.Max();
        double startPct = 100.0 * (f - tMin) / span, endPct = 100.0 * (l - tMin) / span;
        double coverage = 100.0 * (l - f) / span;
        string where = coverage > 60 ? "spread/periodic"
                     : startPct < 10 && endPct < 20 ? "START burst"
                     : endPct > 85 && startPct > 70 ? "END burst"
                     : $"@{startPct:F0}%..{endPct:F0}%";
        Console.WriteLine($"0x{kv.Key:X2} ({kv.Key,3}) {ts.Count,6} {f,8} {l,8} {coverage,5:F0}%  {where}");
    }
}

// Relay bandwidth budget from a real capture. Measures actual 0x81 record sizes and, per
// (recipient B, vehicle V), the native sync spacing. A vehicle is a "relay candidate for B" when
// B already receives it (server deems it relevant) but SLOWLY (gap > target interval) AND some OTHER
// peer receives it densely (a fresh source exists). Projected relay cost = sending each candidate at
// the target interval. Reports current native 0x81 traffic and projected added relay traffic, per
// recipient and total, so a bandwidth cap / target interval can be sized against the 70 Mbps uplink.
static void Bw(string path, string[] a)
{
    int interval = int.TryParse(Opt(a, "--interval"), out var iv) ? iv : 20;   // target relay interval (ticks)
    int dense    = int.TryParse(Opt(a, "--dense"), out var dv) ? dv : 10;       // "fresh source" gap threshold
    double tps   = double.TryParse(Opt(a, "--tps"), out var tv) ? tv : 60;
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0)
        .OrderBy(m => m.Tick).ToList();

    var sizes = new List<int>();
    // (peer,veh) -> (firstTick,lastTick,count) for spacing; and per-peer native 0x81 byte totals
    var span = new Dictionary<(ulong, uint), (uint f, uint l, int n)>();
    var nativeBytes = new Dictionary<ulong, long>();
    uint tMin = uint.MaxValue, tMax = 0;
    foreach (var m in msgs)
    {
        tMin = Math.Min(tMin, m.Tick); tMax = Math.Max(tMax, m.Tick);
        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Tag != 0x81 || r.Status != RecStatus.Ok) continue;
            sizes.Add(r.Length);
            nativeBytes[m.SteamId] = nativeBytes.GetValueOrDefault(m.SteamId) + r.Length;
            uint veh = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 4));
            var k = (m.SteamId, veh);
            if (span.TryGetValue(k, out var s)) span[k] = (s.f, m.Tick, s.n + 1);
            else span[k] = (m.Tick, m.Tick, 1);
        }
    }
    if (sizes.Count == 0) { Console.WriteLine("no 0x81 records"); return; }
    double secs = Math.Max(1, (tMax - tMin) / tps);
    double sMean = sizes.Average();
    var peers = span.Keys.Select(k => k.Item1).Distinct().ToList();

    // best (smallest) native gap per vehicle across peers = is a fresh source available?
    double GapOf((ulong, uint) k) { var s = span[k]; return s.n > 1 ? (double)(s.l - s.f) / (s.n - 1) : double.PositiveInfinity; }
    var bestGap = new Dictionary<uint, double>();
    foreach (var k in span.Keys) bestGap[k.Item2] = Math.Min(bestGap.GetValueOrDefault(k.Item2, double.PositiveInfinity), GapOf(k));

    Console.WriteLine($"peers={peers.Count} vehicles={bestGap.Count} dur={secs:F0}s  0x81 size: mean={sMean:F0} p50={sizes.OrderBy(x=>x).ElementAt(sizes.Count/2)} max={sizes.Max()}");
    Console.WriteLine($"target relay interval={interval}t (~{interval/tps*1000:F0}ms), fresh-source gap<= {dense}t\n");
    Console.WriteLine($"{"peer",-20} {"nativeKbps",11} {"relayVeh",8} {"relayKbps",10}");
    double totNative = 0, totRelay = 0; int totCand = 0;
    foreach (var B in peers)
    {
        double natKbps = nativeBytes[B] * 8 / 1000.0 / secs;
        int cand = 0;
        foreach (var k in span.Keys.Where(k => k.Item1 == B))
        {
            uint V = k.Item2;
            if (GapOf(k) > interval && bestGap[V] <= dense) cand++;   // B gets V slowly, but a fresh source exists
        }
        double relayKbps = cand * sMean * 8 / 1000.0 * (tps / interval);
        Console.WriteLine($"{B,-20} {natKbps,11:F0} {cand,8} {relayKbps,10:F0}");
        totNative += natKbps; totRelay += relayKbps; totCand += cand;
    }
    Console.WriteLine($"\nTOTAL native={totNative/1000:F2} Mbps  +relay={totRelay/1000:F2} Mbps  = {(totNative+totRelay)/1000:F2} Mbps  ({totCand} relay streams)");
    double per = peers.Count > 0 ? totRelay / peers.Count : 0;
    Console.WriteLine($"per-peer avg relay={per/1000:F3} Mbps  →  extrapolate to 10 peers ≈ {per*10/1000:F2} Mbps relay (scales ~O(peers×vehicles))");
}

// Dead-reckoning viability from logs. For a vehicle's root body (body0, always type1) we extract the
// double[3] position series (per peer stream, in tick order). Then, per consecutive triple, we test
// one-interval-ahead constant-velocity extrapolation vs. no extrapolation (hold last pose):
//   v          = (P_i - P_{i-1}) / (t_i - t_{i-1})
//   predicted  = P_i + v*(t_{i+1} - t_i)
//   err_pred   = |predicted - P_{i+1}|     (dead reckoning residual)
//   err_hold   = |P_i        - P_{i+1}|     (what plain relay/hold is off by at t_{i+1})
// If err_pred << err_hold, extrapolation removes most of the lag. With no --id, lists vehicles.
static void Vel(string path, string[] a)
{
    var peer = PeerOpt(a);
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0
                    && (peer is null || m.SteamId == peer))
        .OrderBy(m => m.Tick).ToList();

    // (peer,veh) -> ordered samples (tick, x,y,z)
    var series = new Dictionary<(ulong, uint), List<(uint t, double x, double y, double z)>>();
    foreach (var m in msgs)
        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Tag != 0x81 || r.Status != RecStatus.Ok) continue;
            int p = r.Start + 14;                       // tag4+veh4+time4+bodyCount2
            if (p >= m.Body.Length || m.Body[p] != 1) continue;   // body0 must be type1
            uint veh = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 4));
            int posOff = p + 1 + 16;                     // skip type byte + float[4] rotation
            if (posOff + 24 > m.Body.Length) continue;
            double x = BitConverter.ToDouble(m.Body, posOff);
            double y = BitConverter.ToDouble(m.Body, posOff + 8);
            double z = BitConverter.ToDouble(m.Body, posOff + 16);
            var key = (m.SteamId, veh);
            (series.TryGetValue(key, out var l) ? l : series[key] = new()).Add((m.Tick, x, y, z));
        }

    if (!uint.TryParse(Opt(a, "--id"), out var wantId))
    {
        Console.WriteLine($"{"peer",-20} {"veh",5} {"samples",8} {"meanGap",8}");
        foreach (var kv in series.OrderByDescending(k => k.Value.Count))
        {
            var s = kv.Value; double mg = s.Count > 1 ? (double)(s[^1].t - s[0].t) / (s.Count - 1) : 0;
            Console.WriteLine($"{kv.Key.Item1,-20} {kv.Key.Item2,5} {s.Count,8} {mg,8:F1}");
        }
        Console.WriteLine("\npick one with --id N [--peer S] for velocity + extrapolation-error stats");
        return;
    }

    var preds = new List<double>(); var holds = new List<double>(); var moves = new List<double>();
    var speeds = new List<double>(); var gaps = new List<double>();
    foreach (var kv in series.Where(k => k.Key.Item2 == wantId))
    {
        var s = kv.Value;
        for (int i = 1; i < s.Count; i++)
        {
            double dt = s[i].t - s[i - 1].t; if (dt <= 0) continue;
            double dx = s[i].x - s[i-1].x, dy = s[i].y - s[i-1].y, dz = s[i].z - s[i-1].z;
            speeds.Add(Math.Sqrt(dx*dx+dy*dy+dz*dz) / dt); gaps.Add(dt);
        }
        for (int i = 2; i < s.Count; i++)
        {
            double dt1 = s[i-1].t - s[i-2].t, dt2 = s[i].t - s[i-1].t;
            if (dt1 <= 0 || dt2 <= 0) continue;
            double vx = (s[i-1].x - s[i-2].x)/dt1, vy = (s[i-1].y - s[i-2].y)/dt1, vz = (s[i-1].z - s[i-2].z)/dt1;
            double px = s[i-1].x + vx*dt2, py = s[i-1].y + vy*dt2, pz = s[i-1].z + vz*dt2;
            double ep = Dist(px,py,pz, s[i].x,s[i].y,s[i].z);
            double eh = Dist(s[i-1].x,s[i-1].y,s[i-1].z, s[i].x,s[i].y,s[i].z);
            preds.Add(ep); holds.Add(eh); moves.Add(eh);
        }
    }
    if (speeds.Count == 0) { Console.WriteLine($"no samples for veh {wantId}"); return; }
    Console.WriteLine($"veh {wantId}: {speeds.Count+1} samples across streams");
    Console.WriteLine($"gap  ticks  : {Pct(gaps)}");
    Console.WriteLine($"speed u/tick : {Pct(speeds)}   (×~60 ≈ u/sec)");
    if (preds.Count > 0)
    {
        Console.WriteLine($"\none-interval-ahead error (units):");
        Console.WriteLine($"  hold (no extrap) : {Pct(holds)}");
        Console.WriteLine($"  dead-reckon      : {Pct(preds)}");
        double mp = Median(preds), mh = Median(holds);
        Console.WriteLine($"  median reduction : {(mh>0?100*(1-mp/mh):0):F0}%  (dead-reckon err / hold err = {(mh>0?mp/mh:0):F2})");
    }
}
static double Dist(double x,double y,double z,double x2,double y2,double z2)
    => Math.Sqrt((x-x2)*(x-x2)+(y-y2)*(y-y2)+(z-z2)*(z-z2));
static double Median(List<double> v){ var s=v.OrderBy(t=>t).ToList(); return s.Count==0?0:s[s.Count/2]; }
static string Pct(List<double> v){ var s=v.OrderBy(t=>t).ToList(); if(s.Count==0)return "-";
    double q(double f){ return s[(int)Math.Clamp(f*(s.Count-1),0,s.Count-1)]; }
    return $"p50={q(.5):F3} p90={q(.9):F3} p99={q(.99):F3} max={s[^1]:F3}"; }

// Freeze hunter. For each vehicle, build the body0 (root) position series in tick order (merged
// across recipient streams, deduped per tick). Find the LAST tick at which the root actually moved
// (> eps between consecutive samples). A vehicle whose root then stays put for a long tail while
// records KEEP arriving is "frozen but still syncing" — the vanilla stuck-vehicle signature. Also
// report vehicles that simply go silent (no more records). Prints, per suspicious vehicle, the
// moment of freeze and the surrounding native records so an anomaly (bodyCount/type change, a spike,
// a specific tag) can be spotted. Use --eps to set the movement threshold, --tail min frozen ticks.
static void Frozen(string path, string[] a)
{
    double eps  = double.TryParse(Opt(a, "--eps"), out var ev) ? ev : 0.05;   // units/sample "moved"
    int    tail = int.TryParse(Opt(a, "--tail"), out var tv) ? tv : 120;       // min frozen ticks to flag
    var frames = Container.Read(path).ToList();
    var msgs = Message.Reassemble(frames)
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0)
        .OrderBy(m => m.Tick).ToList();
    uint tMax = msgs.Count > 0 ? msgs.Max(m => m.Tick) : 0;

    // veh -> tick-ordered unique (tick,x,y,z); and veh -> all ticks it got any 0x81 (freeze-but-syncing)
    var pos = new Dictionary<uint, SortedDictionary<uint,(double x,double y,double z)>>();
    var anyRec = new Dictionary<uint, SortedSet<uint>>();
    foreach (var m in msgs)
        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Tag != 0x81 || r.Status != RecStatus.Ok) continue;
            uint veh = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 4));
            (anyRec.TryGetValue(veh, out var s) ? s : anyRec[veh] = new()).Add(m.Tick);
            int p = r.Start + 14;
            if (p >= m.Body.Length || m.Body[p] != 1) continue;       // body0 type1 carries position
            int po = p + 1 + 16;
            if (po + 24 > m.Body.Length) continue;
            double x = BitConverter.ToDouble(m.Body, po), y = BitConverter.ToDouble(m.Body, po + 8), z = BitConverter.ToDouble(m.Body, po + 16);
            var d = pos.TryGetValue(veh, out var pp) ? pp : pos[veh] = new();
            d[m.Tick] = (x, y, z);
        }

    Console.WriteLine($"session tickMax={tMax}  eps={eps} tailMin={tail}\n");
    Console.WriteLine($"{"veh",6} {"samples",8} {"lastMoveT",10} {"frozenFor",10} {"recsAfter",10} {"stillSync",9}  note");
    int flagged = 0;
    foreach (var kv in pos.OrderBy(k => k.Key))
    {
        var series = kv.Value.ToList();
        if (series.Count < 3) continue;
        uint lastMove = series[0].Key;
        for (int i = 1; i < series.Count; i++)
        {
            var A = series[i-1].Value; var B = series[i].Value;
            double dd = Math.Sqrt(Sq(B.x-A.x)+Sq(B.y-A.y)+Sq(B.z-A.z));
            if (dd > eps) lastMove = series[i].Key;
        }
        long frozenFor = (long)tMax - lastMove;
        if (frozenFor < tail) continue;                               // still moving late enough = fine
        int recsAfter = anyRec[kv.Key].Count(t => t > lastMove);      // did records keep coming after it froze?
        bool stillSync = recsAfter > 5;
        string note = stillSync ? "FROZEN but server keeps sending (vanilla stuck signature)"
                                : "went silent (no more records)";
        string sync = stillSync ? "yes" : "no";
        Console.WriteLine($"{kv.Key,6} {series.Count,8} {lastMove,10} {frozenFor,10} {recsAfter,10} {sync,9}  {note}");
        flagged++;
    }
    if (flagged == 0) Console.WriteLine("(no vehicle frozen for the tail window — try a smaller --tail or larger --eps)");
    else Console.WriteLine($"\n{flagged} vehicle(s) frozen for >= {tail} ticks at end. Inspect one with:  swcap track <file> --id N");
}
static double Sq(double v) => v * v;

// List every record (tag, tick, length, first bytes) whose message tick falls in [T-w, T+w]. Used to
// see exactly what the server sent around an F8-marked event (a despawn/unload). --at T --w N.
static void AtTick(string path, string[] a)
{
    if (!long.TryParse(Opt(a, "--at"), out var at)) { Console.WriteLine("need --at T"); return; }
    int w = int.TryParse(Opt(a, "--w"), out var wv) ? wv : 12;
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0
                    && Math.Abs((long)m.Tick - at) <= w)
        .OrderBy(m => m.Tick).ToList();
    Console.WriteLine($"records with tick in [{at-w},{at+w}]:");
    Console.WriteLine($"{"tick",8} {"tag",-10} {"len",5}  firstBytes");
    var tagCount = new SortedDictionary<uint,int>();
    foreach (var m in msgs)
        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Status != RecStatus.Ok) continue;
            tagCount[r.Tag] = tagCount.GetValueOrDefault(r.Tag) + 1;
            // only print the less-common tags (skip the per-frame flood 0x81/0x8E/0xA8/0x05/0x2E)
            if (r.Tag is 0x81 or 0x8E or 0xA8 or 0x05 or 0x2E or 0x1B) continue;
            var bytes = m.Body.AsSpan(r.Start, Math.Min(r.Length, 20)).ToArray();
            Console.WriteLine($"{m.Tick,8} 0x{r.Tag:X2} ({r.Tag,3}) {r.Length,5}  {string.Join(" ", bytes.Select(x=>x.ToString("X2")))}");
        }
    Console.WriteLine("\ntag counts in window:");
    foreach (var kv in tagCount.OrderByDescending(k=>k.Value)) Console.Write($"0x{kv.Key:X2}:{kv.Value}  ");
    Console.WriteLine();
}

// Inflate the zlib payload of every 0x2F (compressed per-vehicle state) record for a given vehicle id
// and hex-dump the decompressed bytes, tagging each with its tick. Lets us diff a vehicle's state at
// load vs at despawn/unload to find the lifecycle flag hidden inside the compressed blob. The 0x2F
// layout is tag(4) id(4) rawLen(4) 00 01 00 00, then a zlib stream (78 01) at record offset 16.
static void Unz(string path, string[] a)
{
    if (!uint.TryParse(Opt(a, "--id"), out var id)) { Console.WriteLine("need --id N"); return; }
    int max = int.TryParse(Opt(a, "--max"), out var mv) ? mv : 128;
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0)
        .OrderBy(m => m.Tick).ToList();
    int shown = 0;
    foreach (var m in msgs)
        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Tag != 0x2F || r.Status != RecStatus.Ok) continue;
            uint veh = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 4));
            if (veh != id) continue;
            int z = r.Start + 16;                                   // zlib stream start
            byte[] outb;
            try {
                using var ms = new MemoryStream(m.Body, z, r.Start + r.Length - z);
                using var zs = new System.IO.Compression.ZLibStream(ms, System.IO.Compression.CompressionMode.Decompress);
                using var o  = new MemoryStream();
                zs.CopyTo(o); outb = o.ToArray();
            } catch (Exception e) { Console.WriteLine($"tick {m.Tick}: inflate failed ({e.Message})"); continue; }
            Console.WriteLine($"\n=== veh {id} tick {m.Tick} : 0x2F rawLen={r.Length} inflated={outb.Length} B ===");
            Dump(outb, Math.Min(max, outb.Length));
            if (++shown >= (int.TryParse(Opt(a, "--take"), out var tk) ? tk : 6)) return;
        }
    if (shown == 0) Console.WriteLine($"no 0x2F for veh {id}");
}

// Per-vehicle 0x81 lifespan: first tick, last tick, sample count. A vehicle's last 0x81 tick ≈ when
// it stopped being synced (unload/despawn); first ≈ load. Sort by last tick so disappearances line up
// with F8 markers (swcap marks shows each marker's ~tick). --near T lists only vehicles whose last
// 0x81 is within 150 ticks of T (the vehicle that vanished at that marker).
static void Lifespan(string path, string[] a)
{
    long near = long.TryParse(Opt(a, "--near"), out var nv) ? nv : -1;
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0)
        .OrderBy(m => m.Tick).ToList();
    var life = new Dictionary<uint,(uint f, uint l, int n)>();
    foreach (var m in msgs)
        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Tag != 0x81 || r.Status != RecStatus.Ok) continue;
            uint veh = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 4));
            life[veh] = life.TryGetValue(veh, out var e) ? (Math.Min(e.f, m.Tick), Math.Max(e.l, m.Tick), e.n + 1) : (m.Tick, m.Tick, 1);
        }
    Console.WriteLine($"{"veh",6} {"firstT",8} {"lastT",8} {"span",7} {"samples",8}");
    foreach (var kv in life.OrderBy(k => k.Value.l))
    {
        if (near >= 0 && Math.Abs((long)kv.Value.l - near) > 150) continue;
        Console.WriteLine($"{kv.Key,6} {kv.Value.f,8} {kv.Value.l,8} {kv.Value.l-kv.Value.f,7} {kv.Value.n,8}");
    }
}

// Find every record (any tag) whose bytes contain a given u32 value (a vehicle id) at any offset —
// used to trace a vehicle's whole lifecycle (spawn/load/unload/despawn), not just its 0x81 syncs.
// Prints one line per (tag) with count + tick range, then a timeline of the tail occurrences so a
// despawn/unload record right after the vehicle's last sync is visible. --near T focuses the timeline
// around tick T; --tags 129,45,.. restricts to certain tags; --last N limits timeline rows.
static void FindId(string path, string[] a)
{
    if (!uint.TryParse(Opt(a, "--id"), out var id)) { Console.WriteLine("need --id N"); return; }
    int lastN = int.TryParse(Opt(a, "--last"), out var lv) ? lv : 60;
    long near = long.TryParse(Opt(a, "--near"), out var nv) ? nv : -1;
    var wantTags = (Opt(a, "--tags") ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries)
                    .Select(s => uint.TryParse(s, out var t) ? t : 0xFFFF).ToHashSet();
    byte[] pat = BitConverter.GetBytes(id);
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0)
        .OrderBy(m => m.Tick).ToList();

    var perTag = new SortedDictionary<uint,(int n, uint f, uint l)>();
    var hits = new List<(uint tick, uint tag, int off, int len)>();
    var seen = new HashSet<(uint,uint,uint)>();      // dedup (tick,tag,off-in-rec) across peer streams
    foreach (var m in msgs)
        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Status != RecStatus.Ok) continue;
            if (wantTags.Count > 0 && !wantTags.Contains(r.Tag)) continue;
            // search the record's bytes for the id (skip the 0x81 vehicle-id field at +4 to avoid noise? no, keep it)
            var span = m.Body.AsSpan(r.Start, r.Length);
            for (int o = 0; o + 4 <= span.Length; o++)
                if (span[o]==pat[0]&&span[o+1]==pat[1]&&span[o+2]==pat[2]&&span[o+3]==pat[3])
                {
                    if (!seen.Add((m.Tick, r.Tag, (uint)o))) break;
                    var e = perTag.TryGetValue(r.Tag, out var v) ? v : (0, m.Tick, m.Tick);
                    perTag[r.Tag] = (e.Item1 + 1, Math.Min(e.Item2, m.Tick), Math.Max(e.Item3, m.Tick));
                    hits.Add((m.Tick, r.Tag, o, r.Length));
                    break;                                    // one hit per record is enough
                }
        }
    // per (tag) offset histogram: a real vehicle-id FIELD sits at a fixed offset (signal); a small-id
    // byte collision scatters across offsets (noise).
    var offHist = new Dictionary<uint, SortedDictionary<int,int>>();
    foreach (var h in hits)
        (offHist.TryGetValue(h.tag, out var d) ? d : offHist[h.tag] = new())[h.off] =
            (offHist[h.tag].TryGetValue(h.off, out var c) ? c : 0) + 1;
    Console.WriteLine($"records containing id {id} (0x{id:X}) at any offset:\n");
    Console.WriteLine($"{"tag",-10} {"count",6} {"firstT",8} {"lastT",8}  offsets(rec-relative):count");
    foreach (var kv in perTag.OrderByDescending(k => k.Value.n))
    {
        var offs = string.Join(" ", offHist[kv.Key].OrderByDescending(o=>o.Value).Take(5).Select(o => $"@{o.Key}:{o.Value}"));
        Console.WriteLine($"0x{kv.Key:X2} ({kv.Key,3}) {kv.Value.n,6} {kv.Value.f,8} {kv.Value.l,8}  {offs}");
    }

    var tail = hits.OrderBy(h => h.tick).ToList();
    if (near >= 0) tail = tail.Where(h => Math.Abs((long)h.tick - near) <= 300).ToList();
    else tail = tail.Skip(Math.Max(0, tail.Count - lastN)).ToList();
    Console.WriteLine($"\ntimeline ({(near>=0?$"near {near}":"tail")}):");
    Console.WriteLine($"{"tick",8} {"tag",-10} {"off",4} {"recLen",6}");
    foreach (var h in tail)
        Console.WriteLine($"{h.tick,8} 0x{h.tag:X2} ({h.tag,3}) {h.off,4} {h.len,6}");
}

// Dump one vehicle's 0x81 records in tick order: tick, ETA delta, bodyCount, body0 type, root pos and
// the per-step move distance. Reveals exactly what the server sent as a vehicle froze — a bodyCount
// change, a type flip, a position spike, or the record simply stopping. --last K limits to the tail;
// --peer restricts to one recipient stream (else merged, deduped per tick).
static void VehRec(string path, string[] a)
{
    if (!uint.TryParse(Opt(a, "--id"), out var id)) { Console.WriteLine("need --id N"); return; }
    int last = int.TryParse(Opt(a, "--last"), out var lv) ? lv : 40;
    var peer = PeerOpt(a);
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0
                    && (peer is null || m.SteamId == peer))
        .OrderBy(m => m.Tick).ToList();

    var rows = new List<(uint tick, long dEta, int bc, int t0, double x, double y, double z)>();
    var seenTick = new HashSet<uint>();
    foreach (var m in msgs)
        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Tag != 0x81 || r.Status != RecStatus.Ok) continue;
            uint veh = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 4));
            if (veh != id) continue;
            if (peer is null && !seenTick.Add(m.Tick)) continue;    // dedup per tick when merging streams
            uint eta = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 8));
            int bc = System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(m.Body.AsSpan(r.Start + 12));
            int p = r.Start + 14; int t0 = p < m.Body.Length ? m.Body[p] : -1;
            double x = 0, y = 0, z = 0;
            if (t0 == 1 && p + 1 + 16 + 24 <= m.Body.Length)
            { int po = p + 1 + 16; x = BitConverter.ToDouble(m.Body, po); y = BitConverter.ToDouble(m.Body, po+8); z = BitConverter.ToDouble(m.Body, po+16); }
            rows.Add((m.Tick, (long)eta - m.Tick, bc, t0, x, y, z));
        }
    if (rows.Count == 0) { Console.WriteLine($"no 0x81 for veh {id}"); return; }
    Console.WriteLine($"veh {id}: {rows.Count} records, tick {rows[0].tick}..{rows[^1].tick}\n");
    Console.WriteLine($"{"tick",8} {"dETA",5} {"bodyCnt",7} {"b0type",6} {"move",9}  pos(x,y,z)");
    int from = Math.Max(0, rows.Count - last);
    double? lx = null, ly = null, lz = null;
    for (int i = from; i < rows.Count; i++)
    {
        var r = rows[i];
        string mv = "-";
        if (r.t0 == 1 && lx is double px) mv = Math.Sqrt(Sq(r.x-px)+Sq(r.y-(double)ly)+Sq(r.z-(double)lz)).ToString("F2");
        Console.WriteLine($"{r.tick,8} {r.dEta,5} {r.bc,7} {r.t0,6} {mv,9}  ({r.x:F1},{r.y:F1},{r.z:F1})");
        if (r.t0 == 1) { lx = r.x; ly = r.y; lz = r.z; }
    }
    // gap report: biggest tick gaps between consecutive records (a stall shows as one large gap)
    long maxGap = 0; uint gapAt = 0;
    for (int i = 1; i < rows.Count; i++) { long g = rows[i].tick - rows[i-1].tick; if (g > maxGap) { maxGap = g; gapAt = rows[i-1].tick; } }
    Console.WriteLine($"\nbiggest inter-record tick gap: {maxGap} ticks after tick {gapAt}");
}

// Full-body anomaly scan for one vehicle. Walks EVERY body (not just body0) of every 0x81 for --id,
// checking type1/type2 quaternion norm (should be ~1), position magnitude, non-finite values, and the
// per-body per-tick move distance (a teleport/spike). --peer restricts to one recipient stream.
static void VehScan(string path, string[] a)
{
    if (!uint.TryParse(Opt(a, "--id"), out var id)) { Console.WriteLine("need --id N"); return; }
    var peer = PeerOpt(a);
    double jumpLim = double.TryParse(Opt(a, "--jump"), out var jl) ? jl : 500;   // per-tick move flag (u/tick)
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0
                    && (peer is null || m.SteamId == peer))
        .OrderBy(m => m.Tick).ToList();

    int recs = 0, bodies = 0, badQuat = 0, absurd = 0, nonFin = 0, jumps = 0, zeroQuat = 0;
    int shown = 0;
    var seenTick = new HashSet<uint>();
    // per-bodyIndex: last (tick, x,y,z) to measure move
    var last = new Dictionary<int, (uint t, double x, double y, double z)>();
    foreach (var m in msgs)
    {
        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Tag != 0x81 || r.Status != RecStatus.Ok) continue;
            uint veh = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 4));
            if (veh != id) continue;
            if (peer is null && !seenTick.Add(m.Tick)) continue;
            recs++;
            int count = System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(m.Body.AsSpan(r.Start + 12));
            int p = r.Start + 14;
            for (int bi = 0; bi < count && p < m.Body.Length; bi++)
            {
                int t = m.Body[p];
                // type1 = quat float[4]@+1 + pos double[3]@+17 (40B). type2 = quat float[4]@+1 + pos
                // float[3]@+17 (28B). Decode both; type2 position is single-precision.
                bool okLen = (t == 1 && p + 1 + 40 <= m.Body.Length) || (t == 2 && p + 1 + 28 <= m.Body.Length);
                if (okLen)
                {
                    bodies++;
                    float q0 = BitConverter.ToSingle(m.Body, p+1),  q1 = BitConverter.ToSingle(m.Body, p+5);
                    float q2 = BitConverter.ToSingle(m.Body, p+9),  q3 = BitConverter.ToSingle(m.Body, p+13);
                    double x, y, z;
                    if (t == 1) { x = BitConverter.ToDouble(m.Body, p+17); y = BitConverter.ToDouble(m.Body, p+25); z = BitConverter.ToDouble(m.Body, p+33); }
                    else        { x = BitConverter.ToSingle(m.Body, p+17); y = BitConverter.ToSingle(m.Body, p+21); z = BitConverter.ToSingle(m.Body, p+25); }
                    double qn = (double)q0*q0 + (double)q1*q1 + (double)q2*q2 + (double)q3*q3;
                    bool nf = !double.IsFinite(x)||!double.IsFinite(y)||!double.IsFinite(z)
                              ||!float.IsFinite(q0)||!float.IsFinite(q1)||!float.IsFinite(q2)||!float.IsFinite(q3);
                    bool zq = qn < 1e-9;
                    bool bq = !zq && double.IsFinite(qn) && (qn < 0.9 || qn > 1.1);
                    bool ab = Math.Abs(x)>1e6 || Math.Abs(y)>1e6 || Math.Abs(z)>1e6;
                    double mv = -1;
                    if (last.TryGetValue(bi, out var lp) && m.Tick > lp.t)
                        mv = Math.Sqrt(Sq(x-lp.x)+Sq(y-lp.y)+Sq(z-lp.z)) / (m.Tick - lp.t);
                    bool bj = mv > jumpLim;
                    if (nf) nonFin++; if (zq) zeroQuat++; if (bq) badQuat++; if (ab) absurd++; if (bj) jumps++;
                    if ((nf||zq||bq||ab||bj) && shown < 30) { shown++;
                        Console.WriteLine($"  [!] tick={m.Tick} body{bi} type{t} pos=({x:F1},{y:F1},{z:F1}) |q|^2={qn:F3} " +
                            $"mv/t={(mv<0?"-":mv.ToString("F1"))}{(nf?" NONFINITE":"")}{(zq?" ZEROQUAT":"")}{(bq?" BADQUAT":"")}{(ab?" ABSURD":"")}{(bj?" JUMP":"")}");
                    }
                    last[bi] = (m.Tick, x, y, z);
                }
                p += 1 + t switch { 0 => 0, 1 => 40, 2 => 28, _ => 0 };
            }
        }
    }
    Console.WriteLine($"veh {id}{(peer is null?"":$" peer {peer}")}: {recs} records, {bodies} type1/2 bodies scanned");
    Console.WriteLine($"  non-finite      : {nonFin}");
    Console.WriteLine($"  zero quaternion : {zeroQuat}");
    Console.WriteLine($"  bad quat |q|^2 off 1 (<0.9|>1.1): {badQuat}");
    Console.WriteLine($"  absurd |pos|>1e6: {absurd}");
    Console.WriteLine($"  per-tick move > {jumpLim}u/t: {jumps}");
    if (nonFin==0 && zeroQuat==0 && badQuat==0 && absurd==0 && jumps==0)
        Console.WriteLine("  => no anomalies: all poses finite, unit quats, bounded positions, smooth motion");
}

// Resend / rewound-world-tick trace hunt. Two things a "resend" would leave behind:
//   (1) Envelope level: on ONE recipient's SEND stream (capture order), a message whose world tick
//       is <= a tick already sent to that recipient — i.e. the server rewound / repeated a tick.
//       A well-behaved forward stream is strictly increasing (ties allowed only if the game batches
//       one tick across frames). We flag tick < prevMaxTick (a true backward step) separately from
//       tick == prevTick (same-tick continuation, benign).
//   (2) Record level: per (recipient, vehicle) a 0x81 whose envelope tick <= the last one seen for
//       that pair (the same vehicle state re-emitted), or whose ETA(@+8) went backwards.
// Whatever we find, the hook's rule stays simple: ignore any message whose world tick is not newer
// than the last one we processed for that recipient (skip rewound ticks) — this scan just tells us
// how often that guard would fire.
static void Resend(string path, string[] a)
{
    var peer = PeerOpt(a);
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8
                    && (peer is null || m.SteamId == peer))
        .ToList();   // capture order preserved

    var maxTick  = new Dictionary<ulong, uint>();     // per recipient: highest tick sent so far
    var prevTick = new Dictionary<ulong, uint>();     // per recipient: immediately previous tick
    int total = 0, backward = 0, sameTick = 0;
    var backSamples = new List<string>();
    var backByPeer = new Dictionary<ulong, int>();

    // record-level
    var vehLastTick = new Dictionary<(ulong, uint), uint>();
    var vehLastEta  = new Dictionary<(ulong, uint), uint>();
    int vehRepeat = 0, etaBack = 0;

    foreach (var m in msgs)
    {
        total++;
        ulong B = m.SteamId; uint t = m.Tick;
        if (prevTick.TryGetValue(B, out var pt) && t == pt) sameTick++;
        if (maxTick.TryGetValue(B, out var mx))
        {
            if (t < mx)
            {
                backward++;
                backByPeer[B] = backByPeer.GetValueOrDefault(B) + 1;
                if (backSamples.Count < 20)
                    backSamples.Add($"  peer={B} tick={t} < maxSeen={mx}  (step -{mx - t})  seq={m.StartSeq}");
            }
            else maxTick[B] = t;
        }
        else maxTick[B] = t;
        prevTick[B] = t;

        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Tag != 0x81 || r.Status != RecStatus.Ok) continue;
            uint veh = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 4));
            uint eta = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 8));
            var k = (B, veh);
            if (vehLastTick.TryGetValue(k, out var lt) && t <= lt) vehRepeat++;
            if (vehLastEta.TryGetValue(k, out var le) && eta < le) etaBack++;
            vehLastTick[k] = t; vehLastEta[k] = eta;
        }
    }

    int recipients = maxTick.Count;
    Console.WriteLine($"send msgs (type8 ch0): {total}   recipients: {recipients}\n");
    Console.WriteLine("-- ENVELOPE world-tick monotonicity (per recipient stream) --");
    Console.WriteLine($"  backward steps (tick < max seen) : {backward}   <-- resend/rewind trace");
    Console.WriteLine($"  same-tick continuations (benign) : {sameTick}");
    if (backward > 0)
    {
        Console.WriteLine("  by recipient:");
        foreach (var kv in backByPeer.OrderByDescending(k => k.Value))
            Console.WriteLine($"    peer={kv.Key} : {kv.Value}");
        Console.WriteLine("  samples:");
        foreach (var s in backSamples) Console.WriteLine(s);
    }
    Console.WriteLine("\n-- RECORD level (per recipient,vehicle 0x81) --");
    Console.WriteLine($"  same/older-tick re-emit of a vehicle : {vehRepeat}");
    Console.WriteLine($"  ETA went backwards                   : {etaBack}");
    if (backward == 0 && vehRepeat == 0)
        Console.WriteLine("\n=> no rewound-tick resend trace. Forward-monotonic guard would never fire (still cheap to keep).");
}

// 0x81 ETA analysis: for every vehicle-sync record, delta = record.time(@+8) - envelope.tick.
// Confirms the hypothesis "time is a FUTURE world-tick ETA = message tick + interval". Also, per
// vehicle, measures the actual spacing (in ticks) between consecutive 0x81s for that vehicle on a
// given recipient stream — i.e. the real sync interval, and whether delta tracks that interval.
static void Eta(string path, string[] a)
{
    var peer = PeerOpt(a);
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0
                    && (peer is null || m.SteamId == peer))
        .OrderBy(m => m.Tick).ToList();

    var deltaHist = new SortedDictionary<long, int>();          // (time - tick) histogram
    // per (recipient, vehicle): ticks at which a 0x81 was sent, to measure real spacing
    var seen = new Dictionary<(ulong peer, uint veh), uint>();
    var spacingHist = new SortedDictionary<long, int>();
    int recs = 0, negative = 0;
    foreach (var m in msgs)
        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Tag != 0x81 || r.Status != RecStatus.Ok) continue;
            recs++;
            uint veh = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 4));
            uint time = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 8));
            long delta = (long)time - m.Tick;
            deltaHist[delta] = deltaHist.GetValueOrDefault(delta) + 1;
            if (delta < 0) negative++;
            var key = (m.SteamId, veh);
            if (seen.TryGetValue(key, out var prev)) spacingHist[(long)m.Tick - prev] = spacingHist.GetValueOrDefault((long)m.Tick - prev) + 1;
            seen[key] = m.Tick;
        }

    Console.WriteLine($"0x81 records: {recs}   (negative ETA delta: {negative})");
    Console.WriteLine("\n-- ETA delta (record.time - envelope.tick) --");
    foreach (var kv in deltaHist) Console.WriteLine($"  delta {kv.Key,6} ticks : {kv.Value}");
    Console.WriteLine("\n-- actual spacing between consecutive 0x81 per (peer,vehicle) --");
    foreach (var kv in spacingHist.Take(30)) Console.WriteLine($"  gap {kv.Key,6} ticks : {kv.Value}");
}

// Distribution of 0x81 body TYPE bytes across all vehicle-sync records — confirms whether any body
// type other than the known 0/1/2 ever appears (an unknown type would break the length walk).
static void BodyTypes(string path)
{
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.MsgType == 8 && m.RecordCount > 0);
    var hist = new Dictionary<int, long>();
    int records = 0;
    foreach (var m in msgs)
        foreach (var r in Records.Walk(m).recs)
        {
            if (r.Tag != 0x81 || r.Status != RecStatus.Ok) continue;
            records++;
            int p = r.Start + 14, count = System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(m.Body.AsSpan(r.Start + 12));
            for (int i = 0; i < count; i++)
            {
                int t = m.Body[p]; hist[t] = hist.GetValueOrDefault(t) + 1;
                p += 1 + t switch { 0 => 0, 1 => 40, 2 => 28, _ => 0 };
            }
        }
    Console.WriteLine($"0x81 records walked: {records}\nbody type histogram:");
    foreach (var kv in hist.OrderBy(k => k.Key))
        Console.WriteLine($"  type {kv.Key} : {kv.Value}{(kv.Key > 2 ? "   <-- UNKNOWN TYPE" : "")}");
}

// Show the record breakdown of one message (or the first message that contains a given --tag),
// including the trailing undecoded bytes — used to bound an unknown tag's length.
static void Walk(string path, string[] a)
{
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.MsgType == 8).ToList();
    Message? m = null;
    if (ulong.TryParse(Opt(a, "--seq"), out var seq)) m = msgs.FirstOrDefault(x => x.StartSeq == seq);
    else if (uint.TryParse(Opt(a, "--tag"), out var wantTag))
        m = msgs.FirstOrDefault(x => { var (r, _, _) = Records.Walk(x); return r.Any(z => z.Tag == wantTag); });
    if (m is null) { Console.WriteLine("no matching message"); return; }

    var (recs, full, consumed) = Records.Walk(m);
    Console.WriteLine($"seq={m.StartSeq} tick={m.Tick} recordCount={m.RecordCount} bodyLen={m.Body.Length} full={full}");
    foreach (var r in recs)
        Console.WriteLine($"  @{r.Start,-4} tag 0x{r.Tag:X2} ({r.Tag,3})  len={r.Length,-5} {r.Status,-8} {r.Note}");
    if (consumed < m.Body.Length)
    {
        int tailStart = recs.Count > 0 ? recs[^1].Start : 20;   // last (undecoded) record's start
        var tail = m.Body.AsSpan(tailStart);
        Console.WriteLine($"  undecoded tail from @{tailStart} ({tail.Length} B):");
        Dump(tail.ToArray(), 96);
    }
}

// Round-trip KPI: walk every type=8 message's records with the known length rules and report how
// far each got. A message is "full" when its records consume the body exactly. Also tallies which
// tag first stops an incomplete walk (the next rule worth decoding) and per-tag Ok/Overrun counts.
static void Validate(string path, string[] a)
{
    var peer = PeerOpt(a);
    var msgs = Message.Reassemble(Container.Read(path).ToList())
        .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8
                    && (peer is null || m.SteamId == peer)).ToList();

    int full = 0, empty = 0;
    var blockedBy = new Dictionary<uint, int>();          // tag that stopped an incomplete walk
    var tagOk = new Dictionary<uint, int>();
    var tagUnknown = new Dictionary<uint, int>();         // no rule yet (stops the walk, not a data error)
    var tagOverrun = new Dictionary<uint, int>();         // rule known but misaligned/short (rule likely wrong)

    foreach (var m in msgs)
    {
        if (m.RecordCount == 0) { empty++; full++; continue; }   // empty heartbeat = trivially full
        var (recs, isFull, _) = Records.Walk(m);
        foreach (var r in recs)
        {
            var bucket = r.Status switch { RecStatus.Ok => tagOk, RecStatus.Unknown => tagUnknown, _ => tagOverrun };
            bucket[r.Tag] = bucket.GetValueOrDefault(r.Tag) + 1;
        }
        if (isFull) full++;
        else { var last = recs.Count > 0 ? recs[^1].Tag : 0xFFFFFFFF;
               blockedBy[last] = blockedBy.GetValueOrDefault(last) + 1; }
    }

    Console.WriteLine($"type=8 ch0 SEND messages: {msgs.Count}");
    Console.WriteLine($"  fully walked : {full}  ({100.0 * full / msgs.Count:F1}%)   (incl. {empty} empty heartbeats)");
    Console.WriteLine($"  incomplete   : {msgs.Count - full}");

    Console.WriteLine("\n-- records decoded OK per tag --");
    foreach (var kv in tagOk.OrderByDescending(k => k.Value))
        Console.WriteLine($"  tag 0x{kv.Key:X2} ({kv.Key,3}) : {kv.Value}");
    if (tagOverrun.Count > 0)
    {
        Console.WriteLine("\n-- records that OVERRAN their rule (RULE LIKELY WRONG — investigate) --");
        foreach (var kv in tagOverrun.OrderByDescending(k => k.Value))
            Console.WriteLine($"  tag 0x{kv.Key:X2} ({kv.Key,3}) : {kv.Value}");
    }
    Console.WriteLine("\n-- unknown tags reached (no rule yet) --");
    foreach (var kv in tagUnknown.OrderByDescending(k => k.Value))
        Console.WriteLine($"  tag 0x{kv.Key:X2} ({kv.Key,3}) : {kv.Value}");
    Console.WriteLine("\n-- tag that STOPPED each incomplete walk (next rule to decode) --");
    foreach (var kv in blockedBy.OrderByDescending(k => k.Value))
        Console.WriteLine($"  tag 0x{kv.Key:X2} ({kv.Key,3}) : {kv.Value} messages");
}

static void Stats(string path)
{
    var frames = Container.Read(path).ToList();
    Console.WriteLine($"{frames.Count} frames, {frames.Sum(f => (long)f.Size):N0} bytes");
    Console.WriteLine($"peers: {string.Join(", ", frames.Select(f => f.SteamId).Where(s => s != 0).Distinct())}");

    Console.WriteLine("\n-- frames by dir/channel --");
    foreach (var g in frames.GroupBy(f => (f.Dir, f.Channel)).OrderBy(g => g.Key))
        Console.WriteLine($"  {g.Key.Dir,-4} ch{g.Key.Channel,-3} {g.Count(),7} frames  {g.Sum(f=>(long)f.Size),12:N0} B");

    var msgs = Message.Reassemble(frames).ToList();
    Console.WriteLine($"\n-- reassembled: {msgs.Count} messages ({msgs.Count(m=>!m.Complete)} incomplete) --");
    foreach (var g in msgs.Where(m => m.Complete)
                          .GroupBy(m => (m.Dir, m.Channel, Type: m.MsgType))
                          .OrderBy(g => g.Key))
        Console.WriteLine($"  {g.Key.Dir,-4} ch{g.Key.Channel,-3} type={g.Key.Type,-6} {g.Count(),7} msgs  " +
                          $"body {g.Min(m=>m.Body.Length)}..{g.Max(m=>m.Body.Length)}");
}

static IEnumerable<Frame> Filter(IEnumerable<Frame> fs, string[] a)
{
    var dir = Opt(a, "--dir"); var ch = Opt(a, "--ch"); var peer = PeerOpt(a);
    if (dir is "S" or "R") fs = fs.Where(f => f.Dir == (dir == "S" ? Dir.Send : Dir.Recv));
    if (int.TryParse(ch, out var c)) fs = fs.Where(f => f.Channel == c);
    if (peer is not null) fs = fs.Where(f => f.SteamId == peer);
    return fs;
}

static void Frames(string path, string[] a)
{
    int take = int.TryParse(Opt(a, "--take"), out var t) ? t : 40;
    var all = Container.Read(path).ToList();
    ulong t0 = all.Count > 0 ? all.Min(f => f.TickMs) : 0;
    foreach (var f in Filter(all, a).Take(take))
        Console.WriteLine($"{(f.Dir==Dir.Send?"S":"R")} [{f.Seq}] t+{f.TickMs-t0}ms peer={f.SteamId} ch={f.Channel} " +
            $"flags={f.Flags} size={f.Size} frag={f.FragFlag} total={f.TotalBodyLen}");
}

static void Msgs(string path, string[] a)
{
    int take = int.TryParse(Opt(a, "--take"), out var t) ? t : 40;
    uint? type = uint.TryParse(Opt(a, "--type"), out var ty) ? ty : null;
    var frames = Container.Read(path).ToList();
    var q = Message.Reassemble(frames).Where(m => m.Complete);
    var dir = Opt(a, "--dir"); var ch = Opt(a, "--ch");
    if (dir is "S" or "R") q = q.Where(m => m.Dir == (dir == "S" ? Dir.Send : Dir.Recv));
    if (int.TryParse(ch, out var c)) q = q.Where(m => m.Channel == c);
    if (type is not null) q = q.Where(m => m.MsgType == type);
    if (PeerOpt(a) is ulong pr) q = q.Where(m => m.SteamId == pr);
    foreach (var m in q.Take(take))
        Console.WriteLine($"{(m.Dir==Dir.Send?"S":"R")} seq={m.StartSeq} ch={m.Channel} frags={m.FragmentCount} " +
            $"bodyLen={m.Body.Length} const1={m.Const1} type={m.MsgType} tick={m.Tick} sub={m.SubType} recs={m.RecordCount}");
}

static void Hex(string path, string[] a)
{
    if (!ulong.TryParse(Opt(a, "--seq"), out var seq)) { Console.WriteLine("need --seq N"); return; }
    int max = int.TryParse(Opt(a, "--max"), out var mx) ? mx : 512;
    if (Flag(a, "--body"))
    {
        var m = Message.Reassemble(Container.Read(path).ToList()).FirstOrDefault(x => x.StartSeq == seq);
        if (m is null) { Console.WriteLine("no message with that head seq"); return; }
        Console.WriteLine($"message seq={seq} type={m.MsgType} recs={m.RecordCount} bodyLen={m.Body.Length} complete={m.Complete}");
        Dump(m.Body, max);
    }
    else
    {
        var f = Container.Read(path).FirstOrDefault(x => x.Seq == seq);
        if (f is null) { Console.WriteLine("no frame with that seq"); return; }
        Console.WriteLine($"frame seq={seq} dir={f.Dir} ch={f.Channel} size={f.Size}");
        Dump(f.Data, max);
    }
}

static void Dump(byte[] d, int max)
{
    int n = Math.Min(d.Length, max);
    for (int i = 0; i < n; i += 16)
    {
        var hex = string.Join(' ', d.Skip(i).Take(16).Select(b => b.ToString("X2")));
        var asc = string.Concat(d.Skip(i).Take(16).Select(b => b >= 32 && b < 127 ? (char)b : '.'));
        Console.WriteLine($"{i,6:D}  {hex,-47}  {asc}");
    }
    if (d.Length > max) Console.WriteLine($"... (+{d.Length - max} bytes)");
}
