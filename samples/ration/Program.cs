// Allocates far more than a small fixed heap can hold (run with DOTNET_GCHeapHardLimit), collecting
// between rounds, so it only finishes if erased memory is reused. Arguments: rounds (default 20),
// megabytes of garbage per round (default 4). Exits 3 on out of memory, 1 if data was damaged.
using System.Runtime.CompilerServices;

int rounds = args.Length > 0 ? int.Parse(args[0]) : 20;
int megabytes = args.Length > 1 ? int.Parse(args[1]) : 4;
var kept = new List<Kept>();
var weak = new List<WeakReference<Garbage>>();
long allocated = 0;
try
{
    for (int round = 1; round <= rounds; round++)
    {
        allocated += MakeGarbage(megabytes * 1024L * 1024, weak);
        for (int i = 0; i < 100; i++) kept.Add(new Kept(kept.Count));
        GC.Collect();
        int damaged = kept.Count(k => !k.IsIntact());
        if (damaged > 0)
        {
            Console.WriteLine($"{damaged} live objects damaged in round {round}");
            return 1;
        }
    }
}
catch (OutOfMemoryException)
{
    Console.WriteLine($"out of memory after {allocated >> 20} MB of garbage, heap {GC.GetTotalMemory(false) >> 20} MB");
    return 3;
}
int weakAlive = weak.Count(w => w.TryGetTarget(out _));
Console.WriteLine($"finished {rounds} rounds, {allocated >> 20} MB of garbage, heap {GC.GetTotalMemory(false) >> 20} MB, " +
    $"{GC.CollectionCount(0)} collections, {kept.Count} live objects intact, {weakAlive} of {weak.Count} weak references alive");
return 0;

[MethodImpl(MethodImplOptions.NoInlining)]
static long MakeGarbage(long bytes, List<WeakReference<Garbage>> weak)
{
    long total = 0;
    for (int i = 0; total < bytes; i++)
    {
        // Every path through the allocator: contexts, direct and large.
        int size = i % 500 == 0 ? 100_000 : i % 50 == 0 ? 10_000 : 16 + i % 300;
        var garbage = new Garbage { Data = new byte[size] };
        if (garbage.Data.AsSpan().IndexOfAnyExcept((byte)0) >= 0)
            throw new InvalidOperationException("new memory was not zero");
        garbage.Data.AsSpan().Fill(0xAB);
        if (i == 0) weak.Add(new WeakReference<Garbage>(garbage));
        total += size + 24 + 24;
    }
    return total;
}

sealed class Garbage { public byte[] Data = []; }

sealed class Kept
{
    readonly int _id;
    readonly int[] _data;
    readonly string _text;
    public Kept(int id)
    {
        _id = id;
        _data = new int[16 + id % 40];
        Array.Fill(_data, id);
        _text = $"kept {id}";
    }
    public bool IsIntact() => _data.Length == 16 + _id % 40 && _data.AsSpan().IndexOfAnyExcept(_id) < 0 && _text == $"kept {_id}";
}
