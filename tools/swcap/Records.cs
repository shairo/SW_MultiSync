using System.Linq;
using System.Buffers.Binary;

namespace SwCap;

// Per-tag record length rules for the msgType=8 record batch. See protocol.md. The KPI is: walk
// every record in a message and land EXACTLY on the body end. A tag whose rule we don't know yet
// returns Length=-1, which stops the walk (we can't skip an unknown-length record). `swcap validate`
// reports, per message and overall, how far the walk got — that's "% fully decoded".

public enum RecStatus { Ok, Unknown, Overrun }

public sealed class RecordInfo
{
    public uint Tag;
    public int Start;        // offset in Body of this record's tag u32
    public int Length;       // total record bytes (tag..end); -1 = rule unknown
    public RecStatus Status;
    public string Note = "";
}

public static class Records
{
    static uint U16(byte[] b, int o) => o + 2 <= b.Length ? BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(o)) : (uint)0xFFFF;
    static float F32(byte[] b, int o) => o + 4 <= b.Length ? BinaryPrimitives.ReadSingleLittleEndian(b.AsSpan(o)) : float.NaN;
    static string Str(byte[] b, int o, int n) => o + n <= b.Length && n >= 0 ? System.Text.Encoding.UTF8.GetString(b, o, n) : "?";
    static double F64(byte[] b, int o) => o + 8 <= b.Length ? BinaryPrimitives.ReadDoubleLittleEndian(b.AsSpan(o)) : double.NaN;
    static uint U32(byte[] b, int o) => o + 4 <= b.Length ? BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(o)) : 0xFFFFFFFF;

    // Decode the record whose tag is at `off`. Never reads past b.Length; if the computed length
    // would overrun, returns Status=Overrun (rule known but data short/misaligned).
    public static RecordInfo Decode(byte[] b, int off)
    {
        var r = new RecordInfo { Start = off, Tag = U32(b, off), Length = -1, Status = RecStatus.Unknown };
        int end;
        switch (r.Tag)
        {
            case 0x39: // low-freq position sync: tag + veh + double[3] + float
                r.Length = 4 + 4 + 24 + 4; r.Status = RecStatus.Ok; break;

            case 0x38: // VEHICLE DESPAWN marker (despawn-ONLY): tag + u32 vehId (8 B). Emitted with a 0x2C
                       // for the same id ONLY on a true despawn (workbench recover / command) — NOT on
                       // unload, NOT on destroy. 0x2C is the universal "remove id"; 0x38 is the despawn
                       // discriminator. Confirmed on session_20260913_221632_497 (6 marked events).
                r.Length = 8; r.Status = off + 8 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"despawn veh={U32(b, off + 4)}"; break;

            case 0x2A: // CHARACTER HP SET: tag + u32 charId + f32 hp (100.0) + u8 + u8 (14 B). The old "tooltip, 24+len" reading
                       // was 0x2A(13) + a 0x95 map-object record (10+n) — that is why recordCount counted it as 2.
                       // Seen after 0x65 heals in combat (session_20260913_005757_200) and before 0x95 in the addon captures.
                r.Length = 14; r.Status = off + 14 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"char={U32(b, off + 4)} hp={F32(b, off + 8):F1} u8=({b[off + 12]},{b[off + 13]})"; break;

            case 0xA8: // PLAYER LOOK broadcast, fixed 22: tag + u16 peer_id + 8 B zero + f32 yaw + f32 pitch. Mirror of client 0x66 (count-matched 241:241, session_20260917_011112). VERIFIED
                r.Length = 22; r.Status = off + 22 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;

            case 0x05: // unknown periodic, fixed 34 (tag + u32 id(=0x3E) + 26 B mostly zero). VERIFIED
                r.Length = 34; r.Status = off + 34 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;

            case 0xA6: // SEAT KEY STATE broadcast, fixed 22: tag + u16 peer_id + u32 keyMask (bits0-3 WASD, 11-14 arrows, 15-20 hotkeys) + 12 B zero. Mirror of client 0x64. VERIFIED (session_20260916_002752)
                r.Length = 22; r.Status = off + 22 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;

            case 0x1A: // unknown, fixed 40. VERIFIED
                r.Length = 40; r.Status = off + 40 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;

            case 0xA7: // SEAT AXIS broadcast, fixed 11: tag + u16 peer_id + u32 axisIdx (0..7) + u8 value. Mirror of client 0x65 (runs of 8 at seat enter). VERIFIED (session_20260917_011112)
                r.Length = 11; r.Status = off + 11 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;

            case 0x07: // SEAT-ENTRY SNAPSHOT broadcast, fixed 71: tag + u16 peer_id + zeros + f32 yaw + f32 pitch + zeros. Mirror of client 0x01 (73 B). Count-matched (session_20260917_011112)
                r.Length = 71; r.Status = off + 71 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;

            case 0x1B: // vehicle-related, HIGH freq while a vehicle exists/moves. fixed 34. VERIFIED
                r.Length = 34; r.Status = off + 34 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;

            case 0x1D: // OBJECT position/physics sync (object counterpart of 0x1B). fixed 34 — the
                       // "variable" solve lengths (68/102/136) were just runs of 1..4 back-to-back 0x1D,
                       // one per object (session_...012132, single-body coal; 0x1D=34 -> 511 recs, 0 bad).
                r.Length = 34; r.Status = off + 34 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;

            case 0xB5: // vehicle TELEPORT / setVehiclePos: tag + u32 vehId + double[16] 4x4 transform
                       // matrix (column-major; diagonal 1.0 = identity rot). Translation X,Y,Z at matrix
                       // indices 12,13,14 -> offsets 104/112/120. fixed 136. session_...211421 addon move.
                r.Length = 136;
                r.Status = off + 136 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                if (off + 128 <= b.Length)
                {
                    double tx = BitConverter.ToDouble(b, off + 104);
                    double ty = BitConverter.ToDouble(b, off + 112);
                    double tz = BitConverter.ToDouble(b, off + 120);
                    r.Note = $"veh={U32(b, off + 4)} pos=({tx:F1},{ty:F1},{tz:F1})";
                }
                break;

            case 0x5E: // PLAYER teleport via map FAST-TRAVEL: tag + double[3] world pos + u8. fixed 29.
                       // session_...215536 map fast-travel x2 (Y=0 sea level). addon teleport uses 0x55.
                r.Length = 29;
                r.Status = off + 29 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                if (off + 28 <= b.Length)
                    r.Note = $"fast-travel pos=({BitConverter.ToDouble(b, off + 4):F1},{BitConverter.ToDouble(b, off + 12):F1},{BitConverter.ToDouble(b, off + 20):F1})";
                break;

            case 0x3F: // TREE felled (destroy) marker: tag + u32=0 + u32 tileX + i32 tileZ + u32 treeIndex
                       // + u32 type + u16. fixed 26. session_...223959 (5 trees knocked down by vehicle).
                r.Length = 26; r.Status = off + 26 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                if (off + 20 <= b.Length)
                    r.Note = $"tree tile=({(int)U32(b, off + 8)},{BitConverter.ToInt32(b, off + 12)}) idx={U32(b, off + 16)}";
                break;
            case 0xAD: // TREE fall physics (pairs ~6 ticks after 0x3F): tag + u32 + float[3] (fall
                       // direction / impact). fixed 20. session_...223959.
                r.Length = 20; r.Status = off + 20 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                if (off + 20 <= b.Length)
                    r.Note = $"tree-fall v=({BitConverter.ToSingle(b, off + 8):F2},{BitConverter.ToSingle(b, off + 12):F2},{BitConverter.ToSingle(b, off + 16):F2})";
                break;

            // grid/tile streaming cells (index+value), emitted in interleaved back-to-back runs; each
            // cell starts with its own tag so fixed lengths let the walker consume a whole run.
            // session_...222504 long flight. index = grid cell, value = small signed int (level/state).
            case 0x4A: // TILE PURCHASED broadcast: tag + i32 tileX + i32 tileZ (12 B), each followed by 0x62. Mirror of the client purchase request. VERIFIED recordCount (session_20260917_012747 single, 013059 buy-all x30)
                r.Length = 12; r.Status = off + 12 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"tile=({(int)U32(b, off + 4)},{(int)U32(b, off + 8)})"; break;
            case 0x46: r.Length = 12; r.Status = off + 12 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                       r.Note = $"tile=({(int)U32(b, off + 4)},{(int)U32(b, off + 8)})"; break; // tile streaming ring: i32 x, i32 z (no client trigger; precedes 0x47 runs)
            case 0x49: r.Length = 12; r.Status = off + 12 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                       r.Note = $"tile=({(int)U32(b, off + 4)},{(int)U32(b, off + 8)})"; break; // FOG REVEALED tile: i32 x, i32 z — mirror of client 0x28, always paired with 0x45 (session_20260917_012608)
            case 0x45: r.Length = 13; r.Status = off + 13 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                       r.Note = $"tile=({(int)U32(b, off + 4)},{(int)U32(b, off + 8)}) f={b[off + 12]}"; break; // fog tile state: i32 x, i32 z, u8 (0 seen) — follows each 0x49

            // --- fixed-length candidates from scenario captures (verified by validate: 0 overruns) ---
            case 0x09: r.Length = 4;  r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // bare tag/flag
            case 0x14: r.Length = 30; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // S3/S5/S6 button-ish
            case 0x15: r.Length = 10; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // S3/S6, pairs with 0x61
            case 0x20: r.Length = 52; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // S4 equip-adjacent
            case 0x50: // NPC/object state (position + HP). VARIABLE = 12 + u32 payloadLen@8. Across all
                       // captures len is 76 (payload 64, the common single form, 3333×) or 328 (payload
                       // 316 = 124-B header + 3×64-B entries; this server's addon target list, id 3727,
                       // pos + HP=100.0f). NPCs and world objects share an id space, so 0x50 may carry
                       // either. Was previously a fixed-76 guess (which mis-split the 328 form's zero tail
                       // into a phantom 0x00 record).
                r.Length = 12 + (int)U32(b, off + 8);
                r.Status = off + r.Length <= b.Length && r.Length >= 12 ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"id={U32(b, off + 4)} payload={U32(b, off + 8)}"; break;
            case 0x55: // PLAYER teleport via ADDON (server.setPlayerPos): tag + u16 + double[3] pos + u8.
                       // fixed 31. session_...215536 addon tp x2 (Y=18.45 = flag altitude). map FT uses 0x5E.
                r.Length = 31;
                r.Status = off + 31 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                if (off + 30 <= b.Length)
                    r.Note = $"addon-tp pos=({BitConverter.ToDouble(b, off + 6):F1},{BitConverter.ToDouble(b, off + 14):F1},{BitConverter.ToDouble(b, off + 22):F1})";
                break;
            case 0x62: r.Length = 12; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // S1/S9
            case 0x67: // CHARACTER VITALS broadcast: tag + u32 charId + u8 1 + f32 hp + u32 (36000) + f32 100 + f32 (5/15). Mirror of client 0x39. VERIFIED (session_20260917_002237)
                r.Length = 25; r.Status = off + 25 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"char={U32(b, off + 4)} hp={BinaryPrimitives.ReadSingleLittleEndian(b.AsSpan(off + 9)):F1}"; break;
            case 0x88: // GAME SETTINGS bool array broadcast: tag + 33 x u8. Mirror of client 0x48 (sent on every settings-menu toggle). VERIFIED (session_20260917_005633)
                r.Length = 37; r.Status = off + 37 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = "bools=" + string.Concat(Enumerable.Range(0, 33).Select(i => off + 4 + i < b.Length ? (char)('0' + b[off + 4 + i]) : '?')); break;
            case 0x94: r.Length = 8;  r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // tag + u32 vehId; HIGH freq w/ multiple vehicles
            case 0x4F: r.Length = 8;  r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; // OBJECT despawn/collect: tag + u32 objectId. Object spawn is 0x4D. (session_...012132: 233 ids paired 0x4D spawn -> 0x4F despawn, ~0.8s lifetime)
                       r.Note = $"obj-despawn id={U32(b, off + 4)}"; break;
            // multiplayer: other-player avatar sync, emitted as a 0x0B+0x0C pair per remote player.
            case 0x0B: r.Length = 38; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // tag + u16 + double[3] pos + 8B
            case 0x0D: // NPC FOLLOW toggle broadcast: tag + u16 peer_id + u32 playerId + u32 npcId. Mirror of client 0x14. VERIFIED (session_20260916_003534)
            case 0x0E: // NPC PICK-UP (carry) broadcast: same layout as 0x0D. Mirror of client 0x13. VERIFIED (session_20260916_003534)
                r.Length = 14; r.Status = off + 14 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"player={U32(b, off + 6)} npc={U32(b, off + 10)}"; break;
            case 0x17: // NPC PUT-DOWN broadcast: tag + u32 playerId + 17 B zero (emitted as a pair). Mirror of client 0x06. VERIFIED (session_20260916_003534)
                r.Length = 25; r.Status = off + 25 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"player={U32(b, off + 4)}"; break;
            case 0x64: // CHARACTER DEATH broadcast (server reply to client 0x37): tag + u32 charId + u32 46 + u32 21 + u32 payloadLen + payload (component-value entries: u8 1 · u32 width · value[width] · u8 0 · u32 idx). 20 + len. session_20260917_002237 (70 B). recordCount counts it as 2 (compound, like 0x38/0x96).
                r.Length = 20 + (int)U32(b, off + 16); r.Status = off + r.Length <= b.Length && r.Length >= 20 ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"char={U32(b, off + 4)} payload={U32(b, off + 16)}"; break;
            case 0x06: // CHARACTER FULL STATE on player join: tag + u16 peer_id + u32 charId + u32 len + payload[len] = 14 + len. The payload is what a 0x4D spawn of that character carries. VERIFIED (session_20260923_182705_304; also 002757 seq 8268, walkfail_20260919_125459)
                r.Length = 14 + (int)U32(b, off + 10); r.Status = off + r.Length <= b.Length && r.Length >= 14 ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"peer={U16(b, off + 4)} char={U32(b, off + 6)} payload={U32(b, off + 10)}"; break;
            case 0x0A: // join sequence: tag + u8 0, between the 0x06 character dump and the 0x88 settings. VERIFIED (same captures)
                r.Length = 5; r.Status = off + 5 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;
            case 0x65: // HEAL applied (first-aid kit): tag + u32 charId + f32 amount (50.0) + u8. Mirror of client 0x38; emitted with the 0x29 primary mirror of item 11. VERIFIED (session_20260917_002831)
                r.Length = 13; r.Status = off + 13 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"char={U32(b, off + 4)} amount={F32(b, off + 8):F1}"; break;
            // --- world / game settings broadcasts (settings.md), VERIFIED session_20260917_005633 ---
            case 0x5A: // WEATHER broadcast: tag + f32[3] (fog/rain/wind sliders) + f32 wind direction (rad). Mirror of client 0x2A (f32[3]) + 0x49.
                r.Length = 20; r.Status = off + 20 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"f=({F32(b, off + 4):F3},{F32(b, off + 8):F3},{F32(b, off + 12):F3},{F32(b, off + 16):F3})"; break;
            case 0x57: // TIME OF DAY broadcast: tag + u32 seconds (= client 0x31 f32 fraction x 108000).
                r.Length = 8; r.Status = off + 8 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"u32={U32(b, off + 4)}"; break;
            case 0x89: // WIND DIRECTION broadcast: tag + f32 rad. Mirror of client 0x49; same value also lands in 0x5A[3].
                r.Length = 8; r.Status = off + 8 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"f={F32(b, off + 4):F3}"; break;
            case 0x8B: // DAY LENGTH (?) broadcast: tag + u32 (20/30/40 seen), mirror of client 0x4B; followed by 0x57 time.
                r.Length = 8; r.Status = off + 8 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"u32={U32(b, off + 4)}"; break;
            case 0x59: // settings bool broadcast: tag + u8. 0x59 = client 0x32 (day/night cycle?), 0x5D = client 0x2C (weather override?), 0x8A = client 0x4A (wind-dir override?).
            case 0x5D:
            case 0x8A:
                r.Length = 5; r.Status = off + 5 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"b={b[off + 4]}"; break;
            // --- combat-capture tags, boundaries from session_20260913_005757_200 (meanings CANDIDATE) ---
            case 0x92: // tag + double[3] world pos + f32[4] (44 B). ~1:1 with 0x76 in the long-range fight.
                r.Length = 44; r.Status = off + 44 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"pos=({F64(b, off + 4):F0},{F64(b, off + 12):F0},{F64(b, off + 20):F0}) f=({F32(b, off + 28):F1},{F32(b, off + 32):F1},{F32(b, off + 36):F1},{F32(b, off + 40):F1})"; break;
            case 0x76: // tag + double[3] world pos + f32 (32 B). recordCount-verified (seq 1208776).
                r.Length = 32; r.Status = off + 32 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"pos=({F64(b, off + 4):F0},{F64(b, off + 12):F0},{F64(b, off + 20):F0}) f={F32(b, off + 28):F1}"; break;
            case 0x31: // VOXEL event: tag + u32 vehId + i32 x + i32 y + i32 z (20 B), runs per vehicle (voxel damage list).
                r.Length = 20; r.Status = off + 20 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"veh={U32(b, off + 4)} voxel=({(int)U32(b, off + 8)},{(int)U32(b, off + 12)},{(int)U32(b, off + 16)})"; break;
            case 0x9D: // tag + u32 id + u32 1 + double[3] pos + 7 x u32/f32 (0,1,0,0,0,0,1) + u8 1 + u16 0xFFFF (67 B), runs.
                r.Length = 67; r.Status = off + 67 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"id={U32(b, off + 4)} pos=({F64(b, off + 12):F0},{F64(b, off + 20):F0},{F64(b, off + 28):F0})"; break;
            case 0x7B: // NOTIFICATION (addon server.notify): tag + u16 n1 + title + u16 n2 + subtitle + u32 type = 12 + n1 + n2. VARIABLE.
            {
                int n1 = (int)U16(b, off + 4); int n2 = (int)U16(b, off + 6 + n1);
                r.Length = 12 + n1 + n2; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"title={Str(b, off + 6, n1)} type={U32(b, off + 8 + n1 + n2)}"; break;
            }
            case 0x4C: // tag + u32 id (8 B) — emitted as a 0x4C/0x8F/0x3D/0x3B quartet per id (map-object family, see 0x3B).
            case 0x8F:
            case 0x3D:
                r.Length = 8; r.Status = off + 8 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"id={U32(b, off + 4)}"; break;
            case 0xA9: // tag + i32 tileX + i32 tileZ + u32 100 + u32 0 + f32 500 + f32 200 + u32 0 (32 B).
                r.Length = 32; r.Status = off + 32 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"tile=({(int)U32(b, off + 4)},{(int)U32(b, off + 8)})"; break;
            case 0x74: // tag + double[3] pos + u32 1 + f32 0.5 + f32 1.0 (40 B); paired with 0x75. CANDIDATE
                r.Length = 40; r.Status = off + 40 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"pos=({F64(b, off + 4):F0},{F64(b, off + 12):F0},{F64(b, off + 20):F0})"; break;
            case 0x75: // tag + double[3] pos + f32 + f32 1.0 (36 B); follows 0x74. CANDIDATE
                r.Length = 36; r.Status = off + 36 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"pos=({F64(b, off + 4):F0},{F64(b, off + 12):F0},{F64(b, off + 20):F0}) f={F32(b, off + 28):F2}"; break;
            // --- lobby / peer records, boundaries from session_20260913_002757. CANDIDATE lengths ---
            case 0x03: // PLAYER JOINED: tag + u16 peer_id + u16 n + name + u16 0 + u8 1 = 11 + n. Followed by a 0x7B "Player Joined" notify and the 0x06 character dump.
            {
                int n = (int)U16(b, off + 6);
                r.Length = 11 + n; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"peer={U16(b, off + 4)} name={Str(b, off + 8, n)}"; break;
            }
            case 0x8D: // PEER-ADDRESSED MESSAGE: tag + u16 peer_id + 6 B (01 01 01 00 00 00) + u16 n1 + text + u16 n2 + sender = 16 + n1 + n2.
            {
                int n1 = (int)U16(b, off + 12); int n2 = (int)U16(b, off + 14 + n1);
                r.Length = 16 + n1 + n2; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"peer={U16(b, off + 4)} text={Str(b, off + 14, n1)}"; break;
            }
            case 0x91: // tag + u16 peer_id + u32 21 + u32 2 + u32 0 + u32 6 + i32 -7 + u16 1 (28 B).
                r.Length = 28; r.Status = off + 28 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"peer={U16(b, off + 4)}"; break;
            case 0x5F: // tag + u16 peer_id + u32 (10 B).
                r.Length = 10; r.Status = off + 10 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"peer={U16(b, off + 4)} u32={U32(b, off + 6)}"; break;
            case 0x16: // tag + u16 peer_id + u32 + u8 (11 B).
                r.Length = 11; r.Status = off + 11 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"peer={U16(b, off + 4)} u32={U32(b, off + 6)}"; break;
            case 0xB8: // tag + u32 vehId + u32 + i32 + u32 + i32 -1 + u32 vehId + u32 + u32 1 + u32 1 + 12 B (52 B), runs.
                r.Length = 52; r.Status = off + 52 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"veh={U32(b, off + 4)} u32={U32(b, off + 8)}"; break;
            case 0xB9: // same header as 0xB8 + 8 B (48 B), runs.
                r.Length = 48; r.Status = off + 48 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"veh={U32(b, off + 4)} u32={U32(b, off + 8)}"; break;
            case 0xBA: // 2x(veh,idx,i32[3]) + u8 (45 B). walkfail_20260919_125459
                r.Length = 45; r.Status = off + 45 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"veh={U32(b, off + 4)} u32={U32(b, off + 8)}"; break;
            case 0xBF: // 4x(veh,idx,i32[3]) + u32 RGBA (88 B), pairs with 0xC0 on the same voxels. walkfail_20260919_125459
                r.Length = 88; r.Status = off + 88 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"veh={U32(b, off + 4)} rgba={U32(b, off + 84):X8}"; break;
            case 0xC0: // 4x(veh,idx,i32[3],f32) (100 B). walkfail_20260919_125459
                r.Length = 100; r.Status = off + 100 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"veh={U32(b, off + 4)}"; break;
            case 0x04: // 16 B fixed, always the last record of its message. walkfail_20260919_125459 (x13)
                r.Length = 16; r.Status = off + 16 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;
            case 0x63: // RESPAWN POSITION: tag + u16 peer_id (probably) + double[3] world pos (30 B). Seen once, right after the death-phase 0x26 records. VERIFIED boundary (session_20260917_002237)
                r.Length = 30; r.Status = off + 30 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"pos=({F64(b, off + 6):F1},{F64(b, off + 14):F1},{F64(b, off + 22):F1})"; break;
            case 0x26: // CHARACTER DEATH/RESPAWN-phase record: tag + u32 charId + u32 2 + 13 B zero + u32 -1 + 5 B zero + u8 1. Boundary from 1 sample (session_20260917_002237, fire death). Length 35 CANDIDATE.
                r.Length = 35; r.Status = off + 35 <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"char={U32(b, off + 4)} u32={U32(b, off + 8)}"; break;
            case 0x0C: r.Length = 14; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;
            case 0x27: r.Length = 12; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // combat
            case 0x35: r.Length = 34;  r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; // BODY DAMAGE: tag+u32 vehId+u32 bodyIdx+float[3] hitPos+float magnitude(1000/5000)+float+u16. One per body damage-application (client expands to voxels). Multi-damage quirk repeats identical records. (session_...005445 vs [Damage] log, vid 74, bodies 0..9).
                       r.Note = $"dmg veh={U32(b, off + 4)} body={U32(b, off + 8)}"; break;
            case 0x61: r.Length = 116; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // RGBA-ish blob, fixed 116

            case 0x19: r.Length = 54; r.Status = off + 54 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // combat, fixed 54
            case 0x2C: r.Length = 8;  r.Status = off + 8  <= b.Length ? RecStatus.Ok : RecStatus.Overrun; // tag + u32 vehId: universal "remove vehicle id from client" (unload / despawn / destroy all emit it)
                       r.Note = $"remove veh={U32(b, off + 4)}"; break;

            // --- vehicle spawn cluster (session_20260914_232332_784, solved ×6; VERIFY on more data) ---
            case 0x96: r.Length = 13; r.Status = off + 13 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; // spawn create: tag + u32 id + 5B. Emitted as a PAIR (two 13-B 0x96: id then 0) = one create, two records.
                       r.Note = $"veh={U32(b, off + 4)}"; break;
            case 0x37: // spawn placement: tag + u32 vehId + double[3] pos + 12B + f32 + u8 u8 + u16 nA + name + u16 nB + strB (e.g. "tan524tan524") = 54 + nA + nB (54 when both empty). walkfail_20260919_125459
            {
                int nA = (int)U16(b, off + 50); int nB = (int)U16(b, off + 52 + nA);
                r.Length = 54 + nA + nB; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"veh={U32(b, off + 4)} name={Str(b, off + 52, nA)} B={Str(b, off + 54 + nA, nB)}"; break;
            }
            case 0x2B: // spawn placement2: tag + double[3] pos + f32[4] quat + u16 nA + strA (spawn-location name, e.g. "hangar_edit") + u16 nB + strB (display name, e.g. "BLUE" for an addon flag) + double[3] + u32 (= vehId for a normal spawn, vehId-2 for the addon flags — TBD) + u32 vehId + u32 + 4 B flags + u16 = 90 + nA + nB. VARIABLE. recordCount-verified (session_20260917_003856 / 005339 / 010256)
            {
                int nA = (int)U16(b, off + 44); int nB = (int)U16(b, off + 46 + nA);
                r.Length = 90 + nA + nB; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"A={Str(b, off + 46, nA)} B={Str(b, off + 48 + nA, nB)} u32={U32(b, off + r.Length - 18)} veh={U32(b, off + r.Length - 14)}"; break;
            }
            case 0x95: // MAP OBJECT create (addon addMapObject): tag + u32 objId + u16 n + label[n] = 10 + n. Always followed by a 0x3A map-label record carrying position / radius / colour. VERIFIED (session_20260917_010256, recordCount)
            {
                int n = (int)U16(b, off + 8);
                r.Length = 10 + n; r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"id={U32(b, off + 4)} label={Str(b, off + 10, n)}"; break;
            }
            case 0x3B: r.Length = 8;  r.Status = off + 8  <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // tag + u32 (runs; precedes 0x3A)
            case 0x3A: // MAP OBJECT (addon addMapObject): tag + u32 posType + u32 markerType + double x,y,z,?,radius + u32 0 + u32 objId + u32 0 + u16 n1 + label + u16 n2 + hoverLabel + f32 1000 + u32 + rgba = 84 + n1 + n2. VARIABLE. Preceded by 0x3B or 0x95. (session_20260915_215903, session_20260917_010256)


                {
                    int p = off + 68; // past header + FFFFFFFF marker + first u16 len field start
                    if (p + 2 <= b.Length) { int l1 = (int)U16(b, p); p += 2 + l1;
                        if (p + 2 <= b.Length) { int l2 = (int)U16(b, p); p += 2 + l2; } }
                    p += 12;
                    r.Length = p - off;
                    r.Status = p <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                }
                break;
            case 0x30: r.Length = 36; r.Status = off + 36 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // fixed 36 (list entries)
            case 0x34: r.Length = 49; r.Status = off + 49 <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break; // fixed 49
            case 0x28: r.Length = U32(b, off + 8) == 1 ? 12 : 46; // subtype @8: 1=minimal(12) else full(46)
                       r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun; break;
            case 0x2D: // per-vehicle data push family (like 0x2E): tag + u32 vehId + u32 payloadLen + payload
            case 0x2F:
                r.Length = 12 + (int)U32(b, off + 8);
                r.Status = off + r.Length <= b.Length && r.Length >= 12 ? RecStatus.Ok : RecStatus.Overrun;
                break;

            case 0x47: // combat: tag + u32 + i32 + u32 payloadLen@12 + payload
                r.Length = 16 + (int)U32(b, off + 12);
                r.Status = off + r.Length <= b.Length && r.Length >= 16 ? RecStatus.Ok : RecStatus.Overrun;
                break;

            case 0x4D: // vehicle spawn full-state: tag + u32 handle + u32 payloadLen + payload
                r.Length = 12 + (int)U32(b, off + 8);
                r.Status = off + r.Length <= b.Length && r.Length >= 12 ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"handle={U32(b, off + 4):X} payload={U32(b, off + 8)}";
                break;

            case 0x01: // CHAT / system message: tag + u16 textLen + text + u16 nameLen + senderName.
                       // Confirmed on session_20260914_230220 (sent hoge/fuga/piyopiyo as "Shairo-jp";
                       // "?echo xyz" came back as text "xyz" sender "[Server]"). Also carries system
                       // log lines ("Connect Restored","[WebMap]"). Low frequency.
            {
                int a2 = (int)U16(b, off + 4);
                int b2 = (int)U16(b, off + 6 + a2);
                r.Length = 8 + a2 + b2;
                r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                break;
            }
            // 0x47 is VARIABLE (21 in solo spawn, longer in combat) — left unknown until decoded (a
            // fixed guess overran 42 msgs). Likewise 0x19/0x28/0x3B/0x35/0x2D/0x61 carry large
            // combat payloads (projectiles/damage) — real decode needed, not a fixed length.

            case 0x29: // equip/inventory ACTION: tag + u32 playerId + u32 slot + u16 strlen + action + u8 +
                       // float[2]. action string = "equip"/"swap"/"store"/etc. VERIFIED
                {
                    int sl = (int)U16(b, off + 12);
                    r.Length = 23 + sl;
                    r.Status = off + r.Length <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                    string act = off + 14 + sl <= b.Length ? System.Text.Encoding.ASCII.GetString(b, off + 14, sl) : "?";
                    r.Note = $"player={U32(b, off + 4)} slot={U32(b, off + 8)} action={act}";
                }
                break;

            case 0x2E: // per-vehicle variable data push: tag + u32 vehId + u32 payloadLen + payload
                r.Length = 12 + (int)U32(b, off + 8);
                r.Status = off + r.Length <= b.Length && r.Length >= 12 ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"veh={U32(b, off + 4)} payload={U32(b, off + 8)}";
                break;

            case 0x8E: // popup: tag + u16 name + name + u16 show + u16 text + text + 4 floats + 3 u32
            {
                int p = off + 4;
                int nameSize = (int)U16(b, p); p += 2 + nameSize;
                p += 2;                                   // show
                int textSize = (int)U16(b, p); p += 2 + textSize;
                p += 16 + 12;                             // x,y,z,renderDist + uiId,vehParent,objParent
                r.Length = p - off;
                r.Status = p <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = $"name={nameSize} text={textSize}";
                break;
            }

            case 0x81: // vehicle body sync: tag + veh + time + u16 bodyCount + bodies
            {
                int p = off + 4 + 4 + 4;
                int bodyCount = (int)U16(b, p); p += 2;
                for (int i = 0; i < bodyCount; i++)
                {
                    if (p >= b.Length) { r.Status = RecStatus.Overrun; break; }
                    int type = b[p]; p += 1;
                    p += type switch { 0 => 0, 1 => 40, 2 => 28, _ => -1 };
                    if (type > 2) { r.Note = $"bad bodyType={type} @body{i}"; r.Status = RecStatus.Overrun; break; }
                }
                r.Length = p - off;
                if (r.Status != RecStatus.Overrun)
                    r.Status = p <= b.Length ? RecStatus.Ok : RecStatus.Overrun;
                r.Note = r.Note == "" ? $"bodies={bodyCount}" : r.Note;
                break;
            }

            default:
                r.Length = -1; r.Status = RecStatus.Unknown; break;
        }
        return r;
    }

    // Walk all records of a type=8 message body (records begin at offset 20). Stops at the first
    // unknown/overrun record. Returns the decoded records plus whether the walk consumed the whole
    // body exactly (fully decoded) and how many bytes it covered.
    public static (List<RecordInfo> recs, bool full, int consumed) Walk(Message m)
    {
        var body = m.Body;
        var list = new List<RecordInfo>();
        int off = 20, count = (int)m.RecordCount;
        for (int i = 0; i < count; i++)
        {
            if (off >= body.Length) break;
            var r = Records.Decode(body, off);
            list.Add(r);
            if (r.Status != RecStatus.Ok || r.Length <= 0) return (list, false, off);
            off += r.Length;
        }
        bool full = off == body.Length && list.Count == count;
        return (list, full, off);
    }
}
