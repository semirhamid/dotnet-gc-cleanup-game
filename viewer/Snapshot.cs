using System.Runtime.InteropServices;

namespace ManualGC.Viewer;

/// <summary>The heap as the GC described it. Index i is one object or one free block, in address order.</summary>
sealed class Snapshot(uint pid, uint gc, ulong heapBase, ulong end, byte flags)
{
    /// <summary>Blocks sent to browsers. The rest of a larger heap cannot be seen or erased.</summary>
    public const int MaxShown = 2_000_000;

    readonly List<uint> _offsets = [], _sizes = [], _types = [];  // in units of 8 bytes
    readonly List<bool> _live = [];
    readonly List<uint> _from = [], _to = [];  // references, as offsets in units of 8 bytes
    readonly List<(uint Offset, byte Kind)> _roots = [];
    readonly List<string> _typeNames = ["free space"];
    readonly Dictionary<ulong, uint> _typeIndex = [];

    public uint Pid => pid;
    public uint Gc => gc;
    public ulong Base => heapBase;
    public ulong End => end;
    /// <summary>The app was started with MANUALGC_HARDCORE=1.</summary>
    public bool HardcoreAllowed => (flags & 1) != 0;
    public TypeNames Names { get; } = new();
    public bool OutOfMemory => (flags & 2) != 0;
    public int Count => _offsets.Count;
    public int Shown => Math.Min(Count, MaxShown);
    public IReadOnlyList<string> TypeNames => _typeNames;

    public void Add(ulong address, ulong sizeAndFlags, ulong type)
    {
        bool free = (sizeAndFlags & 2) != 0;
        uint index = 0;
        if (!free && !_typeIndex.TryGetValue(type, out index))
        {
            _typeIndex[type] = index = (uint)_typeNames.Count;
            _typeNames.Add(Names.Get(type));
        }
        _offsets.Add((uint)((address - heapBase) >> 3));
        _sizes.Add((uint)(sizeAndFlags >> 3));
        _types.Add(index);
        _live.Add((sizeAndFlags & 1) != 0);
    }

    /// <summary>Index of the block at an address, or -1.</summary>
    public int Find(ulong address)
    {
        int i = address < heapBase ? -1 : _offsets.BinarySearch((uint)((address - heapBase) >> 3));
        return i < 0 ? -1 : i;
    }

    public long DeadBytes()
    {
        long bytes = 0;
        for (int i = 0; i < Count; i++)
            if (!IsFree(i) && !IsLive(i)) bytes += Size(i);
        return bytes;
    }

    /// <summary>Records that the GC freed a block.</summary>
    public void SetFree(int i)
    {
        _types[i] = 0;
        _live[i] = false;
    }

    public void AddReference(ulong from, ulong to)
    {
        _from.Add((uint)((from - heapBase) >> 3));
        _to.Add((uint)((to - heapBase) >> 3));
    }

    public void AddRoot(ulong address, byte kind) => _roots.Add(((uint)((address - heapBase) >> 3), kind));

    public int ReferenceCount => _from.Count;
    public int TypeOf(int i) => (int)_types[i];

    /// <summary>References as block indexes. Both must be objects in this snapshot.</summary>
    public IEnumerable<(int From, int To)> References()
    {
        int from = 0;
        for (int e = 0; e < _from.Count; e++)
        {
            // Sources arrive in address order, apart from the few from handles at the end.
            if (_offsets[from] != _from[e])
                from = _offsets.BinarySearch(_from[e]);
            int to = _offsets.BinarySearch(_to[e]);
            if (from >= 0 && to >= 0)
                yield return (from, to);
            from = Math.Max(from, 0);
        }
    }

    public IEnumerable<(int Block, byte Kind)> Roots()
    {
        foreach (var (offset, kind) in _roots)
        {
            int i = _offsets.BinarySearch(offset);
            if (i >= 0) yield return (i, kind);
        }
    }

    public bool IsFree(int i) => _types[i] == 0;
    public bool IsLive(int i) => _live[i];
    public ulong Address(int i) => heapBase + ((ulong)_offsets[i] << 3);
    public long Size(int i) => (long)_sizes[i] << 3;

    /// <summary>u32 tag 1, u32 count, then three u32 arrays: offset / 8, size / 8, type index.</summary>
    public byte[] ToFrame()
    {
        int n = Shown;
        var frame = new byte[8 + n * 12];
        var words = MemoryMarshal.Cast<byte, uint>(frame.AsSpan());
        words[0] = 1;
        words[1] = (uint)n;
        CollectionsMarshal.AsSpan(_offsets)[..n].CopyTo(words[2..]);
        CollectionsMarshal.AsSpan(_sizes)[..n].CopyTo(words[(2 + n)..]);
        CollectionsMarshal.AsSpan(_types)[..n].CopyTo(words[(2 + 2 * n)..]);
        return frame;
    }

    /// <summary>u32 tag 2, u32 count, then one byte per block: 1 live, 0 dead or free.</summary>
    public byte[] LivenessFrame()
    {
        int n = Shown;
        var frame = new byte[8 + n];
        MemoryMarshal.Cast<byte, uint>(frame.AsSpan())[0] = 2;
        MemoryMarshal.Cast<byte, uint>(frame.AsSpan())[1] = (uint)n;
        for (int i = 0; i < n; i++)
            frame[8 + i] = _live[i] ? (byte)1 : (byte)0;
        return frame;
    }
}
