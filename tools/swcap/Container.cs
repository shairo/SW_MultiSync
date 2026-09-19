using System.Buffers.Binary;

namespace SwCap;

public enum Dir : byte { Send = 0, Recv = 1, Marker = 2 }

// One transport frame as captured by swhook.dll (a single SendMessageToUser / received message).
// Size = the frame's TRUE size on the wire. Data may be shorter than Size when the DLL thinned a
// bulk frame (Truncated); Data always holds at least the header bytes.
public sealed record Frame(
    Dir Dir, ulong Seq, ulong TickMs, ulong SteamId,
    int Channel, int Flags, int Size, byte[] Data)
{
    public bool Truncated => Data.Length < Size;

    // Transport framing (see protocol/transport.md): +0 u32 fragFlag, +4 u32 totalBodyLen, +8.. bodyChunk.
    public uint FragFlag     => Data.Length >= 4 ? BinaryPrimitives.ReadUInt32LittleEndian(Data.AsSpan(0)) : 0;
    public uint TotalBodyLen => Data.Length >= 8 ? BinaryPrimitives.ReadUInt32LittleEndian(Data.AsSpan(4)) : 0;
    public bool IsHead       => FragFlag == 0;
    public ReadOnlySpan<byte> BodyChunk => Data.Length > 8 ? Data.AsSpan(8) : ReadOnlySpan<byte>.Empty;
}

// Reads a .swcap file produced by swhook.dll. File header: "SWCAP\x01".
// Per event: u8 dir | u64 seq | u64 tickMs | u64 steamId | i32 ch | i32 flags | u32 cub | bytes.
public static class Container
{
    public static IEnumerable<Frame> Read(string path)
    {
        using var fs = File.OpenRead(path);
        using var r = new BinaryReader(fs);
        var magic = r.ReadBytes(6);
        if (magic.Length != 6 || magic[0] != 'S' || magic[1] != 'W' || magic[2] != 'C' ||
            magic[3] != 'A' || magic[4] != 'P')
            throw new InvalidDataException($"not a .swcap file: {path}");
        int version = magic[5];   // 1 = no thinning, 2 = origCub+storedCub
        if (version is not (1 or 2))
            throw new InvalidDataException($"unsupported .swcap version {version}");

        while (fs.Position < fs.Length)
        {
            var dir = (Dir)r.ReadByte();
            ulong seq = r.ReadUInt64();
            ulong tick = r.ReadUInt64();
            ulong sid = r.ReadUInt64();
            int ch = r.ReadInt32();
            int flags = r.ReadInt32();
            uint origCub = r.ReadUInt32();
            uint storedCub = version == 2 ? r.ReadUInt32() : origCub;
            var data = r.ReadBytes((int)storedCub);
            if (data.Length != (int)storedCub) yield break;  // truncated tail (killed mid-write)
            yield return new Frame(dir, seq, tick, sid, ch, flags, (int)origCub, data);
        }
    }
}
