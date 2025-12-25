using System.Buffers.Binary;
using System.Text.Json;

namespace ManualGC.Viewer;

/// <summary>The current collection, or the last one after the app resumed.</summary>
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
        state = State(),
    };

    object State() => new { lives = _lives, maxLives = rules.Lives, freedBytes = _freedBytes, freedCount = _freedCount };

    public Task BeginAsync(Snapshot snapshot, Stream gc) => Locked(() =>
    {
        _snapshot = snapshot;
        _gc = gc;
        _lives = rules.Lives;
        _freedBytes = 0;
        _freedCount = 0;
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
            browsers.Send(new { type = "resumed", state = State() });
        }
        return Task.CompletedTask;
    });

    public Task OnErasedAsync(List<(ulong Address, byte Status)> results) => Locked(async () =>
    {
        if (_snapshot == null)
            return;
        var freed = new List<int>();
        var live = new List<int>();
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
        }
        browsers.Send(new { type = "erased", freed, live, state = State() });
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
                browser.Send(_snapshot.LivenessFrame());
                break;
            case "erase":
                await EraseAsync(message.GetProperty("blocks"));
                break;
            case "done":
                _wanted.Clear();
                await SendToGcAsync(65, []);
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
            if (i < 0 || i >= _snapshot.Shown || _snapshot.IsFree(i) || _knownLive.Contains(i) || !_asked.Add(i))
                continue;
            if (_snapshot.IsLive(i))
                lives--;  // so one batch cannot cost more lives than are left
            addresses.Add(_snapshot.Address(i));
        }
        if (addresses.Count == 0)
            return;
        var payload = new byte[4 + addresses.Count * 8];
        BinaryPrimitives.WriteInt32LittleEndian(payload, addresses.Count);
        for (int i = 0; i < addresses.Count; i++)
            BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(4 + i * 8), addresses[i]);
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

sealed record GameRules(int Lives);
