namespace ManualGC.Viewer;

/// <summary>
/// The heap shrunk to a small directed graph for the flight game. A node can stand for many objects,
/// but live and dead ones are never mixed in one node. So a node is garbage exactly when no path of
/// edges leads to it from a root node.
/// </summary>
sealed class ObjectGraph
{
    public const int MaxLive = 90, MaxDead = 60, MaxEdges = 700;
    const int MaxSiblings = 5;      // nodes left when many objects of one type have the same holder
    const int MaxDeadByType = 20;
    const int FoldPasses = 4;

    // Added to the size of an object of the app's own types, so that those always rank first.
    const long AppTypeBonus = 1L << 40;

    public sealed record Node(string Type, int Count, long Bytes, int RootKind);

    public List<Node> Nodes { get; } = [];
    public List<int[]> Members { get; } = [];
    public List<(int From, int To)> Edges { get; } = [];
    public int[] NodeOf { get; }               // by block, or -1

    /// <summary>How many objects the GC calls live and this class dead, or the other way round. Should be 0.</summary>
    public int Disagreements { get; }

    readonly Snapshot _snapshot;
    readonly int[] _merged;  // union-find: which block each block was merged into, or itself
    bool[]? _isApp;

    public ObjectGraph(Snapshot snapshot)
    {
        _snapshot = snapshot;
        int n = snapshot.Count;
        _merged = new int[n];
        for (int i = 0; i < n; i++) _merged[i] = i;
        NodeOf = new int[n];

        var (start, targets) = OutEdges();
        var rootKind = new int[n];
        Array.Fill(rootKind, -1);
        var queue = new List<int>();
        var parent = new int[n];
        var reached = new bool[n];
        foreach (var (block, kind) in snapshot.Roots())
        {
            if (reached[block] || snapshot.IsFree(block)) continue;
            rootKind[block] = kind;
            reached[block] = true;
            parent[block] = -1;
            queue.Add(block);
        }

        // Mark again, breadth first, so that parent leads along a shortest path to a root.
        for (int at = 0; at < queue.Count; at++)
        {
            int from = queue[at];
            for (int e = start[from]; e < start[from + 1]; e++)
            {
                int to = targets[e];
                if (reached[to]) continue;
                reached[to] = true;
                parent[to] = from;
                queue.Add(to);
            }
        }
        int objects = 0;
        for (int i = 0; i < n; i++)
        {
            if (snapshot.IsFree(i)) continue;
            objects++;
            if (reached[i] != snapshot.IsLive(i)) Disagreements++;
        }

        // For dead objects, parent is the first dead object that holds them, if any does.
        for (int i = 0; i < n; i++)
            if (!reached[i]) parent[i] = -1;
        for (int from = 0; from < n; from++)
        {
            if (reached[from]) continue;
            for (int e = start[from]; e < start[from + 1]; e++)
                if (!reached[targets[e]] && parent[targets[e]] < 0) parent[targets[e]] = from;
        }

        if (objects > MaxLive + MaxDead)
        {
            FoldLeaves(start, targets, reached, rootKind);
            JoinSiblings(reached, parent, rootKind);
        }
        LimitLive(queue, parent);
        LimitDead(reached);
        Number(reached, rootKind, start, targets, parent);
    }

    (int[] Start, int[] Targets) OutEdges()
    {
        int n = _snapshot.Count;
        var start = new int[n + 1];
        foreach (var (from, _) in _snapshot.References()) start[from + 1]++;
        for (int i = 0; i < n; i++) start[i + 1] += start[i];
        var next = start[..^1];
        var targets = new int[start[n]];
        foreach (var (from, to) in _snapshot.References()) targets[next[from]++] = to;
        return (start, targets);
    }

    int Group(int i)
    {
        while (_merged[i] != i) i = _merged[i] = _merged[_merged[i]];
        return i;
    }

    void Merge(int child, int into) => _merged[Group(child)] = Group(into);

    /// <summary>
    /// Merges objects that hold nothing and have a single holder into that holder. The array inside a
    /// list becomes part of the list, for example.
    /// </summary>
    void FoldLeaves(int[] start, int[] targets, bool[] reached, int[] rootKind)
    {
        int n = _snapshot.Count;
        var holder = new int[n];
        var holds = new bool[n];
        for (int pass = 0; pass < FoldPasses; pass++)
        {
            Array.Fill(holder, -1);
            Array.Clear(holds);
            for (int i = 0; i < n; i++)
            {
                for (int e = start[i]; e < start[i + 1]; e++)
                {
                    int from = Group(i), to = Group(targets[e]);
                    if (from == to) continue;
                    holds[from] = true;
                    holder[to] = holder[to] == -1 || holder[to] == from ? from : -2;
                }
            }
            bool changed = false;
            for (int i = 0; i < n; i++)
            {
                if (_merged[i] != i || holds[i] || holder[i] < 0 || rootKind[i] >= 0 || reached[i] != reached[holder[i]]) continue;
                // Leave the app's own objects for the later passes, which only happen if this one
                // found something else to merge.
                if (pass == 0 && IsAppType(i)) continue;
                Merge(i, holder[i]);
                changed = true;
            }
            if (!changed) return;
        }
    }

    void JoinSiblings(bool[] reached, int[] parent, int[] rootKind)
    {
        var families = new Dictionary<(bool, int, int, int), List<int>>();
        for (int i = 0; i < _snapshot.Count; i++)
        {
            if (_merged[i] != i || _snapshot.IsFree(i)) continue;
            int holder = parent[i] < 0 ? -1 : Group(parent[i]);
            var key = (reached[i], _snapshot.TypeOf(i), holder, rootKind[i]);
            if (!families.TryGetValue(key, out var family)) families[key] = family = [];
            family.Add(i);
        }
        foreach (var family in families.Values)
        {
            if (family.Count <= MaxSiblings) continue;
            for (int i = MaxSiblings; i < family.Count; i++) Merge(family[i], family[i % MaxSiblings]);
        }
    }

    void LimitLive(List<int> order, int[] parent)
    {
        // A subtree is interesting if it has the app's own types in it, or else if it is large.
        var weight = new Dictionary<int, long>();
        var children = new Dictionary<int, List<int>>();
        var holder = new Dictionary<int, int>();
        foreach (int i in order)
        {
            int group = Group(i);
            if (parent[i] < 0) holder[group] = -1;
            else holder.TryAdd(group, Group(parent[i]));
        }
        foreach (var (group, held) in holder)
        {
            if (held < 0) continue;
            if (!children.TryGetValue(held, out var list)) children[held] = list = [];
            list.Add(group);
        }
        var own = new Dictionary<int, long>();
        foreach (int i in order)
            own[Group(i)] = own.GetValueOrDefault(Group(i)) + Interest(i);
        // Holders come before what they hold. Going backwards, a subtree is complete when we get to it.
        foreach (int group in order.Select(Group).Distinct().Reverse())
        {
            weight[group] = weight.GetValueOrDefault(group) + own[group];
            if (holder[group] is >= 0 and var held) weight[held] = weight.GetValueOrDefault(held) + weight[group];
        }

        var kept = new HashSet<int>();
        var candidates = new PriorityQueue<int, long>();
        foreach (var (group, held) in holder)
            if (held < 0) candidates.Enqueue(group, -weight.GetValueOrDefault(group));
        while (kept.Count < MaxLive && candidates.TryDequeue(out int group, out _))
        {
            if (!kept.Add(group)) continue;
            foreach (int child in children.GetValueOrDefault(group) ?? [])
                candidates.Enqueue(child, -weight.GetValueOrDefault(child));
        }

        // Roots that were not kept have no holder to merge into, so they share one node.
        int spare = -1;
        foreach (int i in order)
        {
            int group = Group(i);
            if (kept.Contains(group)) continue;
            if (parent[i] >= 0 && Group(parent[i]) != group) Merge(i, parent[i]);
            else if (parent[i] < 0)
            {
                if (spare < 0) kept.Add(spare = group);
                else Merge(i, spare);
            }
        }
    }

    void LimitDead(bool[] reached)
    {
        var interest = new Dictionary<int, long>();
        for (int i = 0; i < _snapshot.Count; i++)
            if (!reached[i] && !_snapshot.IsFree(i))
                interest[Group(i)] = interest.GetValueOrDefault(Group(i)) + Interest(i);
        if (interest.Count <= MaxDead) return;
        var byType = new Dictionary<int, int>();
        int other = -1, buckets = 0;  // the last bucket takes every type that has none of its own
        foreach (int group in interest.OrderByDescending(p => p.Value).Skip(MaxDead - MaxDeadByType).Select(p => p.Key))
        {
            if (byType.TryGetValue(_snapshot.TypeOf(group), out int bucket)) Merge(group, bucket);
            else if (buckets++ < MaxDeadByType - 1) byType[_snapshot.TypeOf(group)] = group;
            else if (other < 0) other = group;
            else Merge(group, other);
        }
    }

    void Number(bool[] reached, int[] rootKind, int[] start, int[] targets, int[] parent)
    {
        int n = _snapshot.Count;
        Array.Fill(NodeOf, -1);
        var members = new List<List<int>>();
        for (int i = 0; i < n; i++)
        {
            if (_snapshot.IsFree(i)) continue;
            int group = Group(i);
            if (NodeOf[group] < 0)
            {
                NodeOf[group] = members.Count;
                members.Add([]);
            }
            NodeOf[i] = NodeOf[group];
            members[NodeOf[i]].Add(i);
        }

        // Unshuffled, the numbers would give away the order in which objects were allocated.
        int[] order = [.. Enumerable.Range(0, members.Count)];
        new Random((int)_snapshot.Pid ^ (int)_snapshot.Gc).Shuffle(order);
        var renumber = new int[members.Count];
        for (int i = 0; i < order.Length; i++) renumber[order[i]] = i;
        for (int i = 0; i < n; i++)
            if (NodeOf[i] >= 0) NodeOf[i] = renumber[NodeOf[i]];
        foreach (int old in order)
        {
            int[] list = [.. members[old]];
            var types = list.GroupBy(_snapshot.TypeOf).OrderByDescending(g => g.Sum(_snapshot.Size)).ToList();
            var main = types.FirstOrDefault(g => IsAppType(g.First())) ?? types[0];
            string name = _snapshot.TypeNames[main.Key] + (types.Count > 1 ? $" and {types.Count - 1} more" : "");
            Nodes.Add(new Node(name, list.Length, list.Sum(_snapshot.Size), list.Max(i => rootKind[i])));
            Members.Add(list);
        }

        var seen = new HashSet<(int, int)>();
        void Add(int from, int to)
        {
            int a = NodeOf[from], b = NodeOf[to];
            if (a != b && seen.Add((a, b))) Edges.Add((a, b));
        }
        // The edges that decide what is alive must all be there. The others go in while there is room.
        for (int i = 0; i < n; i++)
            if (reached[i] && parent[i] >= 0) Add(parent[i], i);
        for (int i = 0; i < n && Edges.Count < MaxEdges; i++)
            for (int e = start[i]; e < start[i + 1] && Edges.Count < MaxEdges; e++) Add(i, targets[e]);
    }

    long Interest(int i) => _snapshot.Size(i) + (IsAppType(i) ? AppTypeBonus : 0);

    bool IsAppType(int i)
    {
        int type = _snapshot.TypeOf(i);
        _isApp ??= [.. _snapshot.TypeNames.Select(IsAppName)];
        return _isApp[type];
    }

    static readonly string[] s_frameworkPrefixes = ["System.", "Microsoft.", "Interop", "Internal.", "type 0x"];

    static bool IsAppName(string name)
    {
        string element = name.TrimEnd('[', ']', ',', '*');
        if (element == "free space" || s_frameworkPrefixes.Any(element.StartsWith))
            return false;
        bool isKeyword = element.Length > 0 && char.IsLower(element[0]) && !element.Contains('.');
        return !isKeyword;
    }
}
