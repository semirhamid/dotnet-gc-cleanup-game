using System.Text.Json;

namespace ManualGC.Viewer;

/// <summary>The current collection, or the last one after the app resumed.</summary>
sealed class Session(Browsers browsers)
{
    readonly SemaphoreSlim _lock = new(1);
    Snapshot? _snapshot;
    bool _frozen;

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
        frozen = _frozen,
    };

    public Task BeginAsync(Snapshot snapshot) => Locked(async () =>
    {
        _snapshot = snapshot;
        _frozen = true;
        await browsers.SendAsync(SnapshotMessage(snapshot));
        await browsers.SendAsync(snapshot.ToFrame());
    });

    public Task EndAsync() => Locked(async () =>
    {
        if (!_frozen)
            return;
        _frozen = false;
        await browsers.SendAsync(new { type = "resumed" });
    });

    /// <summary>Adds a page and brings it up to date, with no update slipping in between.</summary>
    public Task JoinAsync(Browser browser, Action add) => Locked(async () =>
    {
        add();
        if (_snapshot == null)
            return;
        await browser.SendAsync(SnapshotMessage(_snapshot));
        await browser.SendAsync(_snapshot.ToFrame());
    });

    public Task HandleAsync(Browser browser, JsonElement message) => Locked(async () =>
    {
        switch (message.GetProperty("type").GetString())
        {
            case "reveal" when _snapshot != null:
                await browser.SendAsync(_snapshot.LivenessFrame());
                break;
        }
    });
}
