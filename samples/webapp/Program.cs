// An ordinary web app that knows nothing of the GC and never calls GC.Collect(). Every answer can be
// checked, so freeing something in use would show.
using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text.Json;

var builder = WebApplication.CreateBuilder(args);
builder.Logging.SetMinimumLevel(LogLevel.Warning);
builder.Services.AddSingleton<Orders>();
builder.Services.AddHostedService<Ticker>();
var app = builder.Build();

// About 0.5 MB of garbage a request, of all sizes, through JSON, strings, LINQ and hashing.
app.MapGet("/work/{n:int}", (int n) =>
{
    var items = Enumerable.Range(0, 500).Select(i => new Item(n * i, $"item {n}-{i}", [.. Enumerable.Range(0, 10).Select(k => k * n)])).ToList();
    string json = JsonSerializer.Serialize(items);
    var back = JsonSerializer.Deserialize<List<Item>>(json)!;
    var buffer = new byte[100_000];
    Array.Fill(buffer, (byte)n);
    string hash = Convert.ToHexString(SHA256.HashData(buffer));
    string expected = Convert.ToHexString(SHA256.HashData(Enumerable.Repeat((byte)n, 100_000).ToArray()));
    bool ok = back.Zip(items).All(p => p.First.Id == p.Second.Id && p.First.Name == p.Second.Name && p.First.Parts.SequenceEqual(p.Second.Parts)) && hash == expected;
    return ok ? Results.Text($"ok {n}") : Results.StatusCode(500);
});

// State that outlives requests, checked whenever it is read.
app.MapPost("/orders/{id:int}", (int id, Orders orders) => { orders.Add(id); return Results.Text("ok"); });
app.MapGet("/orders", (Orders orders) => orders.Check() ? Results.Text($"ok {orders.Count}") : Results.StatusCode(500));
app.MapGet("/self", async (HttpContext context) =>
{
    using var client = new HttpClient();
    return Results.Text(await client.GetStringAsync($"http://localhost:{context.Connection.LocalPort}/work/7"));
});
app.Run();

record Item(int Id, string Name, int[] Parts);

sealed class Orders
{
    readonly ConcurrentDictionary<int, (string Text, byte[] Data)> _all = new();
    public int Count => _all.Count;
    public void Add(int id) => _all[id] = ($"order {id}", [.. Enumerable.Range(0, 100).Select(i => (byte)(id + i))]);
    public bool Check() => _all.All(o => o.Value.Text == $"order {o.Key}" && o.Value.Data.Select((b, i) => b == (byte)(o.Key + i)).All(x => x));
}

sealed class Ticker : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stop)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(10));
        while (await timer.WaitForNextTickAsync(stop)) GC.KeepAlive(string.Join(',', Enumerable.Range(0, 100)));
    }
}
