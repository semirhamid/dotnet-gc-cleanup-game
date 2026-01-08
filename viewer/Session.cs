using System.Buffers.Binary;
using System.Text.Json;

namespace ManualGC.Viewer;

/// <summary>
/// The game. Every collection is one round, and after a round its snapshot is kept until the next one
/// starts. This only keeps score: the GC decides what may be freed.
/// </summary>
sealed class Session(Browsers browsers, GameRules rules)
{
    const int MaxBatch = 8000;  // addresses in one Erase; the GC accepts at most 64 KB

    readonly SemaphoreSlim _lock = new(1);
    readonly Queue<int> _wanted = [];
    readonly HashSet<int> _asked = [];  // sent to the GC and not answered
    readonly HashSet<int> _knownLive = [];
    readonly List<AttachedApp> _apps = [];
    Snapshot? _snapshot;
    ObjectGraph? _graph;
    Stream? _gc;          // set while the app is suspended
    int _lives;
    long _freedBytes;
    int _freedCount;
    int _destroyed;
    bool _peeked;       // a round in which somebody looked at what is alive scores nothing
    bool _hardcore;
    string? _endReason;
    long _deadline;     // Environment.TickCount64, or 0 for no limit
    CancellationTokenSource? _timer;
    uint _pid;
    long _totalScore;

    async Task Locked(Func<Task> action)
    {
        await _lock.WaitAsync();
        try { await action(); }
        finally { _lock.Release(); }
    }

    object SnapshotMessage(Snapshot s) => new
    {
        type = "snapshot",
        pid = s.Pid,
        gc = s.Gc,
        bytes = s.End - s.Base,
        heapBase = $"0x{s.Base:x}",
        count = s.Count,
        shown = s.Shown,
        types = s.TypeNames,
        frozen = _gc != null,
        knownLive = _knownLive,
        hardcoreAllowed = s.HardcoreAllowed,
        outOfMemory = s.OutOfMemory,
        state = State(),
    };

    long RoundScore => _peeked ? 0 : _freedBytes;

    object State() => new
    {
        lives = _lives,
        maxLives = rules.Lives,
        freedBytes = _freedBytes,
        freedCount = _freedCount,
        destroyed = _destroyed,
        peeked = _peeked,
        hardcore = _hardcore,
        score = RoundScore,
        totalScore = _totalScore + RoundScore,
        remainingMs = _deadline == 0 ? (long?)null : Math.Max(0, _deadline - Environment.TickCount64),
    };

    public Task BeginAsync(Snapshot snapshot, Stream gc) => Locked(() =>
    {
        _snapshot = snapshot;
        _graph = null;
        _gc = gc;
        _lives = rules.Lives;
        _totalScore = snapshot.Pid == _pid ? _totalScore + RoundScore : 0;
        _pid = snapshot.Pid;
        _freedBytes = 0;
        _freedCount = 0;
        _destroyed = 0;
        _peeked = false;
        _hardcore = false;  // has to be switched on again every round
        _endReason = null;
        StartTimer(snapshot);
        _wanted.Clear();
        _asked.Clear();
        _knownLive.Clear();
        browsers.Send(SnapshotMessage(snapshot));
        browsers.Send(snapshot.ToFrame());
        return Task.CompletedTask;
    });

    public Task EndAsync() => Locked(() =>
    {
        if (_gc != null)
        {
            _gc = null;
            _timer?.Cancel();
            _deadline = 0;
            browsers.Send(new { type = "resumed", state = State(), reason = _endReason ?? "gone", missedBytes = _snapshot!.DeadBytes() });
        }
        return Task.CompletedTask;
    });

    void StartTimer(Snapshot snapshot)
    {
        _timer?.Cancel();
        _deadline = 0;
        if (rules.Seconds <= 0)
            return;
        var timer = _timer = new CancellationTokenSource();
        _deadline = Environment.TickCount64 + rules.Seconds * 1000L;
        _ = Run();

        async Task Run()
        {
            await Task.Delay(TimeSpan.FromSeconds(rules.Seconds), timer.Token)
                .ConfigureAwait(ConfigureAwaitOptions.SuppressThrowing);
            if (!timer.IsCancellationRequested)
                await Locked(() => _snapshot == snapshot ? FinishAsync("time") : Task.CompletedTask);
        }
    }

    async Task FinishAsync(string reason)
    {
        if (_gc == null)
            return;
        _endReason ??= reason;
        _wanted.Clear();
        await SendToGcAsync(MessageType.Done, []);
    }

    sealed record Hit(int Node, bool Live = false, int Count = 0, long Bytes = 0);

    public Task OnErasedAsync(List<(ulong Address, EraseStatus Status)> results) => Locked(async () =>
    {
        if (_snapshot == null)
            return;
        var freed = new List<int>();
        var live = new List<int>();
        var destroyed = new List<int>();
        var hits = new Dictionary<int, Hit>();
        foreach (var (address, status) in results)
        {
            int i = _snapshot.Find(address);
            if (!_asked.Remove(i))
                continue;
            if (_graph != null && status != EraseStatus.NotAnObject)
            {
                int node = _graph.NodeOf[i];
                Hit hit = hits.GetValueOrDefault(node) ?? new Hit(node);
                hits[node] = status == EraseStatus.Alive
                    ? hit with { Live = true }
                    : hit with { Count = hit.Count + 1, Bytes = hit.Bytes + _snapshot.Size(i) };
            }
            if (status == EraseStatus.Freed)
            {
                freed.Add(i);
                _freedBytes += _snapshot.Size(i);
                _freedCount++;
                _snapshot.SetFree(i);
            }
            else if (status == EraseStatus.Alive)
            {
                live.Add(i);
                _knownLive.Add(i);
                _lives = Math.Max(0, _lives - 1);
            }
            else if (status == EraseStatus.Destroyed)
            {
                destroyed.Add(i);
                _destroyed++;
                _snapshot.SetFree(i);
            }
        }
        browsers.Send(new { type = "erased", freed, live, destroyed, hits = hits.Values, state = State() });
        await SendWantedAsync();
    });

    object AppsMessage() => new { type = "apps", apps = _apps.Select(a => new { a.Id, a.Pid, name = Path.GetFileName(a.Name) }) };

    public Task AttachAsync(AttachedApp app) => Locked(() =>
    {
        _apps.Add(app);
        browsers.Send(AppsMessage());
        return Task.CompletedTask;
    });

    public Task DetachAsync(AttachedApp app) => Locked(() =>
    {
        _apps.Remove(app);
        browsers.Send(AppsMessage());
        return Task.CompletedTask;
    });

    /// <summary>Adds a page and brings it up to date. Both under the lock, so that it misses no update.</summary>
    public Task JoinAsync(Browser browser, Action add) => Locked(() =>
    {
        add();
        browser.Send(AppsMessage());
        if (_snapshot != null)
        {
            browser.Send(SnapshotMessage(_snapshot));
            browser.Send(_snapshot.ToFrame());
        }
        return Task.CompletedTask;
    });

    public Task HandleAsync(Browser browser, JsonElement message) => Locked(async () =>
    {
        switch (message.GetProperty("type").GetString())
        {
            case "reveal" when _snapshot != null:
                _peeked |= _gc != null;
                browser.Send(_snapshot.LivenessFrame());
                browsers.Send(new { type = "state", state = State() });
                break;
            case "hardcore" when _snapshot is { HardcoreAllowed: true } && _gc != null:
                _hardcore = message.GetProperty("on").GetBoolean();
                browsers.Send(new { type = "state", state = State() });
                break;
            case "erase":
                await EraseAsync(message.GetProperty("blocks"));
                break;
            case "collect" when _gc == null:
                if (_apps.Find(a => a.Id == message.GetProperty("app").GetInt32()) is { } app)
                    await WriteAsync(app.Stream, MessageType.Collect, []);
                break;
            case "graph" when _snapshot != null:
                SendGraph(browser, _snapshot);
                break;
            case "shoot" when _snapshot != null && _graph != null && _gc != null:
                await ShootAsync(message.GetProperty("node").GetInt32());
                break;
            case "crash" when _gc != null && _lives > 0:
                _lives--;
                browsers.Send(new { type = "state", state = State() });
                break;
            case "done":
                await FinishAsync("done");
                break;
        }
    });

    void SendGraph(Browser browser, Snapshot snapshot)
    {
        if (snapshot.Count > Snapshot.MaxShown)
        {
            browser.Send(new { type = "graph", gc = snapshot.Gc, tooLarge = true });
            return;
        }
        _graph ??= new ObjectGraph(snapshot);
        browser.Send(new
        {
            type = "graph",
            gc = snapshot.Gc,
            nodes = _graph.Nodes,
            edges = _graph.Edges.Select(e => new[] { e.From, e.To }),
            disagreements = _graph.Disagreements,
            gone = Enumerable.Range(0, _graph.Nodes.Count).Where(i => _graph.Members[i].All(snapshot.IsFree)),
        });
    }

    /// <summary>
    /// Erases everything a node stands for. A live node costs one life however many objects are in it,
    /// so the GC is asked about only one of them.
    /// </summary>
    async Task ShootAsync(int node)
    {
        if (node < 0 || node >= _graph!.Members.Count)
            return;
        int[] members = _graph.Members[node];
        bool live = Array.Exists(members, _snapshot!.IsLive);
        foreach (int i in live && !_hardcore ? members[..1] : members)
            _wanted.Enqueue(i);
        await SendWantedAsync();
    }

    async Task EraseAsync(JsonElement blocks)
    {
        foreach (JsonElement block in blocks.EnumerateArray())
            _wanted.Enqueue(block.GetInt32());
        await SendWantedAsync();
    }

    /// <summary>
    /// Sends the next batch, once the last one has been answered. The GC does not read while it writes
    /// results, so with two on the way both sides could end up blocked on full socket buffers.
    /// </summary>
    async Task SendWantedAsync()
    {
        if (_snapshot == null || _gc == null || _asked.Count > 0)
            return;
        int lives = _lives;
        var addresses = new List<ulong>();
        while ((lives > 0 || _hardcore) && addresses.Count < MaxBatch && _wanted.TryDequeue(out int i))
        {
            bool shielded = _knownLive.Contains(i) && !_hardcore;  // already cost a life
            if (i < 0 || i >= _snapshot.Shown || _snapshot.IsFree(i) || shielded || !_asked.Add(i))
                continue;
            if (_snapshot.IsLive(i) && !_hardcore)
                lives--;  // stops the batch before it costs more lives than there are
            addresses.Add(_snapshot.Address(i));
        }
        if (addresses.Count == 0)
            return;
        var payload = new byte[5 + addresses.Count * 8];
        payload[0] = _hardcore ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(1), addresses.Count);
        for (int i = 0; i < addresses.Count; i++)
            BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(5 + i * 8), addresses[i]);
        await SendToGcAsync(MessageType.Erase, payload);
    }

    Task SendToGcAsync(MessageType type, byte[] payload) =>
        _gc == null ? Task.CompletedTask : WriteAsync(_gc, type, payload);

    static async Task WriteAsync(Stream stream, MessageType type, byte[] payload)
    {
        var message = new byte[5 + payload.Length];
        BinaryPrimitives.WriteInt32LittleEndian(message, 1 + payload.Length);
        message[4] = (byte)type;
        payload.CopyTo(message, 5);
        try
        {
            await stream.WriteAsync(message);
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException)
        {
        }
    }
}

sealed record GameRules(int Lives, int Seconds);
