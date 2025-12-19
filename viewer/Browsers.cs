using System.Net.WebSockets;
using System.Text.Json;

namespace ManualGC.Viewer;

/// <summary>One connected page. Sends are serialized because a WebSocket allows only one at a time.</summary>
sealed class Browser(WebSocket socket)
{
    readonly SemaphoreSlim _sending = new(1);

    public Task SendAsync(object message) =>
        SendAsync(JsonSerializer.SerializeToUtf8Bytes(message, JsonSerializerOptions.Web), WebSocketMessageType.Text);

    public Task SendAsync(byte[] frame) => SendAsync(frame, WebSocketMessageType.Binary);

    async Task SendAsync(byte[] data, WebSocketMessageType type)
    {
        await _sending.WaitAsync();
        try
        {
            if (socket.State == WebSocketState.Open)
                await socket.SendAsync(data, type, endOfMessage: true, CancellationToken.None);
        }
        catch (Exception e) when (e is WebSocketException or ObjectDisposedException or OperationCanceledException)
        {
            // The page went away; its receive loop removes it.
        }
        finally
        {
            _sending.Release();
        }
    }
}

sealed class Browsers
{
    readonly List<Browser> _all = [];

    public Task SendAsync(object message) => Task.WhenAll(Current().Select(b => b.SendAsync(message)));
    public Task SendAsync(byte[] frame) => Task.WhenAll(Current().Select(b => b.SendAsync(frame)));

    Browser[] Current()
    {
        lock (_all) return [.. _all];
    }

    public async Task RunAsync(WebSocket socket, Session session, CancellationToken stop)
    {
        var browser = new Browser(socket);
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
                        return;  // no message from the page is this large
                    result = await socket.ReceiveAsync(buffer.AsMemory(length), stop);
                    length += result.Count;
                } while (!result.EndOfMessage);
                if (result.MessageType == WebSocketMessageType.Close)
                {
                    await socket.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, null, CancellationToken.None);
                    return;
                }
                using var message = JsonDocument.Parse(buffer.AsMemory(0, length));
                await session.HandleAsync(browser, message.RootElement);
            }
        }
        catch (Exception e) when (e is WebSocketException or OperationCanceledException or JsonException or InvalidOperationException or KeyNotFoundException)
        {
        }
        finally
        {
            lock (_all) _all.Remove(browser);
        }
    }
}
