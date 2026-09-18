using System.Buffers.Binary;

namespace SwCap;

// Stage-A offline CAUSAL simulation of the relay. Replays a capture and, for every distant
// (recipient B, vehicle V) pair, reconstructs three trajectories over time using ONLY data that
// would have been available at each moment:
//   T(t)       ground truth  = piecewise-linear interp over the union of ALL peers' 0x81(V) samples
//                              (the finest knowledge of V's real path present in the capture)
//   Rnat(t)    what B sees    = B's native 0x81(V) records fed through the client interpolation model
//   Rrelay(t)  with relay     = records we WOULD inject (dead-reckoned from the freshest source known
//                              at that tick), fed through the same client model
// Then it measures position error and effective time-lag (Rnat vs T) and (Rrelay vs T), the % of the
// time under a target lag, and the projected relay bandwidth — the go/no-go numbers for the design.
//
// Client interpolation model (confirmed by `swcap eta`): on receiving a record at tick t_r with pose
// P and ETA E, the client lerps from its current rendered pose to P over [t_r, E]; past E with no new
// record it holds P (freeze). We reproduce exactly that.
public static class Shadow
{
    struct Sample { public uint t; public double x, y, z; public uint eta; public int len; }

    public static void Run(string path, string[] a)
    {
        int interval = ArgI(a, "--interval", 20);   // relay send interval (ticks)
        int dense    = ArgI(a, "--dense", 10);      // a "fresh source" has mean gap <= this
        int stale    = ArgI(a, "--stale", 30);      // don't invent data if freshest source older than this
        int lag      = ArgI(a, "--lag", 5);         // Λ: target render lag (ticks). pose = predict(ETA-Λ)
        double maxSpeed = ArgD(a, "--maxspeed", 1e9); // discontinuity guard: hold if implied speed exceeds this (u/tick)
        bool lodOn = Array.IndexOf(a, "--lod") >= 0;   // per-vehicle LOD: I=gap/4 [5,60], Λ=I/2
        double tps   = ArgD(a, "--tps", 60);
        double targetMs = ArgD(a, "--target-ms", 1000);
        double targetTicks = targetMs / 1000.0 * tps;
        int top      = ArgI(a, "--top", 0);

        var msgs = Message.Reassemble(Container.Read(path).ToList())
            .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0 && m.MsgType == 8 && m.RecordCount > 0)
            .OrderBy(m => m.Tick).ToList();

        // (peer,veh) -> samples in tick order
        var by = new Dictionary<(ulong, uint), List<Sample>>();
        foreach (var m in msgs)
            foreach (var r in Records.Walk(m).recs)
            {
                if (r.Tag != 0x81 || r.Status != RecStatus.Ok) continue;
                int p = r.Start + 14;
                if (p >= m.Body.Length || m.Body[p] != 1) continue;      // body0 must be type1
                int posOff = p + 1 + 16;
                if (posOff + 24 > m.Body.Length) continue;
                var s = new Sample {
                    t = m.Tick,
                    x = BitConverter.ToDouble(m.Body, posOff),
                    y = BitConverter.ToDouble(m.Body, posOff + 8),
                    z = BitConverter.ToDouble(m.Body, posOff + 16),
                    eta = BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 8)),
                    len = r.Length,
                };
                uint veh = BinaryPrimitives.ReadUInt32LittleEndian(m.Body.AsSpan(r.Start + 4));
                var k = (m.SteamId, veh);
                (by.TryGetValue(k, out var l) ? l : by[k] = new()).Add(s);
            }

        // ground-truth merged samples per vehicle (union across peers, tick-sorted, deduped)
        var truth = new Dictionary<uint, List<Sample>>();
        foreach (var kv in by)
        {
            var t = truth.TryGetValue(kv.Key.Item2, out var l) ? l : truth[kv.Key.Item2] = new();
            t.AddRange(kv.Value);
        }
        foreach (var v in truth.Values.ToList())
            v.Sort((p, q) => p.t.CompareTo(q.t));

        double MeanGap(List<Sample> s) => s.Count > 1 ? (double)(s[^1].t - s[0].t) / (s.Count - 1) : double.PositiveInfinity;
        var bestGap = new Dictionary<uint, double>();
        foreach (var kv in by)
            bestGap[kv.Key.Item2] = Math.Min(bestGap.GetValueOrDefault(kv.Key.Item2, double.PositiveInfinity), MeanGap(kv.Value));

        var results = new List<(ulong B, uint V, int n, double eNat, double eRel, double lNat, double lRel, double okNat, double okRel, long bytes)>();
        var gER = new List<double>(); var gLR = new List<double>();        // global relay error / lag samples
        var tierErr = new Dictionary<int, List<double>>();                 // gap tier -> relay errors
        var tierN = new int[3];
        long injTotalBytes = 0; double durSecs = 0;
        if (msgs.Count > 0) durSecs = Math.Max(1, (msgs[^1].Tick - msgs[0].Tick) / tps);

        foreach (var kv in by)
        {
            var (B, V) = kv.Key;
            var nat = kv.Value;
            if (MeanGap(nat) <= (lodOn ? 10 : interval)) continue;   // B already gets V fast enough — no relay
            if (bestGap[V] > dense) continue;                // no fresh source anywhere — give up (honest)
            var tru = truth[V];

            // Build the relay received-stream causally, matching relay.h: KEEP the ETA (server's
            // next-send time), set pose = predict(ETA-Λ). Two sources of records reach B:
            //  (1) each native record, pose-rewritten (ETA = that record's own eta);
            //  (2) appended records at the re-aim cadence, ETA = the last native eta known at that tick
            //      (>tt), else tt+gap — the server's expected next send.
            var relay = new List<Sample>();
            uint tStart = nat[0].t, tEnd = nat[^1].t;
            double natGap = MeanGap(nat);

            // returns false if source stale at `now`; else pose predicted to `targetTick`
            bool Pred(uint now, long targetTick, out double px, out double py, out double pz, out int len)
            {
                px = py = pz = 0; len = 0;
                int fi = UpperIdx(tru, now);
                if (fi < 0 || now - tru[fi].t > stale) return false;
                px = tru[fi].x; py = tru[fi].y; pz = tru[fi].z; len = tru[fi].len;
                int gi = fi - 1;
                if (gi >= 0 && tru[fi].t != tru[gi].t && (tru[fi].t - tru[gi].t) <= dense * 3)
                {
                    double dt = tru[fi].t - tru[gi].t;
                    double vx = (tru[fi].x - tru[gi].x) / dt, vy = (tru[fi].y - tru[gi].y) / dt, vz = (tru[fi].z - tru[gi].z) / dt;
                    double sp = Math.Sqrt(vx * vx + vy * vy + vz * vz);
                    if (sp <= maxSpeed)                         // discontinuity/teleport guard: else hold
                    {
                        double ahead = targetTick - tru[fi].t;
                        px += vx * ahead; py += vy * ahead; pz += vz * ahead;
                    }
                }
                return true;
            }
            long Target(uint eta, uint srcT, int lg) { long t = (long)eta - lg; return t > srcT ? t : srcT; }

            // LOD: choose cadence I and lag Λ from the native gap (relay.h: I=gap/4 clamped [5,60], Λ=I/2).
            int Iuse = interval, Luse = lag;
            if (lodOn) { Iuse = Math.Clamp((int)(natGap / 4), 5, 60); Luse = Iuse / 2; }

            var byTick = new SortedDictionary<uint, Sample>();
            foreach (var n in nat)                                   // (1) rewritten natives (keep eta)
                if (Pred(n.t, Target(n.eta, TruAt(tru, n.t), Luse), out double x, out double y, out double z, out int L))
                    byTick[n.t] = new Sample { t = n.t, x = x, y = y, z = z, eta = n.eta, len = L };
            for (uint tt = tStart; tt <= tEnd; tt += (uint)Iuse)    // (2) appended, ETA = server next send
            {
                if (byTick.ContainsKey(tt)) continue;
                uint enat = LastEtaAt(nat, tt);
                uint Eapp = enat > tt ? enat : tt + (uint)Math.Max(1, (int)natGap);
                if (Pred(tt, Target(Eapp, TruAt(tru, tt), Luse), out double x, out double y, out double z, out int L))
                    byTick[tt] = new Sample { t = tt, x = x, y = y, z = z, eta = Eapp, len = L };
            }
            foreach (var s in byTick.Values) { relay.Add(s); injTotalBytes += s.len; }

            // evaluate on the truth sample grid within B's window
            var natSeg = BuildSegments(nat);
            var relSeg = BuildSegments(relay);
            var eN = new List<double>(); var eR = new List<double>();
            var lN = new List<double>(); var lR = new List<double>();
            int okN = 0, okR = 0, cnt = 0;
            foreach (var g in tru.Where(s => s.t >= tStart && s.t <= tEnd))
            {
                if (!TruePos(tru, g.t, out double tx, out double ty, out double tz)) continue;
                double rnx, rny, rnz, rrx, rry, rrz;
                bool hn = Eval(natSeg, g.t, out rnx, out rny, out rnz);
                bool hr = Eval(relSeg, g.t, out rrx, out rry, out rrz);
                if (!hn) continue;
                cnt++;
                double dN = Dist(rnx, rny, rnz, tx, ty, tz); eN.Add(dN);
                double lagN = TimeLag(tru, rnx, rny, rnz, g.t, targetTicks); lN.Add(lagN);
                if (lagN <= targetTicks) okN++;
                if (hr) {
                    double dR = Dist(rrx, rry, rrz, tx, ty, tz); eR.Add(dR);
                    double lagR = TimeLag(tru, rrx, rry, rrz, g.t, targetTicks); lR.Add(lagR);
                    if (lagR <= targetTicks) okR++;
                    gER.Add(dR); gLR.Add(lagR);
                    int tier = natGap <= 60 ? 0 : natGap <= 240 ? 1 : 2;
                    (tierErr.TryGetValue(tier, out var tl) ? tl : tierErr[tier] = new()).Add(dR);
                    tierN[tier]++;
                }
            }
            if (cnt == 0) continue;
            long bytes = relay.Sum(s => (long)s.len);
            results.Add((B, V, cnt, Med(eN), Med(eR), Med(lN), Med(lR),
                         100.0 * okN / cnt, eR.Count > 0 ? 100.0 * okR / cnt : 0, bytes));
        }

        Console.WriteLine($"shadow sim: interval={interval}t (~{interval/tps*1000:F0}ms)  dense<= {dense}t  stale> {stale}t  target={targetMs:F0}ms ({targetTicks:F0}t)");
        Console.WriteLine($"distant (recipient,vehicle) pairs relayed: {results.Count}\n");
        if (results.Count == 0) { Console.WriteLine("no relay candidates under these params"); return; }

        var allN = results.SelectMany(r => Enumerable.Repeat(r.eNat, r.n)).ToList();
        // weighted medians via per-pair medians (approx): report simple means of per-pair medians too
        double wMedLagN = WMed(results.Select(r => (r.lNat, r.n))); double wMedLagR = WMed(results.Select(r => (r.lRel, r.n)));
        double wMedErrN = WMed(results.Select(r => (r.eNat, r.n))); double wMedErrR = WMed(results.Select(r => (r.eRel, r.n)));
        double okN2 = results.Sum(r => r.okNat / 100.0 * r.n) / results.Sum(r => r.n) * 100;
        double okR2 = results.Sum(r => r.okRel / 100.0 * r.n) / results.Sum(r => r.n) * 100;

        Console.WriteLine("                        native      relay");
        Console.WriteLine($"  median pos error : {wMedErrN,9:F2}  {wMedErrR,9:F2}  units");
        Console.WriteLine($"  median time-lag  : {wMedLagN/tps*1000,9:F0}  {wMedLagR/tps*1000,9:F0}  ms");
        Console.WriteLine($"  % under {targetMs:F0}ms   : {okN2,9:F1}  {okR2,9:F1}  %");
        double relayMbps = injTotalBytes * 8 / 1000.0 / 1000.0 / durSecs;
        Console.WriteLine($"\n  projected relay bandwidth: {relayMbps:F3} Mbps ({results.Count} streams, {durSecs:F0}s)");
        Console.WriteLine($"  (×~4 for 5→10 players ≈ {relayMbps*4:F2} Mbps)");

        Console.WriteLine($"\n-- relay error distribution (units) --  {Pct(gER)}");
        Console.WriteLine($"-- relay time-lag (ms) --  {PctMs(gLR, tps)}");
        Console.WriteLine("\n-- relay error by native-gap tier (horizon proxy) --");
        string[] tn = { "near  (gap<=60t )", "mid   (60-240t )", "far   (gap>240t )" };
        for (int t = 0; t < 3; t++)
            if (tierErr.TryGetValue(t, out var tl))
                Console.WriteLine($"  {tn[t]}  n={tierN[t],-8} err {Pct(tl)}");

        if (top > 0)
        {
            Console.WriteLine($"\n-- worst {top} pairs by native time-lag (ms) --");
            Console.WriteLine($"{"veh",5} {"peer",-20} {"n",6} {"natErr",8} {"relErr",8} {"natLag",8} {"relLag",8} {"ok%N",6} {"ok%R",6}");
            foreach (var r in results.OrderByDescending(r => r.lNat).Take(top))
                Console.WriteLine($"{r.V,5} {r.B,-20} {r.n,6} {r.eNat,8:F2} {r.eRel,8:F2} {r.lNat/tps*1000,8:F0} {r.lRel/tps*1000,8:F0} {r.okNat,6:F0} {r.okRel,6:F0}");
        }
    }

    // interpolation segments for the client render model
    struct Seg { public uint t0; public double sx, sy, sz, px, py, pz; public uint eta; }
    static List<Seg> BuildSegments(List<Sample> recv)
    {
        var segs = new List<Seg>();
        double cx = 0, cy = 0, cz = 0; bool have = false;
        for (int i = 0; i < recv.Count; i++)
        {
            var s = recv[i];
            if (!have) { cx = s.x; cy = s.y; cz = s.z; have = true; }  // first: snap
            segs.Add(new Seg { t0 = s.t, sx = cx, sy = cy, sz = cz, px = s.x, py = s.y, pz = s.z, eta = s.eta });
            // rendered pose at the NEXT record's receipt tick becomes that segment's start
            uint tn = i + 1 < recv.Count ? recv[i + 1].t : s.eta;
            EvalSeg(segs[^1], tn, out cx, out cy, out cz);
        }
        return segs;
    }
    static void EvalSeg(Seg s, uint t, out double x, out double y, out double z)
    {
        double denom = s.eta > s.t0 ? s.eta - s.t0 : 1;
        double r = Math.Clamp((t - (double)s.t0) / denom, 0, 1);
        x = s.sx + (s.px - s.sx) * r; y = s.sy + (s.py - s.sy) * r; z = s.sz + (s.pz - s.sz) * r;
    }
    static bool Eval(List<Seg> segs, uint t, out double x, out double y, out double z)
    {
        x = y = z = 0;
        if (segs.Count == 0 || t < segs[0].t0) return false;
        int lo = 0, hi = segs.Count - 1, idx = 0;
        while (lo <= hi) { int m = (lo + hi) / 2; if (segs[m].t0 <= t) { idx = m; lo = m + 1; } else hi = m - 1; }
        EvalSeg(segs[idx], t, out x, out y, out z);
        return true;
    }

    static bool TruePos(List<Sample> s, uint t, out double x, out double y, out double z)
    {
        x = y = z = 0;
        if (s.Count == 0) return false;
        if (t <= s[0].t) { x = s[0].x; y = s[0].y; z = s[0].z; return true; }
        if (t >= s[^1].t) { x = s[^1].x; y = s[^1].y; z = s[^1].z; return true; }
        int lo = 0, hi = s.Count - 1, idx = 0;
        while (lo <= hi) { int m = (lo + hi) / 2; if (s[m].t <= t) { idx = m; lo = m + 1; } else hi = m - 1; }
        var a = s[idx]; var b = s[Math.Min(idx + 1, s.Count - 1)];
        double d = b.t > a.t ? b.t - a.t : 1, r = (t - (double)a.t) / d;
        x = a.x + (b.x - a.x) * r; y = a.y + (b.y - a.y) * r; z = a.z + (b.z - a.z) * r;
        return true;
    }

    // effective time-lag: find the past truth time t' whose pose is closest to the rendered pose;
    // lag = t - t'. Searches back up to ~2×target ticks. Approximates "how stale the view is".
    static double TimeLag(List<Sample> tru, double rx, double ry, double rz, uint now, double targetTicks)
    {
        double best = double.MaxValue; double bestLag = 0; int back = (int)(targetTicks * 3) + 30;
        for (int dt = 0; dt <= back; dt += 1)
        {
            long tp = (long)now - dt; if (tp < tru[0].t) break;
            TruePos(tru, (uint)tp, out double tx, out double ty, out double tz);
            double d = Dist(rx, ry, rz, tx, ty, tz);
            if (d < best) { best = d; bestLag = dt; }
        }
        return bestLag;
    }

    static uint TruAt(List<Sample> s, uint t) { int i = UpperIdx(s, t); return i < 0 ? (s.Count > 0 ? s[0].t : t) : s[i].t; }
    static uint LastEtaAt(List<Sample> nat, uint tt) { int i = UpperIdx(nat, tt); return i < 0 ? 0u : nat[i].eta; }

    static int UpperIdx(List<Sample> s, uint t)   // last index with s[i].t <= t
    {
        int lo = 0, hi = s.Count - 1, idx = -1;
        while (lo <= hi) { int m = (lo + hi) / 2; if (s[m].t <= t) { idx = m; lo = m + 1; } else hi = m - 1; }
        return idx;
    }
    static double Dist(double x, double y, double z, double x2, double y2, double z2)
        => Math.Sqrt((x - x2) * (x - x2) + (y - y2) * (y - y2) + (z - z2) * (z - z2));
    static double Med(List<double> v) { if (v.Count == 0) return 0; var s = v.OrderBy(t => t).ToList(); return s[s.Count / 2]; }
    static string Pct(List<double> v) { var s = v.OrderBy(t => t).ToList(); if (s.Count == 0) return "-";
        double q(double f) => s[(int)Math.Clamp(f * (s.Count - 1), 0, s.Count - 1)];
        return $"p50={q(.5):F2} p90={q(.9):F2} p99={q(.99):F2} max={s[^1]:F2}"; }
    static string PctMs(List<double> v, double tps) { var s = v.OrderBy(t => t).ToList(); if (s.Count == 0) return "-";
        double q(double f) => s[(int)Math.Clamp(f * (s.Count - 1), 0, s.Count - 1)] / tps * 1000;
        return $"p50={q(.5):F0} p90={q(.9):F0} p99={q(.99):F0} max={s[^1] / tps * 1000:F0}"; }
    static double WMed(IEnumerable<(double val, int w)> items)
    {
        var s = items.Where(i => i.w > 0).OrderBy(i => i.val).ToList();
        long tot = s.Sum(i => (long)i.w), acc = 0;
        foreach (var i in s) { acc += i.w; if (acc * 2 >= tot) return i.val; }
        return s.Count > 0 ? s[^1].val : 0;
    }
    static int ArgI(string[] a, string k, int d) { for (int i = 0; i < a.Length - 1; i++) if (a[i] == k && int.TryParse(a[i + 1], out var v)) return v; return d; }
    static double ArgD(string[] a, string k, double d) { for (int i = 0; i < a.Length - 1; i++) if (a[i] == k && double.TryParse(a[i + 1], out var v)) return v; return d; }
}
