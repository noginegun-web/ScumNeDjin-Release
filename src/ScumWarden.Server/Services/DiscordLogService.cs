using System.Collections.Concurrent;
using System.Globalization;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

namespace ScumWarden.Server.Services;

public sealed class DiscordLogService : BackgroundService
{
    private readonly ConcurrentQueue<DiscordDispatchItem> _queue = new();
    private readonly Queue<string> _seenOrder = new();
    private readonly HashSet<string> _seen = new(StringComparer.Ordinal);
    private readonly object _seenGate = new();
    private readonly IHttpClientFactory _httpClientFactory;
    private readonly WardenEventStore _events;
    private readonly PluginConfigStore _store;

    private long _enqueued;
    private long _sent;
    private long _failed;
    private long _skipped;
    private DateTimeOffset? _lastSentUtc;
    private DateTimeOffset? _lastFailedUtc;
    private DateTimeOffset? _lastSkippedUtc;
    private string _lastRoute = "";
    private string _lastTarget = "";
    private string _lastError = "";
    private string _lastSkipReason = "";
    private bool _startupQueued;

    public DiscordLogService(IHttpClientFactory httpClientFactory, WardenEventStore events, PluginConfigStore store)
    {
        _httpClientFactory = httpClientFactory;
        _events = events;
        _store = store;
    }

    public object GetStatus()
    {
        var config = _store.GetConfig("discord-log");
        var mode = Str(config, "TransportMode", "transportMode");
        if (mode.Length == 0)
        {
            mode = "Webhook";
        }

        return new
        {
            enabled = Bool(config, "Enabled", "enabled", false),
            transportMode = mode,
            queueLength = _queue.Count,
            counts = new
            {
                enqueued = Interlocked.Read(ref _enqueued),
                sent = Interlocked.Read(ref _sent),
                failed = Interlocked.Read(ref _failed),
                skipped = Interlocked.Read(ref _skipped)
            },
            last = new
            {
                sentUtc = _lastSentUtc,
                failedUtc = _lastFailedUtc,
                skippedUtc = _lastSkippedUtc,
                route = _lastRoute,
                target = _lastTarget,
                error = _lastError,
                skipReason = _lastSkipReason
            },
            routes = new[]
            {
                RouteStatus(config, DiscordRoute.System),
                RouteStatus(config, DiscordRoute.Presence),
                RouteStatus(config, DiscordRoute.Chat),
                RouteStatus(config, DiscordRoute.Combat)
            }
        };
    }

    public async Task<object> QueueTestAsync(string? routeName, string? message)
    {
        var config = _store.GetConfig("discord-log");
        if (!Bool(config, "Enabled", "enabled", false))
        {
            return new { queued = false, reason = "Discord Log выключен в настройках." };
        }

        var route = ParseRoute(routeName);
        var target = ResolveTarget(config, route);
        if (!target.IsValid)
        {
            return new { queued = false, reason = "Для выбранного Discord-канала не настроен webhook или bot channel.", route = RouteName(route) };
        }

        var item = new DiscordDispatchItem(
            Fingerprint: "manual|" + RouteName(route) + "|" + DateTimeOffset.UtcNow.ToUnixTimeMilliseconds().ToString(CultureInfo.InvariantCulture),
            Route: route,
            Title: "Проверка Discord",
            Description: string.IsNullOrWhiteSpace(message) ? "Ручная проверка отправки из панели ScumNeDjin." : message.Trim(),
            Color: 0x00BCD4,
            Source: "panel-test");

        if (!Enqueue(config, item))
        {
            return new { queued = false, reason = "Очередь Discord переполнена.", route = RouteName(route) };
        }

        await _store.AppendStateAsync("discord-log", new
        {
            utc = DateTimeOffset.UtcNow,
            status = "queued",
            route = RouteName(route),
            source = "panel-test",
            message = item.Description
        });

        return new { queued = true, route = RouteName(route), queueLength = _queue.Count };
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                var config = _store.GetConfig("discord-log");
                if (Bool(config, "Enabled", "enabled", false))
                {
                    QueueStartupIfNeeded(config);
                    QueueNewEvents(config);
                    QueueLuaDiscordState(config);
                    await DrainQueueAsync(config, stoppingToken);
                }

                await Task.Delay(1000, stoppingToken);
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception ex)
            {
                RecordFailure("system", "loop", ex.Message);
                await _store.AppendStateAsync("discord-log", new
                {
                    utc = DateTimeOffset.UtcNow,
                    status = "service-error",
                    message = Trim(ex.Message, 500)
                });
                await Task.Delay(2000, stoppingToken);
            }
        }
    }

    private void QueueStartupIfNeeded(JsonElement config)
    {
        if (_startupQueued || !Bool(config, "NotifyOnLoad", "notifyOnLoad", true))
        {
            return;
        }

        _startupQueued = true;
        Enqueue(config, new DiscordDispatchItem(
            Fingerprint: "system|startup|" + DateTimeOffset.UtcNow.ToUnixTimeSeconds().ToString(CultureInfo.InvariantCulture),
            Route: DiscordRoute.System,
            Title: "ScumNeDjin запущен",
            Description: "Discord Log готов принимать события сервера.",
            Color: 0x57F287,
            Source: "startup"));
    }

    private void QueueNewEvents(JsonElement config)
    {
        foreach (var entry in _events.Get(null, 500).Reverse())
        {
            var item = BuildFromEvent(config, entry);
            if (item is null)
            {
                continue;
            }

            Enqueue(config, item);
        }
    }

    private void QueueLuaDiscordState(JsonElement config)
    {
        var state = _store.GetState("discord-log");
        if (state.ValueKind != JsonValueKind.Array)
        {
            return;
        }

        foreach (var row in state.EnumerateArray().TakeLast(200))
        {
            var status = Str(row, "status").ToLowerInvariant();
            if (status is "sent" or "failed" or "queued" or "skipped" or "service-error")
            {
                continue;
            }

            var key = "lua|" + Fingerprint(row.GetRawText());
            if (AlreadySeen(key))
            {
                continue;
            }

            var route = ParseRoute(Str(row, "route", "feed"));
            var note = First(Str(row, "message"), Str(row, "note"), Str(row, "status"));
            if (note.Length == 0)
            {
                note = row.GetRawText();
            }

            Enqueue(config, new DiscordDispatchItem(
                Fingerprint: key,
                Route: route,
                Title: "Событие Discord Log",
                Description: Trim(note, 1400),
                Color: 0x5865F2,
                Source: "lua-state"));
        }
    }

    private DiscordDispatchItem? BuildFromEvent(JsonElement config, WardenEvent entry)
    {
        var fingerprint = EventFingerprint(config, entry);
        if (AlreadySeen(fingerprint))
        {
            return null;
        }

        return entry.Type.ToLowerInvariant() switch
        {
            "chat" => BuildChat(config, entry, fingerprint),
            "login" => BuildPresence(config, entry, fingerprint),
            "kill" => BuildKill(config, entry, fingerprint),
            _ => null
        };
    }

    private DiscordDispatchItem? BuildChat(JsonElement config, WardenEvent entry, string fingerprint)
    {
        if (!Bool(config, "NotifyPlayerChat", "notifyPlayerChat", true))
        {
            return null;
        }

        var message = entry.Message.Trim();
        if (message.Length == 0 || ShouldIgnoreChat(config, message))
        {
            return null;
        }

        var channel = NormalizeChannel(DataString(entry, "channel"));
        if (!ShouldForwardChannel(config, channel))
        {
            return null;
        }

        var name = First(DataString(entry, "name"), DataString(entry, "player"), "Игрок");
        var steam = DataString(entry, "steamId");
        var description = "**" + EscapeDiscord(name) + "** [" + EscapeDiscord(ChannelName(channel)) + "]\n" + EscapeDiscord(Trim(message, 1400));
        if (Bool(config, "IncludeSteamId", "includeSteamId", true) && steam.Length > 0)
        {
            description += "\n`SteamID: " + steam + "`";
        }

        return new DiscordDispatchItem(fingerprint, DiscordRoute.Chat, "Чат: " + ChannelName(channel), description, 0x5865F2, "event-chat");
    }

    private DiscordDispatchItem? BuildPresence(JsonElement config, WardenEvent entry, string fingerprint)
    {
        var state = DataString(entry, "state");
        var joined = state.Equals("joined", StringComparison.OrdinalIgnoreCase) || entry.Message.Contains("logged in", StringComparison.OrdinalIgnoreCase);
        if (joined && !Bool(config, "NotifyPlayerConnected", "notifyPlayerConnected", true))
        {
            return null;
        }

        if (!joined && !Bool(config, "NotifyPlayerDisconnected", "notifyPlayerDisconnected", true))
        {
            return null;
        }

        var name = First(DataString(entry, "name"), DataString(entry, "player"), "Игрок");
        var steam = DataString(entry, "steamId");
        var description = "**" + EscapeDiscord(name) + "**";
        if (Bool(config, "IncludeSteamId", "includeSteamId", true) && steam.Length > 0)
        {
            description += "\n`SteamID: " + steam + "`";
        }

        return new DiscordDispatchItem(
            fingerprint,
            DiscordRoute.Presence,
            joined ? "Игрок подключился" : "Игрок отключился",
            description,
            joined ? 0x57F287 : 0xED4245,
            "event-presence");
    }

    private DiscordDispatchItem? BuildKill(JsonElement config, WardenEvent entry, string fingerprint)
    {
        if (!Bool(config, "NotifyPlayerKills", "notifyPlayerKills", true) && !Bool(config, "NotifyPlayerDeaths", "notifyPlayerDeaths", true))
        {
            return null;
        }

        var killer = First(DataString(entry, "killer"), "Неизвестно");
        var victim = First(DataString(entry, "victim"), "Неизвестно");
        var killerSteam = DataString(entry, "killerSteamId");
        var victimSteam = DataString(entry, "victimSteamId");
        var weapon = DataString(entry, "weapon");
        var distance = DataString(entry, "distance");
        var includeSteam = Bool(config, "IncludeSteamId", "includeSteamId", true);
        var description = "**" + EscapeDiscord(killer) + "** -> **" + EscapeDiscord(victim) + "**";
        if (weapon.Length > 0)
        {
            description += "\nОружие: `" + EscapeDiscord(weapon) + "`";
        }

        if (distance.Length > 0)
        {
            description += "\nДистанция: `" + EscapeDiscord(distance) + "`";
        }

        if (includeSteam)
        {
            if (killerSteam.Length > 0)
            {
                description += "\nKiller SteamID: `" + killerSteam + "`";
            }

            if (victimSteam.Length > 0)
            {
                description += "\nVictim SteamID: `" + victimSteam + "`";
            }
        }

        return new DiscordDispatchItem(fingerprint, DiscordRoute.Combat, "PvP убийство", description, 0xFEE75C, "event-kill");
    }

    private bool Enqueue(JsonElement config, DiscordDispatchItem item)
    {
        if (!Remember(item.Fingerprint))
        {
            return false;
        }

        var maxQueue = Math.Clamp(Int(config, "MaxQueueLength", "maxQueueLength", 200), 10, 2000);
        if (_queue.Count >= maxQueue)
        {
            RecordSkip(item, "Очередь Discord переполнена.", "");
            return false;
        }

        if (!ResolveTarget(config, item.Route).IsValid)
        {
            RecordSkip(item, "Для маршрута Discord не настроено назначение.", RouteName(item.Route));
            return false;
        }

        _queue.Enqueue(item);
        Interlocked.Increment(ref _enqueued);
        _lastRoute = RouteName(item.Route);
        return true;
    }

    private async Task DrainQueueAsync(JsonElement config, CancellationToken cancellationToken)
    {
        var minInterval = Math.Clamp(Int(config, "MinPostIntervalMs", "minPostIntervalMs", 800), 0, 60000);
        var deadline = DateTimeOffset.UtcNow.AddSeconds(2);
        while (DateTimeOffset.UtcNow < deadline && _queue.TryDequeue(out var item))
        {
            var target = ResolveTarget(config, item.Route);
            if (!target.IsValid)
            {
                RecordSkip(item, "Назначение Discord больше не настроено.", RouteName(item.Route));
                continue;
            }

            try
            {
                await SendAsync(config, target, item, cancellationToken);
                RecordSent(item, target);
                await _store.AppendStateAsync("discord-log", new
                {
                    utc = DateTimeOffset.UtcNow,
                    status = "sent",
                    route = RouteName(item.Route),
                    source = item.Source,
                    title = item.Title,
                    message = Trim(item.Description, 500)
                });
            }
            catch (Exception ex)
            {
                RecordFailure(RouteName(item.Route), DescribeTarget(target), ex.Message);
                await _store.AppendStateAsync("discord-log", new
                {
                    utc = DateTimeOffset.UtcNow,
                    status = "failed",
                    route = RouteName(item.Route),
                    source = item.Source,
                    error = Trim(ex.Message, 500)
                });
            }

            if (minInterval > 0)
            {
                await Task.Delay(minInterval, cancellationToken);
            }
        }
    }

    private async Task SendAsync(JsonElement config, DiscordTarget target, DiscordDispatchItem item, CancellationToken cancellationToken)
    {
        using var client = _httpClientFactory.CreateClient(nameof(DiscordLogService));
        client.Timeout = TimeSpan.FromSeconds(Math.Clamp(Int(config, "HttpTimeoutSeconds", "httpTimeoutSeconds", 10), 3, 60));

        var payload = BuildPayload(config, item);
        using var request = new HttpRequestMessage(HttpMethod.Post, target.Url)
        {
            Content = new StringContent(JsonSerializer.Serialize(payload, WardenJson.Options), Encoding.UTF8, "application/json")
        };

        if (target.Kind == DiscordTargetKind.BotChannel)
        {
            request.Headers.Authorization = new AuthenticationHeaderValue("Bot", target.Token);
        }

        using var response = await client.SendAsync(request, cancellationToken);
        if (!response.IsSuccessStatusCode)
        {
            var body = await response.Content.ReadAsStringAsync(cancellationToken);
            throw new InvalidOperationException("Discord HTTP " + (int)response.StatusCode + ": " + Trim(body, 500));
        }
    }

    private object BuildPayload(JsonElement config, DiscordDispatchItem item)
    {
        var footer = Str(config, "ServerLabel", "serverLabel");
        var embed = new Dictionary<string, object?>
        {
            ["title"] = Trim(item.Title, 240),
            ["description"] = Trim(item.Description, 3900),
            ["color"] = item.Color,
            ["timestamp"] = DateTimeOffset.UtcNow.ToString("O", CultureInfo.InvariantCulture)
        };
        if (footer.Length > 0)
        {
            embed["footer"] = new { text = footer + " / " + item.Source };
        }

        var payload = new Dictionary<string, object?>
        {
            ["content"] = "",
            ["embeds"] = new[] { embed }
        };

        var username = Str(config, "Username", "username");
        if (username.Length > 0)
        {
            payload["username"] = Trim(username, 80);
        }

        var avatar = Str(config, "AvatarUrl", "avatarUrl");
        if (avatar.StartsWith("http://", StringComparison.OrdinalIgnoreCase) || avatar.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
        {
            payload["avatar_url"] = avatar;
        }

        payload["allowed_mentions"] = new { parse = Array.Empty<string>() };
        return payload;
    }

    private DiscordTarget ResolveTarget(JsonElement config, DiscordRoute route)
    {
        var mode = Str(config, "TransportMode", "transportMode");
        if (mode.Equals("BotChannel", StringComparison.OrdinalIgnoreCase))
        {
            var channel = route switch
            {
                DiscordRoute.Presence => First(Str(config, "PresenceChannelId", "presenceChannelId"), Str(config, "DefaultChannelId", "defaultChannelId")),
                DiscordRoute.Chat => First(Str(config, "ChatChannelId", "chatChannelId"), Str(config, "DefaultChannelId", "defaultChannelId")),
                DiscordRoute.Combat => First(Str(config, "CombatChannelId", "combatChannelId"), Str(config, "DefaultChannelId", "defaultChannelId")),
                _ => First(Str(config, "SystemChannelId", "systemChannelId"), Str(config, "DefaultChannelId", "defaultChannelId"))
            };
            var token = First(Str(config, "BotToken", "botToken"), Environment.GetEnvironmentVariable("SCUM_NEDJIN_DISCORD_BOT_TOKEN") ?? "");
            if (channel.All(char.IsDigit) && channel.Length > 10 && token.Length > 0)
            {
                return new DiscordTarget(DiscordTargetKind.BotChannel, "https://discord.com/api/v10/channels/" + channel + "/messages", token);
            }

            return DiscordTarget.Invalid;
        }

        var webhook = route switch
        {
            DiscordRoute.Presence => First(Str(config, "PresenceWebhookUrl", "presenceWebhookUrl"), Str(config, "DefaultWebhookUrl", "defaultWebhookUrl")),
            DiscordRoute.Chat => First(Str(config, "ChatWebhookUrl", "chatWebhookUrl"), Str(config, "DefaultWebhookUrl", "defaultWebhookUrl")),
            DiscordRoute.Combat => First(Str(config, "CombatWebhookUrl", "combatWebhookUrl"), Str(config, "DefaultWebhookUrl", "defaultWebhookUrl")),
            _ => First(Str(config, "SystemWebhookUrl", "systemWebhookUrl"), Str(config, "DefaultWebhookUrl", "defaultWebhookUrl"))
        };

        return webhook.StartsWith("https://discord.com/api/webhooks/", StringComparison.OrdinalIgnoreCase) ||
               webhook.StartsWith("https://discordapp.com/api/webhooks/", StringComparison.OrdinalIgnoreCase)
            ? new DiscordTarget(DiscordTargetKind.Webhook, webhook, "")
            : DiscordTarget.Invalid;
    }

    private object RouteStatus(JsonElement config, DiscordRoute route)
    {
        var target = ResolveTarget(config, route);
        return new
        {
            route = RouteName(route),
            configured = target.IsValid,
            target = DescribeTarget(target)
        };
    }

    private bool Remember(string fingerprint)
    {
        lock (_seenGate)
        {
            if (!_seen.Add(fingerprint))
            {
                return false;
            }

            _seenOrder.Enqueue(fingerprint);
            while (_seenOrder.Count > 3000)
            {
                _seen.Remove(_seenOrder.Dequeue());
            }

            return true;
        }
    }

    private bool AlreadySeen(string fingerprint)
    {
        lock (_seenGate)
        {
            return _seen.Contains(fingerprint);
        }
    }

    private void RecordSent(DiscordDispatchItem item, DiscordTarget target)
    {
        Interlocked.Increment(ref _sent);
        _lastSentUtc = DateTimeOffset.UtcNow;
        _lastRoute = RouteName(item.Route);
        _lastTarget = DescribeTarget(target);
        _lastError = "";
        _lastSkipReason = "";
    }

    private void RecordFailure(string route, string target, string error)
    {
        Interlocked.Increment(ref _failed);
        _lastFailedUtc = DateTimeOffset.UtcNow;
        _lastRoute = route;
        _lastTarget = target;
        _lastError = Trim(error, 500);
    }

    private void RecordSkip(DiscordDispatchItem item, string reason, string target)
    {
        Interlocked.Increment(ref _skipped);
        _lastSkippedUtc = DateTimeOffset.UtcNow;
        _lastRoute = RouteName(item.Route);
        _lastTarget = target;
        _lastSkipReason = Trim(reason, 200);
    }

    private static bool ShouldIgnoreChat(JsonElement config, string message)
    {
        if (!Bool(config, "IgnoreSlashCommands", "ignoreSlashCommands", true))
        {
            return false;
        }

        var prefixes = StringArray(config, "IgnoredChatPrefixes", "ignoredChatPrefixes").DefaultIfEmpty("/").ToArray();
        return prefixes.Any(prefix => prefix.Length > 0 && message.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
    }

    private static bool ShouldForwardChannel(JsonElement config, string channel)
    {
        var normalized = NormalizeChannel(channel);
        if (normalized.Contains("local", StringComparison.OrdinalIgnoreCase) || normalized.Contains("лок", StringComparison.OrdinalIgnoreCase))
        {
            return Bool(config, "ForwardLocalChat", "forwardLocalChat", true);
        }

        if (normalized.Contains("squad", StringComparison.OrdinalIgnoreCase) || normalized.Contains("отряд", StringComparison.OrdinalIgnoreCase))
        {
            return Bool(config, "ForwardSquadChat", "forwardSquadChat", true);
        }

        if (normalized.Contains("admin", StringComparison.OrdinalIgnoreCase) || normalized.Contains("админ", StringComparison.OrdinalIgnoreCase))
        {
            return Bool(config, "ForwardAdminChat", "forwardAdminChat", false);
        }

        return Bool(config, "ForwardGlobalChat", "forwardGlobalChat", true);
    }

    private static string ChannelName(string channel)
    {
        var normalized = NormalizeChannel(channel);
        return normalized.Length == 0 ? "Global" : normalized;
    }

    private static string DataString(WardenEvent entry, string key)
    {
        return entry.Data.TryGetValue(key, out var value) ? Convert.ToString(value, CultureInfo.InvariantCulture)?.Trim() ?? "" : "";
    }

    private static DiscordRoute ParseRoute(string? route)
    {
        return (route ?? "").Trim().ToLowerInvariant() switch
        {
            "presence" or "player" or "players" or "входы" => DiscordRoute.Presence,
            "chat" or "чат" => DiscordRoute.Chat,
            "combat" or "kill" or "kills" or "pvp" or "бои" => DiscordRoute.Combat,
            _ => DiscordRoute.System
        };
    }

    private static string RouteName(DiscordRoute route)
    {
        return route switch
        {
            DiscordRoute.Presence => "presence",
            DiscordRoute.Chat => "chat",
            DiscordRoute.Combat => "combat",
            _ => "system"
        };
    }

    private static string DescribeTarget(DiscordTarget target)
    {
        if (!target.IsValid)
        {
            return "not-configured";
        }

        return target.Kind == DiscordTargetKind.BotChannel ? "bot-channel" : "webhook";
    }

    private static bool TryGet(JsonElement element, string propertyName, out JsonElement value)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            foreach (var property in element.EnumerateObject())
            {
                if (property.Name.Equals(propertyName, StringComparison.OrdinalIgnoreCase))
                {
                    value = property.Value;
                    return true;
                }
            }
        }

        value = default;
        return false;
    }

    private static string Str(JsonElement element, params string[] names)
    {
        foreach (var name in names)
        {
            if (!TryGet(element, name, out var value))
            {
                continue;
            }

            return value.ValueKind switch
            {
                JsonValueKind.String => value.GetString()?.Trim() ?? "",
                JsonValueKind.Number => value.ToString().Trim(),
                JsonValueKind.True => "true",
                JsonValueKind.False => "false",
                _ => ""
            };
        }

        return "";
    }

    private static int Int(JsonElement element, string upper, string lower, int fallback)
    {
        var text = Str(element, upper, lower);
        return int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value) ? value : fallback;
    }

    private static bool Bool(JsonElement element, string upper, string lower, bool fallback)
    {
        if (!(TryGet(element, upper, out var value) || TryGet(element, lower, out value)))
        {
            return fallback;
        }

        if (value.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            return value.GetBoolean();
        }

        if (value.ValueKind == JsonValueKind.String)
        {
            return bool.TryParse(value.GetString(), out var result) ? result : fallback;
        }

        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var n))
        {
            return n != 0;
        }

        return fallback;
    }

    private static IReadOnlyList<string> StringArray(JsonElement element, string upper, string lower)
    {
        if (!(TryGet(element, upper, out var value) || TryGet(element, lower, out value)) || value.ValueKind != JsonValueKind.Array)
        {
            return Array.Empty<string>();
        }

        return value.EnumerateArray()
            .Select(item => item.ValueKind == JsonValueKind.String ? item.GetString()?.Trim() ?? "" : item.ToString().Trim())
            .Where(item => item.Length > 0)
            .ToArray();
    }

    private static string First(params string[] values) => values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? "";

    private static string EventFingerprint(JsonElement config, WardenEvent entry)
    {
        var type = NormalizeDedupeText(entry.Type);
        var bucket = entry.TimestampUtc.ToUnixTimeSeconds() / DuplicateWindowSeconds(config);
        var message = NormalizeDedupeText(entry.Message);

        if (type == "chat")
        {
            var channel = NormalizeChannel(DataString(entry, "channel"));
            var actor = First(DataString(entry, "steamId"), DataString(entry, "SteamId"), DataString(entry, "steam"), DataString(entry, "name"), DataString(entry, "player"));
            return "event|chat|" + bucket.ToString(CultureInfo.InvariantCulture) + "|" + channel + "|" + NormalizeDedupeText(actor) + "|" + Fingerprint(message);
        }

        if (type == "login" || type == "presence")
        {
            var actor = First(DataString(entry, "steamId"), DataString(entry, "SteamId"), DataString(entry, "steam"), DataString(entry, "name"), DataString(entry, "player"));
            var state = NormalizeDedupeText(DataString(entry, "state"));
            return "event|presence|" + bucket.ToString(CultureInfo.InvariantCulture) + "|" + NormalizeDedupeText(actor) + "|" + state + "|" + Fingerprint(message);
        }

        if (type == "kill")
        {
            var killer = First(DataString(entry, "killerSteamId"), DataString(entry, "killer"), DataString(entry, "attackerSteamId"), DataString(entry, "attacker"));
            var victim = First(DataString(entry, "victimSteamId"), DataString(entry, "victim"), DataString(entry, "targetSteamId"), DataString(entry, "target"));
            return "event|kill|" + bucket.ToString(CultureInfo.InvariantCulture) + "|" + NormalizeDedupeText(killer) + "|" + NormalizeDedupeText(victim) + "|" + Fingerprint(message);
        }

        return "event|" + type + "|" + bucket.ToString(CultureInfo.InvariantCulture) + "|" + Fingerprint(message);
    }

    private static int DuplicateWindowSeconds(JsonElement config) =>
        Math.Clamp(Int(config, "DuplicateWindowSeconds", "duplicateWindowSeconds", 15), 1, 300);

    private static string NormalizeChannel(string channel)
    {
        var normalized = NormalizeDedupeText(channel);
        return normalized switch
        {
            "0" => "local",
            "1" => "squad",
            "2" => "global",
            "3" => "admin",
            "4" => "command",
            "" or "chat" or "server" => "global",
            _ when normalized.Contains("local", StringComparison.Ordinal) ||
                   normalized.Contains("proximity", StringComparison.Ordinal) ||
                   normalized.Contains("vicinity", StringComparison.Ordinal) ||
                   normalized.Contains("лок", StringComparison.Ordinal) => "local",
            _ when normalized.Contains("squad", StringComparison.Ordinal) ||
                   normalized.Contains("team", StringComparison.Ordinal) ||
                   normalized.Contains("clan", StringComparison.Ordinal) ||
                   normalized.Contains("party", StringComparison.Ordinal) ||
                   normalized.Contains("отряд", StringComparison.Ordinal) => "squad",
            _ when normalized.Contains("admin", StringComparison.Ordinal) ||
                   normalized.Contains("moderator", StringComparison.Ordinal) ||
                   normalized.Contains("админ", StringComparison.Ordinal) => "admin",
            _ when normalized.Contains("command", StringComparison.Ordinal) => "command",
            _ when normalized.Contains("global", StringComparison.Ordinal) ||
                   normalized.Contains("world", StringComparison.Ordinal) ||
                   normalized.Contains("глоб", StringComparison.Ordinal) => "global",
            _ => normalized
        };
    }

    private static string NormalizeDedupeText(string? value) =>
        string.Join(' ', (value ?? "").Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            .ToLowerInvariant();

    private static string Trim(string? value, int max)
    {
        var text = value ?? "";
        return text.Length <= max ? text : text[..max];
    }

    private static string Fingerprint(string value) => Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes(value ?? "")))[..16];

    private static string EscapeDiscord(string value)
    {
        return (value ?? "")
            .Replace("\\", "\\\\", StringComparison.Ordinal)
            .Replace("*", "\\*", StringComparison.Ordinal)
            .Replace("_", "\\_", StringComparison.Ordinal)
            .Replace("`", "\\`", StringComparison.Ordinal)
            .Replace("~", "\\~", StringComparison.Ordinal)
            .Replace("|", "\\|", StringComparison.Ordinal)
            .Replace(">", "\\>", StringComparison.Ordinal)
            .Trim();
    }

    private enum DiscordRoute
    {
        System,
        Presence,
        Chat,
        Combat
    }

    private enum DiscordTargetKind
    {
        Invalid,
        Webhook,
        BotChannel
    }

    private sealed record DiscordDispatchItem(string Fingerprint, DiscordRoute Route, string Title, string Description, int Color, string Source);

    private sealed record DiscordTarget(DiscordTargetKind Kind, string Url, string Token)
    {
        public bool IsValid => Kind != DiscordTargetKind.Invalid && Url.Length > 0;
        public static readonly DiscordTarget Invalid = new(DiscordTargetKind.Invalid, "", "");
    }
}
