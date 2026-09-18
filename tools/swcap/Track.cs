using System.Buffers.Binary;

namespace SwCap;

// Record-level views of the type=8 stream. NOTE: record-length rules are not decoded yet, so for
// messages with recordCount > 1 we can only show the raw record region (can't split it). For the
// common recordCount == 1 case the single record IS the whole body from offset 20, so it decodes
// cleanly — that's where field-meaning RE happens. `@20 u32` is treated as the record id here; the
// `rec` listing is how you check whether that id is actually stable per object.
public static class Track
{
    const int RecStart = 20;   // records begin at body offset 20 (after the 20-byte type-8 header)

    static uint U32(byte[] b, int o) => o + 4 <= b.Length ? BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(o)) : 0;
    static float F32(byte[] b, int o) => o + 4 <= b.Length ? BinaryPrimitives.ReadSingleLittleEndian(b.AsSpan(o)) : 0;

    static IEnumerable<Message> Type8Records(string path) =>
        Message.Reassemble(Container.Read(path).ToList())
            .Where(m => m.Complete && m.Dir == Dir.Send && m.Channel == 0
                        && m.MsgType == 8 && m.RecordCount >= 1);

    // List every records-bearing type=8 message: tick, recordCount, first-record id (@20),
    // body length, and the raw record region as hex. Skim this to see which ids recur.
    public static void Rec(string path, int take)
    {
        Console.WriteLine($"{"seq",-7} {"tick",-8} {"recs",-4} {"id@20",-8} {"len",-5} record-bytes");
        foreach (var m in Type8Records(path).Take(take))
        {
            uint id = U32(m.Body, RecStart);
            var rec = m.Body.AsSpan(RecStart);
            var hex = string.Join(' ', rec.ToArray().Take(40).Select(x => x.ToString("X2")));
            Console.WriteLine($"{m.StartSeq,-7} {m.Tick,-8} {m.RecordCount,-4} {id,-8} {rec.Length,-5} {hex}{(rec.Length > 40 ? " ..." : "")}");
        }
    }

    // Time series for one record id. recordCount==1 messages decode fully; for those we print the
    // candidate float fields at @34/@38 (churn-identified movers) so you can watch them evolve
    // while driving and map them to speed/heading/position.
    public static void TrackId(string path, uint id, int take)
    {
        Console.WriteLine($"tracking id={id} (recordCount==1 messages decode floats; others show raw)\n");
        Console.WriteLine($"{"tick",-8} {"recs",-4} {"@28",-10} {"f@34",-12} {"f@38",-12} {"f@42",-12}");
        int shown = 0;
        foreach (var m in Type8Records(path))
        {
            if (m.RecordCount != 1) continue;              // multi-record split not decoded yet
            if (U32(m.Body, RecStart) != id) continue;
            Console.WriteLine($"{m.Tick,-8} {m.RecordCount,-4} {U32(m.Body,28),-10} " +
                $"{F32(m.Body,34),-12:G6} {F32(m.Body,38),-12:G6} {F32(m.Body,42),-12:G6}");
            if (++shown >= take) break;
        }
        if (shown == 0) Console.WriteLine("(no recordCount==1 message carried that id)");
    }
}
