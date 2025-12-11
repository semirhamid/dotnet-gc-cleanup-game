// Allocates several hundred MB in many shapes and checks the data afterwards. Exits 1 if a
// check fails. First argument: megabytes to allocate in the main workload (default 400), or
// "oom" to fill the heap with live arrays and catch the OutOfMemoryException.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

if (args is ["oom"])
{
    var blocks = new List<byte[]>(4096);
    try
    {
        while (true) blocks.Add(new byte[1_000_000]);
    }
    catch (OutOfMemoryException)
    {
        // One failed allocation must not poison later ones.
        var small = new byte[1000];
        Console.WriteLine($"caught OutOfMemoryException after {blocks.Count} blocks, then allocated {small.Length} bytes");
        return 0;
    }
}

int megabytes = args.Length > 0 ? int.Parse(args[0]) : 400;
int failures = 0;

void Check(bool condition, string what)
{
    Console.WriteLine($"{(condition ? "ok  " : "FAIL")} {what}");
    if (!condition) failures++;
}

// Several threads, each with its own allocation context. One linked list per thread stays alive
// and is checked at the end, so overlapping or badly zeroed allocations show up.
int threads = Math.Max(4, Environment.ProcessorCount / 2);
long perThread = megabytes * 1024L * 1024 / threads;
var sums = new (long nodes, bool good)[threads];
var workers = Enumerable.Range(0, threads).Select(t => new Thread(() =>
{
    Node? head = null;
    long nodes = 0, allocated = 0, i = 0;
    while (allocated < perThread)
    {
        int length = 16 + (int)(i % 200);
        var payload = new byte[length];
        bool zero = payload.AsSpan().IndexOfAnyExcept((byte)0) < 0;
        payload.AsSpan().Fill((byte)(t + i));
        allocated += length + 24;
        if (i % 8 == 0)
        {
            head = new Node(head, payload, i);
            nodes++;
        }
        if (!zero) { sums[t] = (nodes, false); return; }
        i++;
    }
    bool good = true;
    for (var n = head; n != null; n = n.Next)
        good &= n.Payload.Length == 16 + (int)(n.Id % 200) && n.Payload.AsSpan().IndexOfAnyExcept((byte)(t + n.Id)) < 0;
    sums[t] = (nodes, good);
})).ToList();
workers.ForEach(w => w.Start());
workers.ForEach(w => w.Join());
Check(sums.All(s => s.good && s.nodes > 0), $"{threads} threads, {sums.Sum(s => s.nodes)} live list nodes intact");

var large = Enumerable.Range(0, 20).Select(i => { var a = new byte[1_000_000 + i]; a[^1] = (byte)i; return a; }).ToArray();
Check(large.Select((a, i) => a[0] == 0 && a[^1] == i).All(x => x), "large arrays");

var edges = Enumerable.Range(8_000, 500).Select(n => { var a = new byte[n]; Array.Fill(a, (byte)n); return a; }).ToArray();
Check(edges.All(a => a.AsSpan().IndexOfAnyExcept((byte)a.Length) < 0), "sizes around the direct-allocation threshold");

var source = Enumerable.Range(0, 100_000).Select(i => new object()).ToArray();
var target = new object[source.Length];
Array.Copy(source, target, source.Length);          // bulk reference copy: writes the card tables
Array.Copy(target, 0, target, 1, target.Length - 1); // overlapping
Check(target[0] == source[0] && target[^1] == source[^2], "bulk reference copies");

var dictionary = new Dictionary<string, List<int>>();
for (int i = 0; i < 50_000; i++)
{
    string key = "k" + i % 1000;
    if (!dictionary.TryGetValue(key, out var list)) dictionary[key] = list = [];
    list.Add(i);
}
Check(dictionary.Values.Sum(l => l.Count) == 50_000, "dictionary of lists");

var pinned = GC.AllocateArray<byte>(4096, pinned: true);
Check(pinned.All(b => b == 0), "pinned object heap");
var normal = GCHandle.Alloc("strong");
var weak = new WeakReference<object>(source[5]);
Check((string)normal.Target! == "strong" && weak.TryGetTarget(out var w) && w == source[5], "strong and weak handles");
normal.Free();
var table = new ConditionalWeakTable<object, string>();
table.Add(source[7], "seven");
Check(table.TryGetValue(source[7], out var seven) && seven == "seven", "dependent handles");
var handles = Enumerable.Range(0, 5000).Select(i => GCHandle.Alloc(source[i], (GCHandleType)(i % 4 == 3 ? 2 : i % 4))).ToArray();
Check(handles.Select((h, i) => h.Target == source[i]).All(x => x), "5000 handles across chunks");
foreach (var h in handles) h.Free();

int caught = 0;
for (int i = 0; i < 500; i++)
    try { throw new InvalidOperationException(); } catch (InvalidOperationException) { caught++; }
Check(caught == 500, "exceptions");
Check((await Task.WhenAll(Enumerable.Range(0, 200).Select(async i => { await Task.Delay(1); return i; }))).Sum() == 19900, "tasks and timers");
Check(Parallel.For(0, 10_000, i => { lock (source) { _ = new int[i % 50]; } }).IsCompleted, "locks under contention");

// The precise count suspends every managed thread, so this exercises the suspend/resume path the
// VM shares with collections: busy threads, threads returning from native code, and allocation.
bool stop = false;
long spins = 0;
var busy = Enumerable.Range(0, threads).Select(t => new Thread(() =>
{
    long n = 0;
    while (!Volatile.Read(ref stop))
    {
        _ = new byte[64];
        if (t % 2 == 0) Thread.Sleep(0);
        n++;
    }
    Interlocked.Add(ref spins, n);
})).ToList();
busy.ForEach(b => b.Start());
long last = 0;
bool monotonic = true;
for (int i = 0; i < 2000; i++)
{
    long now = GC.GetTotalAllocatedBytes(precise: true);
    monotonic &= now >= last;
    last = now;
}
Volatile.Write(ref stop, true);
busy.ForEach(b => b.Join());
Check(monotonic && spins > 0, "2000 EE suspensions with busy threads");

_ = new Finalizable();
long before = GC.GetAllocatedBytesForCurrentThread();
var measured = new byte[1000];
long delta = GC.GetAllocatedBytesForCurrentThread() - before;
Check(delta == 1024 && measured.Length == 1000, $"per-thread allocation accounting (1000-byte array = {delta} bytes)");
GC.Collect();
GC.WaitForPendingFinalizers();
Check(GC.CollectionCount(0) == 0 && Finalizable.Runs == 0, "GC.Collect is a no-op, finalizers do not run");
Check(GC.GetTotalMemory(false) > megabytes * 1024L * 1024, $"heap grew to {GC.GetTotalMemory(false) >> 20} MB");
GC.KeepAlive(large); GC.KeepAlive(handles);
return failures == 0 ? 0 : 1;

sealed class Node(Node? next, byte[] payload, long id)
{
    public readonly Node? Next = next;
    public readonly byte[] Payload = payload;
    public readonly long Id = id;
}

sealed class Finalizable
{
    public static int Runs;
    ~Finalizable() => Runs++;
}
