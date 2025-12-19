// Plays the browser's part over the viewer's WebSocket. Usage: player <port> look
//   look   wait for one snapshot, print counts by type, and check the blocks tile the heap.
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Text.Json;

using var stop = new CancellationTokenSource(TimeSpan.FromSeconds(60));
using var socket = new ClientWebSocket();
await socket.ConnectAsync(new Uri($"ws://localhost:{args[0]}/ws"), stop.Token);
Console.WriteLine("connected");

async Task<(JsonElement? json, byte[] data)> Receive()
{
    var stream = new MemoryStream();
    var buffer = new byte[256 * 1024];
    WebSocketReceiveResult result;
    do
    {
        result = await socket.ReceiveAsync(buffer, stop.Token);
        stream.Write(buffer, 0, result.Count);
    } while (!result.EndOfMessage);
    byte[] data = stream.ToArray();
    return (result.MessageType == WebSocketMessageType.Text ? JsonDocument.Parse(data).RootElement : null, data);
}

Task Send(object message) => socket.SendAsync(JsonSerializer.SerializeToUtf8Bytes(message), WebSocketMessageType.Text, true, stop.Token);

JsonElement info = default;
uint[] words = [];
while (words.Length == 0)
{
    var (json, data) = await Receive();
    if (json?.GetProperty("type").GetString() == "snapshot") info = json.Value;
    else if (json == null && BitConverter.ToUInt32(data) == 1) words = MemoryMarshal.Cast<byte, uint>(data.AsSpan()).ToArray();
}
int n = (int)words[1];
await Send(new { type = "reveal" });
byte[] live = [];
while (live.Length == 0)
{
    var (json, data) = await Receive();
    if (json == null && BitConverter.ToUInt32(data) == 2) live = data[8..];
}

string[] types = [.. info.GetProperty("types").EnumerateArray().Select(t => t.GetString()!)];
bool tiled = true;
ulong next = 0;
var counts = new SortedDictionary<string, (int live, int dead)>(StringComparer.Ordinal);
for (int i = 0; i < n; i++)
{
    tiled &= words[2 + i] == next;
    next = words[2 + i] + (ulong)words[2 + n + i];
    string type = types[words[2 + 2 * n + i]];
    var c = counts.GetValueOrDefault(type);
    counts[type] = live[i] != 0 ? (c.live + 1, c.dead) : (c.live, c.dead + 1);
}
foreach (var (type, c) in counts)
    Console.WriteLine($"{type}: live {c.live} dead {c.dead}");
Console.WriteLine($"tiled: {tiled && next * 8 == info.GetProperty("bytes").GetUInt64()}");
Console.WriteLine($"unnamed types: {types.Count(t => t.StartsWith("type 0x"))} of {types.Length}");
