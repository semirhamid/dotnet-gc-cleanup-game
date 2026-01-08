using System.Net.WebSockets;
using System.Text.Json;
using System.Threading.Channels;

namespace ManualGC.Viewer;

/// <summary>
/// One connected page. Messages are queued, so that a page which reads slowly cannot hold up the
/// session or the suspended app.
/// </summary>
sealed class Browser(WebSocket socket)
{
    readonly Channel<(byte[] Data, WebSocketMessageType Type)> _queue =
        Channel.CreateUnbounded<(byte[], WebSocketMessageType)>(new() { SingleReader = true });

    public void Send(object message) =>
        _queue.Writer.TryWrite((JsonSerializer.SerializeToUtf8Bytes(message, JsonSerializerOptions.Web), WebSocketMessageType.Text));

    public void Send(byte[] frame) => _queue.Writer.TryWrite((frame, WebSocketMessageType.Binary));

    public void Close() => _queue.Writer.TryComplete();

    public async Task PumpAsync()
    {
        try
        {
            await foreach (var (data, type) in _queue.Reader.ReadAllAsync())
                await socket.SendAsync(data, type, endOfMessage: true, CancellationToken.None);
        }
        catch (Exception e) when (e is WebSocketException or ObjectDisposedException or OperationCanceledException)
        {
        }
    }
}

sealed class Browsers
{
    readonly List<Browser> _all = [];

    public void Send(object message)
    {
        foreach (Browser browser in Current()) browser.Send(message);
    }

    public void Send(byte[] frame)
    {
        foreach (Browser browser in Current()) browser.Send(frame);
    }

    Browser[] Current()
    {
        lock (_all) return [.. _all];
    }

    public async Task RunAsync(WebSocket socket, Session session, CancellationToken stop)
    {
        var browser = new Browser(socket);
        Task pump = browser.PumpAsync();
        await session.JoinAsync(browser, () => { lock (_all) _all.Add(browser); });
        try
        {
            byte[] buffer = new byte[1024 * 1024];
            while (true)
            {
                int length = 0;
                ValueWebSocketReceiveResult result;
                do
                {
                    if (length == buffer.Length)
                        return;
                    result = await socket.ReceiveAsync(buffer.AsMemory(length), stop);
                    length += result.Count;
                } while (!result.EndOfMessage);
                if (result.MessageType == WebSocketMessageType.Close)
                    return;
                using var message = JsonDocument.Parse(buffer.AsMemory(0, length));
                await session.HandleAsync(browser, message.RootElement);
            }
        }
        catch (Exception e) when (e is WebSocketException or OperationCanceledException or JsonException or InvalidOperationException or KeyNotFoundException)
        {
            // Closed, or sent something that makes no sense. Either way, drop the page.
        }
        finally
        {
            lock (_all) _all.Remove(browser);
            browser.Close();
            await pump.WaitAsync(TimeSpan.FromSeconds(1)).ConfigureAwait(ConfigureAwaitOptions.SuppressThrowing);
        }
    }
}
