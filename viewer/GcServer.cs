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

/// <summary>Accepts the GC's connection, one collection at a time. The protocol is in docs/protocol.md.</summary>
sealed class GcServer(GcPort port, Session session, ILogger<GcServer> log) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stop)
    {
        var listener = new TcpListener(IPAddress.Loopback, port.Value);
        listener.Start();
        while (!stop.IsCancellationRequested)
        {
            using TcpClient client = await listener.AcceptTcpClientAsync(stop);
            client.NoDelay = true;
            try
            {
                await ServeAsync(client.GetStream(), stop);
            }
            catch (Exception e) when (e is IOException or InvalidDataException)
            {
                log.LogWarning("GC connection ended: {Message}", e.Message);
            }
            finally
            {
                await session.EndAsync();
            }
        }
    }

    async Task ServeAsync(NetworkStream stream, CancellationToken stop)
    {
        Snapshot? snapshot = null;
        byte[] buffer = new byte[64 * 1024];
        while (true)
        {
            if (await stream.ReadAtLeastAsync(buffer.AsMemory(0, 4), 4, throwOnEndOfStream: false, stop) < 4)
                return;
            int length = BinaryPrimitives.ReadInt32LittleEndian(buffer);
            if (length is < 1 or > 4 * 1024 * 1024)
                throw new InvalidDataException($"bad message length {length}");
            if (buffer.Length < length)
                buffer = new byte[length];
            await stream.ReadExactlyAsync(buffer.AsMemory(0, length), stop);

            var reader = new ProtocolReader(buffer, 1, length);
            byte type = buffer[0];
            if (type == 1)
            {
                uint version = reader.U32();
                if (version != 1)
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
                case 5:
                    ulong count = reader.U64();
                    if (count != (ulong)snapshot.Count)
                        throw new InvalidDataException($"got {snapshot.Count} of {count} objects");
                    log.LogInformation("Collection {Gc} of process {Pid}: {Count} entries, {Bytes:N0} bytes",
                        snapshot.Gc, snapshot.Pid, snapshot.Count, snapshot.End - snapshot.Base);
                    await session.BeginAsync(snapshot, stream);
                    break;
                case 6:
                    var results = new List<(ulong Address, byte Status)>();
                    while (reader.Remaining > 0)
                        results.Add((reader.U64(), reader.U8()));
                    await session.OnErasedAsync(results);
                    break;
                default:
                    throw new InvalidDataException($"unknown message {type}");
            }
        }
    }
}
