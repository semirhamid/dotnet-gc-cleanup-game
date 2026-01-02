using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace ManualGC.Viewer;

/// <summary>Reads one message payload.</summary>
sealed class ProtocolReader(byte[] buffer, int start, int end)
{
    int _at = start;
    public int Remaining => end - _at;
    ReadOnlySpan<byte> Take(int n)
    {
        if (Remaining < n)
            throw new InvalidDataException("message too short");
        _at += n;
        return buffer.AsSpan(_at - n, n);
    }
    public byte U8() => Take(1)[0];
    public ushort U16() => BinaryPrimitives.ReadUInt16LittleEndian(Take(2));
    public uint U32() => BinaryPrimitives.ReadUInt32LittleEndian(Take(4));
    public ulong U64() => BinaryPrimitives.ReadUInt64LittleEndian(Take(8));
    public string Rest() => Encoding.UTF8.GetString(Take(Remaining));
}

/// <summary>An app that is running under the GC and can be asked to collect.</summary>
sealed record AttachedApp(int Id, uint Pid, string Name, Stream Stream);

/// <summary>
/// Accepts connections from apps. The first message tells a control connection from a collection.
/// Collections are played one at a time; another app's waits, suspended, for its turn. The protocol is
/// in docs/protocol.md.
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
            if (first[0] == 9)
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
            byte type = next[0];
            if (type == 1)
            {
                uint version = reader.U32();
                if (version != 3)
                    throw new InvalidDataException($"protocol version {version}");
                snapshot = new Snapshot(reader.U32(), reader.U32(), reader.U64(), reader.U64(), reader.U8());
                continue;
            }
            if (snapshot == null)
                throw new InvalidDataException("no Hello");
            switch (type)
            {
                case 2:
                    snapshot.Names.AddModule(reader.U32(), reader.Rest());
                    break;
                case 3:
                    snapshot.Names.AddType(reader);
                    break;
                case 4:
                    while (reader.Remaining > 0)
                        snapshot.Add(reader.U64(), reader.U64(), reader.U64());
                    break;
                case 6:
                    var results = new List<(ulong Address, byte Status)>();
                    while (reader.Remaining > 0)
                        results.Add((reader.U64(), reader.U8()));
                    await session.OnErasedAsync(results);
                    break;
                case 7:
                    while (reader.Remaining > 0)
                        snapshot.AddReference(reader.U64(), reader.U64());
                    break;
                case 8:
                    while (reader.Remaining > 0)
                        snapshot.AddRoot(reader.U64(), reader.U8());
                    break;
                case 5:
                    ulong count = reader.U64();
                    if (count != (ulong)snapshot.Count)
                        throw new InvalidDataException($"got {snapshot.Count} of {count} objects");
                    log.LogInformation("Collection {Gc} of process {Pid}: {Count} entries, {Bytes:N0} bytes",
                        snapshot.Gc, snapshot.Pid, snapshot.Count, snapshot.End - snapshot.Base);
                    await session.BeginAsync(snapshot, stream);
                    break;
                default:
                    throw new InvalidDataException($"unknown message {type}");
            }
        }
    }
}
