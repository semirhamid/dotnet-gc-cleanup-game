// Plays the browser's part over the viewer's WebSocket. Usage: player <port> <mode>
//   look    wait for one snapshot, print counts by type, check that the blocks tile the heap, press Done
//   clean   in every collection, erase everything dead and press Done
//   idle    in every collection, press Done at once
//   greedy  in one collection, spend every life on live objects, then try to erase something dead
//   random  in every collection, erase a random half of everything dead
//   bytes   in every collection, erase every byte[] without peeking, and print the score
//   wait    do nothing and let the time run out
//   hardcore  in one collection, turn hardcore on and erase every live Kept object
// Modes that play every collection stop when the viewer goes away.
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Text.Json;

string mode = args[1];
using var socket = new ClientWebSocket();
await socket.ConnectAsync(new Uri($"ws://localhost:{args[0]}/ws"), CancellationToken.None);
Console.WriteLine("connected");

JsonElement info = default;
uint[] words = [];
byte[] live = [];
var random = new Random(1);
int rounds = 0, freed = 0, refused = 0, expected = 0;
try
{
    while (await Receive() is var (json, data))
    {
        if (json is { } message)
        {
            switch (message.GetProperty("type").GetString())
            {
                case "snapshot": info = message; break;
                case "erased":
                    freed += message.GetProperty("freed").GetArrayLength();
                    refused += message.GetProperty("live").GetArrayLength();
                    if (await OnErased(message)) return 0;
                    break;
                case "resumed":
                    Console.WriteLine($"resumed: {message.GetProperty("reason").GetString()}, missed {message.GetProperty("missedBytes")}, {message.GetProperty("state")}");
                    if (mode is "look" or "greedy" or "hardcore" or "wait") return 0;
                    break;
            }
            continue;
        }
        uint tag = BitConverter.ToUInt32(data);
        if (tag == 1)
        {
            words = MemoryMarshal.Cast<byte, uint>(data.AsSpan()).ToArray();
            rounds++;
            // A finished collection is sent to new pages too; there is nothing to play in it.
            if (!info.GetProperty("frozen").GetBoolean()) continue;
            if (mode == "bytes")
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
        else if (tag == 2)
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

async Task Erase(int[] targets)
{
    foreach (int[] chunk in targets.Chunk(10_000)) await Send(new { type = "erase", blocks = chunk });
}

async Task<bool> OnErased(JsonElement message)
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
    return false;
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

Task Send(object message) => socket.SendAsync(JsonSerializer.SerializeToUtf8Bytes(message), WebSocketMessageType.Text, true, CancellationToken.None);
