using SwCap;

namespace SwCap;

// RE helpers: see which bytes move. `diff` compares two message bodies; `churn` aggregates, over a
// whole stream of same-shaped messages, how often each byte offset changes — separating static
// structure from live fields.
public static class Diff
{
    public static void DiffTwo(string path, ulong a, ulong b)
    {
        var msgs = Message.Reassemble(Container.Read(path).ToList())
                          .Where(m => m.Complete).ToList();
        var ma = msgs.FirstOrDefault(m => m.StartSeq == a);
        var mb = msgs.FirstOrDefault(m => m.StartSeq == b);
        if (ma is null || mb is null) { Console.WriteLine("one of the seqs is not a complete message"); return; }
        Console.WriteLine($"A seq={a} type={ma.MsgType} recs={ma.RecordCount} len={ma.Body.Length}");
        Console.WriteLine($"B seq={b} type={mb.MsgType} recs={mb.RecordCount} len={mb.Body.Length}");
        int n = Math.Max(ma.Body.Length, mb.Body.Length);
        int runStart = -1;
        for (int i = 0; i <= n; i++)
        {
            byte? x = i < ma.Body.Length ? ma.Body[i] : null;
            byte? y = i < mb.Body.Length ? mb.Body[i] : null;
            bool diff = i < n && x != y;
            if (diff && runStart < 0) runStart = i;
            if (!diff && runStart >= 0)
            {
                Console.Write($"  @{runStart,-5} len {i - runStart,-3}  A:");
                for (int k = runStart; k < i; k++) Console.Write(k < ma.Body.Length ? $"{ma.Body[k]:X2} " : "-- ");
                Console.Write(" B:");
                for (int k = runStart; k < i; k++) Console.Write(k < mb.Body.Length ? $"{mb.Body[k]:X2} " : "-- ");
                Console.WriteLine();
                runStart = -1;
            }
        }
    }

    // For all complete messages of a (dir,channel,type), bucket by body length, then per bucket
    // count how many messages differ from the previous one at each offset. High-churn offsets =
    // live fields (positions, timers); zero-churn = static structure (ids, labels, type tags).
    public static void Churn(string path, Dir dir, int ch, uint type, int topN)
    {
        var msgs = Message.Reassemble(Container.Read(path).ToList())
            .Where(m => m.Complete && m.Dir == dir && m.Channel == ch && m.MsgType == type)
            .ToList();
        Console.WriteLine($"{msgs.Count} complete {dir} ch{ch} type={type} messages");

        foreach (var g in msgs.GroupBy(m => m.Body.Length).OrderByDescending(g => g.Count()).Take(6))
        {
            var list = g.ToList();
            if (list.Count < 2) continue;
            int len = g.Key;
            var churn = new int[len];
            for (int i = 1; i < list.Count; i++)
                for (int o = 0; o < len; o++)
                    if (list[i].Body[o] != list[i - 1].Body[o]) churn[o]++;

            int pairs = list.Count - 1;
            Console.WriteLine($"\n-- body len {len}: {list.Count} msgs ({pairs} transitions) --");
            // summarize as offset ranges by churn class
            Console.WriteLine("  offset churn map (. =static  : <10%  + <50%  # >=50%):");
            var map = new char[len];
            for (int o = 0; o < len; o++)
            {
                double r = (double)churn[o] / pairs;
                map[o] = r == 0 ? '.' : r < 0.10 ? ':' : r < 0.50 ? '+' : '#';
            }
            for (int o = 0; o < len; o += 64)
                Console.WriteLine($"  {o,5}: {new string(map, o, Math.Min(64, len - o))}");

            Console.WriteLine($"  top {topN} churning offsets:");
            foreach (var (o, c) in churn.Select((c, o) => (o, c)).OrderByDescending(t => t.c).Take(topN))
                if (c > 0) Console.WriteLine($"     @{o,-5} {100.0 * c / pairs,5:F1}%");
        }
    }
}
