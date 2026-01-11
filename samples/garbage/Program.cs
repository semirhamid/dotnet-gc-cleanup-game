// Builds heaps whose live and dead objects are known exactly, then collects. Each expectation is
// printed as "EXPECT gc N type 0xMT live COUNT BYTES dead COUNT BYTES", which is how the GC logs
// its own count at MANUALGC_LOG=2, so the test can compare them verbatim.
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

static class Program
{
    static int s_gc;

    static void Expect<T>(int live, long liveBytes, int dead, long deadBytes) => Expect(typeof(T), live, liveBytes, dead, deadBytes);

    static void Expect(Type type, int live, long liveBytes, int dead, long deadBytes) =>
        Console.WriteLine($"EXPECT gc {s_gc + 1} type 0x{type.TypeHandle.Value:x} live {live} {liveBytes} dead {dead} {deadBytes}    {type.Name}");

    static void Collect() { s_gc++; GC.Collect(); }

    // The JIT is free to drop allocations that never escape.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Escape(object _) { }

    static LiveNode? s_list;
    [ThreadStatic] static ThreadStaticRoot? t_root;
    static GCHandle s_strong, s_pinned;
    static WeakReference<WeakTarget>? s_weak;
    static ConditionalWeakTable<Key, Value> s_table = new();
    static Key? s_key;
    static Pair[]? s_pairs;
    static object? s_collectible;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Build()
    {
        for (int i = 0; i < 500; i++) s_list = new LiveNode(s_list);
        for (int i = 0; i < 1000; i++) Escape(new Garbage());
        for (int i = 0; i < 10; i++)
        {
            var a = new CycleNode();
            a.Other = new CycleNode { Other = a };
        }
        for (int i = 0; i < 7; i++) Escape(new DeadFinalizable());
        for (int i = 0; i < 3; i++) Escape(new DeadArrayElement[100]);

        t_root = new ThreadStaticRoot();
        s_strong = GCHandle.Alloc(new StrongTarget());
        s_pinned = GCHandle.Alloc(new PinnedTarget(), GCHandleType.Pinned);
        s_weak = new WeakReference<WeakTarget>(new WeakTarget());

        // A live key keeps its value; a dead key does not, even if the value points at the key.
        s_table.Add(s_key = new Key(), new Value());
        var deadKey = new Key();
        s_table.Add(deadKey, new Value { Back = deadKey });

        s_pairs = new Pair[100];
        for (int i = 0; i < 100; i++)
            s_pairs[i] = new Pair { Left = new InStruct(), Number = -1, Right = new InStruct(), Inner = new Inner { Deep = new InStruct() } };

        // Only the instance is kept. Its type must keep the collectible assembly's LoaderAllocator.
        var assembly = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("unloadable"), AssemblyBuilderAccess.RunAndCollect);
        s_collectible = Activator.CreateInstance(assembly.DefineDynamicModule("m").DefineType("Unloadable").CreateType());
    }

    static void ExpectBuilt()
    {
        Expect<LiveNode>(500, 500 * 24, 0, 0);
        Expect<Garbage>(0, 0, 1000, 1000 * 32);
        Expect<CycleNode>(0, 0, 20, 20 * 24);
        Expect<DeadFinalizable>(0, 0, 7, 7 * 24);
        Expect<DeadArrayElement[]>(0, 0, 3, 3 * (24 + 800));
        Expect<ThreadStaticRoot>(1, 24, 0, 0);
        Expect<StrongTarget>(1, 24, 0, 0);
        Expect<PinnedTarget>(1, 24, 0, 0);
        Expect<WeakTarget>(0, 0, 1, 24);
        Expect<Key>(1, 24, 1, 24);
        Expect<Value>(1, 24, 1, 24);
        Expect<Pair[]>(1, 24 + 100 * 32, 0, 0);
        Expect<InStruct>(300, 300 * 24, 0, 0);
        Expect(s_collectible!.GetType(), 1, 24, 0, 0);
        Expect(Type.GetType("System.Reflection.LoaderAllocator", throwOnError: true)!, 1, 48, 0, 0);
    }

    // Interior pointers: the only reference to each object points inside it

    [MethodImpl(MethodImplOptions.NoInlining)]
    static ref long LastElement() => ref (new long[1000])[999];
    [MethodImpl(MethodImplOptions.NoInlining)]
    static ref int FirstElement() => ref (new int[50])[0];
    [MethodImpl(MethodImplOptions.NoInlining)]
    static ref short LargeMiddle() => ref (new short[500_000])[250_000];
    [MethodImpl(MethodImplOptions.NoInlining)]
    static ref int Field() => ref new HasField().B;
    [MethodImpl(MethodImplOptions.NoInlining)]
    static Span<char> EmptyEnd() => new char[4].AsSpan()[4..];
    [MethodImpl(MethodImplOptions.NoInlining)]
    static ref object Reference() => ref new HasReference { Target = new ViaInterior() }.Target;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void InteriorPointers()
    {
        ref long last = ref LastElement();
        ref int first = ref FirstElement();
        ref short middle = ref LargeMiddle();
        ref int field = ref Field();
        ref object reference = ref Reference();
        Span<char> empty = EmptyEnd();
        for (int i = 0; i < 5; i++) { _ = LastElement(); }

        Expect<long[]>(1, 24 + 8000, 5, 5 * (24 + 8000));
        Expect<short[]>(1, 24 + 1_000_000, 0, 0);
        Expect<HasField>(1, 32, 0, 0);
        Expect<HasReference>(1, 24, 0, 0);
        Expect<ViaInterior>(1, 24, 0, 0);
        Collect();
        last = 1; first = 2; middle = 3; field = 4;
        GC.KeepAlive(reference);
        Console.WriteLine($"interior targets usable: {last + first + middle + field == 10 && reference is ViaInterior && empty.Length == 0}");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void OtherThreads()
    {
        using var ready = new CountdownEvent(2);
        using var release = new ManualResetEventSlim();
        bool stop = false;
        var blocked = new Thread(() =>
        {
            var local = new OnBlockedStack();
            ready.Signal();
            release.WaitHandle.WaitOne();
            GC.KeepAlive(local);
        });
        var running = new Thread(() =>
        {
            var local = new OnRunningStack();
            ready.Signal();
            while (!Volatile.Read(ref stop)) { }
            GC.KeepAlive(local);
        });
        blocked.Start();
        running.Start();
        ready.Wait();
        Expect<OnBlockedStack>(1, 24, 0, 0);
        Expect<OnRunningStack>(1, 24, 0, 0);
        Collect();
        Volatile.Write(ref stop, true);
        release.Set();
        blocked.Join();
        running.Join();
    }

    static Deep? s_deep;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void BuildDeep()
    {
        for (int i = 0; i < 1_000_000; i++) s_deep = new Deep(s_deep);
    }

    static int Main()
    {
        Build();
        ExpectBuilt();
        Collect();

        // Nothing is freed, so the same garbage is found again and dropped roots turn dead.
        s_list = null;
        s_pairs = null;
        s_key = null;
        s_strong.Free();
        Expect<LiveNode>(0, 0, 500, 500 * 24);
        Expect<Garbage>(0, 0, 1000, 1000 * 32);
        Expect<InStruct>(0, 0, 300, 300 * 24);
        Expect<StrongTarget>(0, 0, 1, 24);
        Expect<Key>(0, 0, 2, 48);
        Expect<Value>(0, 0, 2, 48);
        Collect();

        InteriorPointers();
        OtherThreads();

        BuildDeep();
        Expect<Deep>(1_000_000, 24_000_000, 0, 0);
        Collect();
        s_deep = null;
        Expect<Deep>(0, 0, 1_000_000, 24_000_000);
        Collect();

        GC.KeepAlive(s_collectible);
        Console.WriteLine($"collections requested: {s_gc}");
        s_pinned.Free();
        return s_weak != null && t_root != null ? 0 : 1;
    }
}

sealed class LiveNode(LiveNode? next) { public LiveNode? Next = next; }
sealed class Garbage { public long A = 1, B = 2; }
sealed class CycleNode { public CycleNode? Other; }
sealed class DeadFinalizable { ~DeadFinalizable() => Environment.FailFast("finalizer ran"); }
sealed class DeadArrayElement;
sealed class ThreadStaticRoot;
sealed class StrongTarget;
[StructLayout(LayoutKind.Sequential)] sealed class PinnedTarget { public long Value; }
sealed class WeakTarget;
sealed class Key;
sealed class Value { public object? Back; }
sealed class InStruct;
struct Inner { public InStruct Deep; }
struct Pair { public InStruct Left; public long Number; public InStruct Right; public Inner Inner; }
sealed class HasField { public int A = 1, B; public long C = 2; }
sealed class HasReference { public object Target = null!; }
sealed class ViaInterior;
sealed class OnBlockedStack;
sealed class OnRunningStack;
sealed class Deep(Deep? next) { public Deep? Next = next; }
