using System.Buffers.Binary;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Options;
using ScumWarden.Server.Configuration;

namespace ScumWarden.Server.Services;

public sealed class LocalRconService
{
    private readonly IOptionsMonitor<WardenOptions> _options;
    private readonly SemaphoreSlim _lock = new(1, 1);
    private int _requestId = 1000;

    public LocalRconService(IOptionsMonitor<WardenOptions> options)
    {
        _options = options;
    }

    public bool IsEnabled() => LoadConfig().Enabled;

    public async Task<CommandResult?> ExecuteAsync(string command, int? timeoutMs = null)
    {
        var config = LoadConfig();
        if (!config.Enabled)
        {
            return null;
        }

        var cleanCommand = NormalizeCommand(command);
        if (cleanCommand.Length == 0)
        {
            return new CommandResult(false, "server_command", "server-command", "Команда пустая.");
        }

        await _lock.WaitAsync();
        try
        {
            using var cts = new CancellationTokenSource(timeoutMs ?? config.TimeoutMs);
            using var client = new TcpClient();
            await client.ConnectAsync(config.Host, config.Port, cts.Token);
            await using var stream = client.GetStream();

            var authId = NextRequestId();
            await WritePacketAsync(stream, authId, 3, config.Password, cts.Token);

            var authed = false;
            for (var i = 0; i < 4; i++)
            {
                var packet = await ReadPacketAsync(stream, cts.Token);
                if (packet.Id == -1)
                {
                    return new CommandResult(false, "server_command", "server-command", "Командный канал сервера отклонил пароль.");
                }

                if (packet.Id == authId)
                {
                    authed = true;
                    break;
                }
            }

            if (!authed)
            {
                return new CommandResult(false, "server_command", "server-command", "Командный канал сервера не подтвердил авторизацию.");
            }

            var commandId = NextRequestId();
            var markerId = NextRequestId();
            await WritePacketAsync(stream, commandId, 2, cleanCommand, cts.Token);
            await WritePacketAsync(stream, markerId, 2, "", cts.Token);

            var response = new StringBuilder();
            var markerSeen = false;
            for (var i = 0; i < 64; i++)
            {
                var packet = await ReadPacketAsync(stream, cts.Token);
                if (packet.Id == commandId && packet.Body.Length > 0)
                {
                    response.Append(packet.Body);
                }

                if (packet.Id == markerId)
                {
                    markerSeen = true;
                    break;
                }
            }

            var body = response.ToString().Trim();
            if (!markerSeen && body.Length == 0)
            {
                return new CommandResult(false, "server_command", "server-command", "Командный канал сервера не вернул ответ.");
            }

            if (LooksLikeCommandFailure(body))
            {
                var failureBody = markerSeen ? body : body + "\n[warning] Маркер окончания ответа не получен.";
                return new CommandResult(false, "server_command", "server-command", failureBody, PayloadFromCommand(cleanCommand, body));
            }

            return new CommandResult(true, "server_command", "server-command",
                body.Length == 0
                    ? "Команда принята серверным командным каналом."
                    : markerSeen ? body : body + "\n[warning] Маркер окончания ответа не получен.",
                PayloadFromCommand(cleanCommand, body));
        }
        catch (OperationCanceledException)
        {
            return new CommandResult(false, "server_command", "server-command", $"Командный канал сервера не ответил за {timeoutMs ?? config.TimeoutMs} мс.");
        }
        catch (Exception ex)
        {
            return new CommandResult(false, "server_command", "server-command", $"Командный канал сервера недоступен: {ex.Message}");
        }
        finally
        {
            _lock.Release();
        }
    }

    public async Task<CommandResult?> ExecuteItemBatchAsync(ItemBatchRequest request, IReadOnlyList<ItemGrantRequest> items)
    {
        var target = TargetRef(request.SteamId, request.Name);
        if (target.Length == 0 || !IsEnabled())
        {
            return null;
        }

        var warnings = new List<string>();
        foreach (var item in items)
        {
            var itemId = CleanToken(item.ItemId);
            var quantity = Math.Clamp(item.Quantity, 1, 100);
            if (itemId.Length == 0)
            {
                warnings.Add("Skipped empty item id.");
                continue;
            }

            var result = await ExecuteAsync($"SpawnItem {itemId} {quantity} Location {target}");
            if (result is null)
            {
                return null;
            }

            if (!result.Ok)
            {
                return result with { Command = "deliver_items_batch", Warnings = warnings };
            }

            if (!string.IsNullOrWhiteSpace(result.Message))
            {
                warnings.Add($"{itemId}: {result.Message}");
            }
        }

        return new CommandResult(true, "deliver_items_batch", "server-command",
            $"Пакет предметов отправлен через командный канал сервера: {items.Count}.", Warnings: warnings);
    }

    public Task<CommandResult?> ExecuteVehicleSpawnAsync(VehicleSpawnRequest request)
    {
        var target = TargetRef(request.SteamId, request.Name);
        if (target.Length == 0 || !IsEnabled())
        {
            return Task.FromResult<CommandResult?>(null);
        }

        var vehicleId = CleanToken(VehicleCatalog.NormalizeVehicleId(request.VehicleId));
        return vehicleId.Length == 0
            ? Task.FromResult<CommandResult?>(new CommandResult(false, "spawn_vehicle", "server-command", "ID транспорта не указан."))
            : ExecuteAsync($"SpawnVehicle {vehicleId} 1 Location {target}");
    }

    public Task<CommandResult?> ExecuteDestroyVehicleAsync(string vehicleRef)
    {
        var clean = CleanToken(vehicleRef);
        return clean.Length == 0
            ? Task.FromResult<CommandResult?>(new CommandResult(false, "destroy_vehicle_ref", "server-command", "ID транспорта не указан."))
            : ExecuteAsync($"DestroyVehicle {clean}");
    }

    public Task<CommandResult?> ExecuteMoneyAsync(MoneyRequest request)
    {
        var target = TargetRef(request.SteamId, request.Name);
        if (target.Length == 0 || !IsEnabled())
        {
            return Task.FromResult<CommandResult?>(null);
        }

        var currency = CleanToken(string.IsNullOrWhiteSpace(request.Currency) ? "Normal" : request.Currency);
        return ExecuteAsync($"ChangeCurrencyBalance {currency} {request.Amount} {target}");
    }

    public Task<CommandResult?> ExecuteFameAsync(MoneyRequest request)
    {
        var target = TargetRef(request.SteamId, request.Name);
        return target.Length == 0 || !IsEnabled()
            ? Task.FromResult<CommandResult?>(null)
            : ExecuteAsync($"ChangeFamePoints {request.Amount} {target}");
    }

    private static bool LooksLikeCommandFailure(string body)
    {
        if (string.IsNullOrWhiteSpace(body))
        {
            return false;
        }

        var text = body.ToLowerInvariant();
        var needles = new[]
        {
            "not authorized",
            "not authorised",
            "unauthorized",
            "unauthorised",
            "unknown command",
            "not recognized",
            "unrecognized command",
            "command failed",
            "cannot spawn",
            "can't spawn",
            "could not spawn",
            "failed to spawn",
            "cannot schedule",
            "can't schedule",
            "could not schedule",
            "failed to schedule",
            "cannot execute",
            "can't execute",
            "could not execute",
            "unable to",
            "not allowed",
            "no permission",
            "permission denied",
            "insufficient permission",
            "cooldown",
            "cool down",
            "on cooldown",
            "invalid location",
            "invalid target",
            "invalid argument",
            "invalid command",
            "fatal error"
        };

        foreach (var needle in needles)
        {
            if (text.Contains(needle, StringComparison.Ordinal))
            {
                return true;
            }
        }

        return false;
    }

    private LocalRconConfig LoadConfig()
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var path in CandidateIniPaths())
        {
            if (!File.Exists(path))
            {
                continue;
            }

            foreach (var line in File.ReadLines(path))
            {
                var trimmed = line.Trim();
                if (trimmed.Length == 0 || trimmed.StartsWith(';') || trimmed.StartsWith('#'))
                {
                    continue;
                }

                var index = trimmed.IndexOf('=');
                if (index <= 0)
                {
                    continue;
                }

                values[trimmed[..index].Trim()] = trimmed[(index + 1)..].Trim();
            }

            break;
        }

        var enabled = Bool(values, "server_command_enabled", Bool(values, "local_rcon_enabled", false));
        var host = Text(values, "server_command_host", Text(values, "local_rcon_host", "127.0.0.1"));
        var port = Int(values, "server_command_port", Int(values, "local_rcon_port", 28015));
        var password = Text(values, "server_command_password", Text(values, "local_rcon_password", ""));
        var timeout = Math.Clamp(Int(values, "server_command_timeout_ms", Int(values, "local_rcon_timeout_ms", 8000)), 1000, 60000);
        return new LocalRconConfig(enabled && password.Length > 0, host, port, password, timeout);
    }

    private IEnumerable<string> CandidateIniPaths()
    {
        var options = _options.CurrentValue;
        if (!string.IsNullOrWhiteSpace(options.ServerRoot))
        {
            yield return Path.Combine(options.ServerRoot, "ScumNeDjin", "nedjin.ini");
        }

        if (!string.IsNullOrWhiteSpace(options.BridgePath))
        {
            yield return Path.Combine(options.BridgePath, "nedjin.ini");
            var parent = Directory.GetParent(options.BridgePath);
            if (parent is not null)
            {
                yield return Path.Combine(parent.FullName, "nedjin.ini");
                yield return Path.Combine(parent.FullName, "ScumNeDjin", "nedjin.ini");
                var grandParent = parent.Parent;
                if (grandParent is not null)
                {
                    yield return Path.Combine(grandParent.FullName, "ScumNeDjin", "nedjin.ini");
                }
            }
        }

        yield return Path.Combine(Directory.GetCurrentDirectory(), "ScumNeDjin", "nedjin.ini");
        yield return Path.Combine(AppContext.BaseDirectory, "ScumNeDjin", "nedjin.ini");
    }

    private int NextRequestId() => Interlocked.Increment(ref _requestId);

    private static async Task WritePacketAsync(NetworkStream stream, int id, int type, string body, CancellationToken cancellationToken)
    {
        var bodyBytes = Encoding.UTF8.GetBytes(body);
        var size = 4 + 4 + bodyBytes.Length + 2;
        var buffer = new byte[4 + size];
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(0, 4), size);
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(4, 4), id);
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(8, 4), type);
        bodyBytes.CopyTo(buffer.AsSpan(12));
        await stream.WriteAsync(buffer, cancellationToken);
    }

    private static async Task<RconPacket> ReadPacketAsync(NetworkStream stream, CancellationToken cancellationToken)
    {
        var sizeBuffer = await ReadExactAsync(stream, 4, cancellationToken);
        var size = BinaryPrimitives.ReadInt32LittleEndian(sizeBuffer);
        if (size < 10 || size > 1024 * 1024)
        {
            throw new InvalidDataException($"Некорректный размер пакета командного канала: {size}.");
        }

        var payload = await ReadExactAsync(stream, size, cancellationToken);
        var id = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(0, 4));
        var type = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(4, 4));
        var bodyLength = Math.Max(0, size - 10);
        var body = bodyLength == 0 ? "" : Encoding.UTF8.GetString(payload, 8, bodyLength);
        return new RconPacket(id, type, body);
    }

    private static async Task<byte[]> ReadExactAsync(NetworkStream stream, int length, CancellationToken cancellationToken)
    {
        var buffer = new byte[length];
        var offset = 0;
        while (offset < length)
        {
            var read = await stream.ReadAsync(buffer.AsMemory(offset, length - offset), cancellationToken);
            if (read <= 0)
            {
                throw new IOException("Соединение командного канала закрыто.");
            }

            offset += read;
        }

        return buffer;
    }

    private static JsonElement PayloadFromCommand(string command, string response)
    {
        using var doc = JsonDocument.Parse(JsonSerializer.Serialize(new
        {
            command,
            response
        }, WardenJson.Options));
        return doc.RootElement.Clone();
    }

    private static string NormalizeCommand(string command)
    {
        var trimmed = (command ?? "").Trim();
        return trimmed.StartsWith('#') ? trimmed[1..].Trim() : trimmed;
    }

    private static string TargetRef(string? steamId, string? name)
    {
        var steam = (steamId ?? "").Trim();
        if (steam.Length >= 15 && steam.All(char.IsDigit))
        {
            return steam;
        }
        return "";
    }

    private static string CleanToken(string? value)
    {
        var text = (value ?? "").Trim();
        return text.Replace("|", "", StringComparison.Ordinal)
            .Replace(";", "", StringComparison.Ordinal)
            .Replace("\"", "", StringComparison.Ordinal)
            .Replace("\r", "", StringComparison.Ordinal)
            .Replace("\n", "", StringComparison.Ordinal);
    }

    private static string Text(IReadOnlyDictionary<string, string> values, string key, string fallback) =>
        values.TryGetValue(key, out var value) && !string.IsNullOrWhiteSpace(value) ? value.Trim() : fallback;

    private static int Int(IReadOnlyDictionary<string, string> values, string key, int fallback) =>
        values.TryGetValue(key, out var value) && int.TryParse(value, out var parsed) ? parsed : fallback;

    private static bool Bool(IReadOnlyDictionary<string, string> values, string key, bool fallback)
    {
        if (!values.TryGetValue(key, out var value))
        {
            return fallback;
        }

        return value.Trim().ToLowerInvariant() switch
        {
            "1" or "true" or "yes" or "on" => true,
            "0" or "false" or "no" or "off" => false,
            _ => fallback
        };
    }

    private sealed record LocalRconConfig(bool Enabled, string Host, int Port, string Password, int TimeoutMs);
    private sealed record RconPacket(int Id, int Type, string Body);
}
