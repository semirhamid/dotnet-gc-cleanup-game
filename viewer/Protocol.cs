using System.Buffers.Binary;
using System.Text;

namespace ManualGC.Viewer;

/// <summary>Message types. docs/protocol.md describes them; the GC's copy is in gc/protocol.h.</summary>
enum MessageType : byte
{
    Hello = 1,
    Module = 2,
    Type = 3,
    Objects = 4,
    SnapshotEnd = 5,
    EraseResults = 6,
    References = 7,
    Roots = 8,
    Attach = 9,

    Erase = 64,
    Done = 65,
    Collect = 66,
}

enum EraseStatus : byte
{
    Freed = 0,
    Alive = 1,
    NotAnObject = 2,
    Destroyed = 3,  // freed although alive, in hardcore mode
}

sealed class ProtocolReader(byte[] buffer, int start, int end)
{
    public const uint Version = 3;

    int _at = start;

    public int Remaining => end - _at;

    public byte U8() => Take(1)[0];
    public ushort U16() => BinaryPrimitives.ReadUInt16LittleEndian(Take(2));
    public uint U32() => BinaryPrimitives.ReadUInt32LittleEndian(Take(4));
    public ulong U64() => BinaryPrimitives.ReadUInt64LittleEndian(Take(8));
    public string Rest() => Encoding.UTF8.GetString(Take(Remaining));

    ReadOnlySpan<byte> Take(int n)
    {
        if (Remaining < n)
            throw new InvalidDataException("message too short");
        _at += n;
        return buffer.AsSpan(_at - n, n);
    }
}
