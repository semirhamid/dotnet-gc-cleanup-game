// A heap with a known number of objects of recognisable types, for looking at in the viewer.
// Arguments: how many collections to request (default 1), and "wait" to pause for Enter before each.
using System.Runtime.CompilerServices;

int rounds = args.Length > 0 && int.TryParse(args[0], out int n) ? n : 1;
bool wait = args.Contains("wait");
var orchard = new Orchard();
for (int round = 1; round <= rounds; round++)
{
    orchard.Grow();
    if (wait)
    {
        Console.WriteLine("Press Enter to call GC.Collect().");
        Console.ReadLine();
    }
    Console.WriteLine($"collecting ({round} of {rounds})");
    GC.Collect();
    Console.WriteLine("resumed");
}
GC.KeepAlive(orchard);

sealed class Orchard
{
    readonly List<object> _kept = [];

    [MethodImpl(MethodImplOptions.NoInlining)]
    public void Grow()
    {
        for (int i = 0; i < 100; i++) { _kept.Add(new Apple()); Drop(new Apple()); }
        for (int i = 0; i < 50; i++) Drop(new Basket<Apple, int>());
        for (int i = 0; i < 20; i++) _kept.Add(new Basket<string, Tree.Leaf>());
        for (int i = 0; i < 10; i++) { _kept.Add(new Tree.Leaf[64]); Drop(new Apple[4][]); }
        for (int i = 0; i < 5; i++) Drop(new Pear[8, 8]);
        Drop(new Dictionary<Apple, List<Pear>> { [new Apple()] = [new Pear()] });
        for (int i = 0; i < 200; i++) Drop(new byte[500 + i]);
    }

    // The JIT is free to drop allocations that never escape.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Drop(object _) { }
}

sealed class Apple { public long Weight = 1; public object? Worm = null; }
sealed class Pear { public int Weight = 1; }
sealed class Basket<TFruit, TTag> { public TFruit? Fruit = default; public TTag? Tag = default; }
sealed class Tree { public struct Leaf(long a) { public long A = a; } }
