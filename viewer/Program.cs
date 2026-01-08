using ManualGC.Viewer;

var builder = WebApplication.CreateSlimBuilder(new WebApplicationOptions
{
    Args = args,
    ContentRootPath = AppContext.BaseDirectory,
});
int webPort = builder.Configuration.GetValue("WebPort", 5711);
int gcPort = builder.Configuration.GetValue("GcPort", 5710);

builder.Logging.SetMinimumLevel(LogLevel.Warning).AddFilter("ManualGC", LogLevel.Information);
builder.WebHost.ConfigureKestrel(o => o.ListenLocalhost(webPort));
// Only accept loopback host names. Otherwise a website could reach this server by pointing its own
// DNS name at 127.0.0.1.
builder.Services.Configure<Microsoft.AspNetCore.HostFiltering.HostFilteringOptions>(o =>
    o.AllowedHosts = ["localhost", "127.0.0.1", "[::1]"]);
builder.Services.AddHostFiltering(_ => { });
builder.Services.AddSingleton<Browsers>();
builder.Services.AddSingleton<Session>();
builder.Services.AddSingleton(new GcPort(gcPort));
builder.Services.AddSingleton(new GameRules(
    Lives: builder.Configuration.GetValue("Lives", 3),
    Seconds: builder.Configuration.GetValue("Seconds", 120)));
builder.Services.AddHostedService<GcServer>();

var app = builder.Build();
app.UseHostFiltering();
app.UseDefaultFiles();
app.UseStaticFiles(new StaticFileOptions
{
    OnPrepareResponse = c => c.Context.Response.Headers.CacheControl = "no-cache",
});
app.UseWebSockets();
app.Map("/ws", async (HttpContext context, Session session, Browsers browsers, IHostApplicationLifetime lifetime) =>
{
    if (!context.WebSockets.IsWebSocketRequest)
    {
        context.Response.StatusCode = StatusCodes.Status400BadRequest;
        return;
    }
    // Browsers always send Origin, so this turns away pages from other sites.
    string? origin = context.Request.Headers.Origin;
    if (origin != null && origin != $"{context.Request.Scheme}://{context.Request.Host}")
    {
        context.Response.StatusCode = StatusCodes.Status403Forbidden;
        return;
    }
    using var socket = await context.WebSockets.AcceptWebSocketAsync();
    // Without this, an open page would keep the viewer from shutting down.
    using var stop = CancellationTokenSource.CreateLinkedTokenSource(context.RequestAborted, lifetime.ApplicationStopping);
    await browsers.RunAsync(socket, session, stop.Token);
});

Console.WriteLine($"Viewer: http://localhost:{webPort}   waiting for the GC on 127.0.0.1:{gcPort}");
app.Run();

namespace ManualGC.Viewer
{
    sealed record GcPort(int Value);
}
