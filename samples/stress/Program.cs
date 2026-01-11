// Threads keep changing object graphs while another thread collects. Every structure carries what
// is needed to check it, so freeing something alive or handing out memory twice shows up.
// Arguments: seconds (default 10), and microseconds each thread rests between operations, to make
// less garbage (default 0). Exits 1 if a check fails.
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;

double seconds = args.Length > 0 ? double.Parse(args[0]) : 10;
int rest = args.Length > 1 ? int.Parse(args[1]) : 0;
bool stop = false;
long operations = 0;
var failures = new ConcurrentQueue<string>();
void Check(bool ok, string what) { if (!ok) failures.Enqueue(what); }

var table = new ConditionalWeakTable<Node, string>();
var shared = new ConcurrentDictionary<int, Node>();

var workers = Enumerable.Range(0, 6).Select(t => new Thread(() =>
{
    var random = new Random(t);
    var roots = new Node?[64];
    var weak = new WeakReference<Node>[64];
    long n = 0;
    while (!Volatile.Read(ref stop))
    {
        int i = random.Next(roots.Length);
        switch (random.Next(10))
        {
            case 0: case 1: case 2:
                roots[i] = Node.Tree(random, random.Next(1, 6));
                break;
            case 3:
                if (roots[i]?.Left is { } left) { roots[i]!.Left = null; roots[random.Next(roots.Length)] = left; }
                break;
            case 4:
                roots[i] = null;
                break;
            case 5:
                if (roots[i] is { } r) weak[i] = new WeakReference<Node>(r);
                break;
            case 6:
                if (weak[i] != null && weak[i].TryGetTarget(out Node? target)) Check(target.IsIntact(), "weak target");
                break;
            case 7:
                if (roots[i] is { } key)
                {
                    table.AddOrUpdate(key, "value " + key.Id);
                    Check(table.TryGetValue(key, out string? value) && value == "value " + key.Id, "conditional weak table");
                }
                break;
            case 8:
                // Locks with contention and hash codes both use sync blocks.
                if (roots[i] is { } locked)
                {
                    int hash = RuntimeHelpers.GetHashCode(locked);
                    lock (locked) { Check(hash == RuntimeHelpers.GetHashCode(locked), "hash code"); }
                    shared[random.Next(32)] = locked;
                }
                break;
            case 9:
                if (shared.TryGetValue(random.Next(32), out Node? other)) lock (other) { Check(other.IsIntact(), "shared node"); }
                break;
        }
        if (roots[i] is { } root) Check(root.IsIntact(), "tree");
        if (n % 100 == 0)
        {
            string text = string.Join(",", Enumerable.Range(0, 50));
            Check(text.Split(',').Select(int.Parse).Sum() == 1225, "strings and LINQ");
            var big = new long[20_000];
            Check(big.AsSpan().IndexOfAnyExcept(0L) < 0, "large array not zero");
            Array.Fill(big, n);
        }
        n++;
        if (rest > 0 && n % 100 == 0) Thread.Sleep(TimeSpan.FromMicroseconds(rest * 100));
    }
    Interlocked.Add(ref operations, n);
})).ToList();
workers.ForEach(w => w.Start());

var clock = System.Diagnostics.Stopwatch.StartNew();
int collections = 0;
while (clock.Elapsed.TotalSeconds < seconds && failures.IsEmpty)
{
    GC.Collect();
    collections++;
    Thread.Sleep(20);
}
Volatile.Write(ref stop, true);
workers.ForEach(w => w.Join());
foreach (string failure in failures.Take(10)) Console.WriteLine("FAIL " + failure);
Console.WriteLine($"{operations:N0} operations, {collections} collections, heap {GC.GetTotalMemory(false) >> 20} MB, " +
    $"{GC.GetTotalAllocatedBytes() >> 20} MB allocated, {failures.Count} failures");
return failures.IsEmpty ? 0 : 1;

sealed class Node
{
    static int s_next;
    public readonly int Id = Interlocked.Increment(ref s_next);
    public Node? Left, Right;
    readonly int[] _data;
    readonly string _name;

    Node(Random random)
    {
        _data = new int[random.Next(0, 200)];
        Array.Fill(_data, Id);
        _name = Id.ToString();
    }

    public static Node Tree(Random random, int depth) => new(random)
    {
        Left = depth > 1 ? Tree(random, depth - 1) : null,
        Right = depth > 1 ? Tree(random, depth - 1) : null,
    };

    public bool IsIntact() =>
        _data.AsSpan().IndexOfAnyExcept(Id) < 0 && _name == Id.ToString() &&
        (Left?.IsIntact() ?? true) && (Right?.IsIntact() ?? true);
}
