using System.Buffers.Binary;

namespace SwCap;

// A reassembled logical message (one or more fragments joined). Body = the concatenated
// bodyChunks. For msgType 8 the body is [u32=1][u32 msgType][u32 tick][u32 subType]
// [u32 recordCount][records...].
public sealed class Message
{
    public required Dir Dir;
    public required ulong StartSeq;      // seq of the head frame
    public required ulong SteamId;
    public required int Channel;
    public required int Flags;
    public required int FragmentCount;
    public required byte[] Body;         // length == head.TotalBodyLen when complete
    public bool Complete;

    uint U32(int off) => off + 4 <= Body.Length ? BinaryPrimitives.ReadUInt32LittleEndian(Body.AsSpan(off)) : 0xFFFFFFFF;
    public uint Const1      => U32(0);
    public uint MsgType     => U32(4);
    public uint Tick        => U32(8);
    public uint SubType     => U32(12);
    public uint RecordCount => U32(16);
    public ReadOnlySpan<byte> Records => Body.Length > 20 ? Body.AsSpan(20) : ReadOnlySpan<byte>.Empty;

    // True when this message is a fully-contained single frame (TotalBodyLen fit in one send).
    // Every gameplay msgType=8 packet observed so far is single-frame. Multi-frame BULK transfers
    // (tiles type=9, big mod-file pushes) use a DIFFERENT, not-yet-decoded chunking scheme where
    // the +0 field takes values 0..3 (a lane/stream id, NOT a head/continuation flag) — see
    // AGENTS.md. We do NOT guess-merge those; each such frame passes through as Bulk=true so no
    // data is silently dropped and the bulk scheme can be studied separately.
    public bool Bulk;

    // Turn transport frames into logical messages. Only genuine single-frame messages are emitted
    // as Complete; a frame that claims to be part of a larger body is emitted as Bulk (unmerged),
    // because the multi-frame reassembly scheme is unsolved. Input must be in seq order.
    public static IEnumerable<Message> Reassemble(IEnumerable<Frame> frames)
    {
        foreach (var f in frames)
        {
            if (f.Dir == Dir.Marker) continue;   // annotation events carry no message
            bool single = f.IsHead && !f.Truncated && f.BodyChunk.Length >= f.TotalBodyLen;
            var m = new Message {
                Dir = f.Dir, StartSeq = f.Seq, SteamId = f.SteamId,
                Channel = f.Channel, Flags = f.Flags, FragmentCount = 1,
                Complete = single, Bulk = !single,
                Body = single ? f.BodyChunk.Slice(0, (int)f.TotalBodyLen).ToArray()
                              : f.Data     // keep the whole raw frame for bulk study
            };
            yield return m;
        }
    }
}
