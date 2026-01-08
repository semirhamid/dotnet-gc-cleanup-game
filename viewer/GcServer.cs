using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;

namespace ManualGC.Viewer;

sealed record AttachedApp(int Id, uint Pid, string Name, Stream Stream);

/// <summary>
/// Accepts connections from apps. The first message says whether it is a control connection or a
/// collection. Only one collection is played at a time; any other app that wants one stays suspended
/// until it is its turn. The protocol is in docs/protocol.md.
/// </summary>
sealed class GcServer(GcPort port, Session session, ILogger<GcServer> log) : BackgroundService
{
    readonly SemaphoreSlim _turn = new(1);
    int _apps;

    protected override async Task ExecuteAsync(CancellationToken stop)
    {
        using var listener = new TcpListener(IPAddress.Loopback, port.Value);
        listener.Start();
        while (!stop.IsCancellationRequested)
            _ = ServeAsync(await listener.AcceptTcpClientAsync(stop), stop);
    }

    async Task ServeAsync(TcpClient client, CancellationToken stop)
    {
        using TcpClient _ = client;
        client.NoDelay = true;
        NetworkStream stream = client.GetStream();
        try
        {
            byte[]? first = await ReadAsync(stream, stop);
            if (first == null)
                return;
            if ((MessageType)first[0] == MessageType.Attach)
                await ServeAppAsync(stream, first, stop);
            else
                await ServeCollectionAsync(stream, first, stop);
        }
        catch (Exception e) when (e is IOException or InvalidDataException or OperationCanceledException)
        {
            if (!stop.IsCancellationRequested)
                log.LogWarning("GC connection ended: {Message}", e.Message);
        }
        catch (Exception e)
        {
            log.LogError(e, "GC connection failed");
        }
    }

    /// <summary>One message, type first, or null at the end of the stream.</summary>
    static async Task<byte[]?> ReadAsync(NetworkStream stream, CancellationToken stop)
    {
        var header = new byte[4];
        if (await stream.ReadAtLeastAsync(header, 4, throwOnEndOfStream: false, stop) < 4)
            return null;
        int length = BinaryPrimitives.ReadInt32LittleEndian(header);
        if (length is < 1 or > 4 * 1024 * 1024)
            throw new InvalidDataException($"bad message length {length}");
        var message = new byte[length];
        await stream.ReadExactlyAsync(message, stop);
        return message;
    }

    async Task ServeAppAsync(NetworkStream stream, byte[] first, CancellationToken stop)
    {
        var reader = new ProtocolReader(first, 1, first.Length);
        var app = new AttachedApp(Interlocked.Increment(ref _apps), reader.U32(), reader.Rest(), stream);
        log.LogInformation("Attached: process {Pid}, {Name}", app.Pid, app.Name);
        await session.AttachAsync(app);
        try
        {
            // Apps have nothing more to say. This only waits for the connection to close.
            while (await ReadAsync(stream, stop) != null) { }
        }
        finally
        {
            await session.DetachAsync(app);
        }
    }

    async Task ServeCollectionAsync(NetworkStream stream, byte[] first, CancellationToken stop)
    {
        await _turn.WaitAsync(stop);
        try
        {
            await PlayAsync(stream, first, stop);
        }
        finally
        {
            await session.EndAsync();
            _turn.Release();
        }
    }

    async Task PlayAsync(NetworkStream stream, byte[] message, CancellationToken stop)
    {
        Snapshot? snapshot = null;
        for (byte[]? next = message; next != null; next = await ReadAsync(stream, stop))
        {
            var reader = new ProtocolReader(next, 1, next.Length);
            var type = (MessageType)next[0];
            if (type == MessageType.Hello)
            {
                uint version = reader.U32();
                if (version != ProtocolReader.Version)
                    throw new InvalidDataException($"protocol version {version}");
                snapshot = new Snapshot(reader.U32(), reader.U32(), reader.U64(), reader.U64(), reader.U8());
                continue;
            }
            if (snapshot == null)
                throw new InvalidDataException("no Hello");
            switch (type)
            {
                case MessageType.Module:
                    snapshot.Names.AddModule(reader.U32(), reader.Rest());
                    break;
                case MessageType.Type:
                    snapshot.Names.AddType(reader);
                    break;
                case MessageType.Objects:
                    while (reader.Remaining > 0)
                        snapshot.Add(reader.U64(), reader.U64(), reader.U64());
                    break;
                case MessageType.References:
                    while (reader.Remaining > 0)
                        snapshot.AddReference(reader.U64(), reader.U64());
                    break;
                case MessageType.Roots:
                    while (reader.Remaining > 0)
                        snapshot.AddRoot(reader.U64(), reader.U8());
                    break;
                case MessageType.SnapshotEnd:
                    ulong count = reader.U64();
                    if (count != (ulong)snapshot.Count)
                        throw new InvalidDataException($"got {snapshot.Count} of {count} objects");
                    log.LogInformation("Collection {Gc} of process {Pid}: {Count} entries, {Bytes:N0} bytes",
                        snapshot.Gc, snapshot.Pid, snapshot.Count, snapshot.End - snapshot.Base);
                    await session.BeginAsync(snapshot, stream);
                    break;
                case MessageType.EraseResults:
                    var results = new List<(ulong Address, EraseStatus Status)>();
                    while (reader.Remaining > 0)
                        results.Add((reader.U64(), (EraseStatus)reader.U8()));
                    await session.OnErasedAsync(results);
                    break;
                default:
                    throw new InvalidDataException($"unknown message {next[0]}");
            }
        }
    }
}
