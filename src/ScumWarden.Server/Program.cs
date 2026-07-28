using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.IO.Compression;
using Microsoft.Extensions.Options;
using ScumWarden.Server.Configuration;
using ScumWarden.Server.Services;

var builder = WebApplication.CreateBuilder(args);

builder.Services.Configure<WardenOptions>(builder.Configuration.GetSection("Warden"));
builder.Services.AddSingleton<JsonSerializerOptions>(_ => WardenJson.Options);
builder.Services.AddSingleton<WardenEventStore>();
builder.Services.AddSingleton<FileBridgeExecutor>();
builder.Services.AddSingleton<LocalRconService>();
builder.Services.AddSingleton<ScumLogService>();
builder.Services.AddSingleton<ScumDatabaseReader>();
builder.Services.AddSingleton<PluginConfigStore>();
builder.Services.AddSingleton<ServerConfigService>();
builder.Services.AddSingleton<ArkPanelControlService>();
builder.Services.AddSingleton<CommandRouter>();
builder.Services.AddSingleton<DiscordLogService>();
builder.Services.AddSingleton<WargmDeliveryService>();
builder.Services.AddSingleton<GameStoresDeliveryService>();
builder.Services.AddHttpClient();
builder.Services.AddHttpClient<WargmShopService>();
builder.Services.AddHttpClient<GameStoresShopService>();
builder.Services.AddHostedService(provider => provider.GetRequiredService<DiscordLogService>());
builder.Services.AddHostedService<LogWatcherService>();
builder.Services.AddHostedService<GameStoresRequestWorker>();

builder.Services.AddCors(options =>
{
    options.AddDefaultPolicy(policy => policy
        .AllowAnyOrigin()
        .AllowAnyMethod()
        .AllowAnyHeader());
});

var app = builder.Build();

var startupOptions = app.Services.GetRequiredService<IOptions<WardenOptions>>().Value;
if (startupOptions.ClientTestMode)
{
    ClientTestSimulator.SeedEvents(app.Services.GetRequiredService<WardenEventStore>(), startupOptions);
}

app.UseCors();
app.UseDefaultFiles();
app.UseStaticFiles();

app.Use(async (context, next) =>
{
    if (context.Request.Path.StartsWithSegments("/api") &&
        !context.Request.Path.StartsWithSegments("/api/health"))
    {
        var options = context.RequestServices.GetRequiredService<IOptions<WardenOptions>>().Value;
        if (!string.IsNullOrWhiteSpace(options.ApiKey))
        {
            var supplied = context.Request.Headers["X-API-KEY"].FirstOrDefault();
            if (!string.Equals(supplied, options.ApiKey, StringComparison.Ordinal))
            {
                context.Response.StatusCode = StatusCodes.Status401Unauthorized;
                await context.Response.WriteAsJsonAsync(ApiEnvelope.Fail("API key is missing or invalid."), WardenJson.Options);
                return;
            }
        }
    }

    await next();
});

static object LoadWebJsonAsset(IWebHostEnvironment env, string fileName, object fallback)
{
    var path = Path.Combine(env.WebRootPath ?? string.Empty, fileName);
    if (!File.Exists(path))
    {
        return fallback;
    }

    var text = File.ReadAllText(path);
    if (string.IsNullOrWhiteSpace(text))
    {
        return fallback;
    }

    return JsonSerializer.Deserialize<object>(text, WardenJson.Options) ?? fallback;
}

static string PlayerString(JsonElement player, string key)
{
    return player.TryGetProperty(key, out var value) ? value.GetString() ?? "" : "";
}

static int PlayerInt(JsonElement player, int fallback, params string[] keys)
{
    foreach (var key in keys)
    {
        if (player.ValueKind != JsonValueKind.Object ||
            !player.TryGetProperty(key, out var value))
        {
            continue;
        }

        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number))
        {
            return number;
        }

        if (value.ValueKind == JsonValueKind.String &&
            int.TryParse(value.GetString(), out var textNumber))
        {
            return textNumber;
        }
    }

    return fallback;
}

static JsonElement ConfigArray(JsonElement config, params string[] keys)
{
    foreach (var key in keys)
    {
        if (config.ValueKind == JsonValueKind.Object &&
            config.TryGetProperty(key, out var value) &&
            value.ValueKind == JsonValueKind.Array)
        {
            return value.Clone();
        }
    }

    using var doc = JsonDocument.Parse("[]");
    return doc.RootElement.Clone();
}

static bool JsonArrayHasItems(JsonElement element) =>
    element.ValueKind == JsonValueKind.Array && element.GetArrayLength() > 0;

static object LimitJsonArray(JsonElement element, int limit, bool dedupeChat = false)
{
    if (element.ValueKind != JsonValueKind.Array)
    {
        return Array.Empty<object>();
    }

    var safeLimit = Math.Clamp(limit, 1, 5000);
    var rows = element.EnumerateArray().ToArray();
    var selectedRows = dedupeChat
        ? DedupChatRows(rows, safeLimit)
        : rows.Skip(Math.Max(0, rows.Length - safeLimit));
    return selectedRows
        .Select(row => JsonSerializer.Deserialize<object>(row.GetRawText(), WardenJson.Options) ?? new { })
        .ToArray();
}

static IEnumerable<JsonElement> DedupChatRows(JsonElement[] rows, int safeLimit)
{
    var selected = new List<JsonElement>();
    var timed = new Dictionary<string, List<DateTimeOffset>>(StringComparer.OrdinalIgnoreCase);
    var untimed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    for (var i = rows.Length - 1; i >= 0 && selected.Count < safeLimit; i--)
    {
        var row = rows[i];
        var key = ChatRowDedupeIdentity(row);
        if (key.Length == 0)
        {
            selected.Add(row);
            continue;
        }

        if (TryChatRowTime(row, out var utc))
        {
            if (!timed.TryGetValue(key, out var times))
            {
                times = new List<DateTimeOffset>();
                timed[key] = times;
            }

            if (times.Any(existing => Math.Abs((existing - utc).TotalSeconds) <= 15))
            {
                continue;
            }

            times.Add(utc);
        }
        else if (!untimed.Add(key))
        {
            continue;
        }

        selected.Add(row);
    }

    selected.Reverse();
    return selected;
}

static string ChatRowDedupeIdentity(JsonElement row)
{
    var message = NormalizeChatDedupeText(JsonDedupeText(row, "message", "Message"));
    if (message.Length == 0)
    {
        return "";
    }

    var channel = NormalizeChatDedupeChannel(JsonDedupeText(row, "channel", "Channel"));
    var steam = NormalizeChatDedupeText(JsonDedupeText(row, "steamId", "SteamId", "steam", "SteamID"));
    var name = NormalizeChatDedupeText(JsonDedupeText(row, "name", "Name", "playerName", "PlayerName"));
    return string.Join('|', channel, steam.Length > 0 ? steam : name, message);
}

static string NormalizeChatDedupeChannel(string value)
{
    var channel = NormalizeChatDedupeText(value);
    return channel switch
    {
        "0" => "local",
        "1" => "squad",
        "2" => "global",
        "3" => "admin",
        "4" => "command",
        "" or "chat" => "global",
        _ when channel.Contains("local", StringComparison.Ordinal) ||
               channel.Contains("proximity", StringComparison.Ordinal) ||
               channel.Contains("vicinity", StringComparison.Ordinal) ||
               channel.Contains("лок", StringComparison.Ordinal) => "local",
        _ when channel.Contains("squad", StringComparison.Ordinal) ||
               channel.Contains("team", StringComparison.Ordinal) ||
               channel.Contains("clan", StringComparison.Ordinal) ||
               channel.Contains("party", StringComparison.Ordinal) ||
               channel.Contains("отряд", StringComparison.Ordinal) => "squad",
        _ when channel.Contains("admin", StringComparison.Ordinal) ||
               channel.Contains("moderator", StringComparison.Ordinal) ||
               channel.Contains("админ", StringComparison.Ordinal) => "admin",
        _ when channel.Contains("command", StringComparison.Ordinal) => "command",
        _ when channel.Contains("global", StringComparison.Ordinal) ||
               channel.Contains("world", StringComparison.Ordinal) ||
               channel.Contains("глоб", StringComparison.Ordinal) => "global",
        _ => channel
    };
}

static string NormalizeChatDedupeText(string value) =>
    string.Join(' ', (value ?? "").Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        .ToLowerInvariant();

static bool TryChatRowTime(JsonElement row, out DateTimeOffset utc)
{
    foreach (var key in new[] { "timestampUtc", "utc", "createdAtUtc" })
    {
        var text = JsonDedupeText(row, key);
        if (DateTimeOffset.TryParse(text, out utc))
        {
            return true;
        }
    }

    utc = default;
    return false;
}

static string JsonDedupeText(JsonElement row, params string[] keys)
{
    if (row.ValueKind != JsonValueKind.Object)
    {
        return "";
    }

    foreach (var property in row.EnumerateObject())
    {
        if (!keys.Any(key => property.Name.Equals(key, StringComparison.OrdinalIgnoreCase)))
        {
            continue;
        }

        return property.Value.ValueKind switch
        {
            JsonValueKind.String => property.Value.GetString() ?? "",
            JsonValueKind.Number => property.Value.ToString(),
            JsonValueKind.True => "true",
            JsonValueKind.False => "false",
            _ => ""
        };
    }

    return "";
}

static bool WantsLiveBridge(string? refresh, string? live, string? mode)
{
    static bool IsYes(string? value) =>
        string.Equals(value, "1", StringComparison.OrdinalIgnoreCase) ||
        string.Equals(value, "true", StringComparison.OrdinalIgnoreCase) ||
        string.Equals(value, "yes", StringComparison.OrdinalIgnoreCase);

    static bool IsNo(string? value) =>
        string.Equals(value, "0", StringComparison.OrdinalIgnoreCase) ||
        string.Equals(value, "false", StringComparison.OrdinalIgnoreCase) ||
        string.Equals(value, "no", StringComparison.OrdinalIgnoreCase);

    if (string.Equals(mode, "log", StringComparison.OrdinalIgnoreCase) || IsNo(live))
    {
        return false;
    }

    return IsYes(refresh) ||
        IsYes(live) ||
        string.Equals(mode, "live", StringComparison.OrdinalIgnoreCase);
}

static object PlayersArrayFromResponse(object response)
{
    var json = JsonSerializer.SerializeToElement(response, WardenJson.Options);
    if (json.ValueKind == JsonValueKind.Object &&
        json.TryGetProperty("payload", out var payload) &&
        payload.ValueKind == JsonValueKind.Object &&
        payload.TryGetProperty("players", out var players))
    {
        return JsonSerializer.Deserialize<object>(players.GetRawText(), WardenJson.Options) ?? Array.Empty<object>();
    }

    return Array.Empty<object>();
}

static ApiEnvelope PlayersApiEnvelope(CommandResult result)
{
    var payload = result.Payload is { ValueKind: JsonValueKind.Object } element
        ? element
        : JsonDocument.Parse("""{"players":[],"count":0}""").RootElement.Clone();
    return ApiEnvelope.Ok(NormalizedPlayersApiPayload(
        payload,
        result.Command,
        result.Source,
        result.Message,
        result.Warnings));
}

static ApiEnvelope PlayersLogApiEnvelope(object logResponse)
{
    var json = JsonSerializer.SerializeToElement(logResponse, WardenJson.Options);
    var command = PlayersApiJsonString(json, "command", "players_snapshot");
    var source = PlayersApiJsonString(json, "source", "server-log");
    var message = PlayersApiJsonString(json, "message", "Список игроков восстановлен по SCUM.log; live runtime bridge не запрошен.");
    var payload = json.ValueKind == JsonValueKind.Object &&
        json.TryGetProperty("payload", out var payloadElement) &&
        payloadElement.ValueKind == JsonValueKind.Object
            ? payloadElement
            : JsonDocument.Parse("""{"players":[],"count":0}""").RootElement.Clone();
    return ApiEnvelope.Ok(NormalizedPlayersApiPayload(payload, command, source, message, Array.Empty<string>()));
}

static object NormalizedPlayersApiPayload(
    JsonElement payload,
    string command,
    string source,
    string message,
    IReadOnlyList<string>? warnings)
{
    var players = PlayersApiJsonPlayersArray(payload);
    var count = players.Length;
    return new
    {
        ok = true,
        command,
        source,
        message,
        players,
        count,
        runtimeCount = PlayersApiJsonInt(payload, "runtimeCount", count),
        logPlayerCount = PlayersApiJsonInt(payload, "logPlayerCount", count),
        identityOverlay = PlayersApiJsonObjectOrEmpty(payload, "identityOverlay"),
        staleRuntimeIgnored = PlayersApiJsonBool(payload, "staleRuntimeIgnored", false),
        runtimeReady = PlayersApiJsonBool(payload, "runtimeReady", false),
        bridgeOnline = PlayersApiJsonBool(payload, "bridgeOnline", true),
        payload = PlayersApiJsonObjectOrEmpty(payload),
        warnings = warnings ?? Array.Empty<string>(),
        completedAtUtc = DateTimeOffset.UtcNow
    };
}

static object[] PlayersApiJsonPlayersArray(JsonElement payload)
{
    if (payload.ValueKind != JsonValueKind.Object ||
        !payload.TryGetProperty("players", out var players) ||
        players.ValueKind != JsonValueKind.Array)
    {
        return Array.Empty<object>();
    }

    return players.EnumerateArray()
        .Select(row => JsonSerializer.Deserialize<object>(row.GetRawText(), WardenJson.Options) ?? new { })
        .ToArray();
}

static string PlayersApiJsonString(JsonElement element, string propertyName, string fallback)
{
    if (element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(propertyName, out var value) &&
        value.ValueKind == JsonValueKind.String)
    {
        return value.GetString() ?? fallback;
    }

    return fallback;
}

static int PlayersApiJsonInt(JsonElement element, string propertyName, int fallback)
{
    if (element.ValueKind != JsonValueKind.Object ||
        !element.TryGetProperty(propertyName, out var value))
    {
        return fallback;
    }

    if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number))
    {
        return number;
    }

    return value.ValueKind == JsonValueKind.String && int.TryParse(value.GetString(), out var textNumber)
        ? textNumber
        : fallback;
}

static bool PlayersApiJsonBool(JsonElement element, string propertyName, bool fallback)
{
    if (element.ValueKind != JsonValueKind.Object ||
        !element.TryGetProperty(propertyName, out var value))
    {
        return fallback;
    }

    return value.ValueKind switch
    {
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        JsonValueKind.String when bool.TryParse(value.GetString(), out var parsed) => parsed,
        _ => fallback
    };
}

static object PlayersApiJsonObjectOrEmpty(JsonElement element, string? propertyName = null)
{
    var value = element;
    if (!string.IsNullOrWhiteSpace(propertyName))
    {
        if (element.ValueKind != JsonValueKind.Object ||
            !element.TryGetProperty(propertyName, out value))
        {
            return new { };
        }
    }

    return value.ValueKind is JsonValueKind.Object or JsonValueKind.Array
        ? JsonSerializer.Deserialize<object>(value.GetRawText(), WardenJson.Options) ?? new { }
        : new { };
}

static object StateOrEvents(PluginConfigStore store, WardenEventStore events, string stateKey, string eventType, int limit)
{
    var state = store.GetState(stateKey);
    return JsonArrayHasItems(state)
        ? LimitJsonArray(state, limit, stateKey.Equals("chat", StringComparison.OrdinalIgnoreCase))
        : events.Get(eventType, limit);
}

static object StateWithEvents(PluginConfigStore store, WardenEventStore events, string stateKey, IReadOnlyList<string> eventTypes, int limit)
{
    var safeLimit = Math.Clamp(limit, 1, 1000);
    var rows = new List<object?>();
    var state = store.GetState(stateKey);
    if (state.ValueKind == JsonValueKind.Array)
    {
        rows.AddRange(state.EnumerateArray()
            .Select(item => SanitizeUiJson(item)));
    }

    foreach (var eventType in eventTypes)
    {
        rows.AddRange(events.Get(eventType, safeLimit)
            .Select(item => SanitizeUiObject(item)));
    }

    return rows
        .Where(row => row is not null)
        .OrderByDescending(UiRowTimestamp)
        .Take(safeLimit)
        .ToArray();
}

static object EventsByRoute(PluginConfigStore store, WardenEventStore events, string? route, int? limit)
{
    var safeLimit = Math.Clamp(limit ?? 250, 1, 1000);
    var key = (route ?? "").Trim().ToLowerInvariant();
    return key switch
    {
        "chat" or "global" or "local" or "squad" or "admin" => StateOrEvents(store, events, "chat", "chat", safeLimit),
        "kill" or "kills" or "combat" or "death" => StateOrEvents(store, events, "kills", "kill", safeLimit),
        "economy" or "money" or "trader" => StateOrEvents(store, events, "economy", "economy", safeLimit),
        "discord" => LimitJsonArray(store.GetState("discord-log"), safeLimit),
        "wargm" or "shop" => LimitJsonArray(store.GetState("wargm-shop"), safeLimit),
        _ => StateWithEvents(store, events, "action-log", new[] { "action", "command" }, safeLimit)
    };
}

static IReadOnlyList<(string State, string Event)> LogClearTargets(LogClearRequest request)
{
    var rawTargets = new List<string>();
    if (!string.IsNullOrWhiteSpace(request.Target))
    {
        rawTargets.Add(request.Target);
    }

    if (request.Targets is not null)
    {
        rawTargets.AddRange(request.Targets.Where(target => !string.IsNullOrWhiteSpace(target)));
    }

    if (rawTargets.Count == 0)
    {
        rawTargets.Add("chat");
    }

    var targets = new List<(string State, string Event)>();
    foreach (var raw in rawTargets.Select(target => target.Trim().ToLowerInvariant()))
    {
        switch (raw)
        {
            case "all":
            case "logs":
                targets.Add(("chat", "chat"));
                targets.Add(("kills", "kill"));
                targets.Add(("action-log", "action"));
                targets.Add(("action-log", "command"));
                break;
            case "chat":
            case "global":
            case "local":
            case "squad":
            case "admin":
            case "command-chat":
                targets.Add(("chat", "chat"));
                break;
            case "kills":
            case "kill":
            case "killfeed":
            case "combat":
                targets.Add(("kills", "kill"));
                break;
            case "actions":
            case "action":
            case "nedjin":
            case "command":
            case "commands":
            case "action-log":
                targets.Add(("action-log", "action"));
                targets.Add(("action-log", "command"));
                break;
        }
    }

    return targets
        .DistinctBy(target => target.State + "|" + target.Event)
        .ToArray();
}

static object? SanitizeUiObject(object? value) =>
    value is null
        ? null
        : SanitizeUiJson(JsonSerializer.SerializeToElement(value, WardenJson.Options));

static object? SanitizeUiJson(JsonElement element, int depth = 0, string propertyName = "")
{
    const int maxString = 1200;
    const int maxArrayItems = 80;
    const int maxDepth = 5;

    if (depth > maxDepth)
    {
        return "...";
    }

    return element.ValueKind switch
    {
        JsonValueKind.Object => element.EnumerateObject().ToDictionary(
            prop => prop.Name,
            prop => SanitizeUiJson(prop.Value, depth + 1, prop.Name),
            StringComparer.OrdinalIgnoreCase),
        JsonValueKind.Array => element.EnumerateArray()
            .Take(maxArrayItems)
            .Select(item => SanitizeUiJson(item, depth + 1, propertyName))
            .ToArray(),
        JsonValueKind.String => TrimUiString(element.GetString(), propertyName, maxString),
        JsonValueKind.Number => JsonSerializer.Deserialize<object>(element.GetRawText(), WardenJson.Options),
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        _ => null
    };
}

static string TrimUiString(string? value, string propertyName, int maxString)
{
    var text = value ?? "";
    var limit = propertyName.Equals("attempts", StringComparison.OrdinalIgnoreCase) ||
        propertyName.Equals("raw", StringComparison.OrdinalIgnoreCase) ||
        propertyName.Equals("stack", StringComparison.OrdinalIgnoreCase)
            ? 600
            : maxString;

    return text.Length <= limit
        ? text
        : text[..limit] + $"... [truncated {text.Length - limit} chars]";
}

static DateTimeOffset UiRowTimestamp(object? row)
{
    if (row is null)
    {
        return DateTimeOffset.MinValue;
    }

    var element = JsonSerializer.SerializeToElement(row, WardenJson.Options);
    if (element.ValueKind != JsonValueKind.Object)
    {
        return DateTimeOffset.MinValue;
    }

    foreach (var key in new[] { "timestampUtc", "utc", "time", "at", "startedAt", "createdAt" })
    {
        if (element.TryGetProperty(key, out var value))
        {
            if (value.ValueKind == JsonValueKind.String &&
                DateTimeOffset.TryParse(value.GetString(), out var parsed))
            {
                return parsed;
            }

            if (value.ValueKind == JsonValueKind.Number &&
                value.TryGetInt64(out var unixSeconds))
            {
                return DateTimeOffset.FromUnixTimeSeconds(unixSeconds);
            }
        }
    }

    return DateTimeOffset.MinValue;
}

static object[] BuildDownloadList(IWebHostEnvironment env)
{
    var contentRoot = env.ContentRootPath;
    var projectRoot = Directory.GetParent(contentRoot)?.Parent?.FullName ?? contentRoot;
    var binRoot = AppContext.BaseDirectory;
    var candidates = new (string Name, string Path, string Description)[]
    {
        ("README", Path.Combine(projectRoot, "README.md"), "Краткая инструкция по установке и запуску."),
        ("Сборка пакета для хостинга", Path.Combine(projectRoot, "tools", "Build-HostPackage.ps1"), "Скрипт сборки переносимого пакета."),
        ("Чистая локальная установка", Path.Combine(projectRoot, "tools", "Install-CleanLocal.ps1"), "Скрипт очистки старых файлов и локальной установки."),
        ("UE4SS Lua bridge", Path.Combine(projectRoot, "ue4ss", "Mods", "ScumNeDjinBridge", "scripts", "main.lua"), "Игровой мост ScumNeDjin."),
        ("Панель управления", Path.Combine(binRoot, "ScumWarden.Server.exe"), "Запускаемый файл веб-панели."),
        ("Каталог предметов", Path.Combine(contentRoot, "wwwroot", "spawnable-items.json"), "Каталог предметов для панели и выдачи."),
        ("Каталог транспорта", Path.Combine(contentRoot, "wwwroot", "spawnable-vehicles.json"), "Каталог транспорта для панели."),
        ("Иконки предметов", Path.Combine(contentRoot, "wwwroot", "item-icons-manifest.json"), "Манифест иконок предметов."),
        ("Нативный загрузчик", Path.Combine(projectRoot, "build", "native", "out", "version.dll"), "Нативная часть для SCUM Win64, если она собрана.")
    };

    return candidates
        .Where(item => File.Exists(item.Path))
        .Select(item =>
        {
            var file = new FileInfo(item.Path);
            return new
            {
                name = item.Name,
                path = item.Path,
                description = item.Description,
                sizeBytes = file.Length,
                modifiedUtc = file.LastWriteTimeUtc
            };
        })
        .Cast<object>()
        .ToArray();
}

static bool TryResolveFastTravel(JsonElement config, string? alias, out double x, out double y, out double z, out string displayName)
{
    x = y = z = 0;
    displayName = "";
    var cleanAlias = (alias ?? "").Trim();
    if (cleanAlias.Length == 0)
    {
        return false;
    }

    foreach (var route in ConfigArray(config, "Outposts", "outposts").EnumerateArray())
    {
        var routeAlias = PlayerString(route, "CommandAlias");
        if (routeAlias.Length == 0)
        {
            routeAlias = PlayerString(route, "commandAlias");
        }

        if (!string.Equals(routeAlias, cleanAlias, StringComparison.OrdinalIgnoreCase))
        {
            continue;
        }

        displayName = PlayerString(route, "DisplayName");
        if (displayName.Length == 0)
        {
            displayName = PlayerString(route, "displayName");
        }

        var point = route.TryGetProperty("ArrivalPoint", out var upper) ? upper :
            route.TryGetProperty("arrivalPoint", out var lower) ? lower : default;
        if (point.ValueKind == JsonValueKind.Array && point.GetArrayLength() >= 3)
        {
            x = point[0].GetDouble();
            y = point[1].GetDouble();
            z = point[2].GetDouble();
            return true;
        }
    }

    return false;
}

static string PluginCommandText(string? value) =>
    (value ?? string.Empty).Trim()
        .Replace("\r", " ", StringComparison.Ordinal)
        .Replace("\n", " ", StringComparison.Ordinal)
        .Replace("\t", " ", StringComparison.Ordinal);

static string[] PlayerCommandAliasFieldsFor(string key) => key.ToLowerInvariant() switch
{
    "welcomepack" => new[] { "WelcomePack", "Starter", "welcomePack", "starter" },
    "sethome" => new[] { "SetHome", "setHome", "sethome" },
    "home" => new[] { "Home", "home" },
    "travel" => new[] { "Travel", "FastTravel", "travel", "fastTravel" },
    "rent" => new[] { "Rent", "Rental", "VehicleRental", "rent", "rental", "vehicleRental" },
    "scan" => new[] { "SectorScan", "Scan", "sectorScan", "scan" },
    _ => Array.Empty<string>()
};

static string NormalizePlayerCommandAlias(string? value)
{
    var text = PluginCommandText(value).ToLowerInvariant();
    if (text.Length == 0)
    {
        return "";
    }

    text = Regex.Replace(text, "\\s+", "");
    if (text.Length == 0)
    {
        return "";
    }

    if (text[0] == '/' || text[0] == '!')
    {
        var body = text[1..].TrimStart('/', '!');
        return body.Length == 0 ? "" : text[0] + body;
    }

    return "/" + text.TrimStart('/', '!');
}

static string PlayerCommandAlias(PluginConfigStore store, string key, string fallback)
{
    var config = store.GetConfig("command-aliases");
    if (config.ValueKind != JsonValueKind.Object)
    {
        return fallback;
    }

    if (config.TryGetProperty("Enabled", out var enabled) && enabled.ValueKind == JsonValueKind.False)
    {
        return fallback;
    }

    var fields = PlayerCommandAliasFieldsFor(key);
    if (fields.Length == 0)
    {
        return fallback;
    }

    foreach (var field in fields)
    {
        if (!config.TryGetProperty(field, out var value))
        {
            continue;
        }

        if (value.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in value.EnumerateArray())
            {
                var alias = item.ValueKind == JsonValueKind.String ? NormalizePlayerCommandAlias(item.GetString()) : "";
                if (alias.Length > 0)
                {
                    return alias;
                }
            }
        }
        else if (value.ValueKind == JsonValueKind.String)
        {
            foreach (var token in Regex.Split(value.GetString() ?? "", "[,;\\s]+"))
            {
                var alias = NormalizePlayerCommandAlias(token);
                if (alias.Length > 0)
                {
                    return alias;
                }
            }
        }
    }

    return fallback;
}

static string PlayerCommand(PluginConfigStore store, string key, string fallback, string suffix = "") =>
    PlayerCommandAlias(store, key, fallback) + suffix;

static JsonElement ToJsonElement(object value) =>
    JsonSerializer.SerializeToElement(value, WardenJson.Options);

static string JsonString(JsonElement element, params string[] keys)
{
    if (element.ValueKind != JsonValueKind.Object)
    {
        return "";
    }

    foreach (var key in keys)
    {
        if (element.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String)
        {
            return value.GetString() ?? "";
        }
    }

    return "";
}

static JsonElement JsonObjectOrDefault(JsonElement element, params string[] keys)
{
    if (element.ValueKind == JsonValueKind.Object)
    {
        foreach (var key in keys)
        {
            if (element.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.Object)
            {
                return value.Clone();
            }
        }
    }

    return element.Clone();
}

static object InferSchema(JsonElement element)
{
    return element.ValueKind switch
    {
        JsonValueKind.Object => element.EnumerateObject().ToDictionary(
            prop => prop.Name,
            prop => new
            {
                type = prop.Value.ValueKind.ToString().ToLowerInvariant(),
                value = JsonSerializer.Deserialize<object>(prop.Value.GetRawText(), WardenJson.Options),
                children = prop.Value.ValueKind is JsonValueKind.Object or JsonValueKind.Array ? InferSchema(prop.Value) : null
            }),
        JsonValueKind.Array => element.GetArrayLength() == 0
            ? Array.Empty<object>()
            : new[] { InferSchema(element[0]) },
        _ => new { type = element.ValueKind.ToString().ToLowerInvariant() }
    };
}

static object BuildPluginManifest(PluginConfigStore store) =>
    ModuleCatalog.All.Select(module =>
    {
        var defaults = ToJsonElement(ModuleCatalog.DefaultFor(module.Key));
        return new
        {
            key = module.Key,
            name = module.Name,
            category = module.Category,
            description = module.Description,
            config = store.GetConfigReadOnly(module.Key),
            defaults,
            schema = InferSchema(defaults)
        };
    }).ToArray();

static async Task<ApiEnvelope> SavePluginAliasAsync(JsonElement request, PluginConfigStore store)
{
    var name = JsonString(request, "name", "Name", "key", "Key");
    if (string.IsNullOrWhiteSpace(name))
    {
        return ApiEnvelope.Fail("Ключ плагина не указан.");
    }

    var config = JsonObjectOrDefault(request, "config", "Config");
    if (config.ValueKind != JsonValueKind.Object)
    {
        return ApiEnvelope.Fail("Конфиг плагина должен быть JSON-объектом.");
    }

    try
    {
        return ApiEnvelope.Ok(await store.SaveConfigAsync(new PluginConfigSaveRequest(name, config)));
    }
    catch (Exception ex)
    {
        return ApiEnvelope.Fail(ex.Message);
    }
}

static object DiscordChannels(PluginConfigStore store)
{
    var config = store.GetConfigReadOnly("discord-log");
    string Str(params string[] keys) => JsonString(config, keys);
    object Route(string route, string label, string webhookKey, string channelKey) => new
    {
        route,
        label,
        webhookConfigured = !string.IsNullOrWhiteSpace(Str(webhookKey, char.ToLowerInvariant(webhookKey[0]) + webhookKey[1..])),
        channelId = Str(channelKey, char.ToLowerInvariant(channelKey[0]) + channelKey[1..]),
        usesDefaultWebhook = !string.IsNullOrWhiteSpace(Str("DefaultWebhookUrl", "defaultWebhookUrl")),
        usesDefaultChannel = !string.IsNullOrWhiteSpace(Str("DefaultChannelId", "defaultChannelId"))
    };

    return new
    {
        transportMode = Str("TransportMode", "transportMode"),
        routes = new[]
        {
            Route("system", "Система", "SystemWebhookUrl", "SystemChannelId"),
            Route("presence", "Входы и выходы", "PresenceWebhookUrl", "PresenceChannelId"),
            Route("chat", "Чат", "ChatWebhookUrl", "ChatChannelId"),
            Route("combat", "Бой", "CombatWebhookUrl", "CombatChannelId")
        }
    };
}

static object DiscordSecretStatus(PluginConfigStore store)
{
    var config = store.GetConfigReadOnly("discord-log");
    var token = JsonString(config, "BotToken", "botToken");
    var envToken = Environment.GetEnvironmentVariable("SCUM_NEDJIN_DISCORD_BOT_TOKEN") ?? "";
    return new
    {
        configured = !string.IsNullOrWhiteSpace(token) || !string.IsNullOrWhiteSpace(envToken),
        source = !string.IsNullOrWhiteSpace(token) ? "config" : (!string.IsNullOrWhiteSpace(envToken) ? "environment" : "none"),
        tokenPreview = !string.IsNullOrWhiteSpace(token) ? token[..Math.Min(4, token.Length)] + "..." : ""
    };
}

static async Task<object> SaveDiscordSecretAsync(DiscordSecretRequest request, PluginConfigStore store)
{
    var token = (request.BotToken ?? request.Token ?? "").Trim();
    var config = store.GetConfig("discord-log");
    var dict = JsonSerializer.Deserialize<Dictionary<string, object?>>(config.GetRawText(), WardenJson.Options) ?? new();
    if (token.Length == 0)
    {
        dict.Remove("BotToken");
        dict.Remove("botToken");
    }
    else
    {
        dict["BotToken"] = token;
    }

    await store.SaveConfigAsync(new PluginConfigSaveRequest("discord-log", ToJsonElement(dict)));
    return DiscordSecretStatus(store);
}

static string ResolveRentalAlias(JsonElement config, VehicleSpawnRequest request)
{
    var requestedAlias = PluginCommandText(request.Alias);
    if (requestedAlias.Length > 0)
    {
        return requestedAlias;
    }

    var requestedVehicle = PluginCommandText(request.VehicleId);
    if (requestedVehicle.Length == 0)
    {
        return "";
    }

    foreach (var vehicle in ConfigArray(config, "Vehicles", "vehicles").EnumerateArray())
    {
        var alias = PlayerString(vehicle, "Alias");
        if (alias.Length == 0)
        {
            alias = PlayerString(vehicle, "alias");
        }

        var asset = PlayerString(vehicle, "AssetName");
        if (asset.Length == 0)
        {
            asset = PlayerString(vehicle, "assetName");
        }

        if (alias.Length > 0 && string.Equals(asset, requestedVehicle, StringComparison.OrdinalIgnoreCase))
        {
            return alias;
        }
    }

    return "";
}

static JsonElement? FindRentalVehicle(JsonElement config, VehicleSpawnRequest request)
{
    var requestedAlias = PluginCommandText(request.Alias);
    var requestedVehicle = PluginCommandText(request.VehicleId);
    foreach (var group in new[] { ConfigArray(config, "Vehicles", "vehicles"), ConfigArray(config, "VipVehicles", "vipVehicles", "VIPVehicles") })
    {
        foreach (var vehicle in group.EnumerateArray())
        {
            var alias = PlayerString(vehicle, "Alias");
            if (alias.Length == 0)
            {
                alias = PlayerString(vehicle, "alias");
            }

            var asset = PlayerString(vehicle, "AssetName");
            if (asset.Length == 0)
            {
                asset = PlayerString(vehicle, "assetName");
            }

            if ((requestedAlias.Length > 0 && string.Equals(alias, requestedAlias, StringComparison.OrdinalIgnoreCase)) ||
                (requestedVehicle.Length > 0 && string.Equals(asset, requestedVehicle, StringComparison.OrdinalIgnoreCase)))
            {
                return vehicle.Clone();
            }
        }
    }

    return null;
}

static int ResolveRentalMinutes(JsonElement config, VehicleSpawnRequest request)
{
    var globalMin = Math.Max(1, PlayerInt(config, 10, "MinRentalMinutes", "minRentalMinutes", "MinMinutes", "minMinutes"));
    var globalMax = Math.Max(globalMin, PlayerInt(config, 60, "MaxRentalMinutes", "maxRentalMinutes", "MaxMinutes", "maxMinutes"));
    var globalDefault = Math.Clamp(PlayerInt(config, 10, "DefaultRentalMinutes", "defaultRentalMinutes", "DefaultMinutes", "defaultMinutes"), globalMin, globalMax);
    var min = globalMin;
    var max = globalMax;
    var fallback = globalDefault;

    var vehicle = FindRentalVehicle(config, request);
    if (vehicle.HasValue)
    {
        min = Math.Max(1, PlayerInt(vehicle.Value, globalMin, "MinMinutes", "minMinutes", "MinRentalMinutes", "minRentalMinutes"));
        max = Math.Max(min, PlayerInt(vehicle.Value, globalMax, "MaxMinutes", "maxMinutes", "MaxRentalMinutes", "maxRentalMinutes"));
        fallback = Math.Clamp(PlayerInt(vehicle.Value, globalDefault, "DefaultMinutes", "defaultMinutes", "DefaultRentalMinutes", "defaultRentalMinutes"), min, max);
    }

    var requested = request.Minutes > 0 ? request.Minutes : fallback;
    return Math.Clamp(requested, min, max);
}

app.MapGet("/api/health", () => ApiEnvelope.Ok(new { status = "ok", name = "ScumNeDjin" }));

app.MapGet("/api/build", (IOptions<WardenOptions> options) => ApiEnvelope.Ok(new
{
    product = "ScumNeDjin",
    version = ThisAssembly.BuildVersion,
    builtAtUtc = ThisAssembly.BuiltAtUtc,
    serverName = options.Value.ServerName,
    clientTestMode = options.Value.ClientTestMode
}));

app.MapGet("/api/server-info", (ServerConfigService serverConfig) => ApiEnvelope.Ok(serverConfig.GetServerInfo()));

app.MapGet("/api/status", async (
    FileBridgeExecutor bridge,
    ScumDatabaseReader database,
    ScumLogService logs,
    ServerConfigService serverConfig,
    IOptions<WardenOptions> options) =>
{
    var bridgeStatus = await bridge.GetStatusAsync();
    var serverName = serverConfig.GetServerName();
    return ApiEnvelope.Ok(new
    {
        server = serverName,
        serverName,
        mode = options.Value.ClientTestMode ? "client-test" : "server",
        clientTestMode = options.Value.ClientTestMode,
        bridge = bridgeStatus,
        database = database.GetStatus(),
        logs = logs.GetStatus(),
        diagnostics = new
        {
            problem = logs.DetectRecentProblem(),
            packageAvailable = true,
            discord = "https://discord.gg/kQwBDdzxEH"
        }
    });
});

app.MapGet("/api/diagnostics/package", async (
    FileBridgeExecutor bridge,
    ScumDatabaseReader database,
    ScumLogService logs,
    PluginConfigStore store,
    IOptions<WardenOptions> options) =>
    ApiEnvelope.Ok(await BuildDiagnosticsPackageAsync(bridge, database, logs, store, options.Value)));

app.MapGet("/api/diagnostics/archive", async (
    FileBridgeExecutor bridge,
    ScumDatabaseReader database,
    ScumLogService logs,
    PluginConfigStore store,
    IOptions<WardenOptions> options) =>
{
    var archive = await BuildDiagnosticsArchiveAsync(bridge, database, logs, store, options.Value);
    return Results.File(archive.Bytes, "application/zip", archive.FileName);
});

app.MapGet("/api/servers", (IOptions<WardenOptions> options, ServerConfigService serverConfig, ArkPanelControlService panelControl) => ApiEnvelope.Ok(new[]
{
    new
    {
        id = "local",
        name = serverConfig.GetServerName(),
        serverName = serverConfig.GetServerName(),
        host = options.Value.ClientTestMode ? "local client test" : options.Value.ServerRoot,
        panel = options.Value.PublicPanelUrl,
        clientTestMode = options.Value.ClientTestMode,
        online = true,
        control = panelControl.Status()
    }
}));

static async Task<string> BuildDiagnosticsPackageAsync(
    FileBridgeExecutor bridge,
    ScumDatabaseReader database,
    ScumLogService logs,
    PluginConfigStore store,
    WardenOptions options)
{
    var bridgeStatus = await bridge.GetStatusAsync();
    var databaseStatus = database.GetStatus();
    var logStatus = logs.GetStatus();
    var crashStatus = logs.DetectRecentProblem();
    var serverLines = logs.ReadServerLines(1000).Select(RedactDiagnosticLine).ToArray();
    var runtimeLines = logs.ReadRuntimeLines(1000).Select(RedactDiagnosticLine).ToArray();
    var ue4ssLines = logs.ReadUe4ssLines(1000).Select(RedactDiagnosticLine).ToArray();
    var queueStatus = store.GetStateOrDefault(
        "queue-status",
        """{"available":false,"depth":0,"pending":0,"busy":false,"message":"Queue state is not available."}""");
    var actionLog = store.GetStateOrDefault("action-log", "[]");
    var chatLog = store.GetStateOrDefault("chat", "[]");
    var vehicleRentals = store.GetStateOrDefault("vehicle-rentals", "[]");
    var wargm = store.GetStateOrDefault("wargm-shop", "[]");
    var problemLines = DetectProblemLines(serverLines.Concat(ue4ssLines).Concat(runtimeLines)).Take(120).ToArray();

    var sb = new StringBuilder(256 * 1024);
    sb.AppendLine("SCUM NeDjin diagnostics package");
    sb.AppendLine($"Generated UTC: {DateTimeOffset.UtcNow:O}");
    sb.AppendLine();
    sb.AppendLine("Open a Discord ticket manually and attach this file:");
    sb.AppendLine("Discord: https://discord.gg/kQwBDdzxEH");
    sb.AppendLine();
    sb.AppendLine("Important: secrets are redacted, but the package can still contain SteamID, player names, server paths, and gameplay logs.");
    sb.AppendLine();

    AppendDiagnosticsSection(sb, "Summary", new[]
    {
        $"ServerName: {options.ServerName}",
        $"ClientTestMode: {options.ClientTestMode}",
        $"ServerRoot: {options.ServerRoot}",
        $"BridgePath: {options.BridgePath}",
        $"DatabasePath: {options.DatabasePath}",
        $"LogDirectory: {options.LogDirectory}",
        $"RuntimeLogPath: {options.RuntimeLogPath}",
        $"BridgeOnline: {bridgeStatus.Online}",
        $"BridgeMessage: {bridgeStatus.Message}",
        $"BridgeHeartbeatUtc: {bridgeStatus.HeartbeatUtc?.ToString("O") ?? "-"}",
        $"BridgeHeartbeatAgeSeconds: {bridgeStatus.HeartbeatAgeSeconds?.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture) ?? "-"}",
        $"DatabaseOnline: {databaseStatus.Online}",
        $"DatabaseMessage: {databaseStatus.Message}",
        $"LogsOnline: {logStatus.Online}",
        $"LogsMessage: {logStatus.Message}",
        $"CrashProblem: {crashStatus.HasProblem}",
        $"CrashSeverity: {crashStatus.Severity}",
        $"CrashSource: {crashStatus.Source}",
        $"CrashDetectedAtUtc: {crashStatus.DetectedAtUtc?.ToString("O") ?? "-"}"
    });

    AppendDiagnosticsSection(sb, "Detected problem lines", problemLines.Length == 0
        ? new[] { "No obvious error lines detected in collected log tails." }
        : problemLines);

    AppendDiagnosticsJson(sb, "Queue status", queueStatus);
    AppendDiagnosticsJson(sb, "Recent action state", actionLog);
    AppendDiagnosticsJson(sb, "Recent chat state", chatLog);
    AppendDiagnosticsJson(sb, "Vehicle rental state", vehicleRentals);
    AppendDiagnosticsJson(sb, "Wargm state", wargm);
    AppendDiagnosticsJson(sb, "Crash detector", JsonSerializer.SerializeToElement(crashStatus, WardenJson.Options));
    AppendDiagnosticsSection(sb, "SCUM server log tail", serverLines);
    AppendDiagnosticsSection(sb, "UE4SS log tail", ue4ssLines);
    AppendDiagnosticsSection(sb, "NeDjin runtime bridge log tail", runtimeLines);
    return sb.ToString();
}

static async Task<(byte[] Bytes, string FileName)> BuildDiagnosticsArchiveAsync(
    FileBridgeExecutor bridge,
    ScumDatabaseReader database,
    ScumLogService logs,
    PluginConfigStore store,
    WardenOptions options)
{
    var stamp = DateTimeOffset.UtcNow.ToString("yyyyMMdd-HHmmss", System.Globalization.CultureInfo.InvariantCulture);
    var fileName = $"scum-nedjin-diagnostics-{stamp}.zip";
    await using var stream = new MemoryStream();
    using (var zip = new ZipArchive(stream, ZipArchiveMode.Create, leaveOpen: true))
    {
        var packageText = await BuildDiagnosticsPackageAsync(bridge, database, logs, store, options);
        AddZipText(zip, "README_SEND_TO_NEDJIN.txt", string.Join(Environment.NewLine, new[]
        {
            "SCUM NeDjin diagnostic archive",
            $"Generated UTC: {DateTimeOffset.UtcNow:O}",
            "",
            "Open a ticket manually and attach this zip:",
            "Discord: https://discord.gg/kQwBDdzxEH",
            "",
            "No automatic upload is performed. The archive may contain SteamID, player names, server paths and gameplay logs."
        }));
        AddZipText(zip, "summary.txt", packageText);
        AddZipText(zip, "logs/SCUM.log.tail.txt", string.Join(Environment.NewLine, logs.ReadServerLines(1800).Select(RedactDiagnosticLine)));
        AddZipText(zip, "logs/UE4SS.log.tail.txt", string.Join(Environment.NewLine, logs.ReadUe4ssLines(1800).Select(RedactDiagnosticLine)));
        AddZipText(zip, "logs/nedjin_bridge.log.tail.txt", string.Join(Environment.NewLine, logs.ReadRuntimeLines(1800).Select(RedactDiagnosticLine)));
        AddZipJson(zip, "status/crash-detector.json", logs.DetectRecentProblem());
        AddZipJson(zip, "state/queue-status.json", store.GetStateOrDefault(
            "queue-status",
            """{"available":false,"depth":0,"pending":0,"busy":false,"message":"Queue state is not available."}"""));
        AddZipJson(zip, "state/action-log.json", store.GetStateOrDefault("action-log", "[]"));
        AddZipJson(zip, "state/vehicle-rentals.json", store.GetStateOrDefault("vehicle-rentals", "[]"));
        AddZipJson(zip, "state/wargm-shop.json", store.GetStateOrDefault("wargm-shop", "[]"));
        AddZipJson(zip, "state/chat.json", store.GetStateOrDefault("chat", "[]"));

        var win64 = logs.GetWin64Directory();
        if (!string.IsNullOrWhiteSpace(win64))
        {
            AddZipExistingText(zip, "config/UE4SS-settings.ini.txt", Path.Combine(win64, "UE4SS-settings.ini"));
            AddZipExistingText(zip, "config/mods.txt", Path.Combine(win64, "Mods", "mods.txt"));
            AddZipExistingText(zip, "config/nedjin.ini.txt", Path.Combine(win64, "nedjin.ini"));

            var scumNeDjin = Path.Combine(win64, "ScumNeDjin");
            AddConfigDirectory(zip, scumNeDjin, "configs", "config/base");
            AddConfigDirectory(zip, scumNeDjin, Path.Combine("state", "configs"), "config/overlay");
        }

        var artifacts = logs.GetRecentCrashArtifacts();
        AddZipText(zip, "crash-dumps.txt", artifacts.Count == 0
            ? "No recent crash_* files detected."
            : string.Join(Environment.NewLine, artifacts.Select(artifact =>
                $"{artifact.ModifiedUtc:O} {artifact.Kind} {artifact.SizeBytes} bytes {artifact.Path}")));
    }

    return (stream.ToArray(), fileName);
}

static void AddConfigDirectory(ZipArchive zip, string root, string relativeDirectory, string archiveDirectory)
{
    var directory = Path.Combine(root, relativeDirectory);
    if (!Directory.Exists(directory))
    {
        return;
    }

    foreach (var file in Directory.EnumerateFiles(directory, "*.json", SearchOption.TopDirectoryOnly)
        .OrderBy(Path.GetFileName, StringComparer.OrdinalIgnoreCase))
    {
        AddZipExistingText(zip, $"{archiveDirectory}/{Path.GetFileName(file)}.txt", file);
    }
}

static void AddZipExistingText(ZipArchive zip, string entryName, string path)
{
    if (!File.Exists(path))
    {
        return;
    }

    try
    {
        var text = File.ReadAllText(path);
        AddZipText(zip, entryName, RedactDiagnosticLine(text));
    }
    catch
    {
        AddZipText(zip, entryName, "(unreadable)");
    }
}

static void AddZipJson(ZipArchive zip, string entryName, object value) =>
    AddZipText(zip, entryName, RedactDiagnosticLine(JsonSerializer.Serialize(value, WardenJson.Options)));

static void AddZipText(ZipArchive zip, string entryName, string text)
{
    var entry = zip.CreateEntry(entryName, CompressionLevel.Fastest);
    using var writer = new StreamWriter(entry.Open(), new UTF8Encoding(false));
    writer.Write(text ?? "");
}

static void AppendDiagnosticsSection(StringBuilder sb, string title, IEnumerable<string> lines)
{
    sb.AppendLine();
    sb.AppendLine($"===== {title} =====");
    var wrote = false;
    foreach (var line in lines)
    {
        wrote = true;
        sb.AppendLine(line);
    }
    if (!wrote)
    {
        sb.AppendLine("(empty)");
    }
}

static void AppendDiagnosticsJson(StringBuilder sb, string title, JsonElement element)
{
    sb.AppendLine();
    sb.AppendLine($"===== {title} =====");
    try
    {
        sb.AppendLine(RedactDiagnosticLine(JsonSerializer.Serialize(element, WardenJson.Options)));
    }
    catch
    {
        sb.AppendLine("(unreadable json)");
    }
}

static IEnumerable<string> DetectProblemLines(IEnumerable<string> lines)
{
    foreach (var line in lines)
    {
        var lower = line.ToLowerInvariant();
        if (lower.Contains("error") ||
            lower.Contains("failed") ||
            lower.Contains("fatal") ||
            lower.Contains("exception") ||
            lower.Contains("crash") ||
            lower.Contains("timeout") ||
            lower.Contains("stale") ||
            lower.Contains("unreadable") ||
            lower.Contains("lua error") ||
            lower.Contains("ue4ss error") ||
            (lower.Contains("bridge") &&
                (lower.Contains("offline") ||
                 lower.Contains("stale") ||
                 lower.Contains("failed") ||
                 lower.Contains("error") ||
                 lower.Contains("timeout") ||
                 lower.Contains("unreadable"))))
        {
            yield return line;
        }
    }
}

static string RedactDiagnosticLine(string line)
{
    var value = line ?? string.Empty;
    value = Regex.Replace(
        value,
        "(?i)(\"?(?:api[_-]?key|token|password|secret|botToken|webhook|webhookUrl|defaultWebhookUrl|chatWebhookUrl|combatWebhookUrl|systemWebhookUrl|presenceWebhookUrl)\"?\\s*[:=]\\s*\")[^\"]*(\")",
        "$1<redacted>$2");
    value = Regex.Replace(
        value,
        "(?i)(api[_-]?key|token|password|secret|botToken|webhook|webhookUrl)\\s*[:=]\\s*[^\\s,;\\\"']+",
        "$1=<redacted>");
    value = Regex.Replace(value, "(?i)(client=\\d+:)[^\\s&]+", "$1<redacted>");
    return value;
}

app.MapGet("/api/client-test", (IOptions<WardenOptions> options) => ApiEnvelope.Ok(new
{
    enabled = options.Value.ClientTestMode,
    playerName = options.Value.ClientTestPlayerName,
    note = options.Value.ClientTestMode
        ? "Панель запущена как локальный клиентский стенд. Игровые действия симулируются и не внедряются в SCUM-клиент."
        : "Клиентский тестовый режим выключен."
}));

app.MapGet("/api/players", async (string? refresh, string? live, string? mode, CommandRouter router, ScumLogService logs) =>
    WantsLiveBridge(refresh, live, mode)
        ? PlayersApiEnvelope(await router.GetPlayersAsync())
        : PlayersLogApiEnvelope(logs.InferOnlinePlayersFromLog()));
app.MapGet("/api/player-details", async (string? steamId, string? name, string? runtimeKey, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.GetPlayerDetailsAsync(steamId, name, runtimeKey)));
app.MapGet("/api/map", async (string? refresh, string? live, string? mode, CommandRouter router, ScumDatabaseReader database, ScumLogService logs, IOptions<WardenOptions> options) =>
{
    var wantsLive = WantsLiveBridge(refresh, live, mode);
    var livePlayers = wantsLive ? await router.GetPlayersAsync() : null;
    var logPlayers = wantsLive ? null : logs.InferOnlinePlayersFromLog();
    var flags = database.GetFlags(500);
    var vehicles = database.GetVehicles(500);
    return ApiEnvelope.Ok(new
    {
        mapImageUrl = options.Value.MapImageUrl,
        bounds = options.Value.MapBounds,
        players = wantsLive && livePlayers is not null ? livePlayers.DataOrDefault("players") : PlayersArrayFromResponse(logPlayers!),
        flags,
        vehicles,
        playerSource = wantsLive && livePlayers is not null ? livePlayers.Source : "server-log",
        generatedAtUtc = DateTimeOffset.UtcNow
    });
});

app.MapGet("/api/chat", (WardenEventStore events, PluginConfigStore store, int? limit) =>
    ApiEnvelope.Ok(StateOrEvents(store, events, "chat", "chat", Math.Clamp(limit ?? 250, 1, 1000))));
app.MapGet("/api/chat-log", (WardenEventStore events, PluginConfigStore store, int? limit) =>
    ApiEnvelope.Ok(StateOrEvents(store, events, "chat", "chat", Math.Clamp(limit ?? 250, 1, 1000))));
app.MapGet("/api/kills", (WardenEventStore events, PluginConfigStore store) =>
    ApiEnvelope.Ok(StateOrEvents(store, events, "kills", "kill", 250)));
app.MapGet("/api/economy", (WardenEventStore events, ScumDatabaseReader database, PluginConfigStore store) => ApiEnvelope.Ok(new
{
    recent = StateOrEvents(store, events, "economy", "economy", 250),
    leaderboard = database.GetEconomySnapshot(100)
}));
app.MapGet("/api/squads", (ScumDatabaseReader database) => ApiEnvelope.Ok(database.GetSquadsSnapshot(250)));
app.MapGet("/api/squads/diagnostics", async (ScumDatabaseReader database, PluginConfigStore store) =>
{
    var diagnostics = database.GetSquadsDiagnostics();
    await store.SetStateAsync("squads-diagnostics", diagnostics);
    return ApiEnvelope.Ok(diagnostics);
});
app.MapPost("/api/squads/kick", () =>
    ApiEnvelope.Fail("Исключение игрока из отряда через безопасный маршрут пока недоступно в этой версии SCUM. Используйте игровые инструменты управления отрядом."));
app.MapGet("/api/flags", (ScumDatabaseReader database) => ApiEnvelope.Ok(database.GetFlags(500)));
app.MapGet("/api/vehicles", (ScumDatabaseReader database) => ApiEnvelope.Ok(database.GetVehicles(500)));

app.MapGet("/api/runtime-log", (ScumLogService logs) => ApiEnvelope.Ok(logs.ReadRuntimeLines(600)));
app.MapGet("/api/server-log", (ScumLogService logs, int? limit) => ApiEnvelope.Ok(logs.ReadServerLines(limit ?? 600)));
app.MapGet("/api/logs", (ScumLogService logs, int? limit) =>
{
    var safeLimit = Math.Clamp(limit ?? 800, 1, 2000);
    var runtime = logs.ReadRuntimeLines(safeLimit);
    var server = logs.ReadServerLines(safeLimit);
    return ApiEnvelope.Ok(new
    {
        text = string.Join(Environment.NewLine, runtime),
        runtime,
        server
    });
});
app.MapGet("/api/server-control/status", (ArkPanelControlService control) => ApiEnvelope.Ok(control.Status()));
app.MapPost("/api/server-control", async (ServerControlRequest request, ArkPanelControlService control) =>
{
    try
    {
        return ApiEnvelope.Ok(await control.RunActionAsync(request.Action));
    }
    catch (Exception ex)
    {
        return ApiEnvelope.Fail(ex.Message);
    }
});
app.MapPost("/api/server-control/{action}", async (string action, ArkPanelControlService control) =>
{
    try
    {
        return ApiEnvelope.Ok(await control.RunActionAsync(action));
    }
    catch (Exception ex)
    {
        return ApiEnvelope.Fail(ex.Message);
    }
});
app.MapGet("/api/server-configs", (ServerConfigService configs) => ApiEnvelope.Ok(configs.ListConfigs()));
app.MapGet("/api/server-config", (string? name, ServerConfigService configs) =>
{
    try
    {
        return ApiEnvelope.Ok(configs.ReadConfig(name));
    }
    catch (Exception ex)
    {
        return ApiEnvelope.Fail(ex.Message);
    }
});
app.MapPost("/api/server-config", async (ServerConfigSaveRequest request, ServerConfigService configs) =>
{
    try
    {
        return ApiEnvelope.Ok(await configs.SaveConfigAsync(request));
    }
    catch (Exception ex)
    {
        return ApiEnvelope.Fail(ex.Message);
    }
});
app.MapPost("/api/logs/clear", async (LogClearRequest request, PluginConfigStore store, WardenEventStore events) =>
{
    var targets = LogClearTargets(request);
    if (targets.Count == 0)
    {
        return ApiEnvelope.Fail("Не выбран журнал для очистки.");
    }

    var stateNames = targets.Select(target => target.State).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    var stateResult = await store.ClearStatesAsync(stateNames);
    var memory = targets
        .GroupBy(target => target.Event, StringComparer.OrdinalIgnoreCase)
        .Select(group => new { type = group.Key, removed = events.Clear(group.Key) })
        .ToArray();

    return ApiEnvelope.Ok(new
    {
        cleared = stateNames,
        state = stateResult,
        memory,
        message = "Журналы проекта очищены. SCUM.log не изменялся."
    });
});
app.MapGet("/api/downloads", (IWebHostEnvironment env) => ApiEnvelope.Ok(BuildDownloadList(env)));
app.MapGet("/api/command-trace", (WardenEventStore events) => ApiEnvelope.Ok(events.Get("command", 250)));
app.MapGet("/api/action-log", (PluginConfigStore store, WardenEventStore events, int? limit) =>
    ApiEnvelope.Ok(StateWithEvents(store, events, "action-log", new[] { "action", "command" }, Math.Clamp(limit ?? 400, 1, 1000))));
app.MapGet("/api/events", (string? route, int? limit, PluginConfigStore store, WardenEventStore events) =>
    ApiEnvelope.Ok(EventsByRoute(store, events, route, limit)));
app.MapGet("/api/event-log", (string? route, int? limit, PluginConfigStore store, WardenEventStore events) =>
    ApiEnvelope.Ok(EventsByRoute(store, events, route, limit)));
app.MapGet("/api/queue", (PluginConfigStore store) => ApiEnvelope.Ok(store.GetStateOrDefault(
    "queue-status",
    """{"available":false,"depth":0,"pending":0,"busy":false,"max":0,"spacingMs":0,"nextDelayMs":0,"source":"passive-state","message":"Очередь ещё не опубликовала состояние; live bridge не опрашивался."}""")));
app.MapGet("/api/module-state", (string? key, PluginConfigStore store) =>
    string.IsNullOrWhiteSpace(key) ? ApiEnvelope.Fail("Ключ состояния не указан.") : ApiEnvelope.Ok(store.GetState(key)));
app.MapGet("/api/state", (string? name, string? key, PluginConfigStore store) =>
{
    var stateKey = string.IsNullOrWhiteSpace(name) ? key : name;
    return string.IsNullOrWhiteSpace(stateKey)
        ? ApiEnvelope.Fail("Ключ состояния не указан.")
        : ApiEnvelope.Ok(store.GetState(stateKey));
});
app.MapGet("/api/player-inventory", (string? steamId, string? name, ScumDatabaseReader database) =>
    ApiEnvelope.Ok(database.GetPlayerInventory(steamId, name)));
app.MapGet("/api/player/wallet", (string? steamId, string? name, ScumDatabaseReader database) =>
    ApiEnvelope.Ok(database.GetPlayerWallet(steamId, name)));
app.MapGet("/api/player/attributes", (string? steamId, string? name, ScumDatabaseReader database) =>
    ApiEnvelope.Ok(database.GetPlayerAttributes(steamId, name)));
app.MapGet("/api/player/skills", (string? steamId, string? name, string? runtimeKey, ScumDatabaseReader database) =>
{
    var result = database.GetPlayerSkills(steamId, name, runtimeKey);
    return result.Ok
        ? ApiEnvelope.Ok(result.Data)
        : ApiEnvelope.Fail(result.Error ?? "SCUM.db prisoner_skill query failed");
});

app.MapPost("/api/rcon", async (CommandRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.ExecuteRawAsync(request)));
app.MapPost("/api/command", async (CommandRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.ExecuteRawAsync(request)));
app.MapPost("/api/web-rcon", async (CommandRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.ExecuteRawAsync(request)));
app.MapPost("/api/admin-command-probe", () =>
    ApiEnvelope.Fail("admin_command_probe отключён в публичной сборке: live-проба этой команды нестабильна для UE4SS."));
app.MapPost("/api/debug/admin-command-probe", () =>
    ApiEnvelope.Fail("admin_command_probe отключён в публичной сборке: live-проба этой команды нестабильна для UE4SS."));
app.MapPost("/api/chat", async (ChatRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.SendChatAsync(request)));
app.MapPost("/api/broadcast", async (ChatRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.SendChatAsync(new ChatRequest(request.Message, request.Channel))));
app.MapPost("/api/player/teleport", async (TeleportRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.TeleportAsync(request)));
app.MapPost("/api/player/kick", async (PlayerActionRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.AdminPlayerActionAsync("kick", request)));
app.MapPost("/api/player/ban", async (PlayerActionRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.AdminPlayerActionAsync("ban", request)));
app.MapPost("/api/player/change-money", async (MoneyRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.ChangeMoneyAsync(request)));
app.MapPost("/api/player/set-fame", async (MoneyRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.SetFameAsync(request)));
app.MapPost("/api/player/change-fame", async (MoneyRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.ChangeFameAsync(request)));
app.MapPost("/api/player/clear-inventory", async (PlayerActionRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.ClearInventoryAsync(request)));
app.MapPost("/api/player/inventory/delete", async (InventoryItemDeleteRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.DeleteInventoryItemAsync(request)));
app.MapPost("/api/player/delete-inventory-item", async (InventoryItemDeleteRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.DeleteInventoryItemAsync(request)));
app.MapPost("/api/player/destroy-inventory-entities", async (InventoryEntitiesRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.DestroyInventoryEntitiesAsync(request)));
app.MapPost("/api/player/item-batch", async (ItemBatchRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.DeliverItemBatchAsync(request)));
app.MapPost("/api/player/grant-item", async (ItemGrantRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.DeliverItemBatchAsync(ItemBatchRequest.FromSingle(request))));
app.MapPost("/api/player/equip-item", async (ItemGrantRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.EquipItemAsync(request)));
app.MapPost("/api/player/spawn-vehicle", async (VehicleSpawnRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.SpawnVehicleAsync(request)));
app.MapPost("/api/player/spawn-actor", () =>
    ApiEnvelope.Fail("SpawnActor отключён в публичной сборке."));
app.MapPost("/api/actor/spawn", () =>
    ApiEnvelope.Fail("SpawnActor отключён в публичной сборке."));
app.MapPost("/api/player/zombie-infect", () =>
    ApiEnvelope.Fail("Zombie/model-swap отключён в публичной сборке."));
app.MapPost("/api/player/turn-zombie", () =>
    ApiEnvelope.Fail("Zombie/model-swap отключён в публичной сборке."));
app.MapPost("/api/player/set-attributes", async (CharacterAttributesRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.SetAttributesAsync(request)));
app.MapPost("/api/character-stats/apply", async (CharacterAttributesRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.SetAttributesAsync(request)));
app.MapPost("/api/player/set-skill", async (CharacterSkillRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.SetSkillAsync(request)));
app.MapPost("/api/player/apply-character-pack", async (CharacterPackRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.ApplyCharacterPackAsync(request)));
app.MapPost("/api/vehicle/destroy", async (PlayerActionRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.DestroyVehicleAsync(request)));
app.MapPost("/api/player/destroy-vehicle", async (PlayerActionRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.DestroyVehicleAsync(request)));
app.MapPost("/api/private-message/send", async (PrivateMessageRequest request, CommandRouter router, PluginConfigStore store) =>
{
    var result = await router.SendChatAsync(new ChatRequest(request.Message, "server", request.SteamId, request.Name, request.RuntimeKey, request.RuntimeKey));
    if (result.Ok)
    {
        await store.AppendStateAsync("private-messages", new
        {
            utc = DateTimeOffset.UtcNow,
            steamId = request.SteamId,
            name = request.Name,
            message = request.Message
        });
    }

    return ApiEnvelope.FromCommand(result);
});
app.MapPost("/api/plugin-command", async (PluginCommandRequest request, CommandRouter router) =>
{
    if (request.Command.Equals("player_plugin_command", StringComparison.OrdinalIgnoreCase))
    {
        var argsJson = request.Args is { ValueKind: JsonValueKind.Object } json ? json : default;
        return ApiEnvelope.FromCommand(await router.RunPlayerPluginCommandAsync(
            new PlayerPluginCommandRequest(
                JsonString(argsJson, "steamId", "SteamId", "steam", "SteamID"),
                JsonString(argsJson, "name", "Name", "playerName", "PlayerName"),
                JsonString(argsJson, "message", "Message", "commandText", "CommandText"),
                JsonString(argsJson, "runtimeKey", "RuntimeKey", "key", "Key")),
            request.TimeoutMs));
    }

    object args = request.Args is { ValueKind: JsonValueKind.Object } jsonArgs ? jsonArgs : new { };
    return ApiEnvelope.FromCommand(await router.ExecuteBridgeAsync(request.Command, args, request.TimeoutMs));
});
app.MapPost("/api/scheduled-events/run", async (ScheduledEventRunRequest request, int? index, bool? force, CommandRouter router) =>
{
    var jobIndex = index ?? request.Index;
    if (jobIndex is null || jobIndex < 0)
    {
        return ApiEnvelope.Fail("Индекс задания планировщика не задан.");
    }

    return ApiEnvelope.FromCommand(await router.ExecuteBridgeAsync(
        "scheduled_events_run",
        new { index = jobIndex.Value, force = request.Force || force == true },
        request.TimeoutMs ?? 45000));
});
app.MapPost("/api/base-element/db-spawn", (JsonElement request) =>
    ApiEnvelope.Fail("Persistent base-element DB-spawn route is intentionally blocked. Use a verified native C++/game registry route before enabling this action."));
app.MapGet("/api/player/skill-catalog", () => ApiEnvelope.Ok(CharacterCatalog.Skills));
app.MapGet("/api/catalog/items", (IWebHostEnvironment env) =>
    ApiEnvelope.Ok(LoadWebJsonAsset(env, "spawnable-items.json", Array.Empty<object>())));
app.MapGet("/api/catalog/vehicles", (IWebHostEnvironment env) =>
    ApiEnvelope.Ok(LoadWebJsonAsset(env, "spawnable-vehicles.json", Array.Empty<object>())));
app.MapGet("/api/catalog/item-icons", (IWebHostEnvironment env) =>
    ApiEnvelope.Ok(LoadWebJsonAsset(env, "item-icons-manifest.json", new { icons = Array.Empty<object>() })));
app.MapGet("/api/item-icons", (IWebHostEnvironment env) =>
    ApiEnvelope.Ok(LoadWebJsonAsset(env, "item-icons-manifest.json", new { icons = Array.Empty<object>() })));
app.MapPost("/api/items/prewarm", async (ItemPrewarmRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.PrewarmItemsAsync(request)));
app.MapPost("/api/item-class/prewarm", async (ItemPrewarmRequest request, CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.PrewarmItemsAsync(request)));

app.MapGet("/api/plugins", (PluginConfigStore store) => ApiEnvelope.Ok(store.ListModules()));
app.MapGet("/api/plugin", (string? name, string? key, PluginConfigStore store) =>
{
    var moduleKey = string.IsNullOrWhiteSpace(name) ? key : name;
    if (string.IsNullOrWhiteSpace(moduleKey))
    {
        return ApiEnvelope.Ok(store.ListModules());
    }

    var descriptor = ModuleCatalog.All.FirstOrDefault(module => string.Equals(module.Key, moduleKey, StringComparison.OrdinalIgnoreCase));
    return ApiEnvelope.Ok(new
    {
        key = moduleKey,
        descriptor,
        config = store.GetConfigReadOnly(moduleKey)
    });
});
app.MapGet("/api/plugin-manifest", (PluginConfigStore store) => ApiEnvelope.Ok(BuildPluginManifest(store)));
app.MapGet("/api/plugin-schema", (PluginConfigStore store) => ApiEnvelope.Ok(BuildPluginManifest(store)));
app.MapGet("/api/plugin-configs", (PluginConfigStore store) => ApiEnvelope.Ok(BuildPluginManifest(store)));
app.MapGet("/api/pluginconfigs", (PluginConfigStore store) => ApiEnvelope.Ok(BuildPluginManifest(store)));
app.MapPost("/api/plugin", SavePluginAliasAsync);
app.MapDelete("/api/plugin", async (string? name, string? key, PluginConfigStore store) =>
{
    var moduleKey = string.IsNullOrWhiteSpace(name) ? key : name;
    return string.IsNullOrWhiteSpace(moduleKey)
        ? ApiEnvelope.Fail("Ключ плагина не указан.")
        : ApiEnvelope.Ok(await store.DeleteConfigAsync(moduleKey));
});
app.MapGet("/api/plugin-config", (string name, PluginConfigStore store) => ApiEnvelope.Ok(store.GetConfigReadOnly(name)));
app.MapPost("/api/plugin-config", SavePluginAliasAsync);
app.MapPost("/api/plugin-state", async (PluginStateRequest request, PluginConfigStore store) =>
    ApiEnvelope.Ok(await store.SetEnabledAsync(request)));
app.MapPost("/api/reload", async (FileBridgeExecutor bridge) =>
{
    var result = await bridge.ExecuteAsync("reload_bridge", new { }, 3000);
    return ApiEnvelope.Ok(new
    {
        reloaded = true,
        liveConfig = true,
        luaRestartRequested = result.Ok,
        message = result.Ok
            ? "Конфиги плагинов перечитаны, Lua bridge получил команду live-reload."
            : "Конфиги плагинов читаются при каждом действии; Lua bridge сейчас не подтвердил live-reload.",
        bridgeMessage = result.Message,
        bridge = result.Payload
    });
});
app.MapGet("/api/welcome-pack/timers", (PluginConfigStore store) => ApiEnvelope.Ok(store.GetState("welcome-pack-timers")));
app.MapGet("/api/welcomepack/timers", (PluginConfigStore store) => ApiEnvelope.Ok(store.GetState("welcome-pack-timers")));
app.MapPost("/api/welcome-pack/claim", async (PlayerActionRequest request, PluginConfigStore store, CommandRouter router) =>
{
    var config = store.GetConfig("welcome-pack");
    if (config.TryGetProperty("Enabled", out var enabled) && enabled.ValueKind == JsonValueKind.False)
    {
        return ApiEnvelope.Fail("Стартовый набор отключён.");
    }

    return ApiEnvelope.FromCommand(await router.RunPlayerPluginCommandAsync(
        new PlayerPluginCommandRequest(request.SteamId, request.Name, PlayerCommand(store, "welcomepack", "/welcomepack"), request.RuntimeKey),
        15000));
});
app.MapPost("/api/welcome-pack/reset-timer", async (WelcomeTimerResetRequest request, PluginConfigStore store) =>
    ApiEnvelope.Ok(await store.ResetWelcomeTimerAsync(request)));
app.MapPost("/api/welcomepack/reset-timer", async (WelcomeTimerResetRequest request, PluginConfigStore store) =>
    ApiEnvelope.Ok(await store.ResetWelcomeTimerAsync(request)));
app.MapGet("/api/home/list", (PluginConfigStore store) => ApiEnvelope.Ok(store.GetState("homes")));
app.MapPost("/api/home/set", async (HomeSetRequest request, PluginConfigStore store, CommandRouter router) =>
{
    var label = string.IsNullOrWhiteSpace(request.Label) ? "home" : request.Label.Trim();
    return ApiEnvelope.FromCommand(await router.RunPlayerPluginCommandAsync(
        new PlayerPluginCommandRequest(request.SteamId, request.Name, PlayerCommand(store, "sethome", "/sethome", " " + PluginCommandText(label)), request.RuntimeKey),
        5000));
});
app.MapPost("/api/home/teleport", async (HomeTeleportRequest request, PluginConfigStore store, CommandRouter router) =>
{
    if (!string.IsNullOrWhiteSpace(request.Label))
    {
        return ApiEnvelope.FromCommand(await router.RunPlayerPluginCommandAsync(
            new PlayerPluginCommandRequest(request.SteamId, request.Name, PlayerCommand(store, "home", "/home", " " + PluginCommandText(request.Label)), request.RuntimeKey),
            10000));
    }

    return ApiEnvelope.FromCommand(await router.TeleportAsync(new TeleportRequest(request.SteamId, request.Name, request.X, request.Y, request.Z, request.RuntimeKey)));
});
app.MapGet("/api/fast-travel/routes", (PluginConfigStore store) =>
    ApiEnvelope.Ok(ConfigArray(store.GetConfigReadOnly("fast-travel"), "Outposts", "outposts")));
app.MapPost("/api/fast-travel/go", async (FastTravelRequest request, PluginConfigStore store, CommandRouter router) =>
{
    if (!string.IsNullOrWhiteSpace(request.Alias))
    {
        return ApiEnvelope.FromCommand(await router.RunPlayerPluginCommandAsync(
            new PlayerPluginCommandRequest(request.SteamId, request.Name, PlayerCommand(store, "travel", "/travel", " " + PluginCommandText(request.Alias)), request.RuntimeKey),
            10000));
    }

    var x = request.X;
    var y = request.Y;
    var z = request.Z;
    var routeName = request.Alias ?? "";
    if (!x.HasValue || !y.HasValue || !z.HasValue)
    {
        if (!TryResolveFastTravel(store.GetConfig("fast-travel"), request.Alias, out var rx, out var ry, out var rz, out routeName))
        {
            return ApiEnvelope.Fail("Маршрут быстрого перемещения не найден.");
        }

        x = rx;
        y = ry;
        z = rz;
    }

    var result = await router.TeleportAsync(new TeleportRequest(request.SteamId, request.Name, x.Value, y.Value, z.Value, request.RuntimeKey));
    if (result.Ok)
    {
        await store.AppendStateAsync("fast-travel-trips", new
        {
            utc = DateTimeOffset.UtcNow,
            steamId = request.SteamId,
            name = request.Name,
            route = routeName,
            x,
            y,
            z
        });
    }

    return ApiEnvelope.FromCommand(result);
});
app.MapGet("/api/vehicle-rental", (PluginConfigStore store) => ApiEnvelope.Ok(new
{
    config = store.GetConfigReadOnly("vehicle-rental"),
    state = store.GetState("vehicle-rental"),
    pendingTaxes = store.GetState("vehicle-rental-pending-taxes")
}));
app.MapPost("/api/vehicle-rental/rent", async (VehicleSpawnRequest request, PluginConfigStore store, CommandRouter router) =>
{
    var config = store.GetConfig("vehicle-rental");
    var alias = ResolveRentalAlias(config, request);
    if (alias.Length == 0)
    {
        return ApiEnvelope.Fail("Транспорт для аренды не найден в настройках плагина.");
    }

    var minutes = ResolveRentalMinutes(config, request);
    return ApiEnvelope.FromCommand(await router.RunPlayerPluginCommandAsync(
        new PlayerPluginCommandRequest(request.SteamId, request.Name, PlayerCommand(store, "rent", "/rent", " " + PluginCommandText(alias) + " " + minutes), request.RuntimeKey),
        15000));
});
app.MapPost("/api/vehicle-rental/cleanup", async (CommandRouter router) =>
    ApiEnvelope.FromCommand(await router.ExecuteBridgeAsync("cleanup_rentals", new { }, 5000)));
app.MapPost("/api/sector-scan", async (SectorScanRequest request, PluginConfigStore store, CommandRouter router) =>
{
    if (string.IsNullOrWhiteSpace(request.SteamId) &&
        string.IsNullOrWhiteSpace(request.Name) &&
        string.IsNullOrWhiteSpace(request.RuntimeKey))
    {
        return ApiEnvelope.Fail("Укажи игрока: скан берёт текущий квадрат именно этого игрока.");
    }

    var sector = request.Sector?.Trim();
    var command = string.IsNullOrWhiteSpace(sector)
        ? PlayerCommand(store, "scan", "/scan")
        : PlayerCommand(store, "scan", "/scan", " " + PluginCommandText(sector));

    return ApiEnvelope.FromCommand(await router.RunPlayerPluginCommandAsync(
        new PlayerPluginCommandRequest(request.SteamId, request.Name, command, request.RuntimeKey),
        10000));
});
app.MapPost("/api/wargm/manual-deliver", async (WargmManualDeliverRequest request, WargmDeliveryService delivery) =>
    ApiEnvelope.FromCommand(await delivery.DeliverManualAsync(request)));
app.MapPost("/api/wargm/sync", async (PluginConfigStore store) =>
    ApiEnvelope.Ok(await SignalNativeWargmAsync(store, "wargm-sync-request", "panel-sync")));
app.MapPost("/api/wargm/deliver-pending", async (PluginConfigStore store) =>
    ApiEnvelope.Ok(await SignalNativeWargmAsync(store, "wargm-sync-request", "panel-deliver-pending")));
app.MapPost("/api/wargm/confirm-delivered", async (PluginConfigStore store) =>
    ApiEnvelope.Ok(await SignalNativeWargmAsync(store, "wargm-confirm-request", "panel-confirm")));
app.MapGet("/api/wargm/pending", (PluginConfigStore store) => ApiEnvelope.Ok(new
{
    pending = store.GetState("wargm-pending"),
    delivered = store.GetState("wargm-delivered"),
    confirmed = store.GetState("wargm-confirmed"),
    log = store.GetState("wargm-shop")
}));
app.MapPost("/api/gamestores/sync", async (GameStoresDeliveryService delivery) =>
    ApiEnvelope.Ok(await delivery.SyncPendingAsync("panel-sync")));
app.MapPost("/api/gamestores/deliver-pending", async (GameStoresDeliveryService delivery) =>
    ApiEnvelope.Ok(await delivery.DeliverPendingAsync("panel-deliver-pending")));
app.MapPost("/api/gamestores/confirm-delivered", async (GameStoresDeliveryService delivery) =>
    ApiEnvelope.Ok(await delivery.ConfirmDeliveredAsync("panel-confirm")));
app.MapPost("/api/gamestores/process", async (GameStoresDeliveryService delivery) =>
    ApiEnvelope.Ok(await delivery.ProcessAsync("panel-process")));
app.MapPost("/api/gamestores/claim", async (GameStoresClaimRequest request, GameStoresDeliveryService delivery) =>
    ApiEnvelope.Ok(await delivery.ClaimForPlayerAsync(request, "panel-claim")));
app.MapGet("/api/gamestores/pending", (PluginConfigStore store) => ApiEnvelope.Ok(new
{
    pending = store.GetState("gamestores-pending"),
    delivered = store.GetState("gamestores-delivered"),
    confirmed = store.GetState("gamestores-confirmed"),
    attempts = store.GetState("gamestores-attempts"),
    log = store.GetState("gamestores-shop")
}));
app.MapGet("/api/discord/status", (DiscordLogService discord) =>
    ApiEnvelope.Ok(discord.GetStatus()));
app.MapPost("/api/discord/test", async (DiscordTestRequest request, DiscordLogService discord) =>
    ApiEnvelope.Ok(await discord.QueueTestAsync(request.Route, request.Message)));
app.MapGet("/api/discord-log-bridge/status", (DiscordLogService discord) =>
    ApiEnvelope.Ok(discord.GetStatus()));
app.MapPost("/api/discord-log-bridge/test", async (DiscordTestRequest request, DiscordLogService discord) =>
    ApiEnvelope.Ok(await discord.QueueTestAsync(request.Route, request.Message)));
app.MapGet("/api/discord-log-bridge/channels", (PluginConfigStore store) =>
    ApiEnvelope.Ok(DiscordChannels(store)));
app.MapGet("/api/discord-log-bridge/secret", (PluginConfigStore store) =>
    ApiEnvelope.Ok(DiscordSecretStatus(store)));
app.MapPost("/api/discord-log-bridge/secret", async (DiscordSecretRequest request, PluginConfigStore store) =>
    ApiEnvelope.Ok(await SaveDiscordSecretAsync(request, store)));

app.MapFallbackToFile("index.html");

app.Run();

static async Task<object> SignalNativeWargmAsync(PluginConfigStore store, string signal, string source)
{
    await store.SetStateAsync(signal, new
    {
        utc = DateTimeOffset.UtcNow,
        source
    });

    return new
    {
        accepted = true,
        delegated = "native-wargm-worker",
        signal,
        source
    };
}
