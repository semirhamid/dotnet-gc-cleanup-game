// A puzzle for the flight game. What is dropped looks much like what is kept, so a player has to look at
// what holds it. Argument: how many collections to request (default 3).
using System.Runtime.CompilerServices;

int rounds = args.Length > 0 ? int.Parse(args[0]) : 3;
var field = new Airfield();
for (int round = 1; round <= rounds; round++)
{
    field.Busy(round);
    Console.WriteLine($"collecting ({round} of {rounds}): the app is frozen until the plane lands");
    GC.Collect();
    Console.WriteLine($"resumed, heap {GC.GetTotalMemory(false) / 1024} KB, {field.Check()}");
}
GC.KeepAlive(field);

sealed class Airfield
{
    readonly List<Hangar> _hangars = [];
    readonly Dictionary<string, Pilot> _pilots = [];
    event Action? Departed;
    int _sum;

    [MethodImpl(MethodImplOptions.NoInlining)]
    public void Busy(int round)
    {
        for (int i = 0; i < 4; i++)
        {
            var hangar = new Hangar($"hangar {round}-{i}");
            for (int j = 0; j < 4; j++) hangar.Planes.Add(new Plane(hangar, new Pilot($"pilot {round}-{i}-{j}")));
            _hangars.Add(hangar);
            _pilots[hangar.Name] = hangar.Planes[0].Pilot!;
        }

        // Dropped hangars: everything inside points back at the hangar, so it all looks well tied.
        for (int i = 0; i < 4; i++)
        {
            var hangar = new Hangar($"old hangar {round}-{i}");
            for (int j = 0; j < 4; j++) hangar.Planes.Add(new Plane(hangar, new Pilot($"old pilot {round}-{i}-{j}")));
            Drop(hangar);
        }

        // Dropped planes that refer to live pilots and hangars. That does not keep the planes alive.
        foreach (Hangar hangar in _hangars.TakeLast(4))
            Drop(new Plane(hangar, hangar.Planes[1].Pilot) { Wingman = new Plane(hangar, hangar.Planes[2].Pilot) });

        // A convoy: a chain whose head was dropped, and one still held by its head.
        Drop(Convoy(12));
        _hangars[^1].Convoy = Convoy(8);

        // Held only by an event subscription, the classic leak, so it is alive.
        var forgotten = new Radar();
        Departed += forgotten.Sweep;

        for (int i = 0; i < 3; i++)
        {
            var first = new Beacon();
            var last = first;
            for (int j = 0; j < 5; j++) last = last.Next = new Beacon();
            last.Next = first;
            Drop(first);
        }
        for (int i = 0; i < 12; i++) Drop(new Freight(2000 * (i + 1)));
        Departed?.Invoke();
        _sum = _hangars.Sum(h => h.Sum());
    }

    public string Check() => _sum == _hangars.Sum(h => h.Sum()) && _pilots.Count == _hangars.Count ? "everything kept is intact" : "DAMAGED";

    static Plane Convoy(int length)
    {
        Plane? head = null;
        for (int i = 0; i < length; i++) head = new Plane(null, new Pilot($"convoy {i}")) { Wingman = head };
        return head!;
    }

    // The JIT is free to drop allocations that never escape.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Drop(object _) { }
}

sealed class Hangar(string name)
{
    public string Name = name;
    public List<Plane> Planes = [];
    public Plane? Convoy;
    public int Sum() => Name.Length + Planes.Sum(p => p.Sum()) + (Convoy?.Sum() ?? 0);
}

sealed class Plane(Hangar? home, Pilot? pilot)
{
    public Hangar? Home = home;
    public Pilot? Pilot = pilot;
    public Plane? Wingman;
    public byte[] Fuel = new byte[400];
    public int Sum() => Fuel.Length + (Home?.Name.Length ?? 0) + (Pilot?.Name.Length ?? 0) + (Wingman?.Sum() ?? 0);
}

sealed class Pilot(string name) { public string Name = name; }
sealed class Radar { readonly int[] _echoes = new int[64]; public void Sweep() => _echoes[0]++; }
sealed class Beacon { public Beacon? Next; }
sealed class Freight(int size) { public byte[] Load = new byte[size]; }
