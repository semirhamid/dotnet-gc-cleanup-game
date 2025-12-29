using System.Buffers.Binary;
using System.Text.Json;

namespace ManualGC.Viewer;

/// <summary>
/// The game: one round per collection. Holds the current collection, or the last one after the app
/// resumed. The GC alone decides what may be freed; this keeps score.
/// </summary>
sealed class Session(Browsers browsers, GameRules rules)
{
    const int MaxBatch = 8000;

    readonly SemaphoreSlim _lock = new(1);
    readonly Queue<int> _wanted = [];   // erase requests not sent yet
    readonly HashSet<int> _asked = [];  // sent to the GC and not answered
    readonly HashSet<int> _knownLive = [];
    Snapshot? _snapshot;
    Stream? _gc;  // set while the app is suspended
    int _lives;
    long _freedBytes;
    int _freedCount;
    int _destroyed;     // live objects freed in hardcore mode
    bool _peeked;       // a round in which somebody looked at what is alive scores nothing
    bool _hardcore;
    string? _endReason;
    long _deadline;     // Environment.TickCount64, or 0 for no limit
    CancellationTokenSource? _timer;
    uint _pid;
    long _totalScore;   // earlier rounds of the same process

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
        _gc = gc;
        _lives = rules.Lives;
        _totalScore = snapshot.Pid == _pid ? _totalScore + RoundScore : 0;
        _pid = snapshot.Pid;
        _freedBytes = 0;
        _freedCount = 0;
        _destroyed = 0;
        _peeked = false;
        _hardcore = false;  // asked for anew every time
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
            await Task.Delay(TimeSpan.FromSeconds(rules.Seconds), timer.Token).ConfigureAwait(ConfigureAwaitOptions.SuppressThrowing);
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
        await SendToGcAsync(65, []);
    }

    public Task OnErasedAsync(List<(ulong Address, byte Status)> results) => Locked(async () =>
    {
        if (_snapshot == null)
            return;
        var freed = new List<int>();
        var live = new List<int>();
        var destroyed = new List<int>();
        foreach (var (address, status) in results)
        {
            int i = _snapshot.Find(address);
            if (!_asked.Remove(i))
                continue;
            if (status == 0)
            {
                freed.Add(i);
                _freedBytes += _snapshot.Size(i);
                _freedCount++;
                _snapshot.SetFree(i);
            }
            else if (status == 1)
            {
                live.Add(i);
                _knownLive.Add(i);
                _lives = Math.Max(0, _lives - 1);
            }
            else if (status == 3)
            {
                destroyed.Add(i);
                _destroyed++;
                _snapshot.SetFree(i);
            }
        }
        browsers.Send(new { type = "erased", freed, live, destroyed, state = State() });
        await SendWantedAsync();
    });

    /// <summary>Adds a page and brings it up to date, with no update slipping in between.</summary>
    public Task JoinAsync(Browser browser, Action add) => Locked(() =>
    {
        add();
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
            case "done":
                await FinishAsync("done");
                break;
        }
    });

    async Task EraseAsync(JsonElement blocks)
    {
        foreach (JsonElement block in blocks.EnumerateArray())
            _wanted.Enqueue(block.GetInt32());
        await SendWantedAsync();
    }

    /// <summary>
    /// Sends the next batch. Only one is ever unanswered: the GC does not read while it writes
    /// results, so more could leave both sides blocked on full socket buffers.
    /// </summary>
    async Task SendWantedAsync()
    {
        if (_snapshot == null || _gc == null || _asked.Count > 0)
            return;
        int lives = _lives;
        var addresses = new List<ulong>();
        while (lives > 0 && addresses.Count < MaxBatch && _wanted.TryDequeue(out int i))
        {
            bool shielded = _knownLive.Contains(i) && !_hardcore;  // already cost a life
            if (i < 0 || i >= _snapshot.Shown || _snapshot.IsFree(i) || shielded || !_asked.Add(i))
                continue;
            if (_snapshot.IsLive(i) && !_hardcore)
                lives--;  // so one batch cannot cost more lives than are left
            addresses.Add(_snapshot.Address(i));
        }
        if (addresses.Count == 0)
            return;
        var payload = new byte[5 + addresses.Count * 8];
        payload[0] = _hardcore ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(1), addresses.Count);
        for (int i = 0; i < addresses.Count; i++)
            BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(5 + i * 8), addresses[i]);
        await SendToGcAsync(64, payload);
    }

    async Task SendToGcAsync(byte type, byte[] payload)
    {
        if (_gc == null)
            return;
        var message = new byte[5 + payload.Length];
        BinaryPrimitives.WriteInt32LittleEndian(message, 1 + payload.Length);
        message[4] = type;
        payload.CopyTo(message, 5);
        try
        {
            await _gc.WriteAsync(message);
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException)
        {
            // The reader notices that the app is gone and ends the session.
        }
    }
}

/// <param name="Seconds">Length of a round; 0 for no limit.</param>
sealed record GameRules(int Lives, int Seconds);
