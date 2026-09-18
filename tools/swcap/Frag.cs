using System.Buffers.Binary;

namespace SwCap;

// Fragmentation investigation: does the GAME split a large logical message into multiple
// SendMessageToUser frames, or does it hand one big frame to Steam (which fragments on the wire,
// invisibly to us)? Works purely from frame structure (fragFlag + totalBodyLen + true size), so it
// is correct even on thinned captures where payloads were dropped.
public static class Frag
{
    public static void Report(string path)
    {
        // per (dir, channel, headMsgType): single-frame vs multi-frame head counts + size extremes
        var b = new Dictionary<(Dir, int, long), (int single, int frag, long maxSingle, long maxFrag)>();
        // non-head ("lane") frames: their +0 value distribution per (dir,channel)
        var lanes = new Dictionary<(Dir, int), Dictionary<uint, int>>();

        foreach (var f in Container.Read(path))
        {
            if (f.IsHead)
            {
                long bodyThisFrame = f.Size - 8;          // true body bytes carried by this frame
                long total = f.TotalBodyLen;
                long msgType = f.Data.Length >= 16
                    ? BinaryPrimitives.ReadUInt32LittleEndian(f.Data.AsSpan(12)) : -1;
                var key = (f.Dir, f.Channel, msgType);
                (int single, int frag, long maxSingle, long maxFrag) v =
                    b.TryGetValue(key, out var cur) ? cur : default;
                if (total <= bodyThisFrame)
                    v = (v.single + 1, v.frag, Math.Max(v.maxSingle, total), v.maxFrag);
                else
                    v = (v.single, v.frag + 1, v.maxSingle, Math.Max(v.maxFrag, total));
                b[key] = v;
            }
            else
            {
                var lk = (f.Dir, f.Channel);
                if (!lanes.TryGetValue(lk, out var d)) lanes[lk] = d = new();
                d[f.FragFlag] = d.GetValueOrDefault(f.FragFlag) + 1;
            }
        }

        Console.WriteLine("== head frames: single-frame vs GAME-level fragmentation ==");
        Console.WriteLine("(single = whole logical message fit in one SendMessageToUser call;");
        Console.WriteLine(" frag   = head claimed more body than one frame carried → game split it)\n");
        Console.WriteLine($"  {"dir",-4} {"ch",-3} {"msgType",-8} {"single",8} {"frag",6}  {"maxSingle",12}  {"maxFrag",12}");
        foreach (var kv in b.OrderBy(k => k.Key))
        {
            var (dir, ch, mt) = kv.Key; var v = kv.Value;
            Console.WriteLine($"  {dir,-4} {ch,-3} {mt,-8} {v.single,8} {v.frag,6}  {v.maxSingle,12:N0}  {v.maxFrag,12:N0}");
        }

        Console.WriteLine("\n== non-head (continuation/lane) frames: +0 value distribution ==");
        if (lanes.Count == 0) Console.WriteLine("  (none)");
        foreach (var kv in lanes.OrderBy(k => k.Key))
            Console.WriteLine($"  {kv.Key.Item1,-4} ch{kv.Key.Item2}: " +
                string.Join("  ", kv.Value.OrderBy(x => x.Key).Select(x => $"+0={x.Key}:{x.Value}")));

        Console.WriteLine("\nInterpretation: if a msgType shows frag>0 the GAME splits it (watch which size");
        Console.WriteLine("triggers it). If gameplay type=8 stays single up to large sizes, Steam is doing the");
        Console.WriteLine("wire-level fragmentation and the game only hand-splits above Steam's ~512 KB cap.");
    }
}
