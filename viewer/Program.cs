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
// Only loopback names, so a web page cannot reach this server by rebinding its own DNS name.
builder.Configuration["AllowedHosts"] = "localhost;127.0.0.1;[::1]";
builder.Services.Configure<Microsoft.AspNetCore.HostFiltering.HostFilteringOptions>(o =>
    o.AllowedHosts = ["localhost", "127.0.0.1", "[::1]"]);
builder.Services.AddHostFiltering(_ => { });
builder.Services.AddSingleton<Browsers>();
builder.Services.AddSingleton<Session>();
builder.Services.AddSingleton(new GcPort(gcPort));
builder.Services.AddHostedService<GcServer>();

var app = builder.Build();
app.UseHostFiltering();
app.UseDefaultFiles();
app.UseStaticFiles(new StaticFileOptions
{
    OnPrepareResponse = c => c.Context.Response.Headers.CacheControl = "no-cache",
});
app.UseWebSockets();
app.Map("/ws", async (HttpContext context, Session session, Browsers browsers) =>
{
    if (!context.WebSockets.IsWebSocketRequest)
    {
        context.Response.StatusCode = StatusCodes.Status400BadRequest;
        return;
    }
    // Browsers always send Origin, so a page from another site is turned away here.
    string? origin = context.Request.Headers.Origin;
    if (origin != null && origin != $"{context.Request.Scheme}://{context.Request.Host}")
    {
        context.Response.StatusCode = StatusCodes.Status403Forbidden;
        return;
    }
    using var socket = await context.WebSockets.AcceptWebSocketAsync();
    await browsers.RunAsync(socket, session, context.RequestAborted);
});

Console.WriteLine($"Viewer: http://localhost:{webPort}   waiting for the GC on 127.0.0.1:{gcPort}");
app.Run();

namespace ManualGC.Viewer
{
    sealed record GcPort(int Value);
}
