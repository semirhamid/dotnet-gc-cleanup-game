// Plays the browser's part over the viewer's WebSocket. Usage: player <port> <mode>
//   look    wait for one snapshot, print counts by type, check that the blocks tile the heap, press Done
//   clean   in every collection, erase everything dead and press Done
//   idle    in every collection, press Done at once
//   greedy  in one collection, spend every life on live objects, then try to erase something dead
//   random  in every collection, erase a random half of everything dead
//   bytes   in every collection, erase every byte[] without peeking, and print the score
//   wait    do nothing and let the time run out
//   hardcore  in one collection, turn hardcore on and erase every live Kept object
//   flight  in every collection, get the object graph, work out from its edges what no root reaches,
//           and shoot all of that; prints how many objects the graph and the GC disagree about
//   strafe  in one collection, shoot one live node that stands for several objects, then crash once
// A third argument of N asks the first attached app to collect every N milliseconds.
// Modes that play every collection stop when the viewer goes away.
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Text.Json;

const uint SnapshotTag = 1, LivenessTag = 2;

string mode = args[1];
using var socket = new ClientWebSocket();
await socket.ConnectAsync(new Uri($"ws://localhost:{args[0]}/ws"), CancellationToken.None);
Console.WriteLine("connected");

JsonElement info = default;
uint[] words = [];
byte[] live = [];
var random = new Random(1);
var sending = new SemaphoreSlim(1);
int app = -1;
if (args.Length > 2)
{
    var every = TimeSpan.FromMilliseconds(int.Parse(args[2]));
    _ = Task.Run(async () =>
    {
        while (true)
        {
            await Task.Delay(every);
            if (app >= 0) await Send(new { type = "collect", app });
        }
    });
}
int rounds = 0, freed = 0, refused = 0, expected = 0, waiting = 0;
try
{
    while (await Receive() is var (json, data))
    {
        if (json is { } message)
        {
            switch (message.GetProperty("type").GetString())
            {
                case "snapshot": info = message; break;
                case "apps":
                    app = message.GetProperty("apps") is { } apps && apps.GetArrayLength() > 0 ? apps[0].GetProperty("id").GetInt32() : -1;
                    Console.WriteLine($"apps {message.GetProperty("apps")}");
                    break;
                case "graph":
                    await PlayGraph(message);
                    break;
                case "state" when mode == "strafe":
                    Console.WriteLine($"after crash: {message.GetProperty("state")}");
                    await Send(new { type = "done" });
                    break;
                case "erased" when mode is "flight" or "strafe":
                    freed += message.GetProperty("freed").GetArrayLength();
                    refused += message.GetProperty("live").GetArrayLength();
                    foreach (JsonElement hit in message.GetProperty("hits").EnumerateArray())
                        waiting -= hit.GetProperty("count").GetInt32();
                    if (mode == "strafe")
                    {
                        Console.WriteLine($"shot: {message.GetProperty("hits")} {message.GetProperty("state")}");
                        await Send(new { type = "crash" });
                    }
                    else if (waiting == 0 || refused > 0) await Send(new { type = "done" });
                    break;
                case "erased":
                    freed += message.GetProperty("freed").GetArrayLength();
                    refused += message.GetProperty("live").GetArrayLength();
                    await OnErased(message);
                    break;
                case "resumed":
                    Console.WriteLine($"resumed: {message.GetProperty("reason").GetString()}, missed {message.GetProperty("missedBytes")}, {message.GetProperty("state")}");
                    if (mode is "look" or "greedy" or "hardcore" or "wait" or "strafe") return 0;
                    break;
            }
            continue;
        }
        uint tag = BitConverter.ToUInt32(data);
        if (tag == SnapshotTag)
        {
            words = MemoryMarshal.Cast<byte, uint>(data.AsSpan()).ToArray();
            rounds++;
            // New pages are also sent a collection that is already over. There is nothing to play in it.
            if (!info.GetProperty("frozen").GetBoolean()) continue;
            if (mode is "flight" or "strafe")
            {
                await Send(new { type = "graph" });
            }
            else if (mode == "bytes")
            {
                live = new byte[words[1]];
                expected = live.Length;
                await Erase([.. Enumerable.Range(0, live.Length).Where(i => info.GetProperty("types")[(int)Type(i)].GetString() == "byte[]")]);
            }
            else if (mode != "wait")
            {
                await Send(new { type = "reveal" });
            }
        }
        else if (tag == LivenessTag)
        {
            live = data[8..];
            await Play();
        }
    }
}
catch (WebSocketException) { }
Console.WriteLine($"rounds {rounds}, freed {freed}, refused {refused}");
return 0;

int[] Blocks(bool alive) => [.. Enumerable.Range(0, live.Length).Where(i => Type(i) != 0 && (live[i] != 0) == alive)];
uint Type(int i) => words[2 + 2 * live.Length + i];

async Task Play()
{
    if (mode == "look")
    {
        Look();
        await Send(new { type = "done" });
    }
    else if (mode == "greedy")
    {
        await Erase([.. Blocks(alive: true).Take(10), .. Blocks(alive: false).Take(10)]);
    }
    else if (mode == "hardcore")
    {
        await Send(new { type = "hardcore", on = true });
        int[] kept = [.. Blocks(alive: true).Where(i => info.GetProperty("types")[(int)Type(i)].GetString() == "Kept")];
        expected = kept.Length;
        await Erase(kept);
    }
    else
    {
        int[] targets = mode == "idle" ? [] : Blocks(alive: false);
        if (mode == "random") targets = [.. targets.Where(_ => random.Next(2) == 0)];
        expected = targets.Length;
        if (expected == 0) await Send(new { type = "done" });
        await Erase(targets);
    }
}

async Task PlayGraph(JsonElement graph)
{
    if (graph.TryGetProperty("tooLarge", out _))
    {
        Console.WriteLine("too large for a graph");
        await Send(new { type = "done" });
        return;
    }
    JsonElement[] nodes = [.. graph.GetProperty("nodes").EnumerateArray()];
    int[][] edges = [.. graph.GetProperty("edges").EnumerateArray().Select(e => new[] { e[0].GetInt32(), e[1].GetInt32() })];
    ILookup<int, int> held = edges.ToLookup(e => e[0], e => e[1]);
    var alive = new HashSet<int>();
    var queue = new Queue<int>(Enumerable.Range(0, nodes.Length).Where(i => nodes[i].GetProperty("rootKind").GetInt32() >= 0));
    while (queue.TryDequeue(out int i))
        if (alive.Add(i))
            foreach (int next in held[i]) queue.Enqueue(next);
    int[] garbage = [.. Enumerable.Range(0, nodes.Length).Where(i => !alive.Contains(i))];
    Console.WriteLine($"graph {graph.GetProperty("gc")}: {nodes.Length} nodes, {edges.Length} edges, {garbage.Length} loose, " +
        $"{graph.GetProperty("disagreements")} disagreements with the GC");
    if (mode == "strafe")
    {
        await Send(new { type = "shoot", node = alive.First(i => nodes[i].GetProperty("count").GetInt32() > 1) });
        return;
    }
    waiting = garbage.Sum(i => nodes[i].GetProperty("count").GetInt32());
    if (waiting == 0) await Send(new { type = "done" });
    foreach (int i in garbage) await Send(new { type = "shoot", node = i });
}

async Task Erase(int[] targets)
{
    foreach (int[] chunk in targets.Chunk(10_000)) await Send(new { type = "erase", blocks = chunk });
}

async Task OnErased(JsonElement message)
{
    int lives = message.GetProperty("state").GetProperty("lives").GetInt32();
    if (mode == "bytes")
    {
        await Send(new { type = "done" });
    }
    else if (mode == "hardcore")
    {
        Console.WriteLine($"destroyed {message.GetProperty("destroyed").GetArrayLength()}, refused {refused}");
        await Send(new { type = "done" });
    }
    else if (mode == "greedy")
    {
        Console.WriteLine($"refused {refused}, freed {freed}, lives {lives}");
        // With no lives left, erasing is over, so this must have no effect.
        await Erase([.. Blocks(alive: false).Take(10)]);
        await Task.Delay(300);
        await Send(new { type = "done" });
    }
    else if (message.GetProperty("live").GetArrayLength() > 0)
    {
        Console.WriteLine("FAIL the GC refused a block its snapshot called dead");
        await Send(new { type = "done" });
    }
    else if ((expected -= message.GetProperty("freed").GetArrayLength()) == 0)
    {
        await Send(new { type = "done" });
    }
}

void Look()
{
    int n = live.Length;
    string[] types = [.. info.GetProperty("types").EnumerateArray().Select(t => t.GetString()!)];
    bool tiled = true;
    ulong next = 0;
    var counts = new SortedDictionary<string, (int live, int dead)>(StringComparer.Ordinal);
    for (int i = 0; i < n; i++)
    {
        tiled &= words[2 + i] == next;
        next = words[2 + i] + (ulong)words[2 + n + i];
        var c = counts.GetValueOrDefault(types[Type(i)]);
        counts[types[Type(i)]] = live[i] != 0 ? (c.live + 1, c.dead) : (c.live, c.dead + 1);
    }
    foreach (var (type, c) in counts)
        Console.WriteLine($"{type}: live {c.live} dead {c.dead}");
    Console.WriteLine($"tiled: {tiled && next * 8 == info.GetProperty("bytes").GetUInt64()}");
    Console.WriteLine($"unnamed types: {types.Count(t => t.StartsWith("type 0x"))} of {types.Length}");
}

async Task<(JsonElement? json, byte[] data)?> Receive()
{
    var stream = new MemoryStream();
    var buffer = new byte[256 * 1024];
    WebSocketReceiveResult result;
    do
    {
        result = await socket.ReceiveAsync(buffer, CancellationToken.None);
        if (result.MessageType == WebSocketMessageType.Close) return null;
        stream.Write(buffer, 0, result.Count);
    } while (!result.EndOfMessage);
    byte[] data = stream.ToArray();
    return (result.MessageType == WebSocketMessageType.Text ? JsonDocument.Parse(data).RootElement : null, data);
}

async Task Send(object message)
{
    await sending.WaitAsync();
    try { await socket.SendAsync(JsonSerializer.SerializeToUtf8Bytes(message), WebSocketMessageType.Text, true, CancellationToken.None); }
    finally { sending.Release(); }
}
