using System.Text.Json;
using Microsoft.Extensions.Options;
using ScumWarden.Server.Configuration;

namespace ScumWarden.Server.Services;

public sealed class FileBridgeExecutor
{
    private const string CommandFileName = "command.json";
    private const string ResultFileName = "result.json";
    private const string HeartbeatFileName = "heartbeat.json";
    private const int HeartbeatReadAttempts = 6;
    private const int HeartbeatReadRetryDelayMilliseconds = 25;

    private readonly IOptionsMonitor<WardenOptions> _options;
    private readonly JsonSerializerOptions _json;
    private readonly SemaphoreSlim _bridgeLock = new(1, 1);

    public FileBridgeExecutor(IOptionsMonitor<WardenOptions> options, JsonSerializerOptions json)
    {
        _options = options;
        _json = json;
    }

    public async Task<BridgeStatus> GetStatusAsync()
    {
        var options = _options.CurrentValue;
        if (options.ClientTestMode)
        {
            return new BridgeStatus(true, "client-test", DateTimeOffset.UtcNow, 0, "Client test mode: simulated bridge is online.");
        }

        var heartbeatPath = Path.Combine(options.BridgePath, HeartbeatFileName);
        string? lastError = null;
        for (var attempt = 1; attempt <= HeartbeatReadAttempts; attempt++)
        {
            if (!File.Exists(heartbeatPath))
            {
                lastError = "Bridge heartbeat not found.";
            }
            else
            {
                try
                {
                    using var doc = JsonDocument.Parse(await File.ReadAllTextAsync(heartbeatPath));
                    var root = doc.RootElement;
                    DateTimeOffset? utc = null;
                    if (root.TryGetProperty("utc", out var utcProp) &&
                        DateTimeOffset.TryParse(utcProp.GetString(), out var parsed))
                    {
                        utc = parsed;
                    }

                    var age = utc is null ? null : (double?)(DateTimeOffset.UtcNow - utc.Value).TotalSeconds;
                    var online = age is not null && age <= 15;
                    return new BridgeStatus(online, options.BridgePath, utc, age, online ? "Bridge is fresh." : "Bridge heartbeat is stale.");
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
                {
                    lastError = $"Bridge heartbeat is unreadable: {ex.Message}";
                }
            }

            if (attempt < HeartbeatReadAttempts)
            {
                await Task.Delay(HeartbeatReadRetryDelayMilliseconds);
            }
        }

        return new BridgeStatus(false, options.BridgePath, null, null, lastError ?? "Bridge heartbeat is unavailable.");
    }

    public async Task<CommandResult> ExecuteAsync(string command, object? args = null, int? timeoutMs = null)
    {
        var options = _options.CurrentValue;
        if (options.ClientTestMode)
        {
            return ClientTestSimulator.Execute(command, args, _json, options);
        }

        Directory.CreateDirectory(options.BridgePath);
        var status = await GetStatusAsync();
        if (!status.Online)
        {
            return new CommandResult(false, command, "bridge", status.Message);
        }

        var commandPath = Path.Combine(options.BridgePath, CommandFileName);
        var resultPath = Path.Combine(options.BridgePath, ResultFileName);
        var requestId = Guid.NewGuid().ToString("N");
        var effectiveTimeout = timeoutMs.GetValueOrDefault(options.BridgeTimeoutMs);

        await _bridgeLock.WaitAsync();
        try
        {
            TryDelete(resultPath);
            var request = new
            {
                id = requestId,
                command,
                args = args ?? new { },
                createdUtc = DateTimeOffset.UtcNow,
                timeoutMs = effectiveTimeout
            };

            var tempPath = commandPath + ".tmp";
            await File.WriteAllTextAsync(tempPath, JsonSerializer.Serialize(request, _json));
            File.Move(tempPath, commandPath, true);

            var started = DateTimeOffset.UtcNow;
            while ((DateTimeOffset.UtcNow - started).TotalMilliseconds < effectiveTimeout)
            {
                if (File.Exists(resultPath))
                {
                    var text = await ReadResultWhenReadyAsync(resultPath);
                    TryDelete(resultPath);
                    var result = ParseResult(command, text);
                    if (IsStaleResult(result.Payload, requestId))
                    {
                        continue;
                    }

                    return result;
                }

                await Task.Delay(15);
            }

            TryDelete(commandPath);
            return new CommandResult(false, command, "bridge", $"Bridge timeout after {effectiveTimeout} ms.");
        }
        catch (Exception ex)
        {
            return new CommandResult(false, command, "bridge", ex.Message);
        }
        finally
        {
            _bridgeLock.Release();
        }
    }

    private CommandResult ParseResult(string command, string text)
    {
        try
        {
            using var doc = JsonDocument.Parse(text);
            var root = doc.RootElement.Clone();
            var ok = root.TryGetProperty("ok", out var okProp) && okProp.ValueKind == JsonValueKind.True;
            var msg = root.TryGetProperty("message", out var messageProp) ? messageProp.GetString() ?? "" : "";
            var source = root.TryGetProperty("source", out var sourceProp) ? sourceProp.GetString() ?? "bridge" : "bridge";
            return new CommandResult(ok, command, source, string.IsNullOrWhiteSpace(msg) ? (ok ? "ok" : "failed") : msg, root);
        }
        catch (Exception ex)
        {
            return new CommandResult(false, command, "bridge", $"Bridge returned invalid JSON: {ex.Message}");
        }
    }

    private static bool IsStaleResult(JsonElement? payload, string requestId)
    {
        if (payload is not { ValueKind: JsonValueKind.Object } root)
        {
            return false;
        }

        if (!root.TryGetProperty("id", out var idProp))
        {
            return false;
        }

        var id = idProp.GetString();
        return !string.IsNullOrWhiteSpace(id) &&
               !string.Equals(id, requestId, StringComparison.Ordinal);
    }

    private static async Task<string> ReadResultWhenReadyAsync(string resultPath)
    {
        string? incompleteText = null;
        for (var i = 0; i < 20; i++)
        {
            try
            {
                var text = await File.ReadAllTextAsync(resultPath);
                if (LooksLikeCompleteJson(text))
                {
                    return text;
                }

                incompleteText = text;
            }
            catch (IOException)
            {
            }

            await Task.Delay(10);
        }

        if (incompleteText is not null)
        {
            return incompleteText;
        }

        return await File.ReadAllTextAsync(resultPath);
    }

    private static bool LooksLikeCompleteJson(string text)
    {
        var trimmed = (text ?? string.Empty).Trim();
        return trimmed.Length >= 2 &&
               ((trimmed[0] == '{' && trimmed[^1] == '}') ||
                (trimmed[0] == '[' && trimmed[^1] == ']'));
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch
        {
            // The next command will overwrite stale files when possible.
        }
    }
}
