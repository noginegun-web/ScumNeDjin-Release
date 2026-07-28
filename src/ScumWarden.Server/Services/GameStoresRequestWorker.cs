namespace ScumWarden.Server.Services;

public sealed class GameStoresRequestWorker : BackgroundService
{
    private readonly GameStoresDeliveryService _delivery;
    private readonly PluginConfigStore _store;
    private readonly ILogger<GameStoresRequestWorker> _logger;

    public GameStoresRequestWorker(GameStoresDeliveryService delivery, PluginConfigStore store, ILogger<GameStoresRequestWorker> logger)
    {
        _delivery = delivery;
        _store = store;
        _logger = logger;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var lastPollUtc = DateTimeOffset.MinValue;
        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(1), stoppingToken);
                var config = _store.GetConfig("gamestores-shop");
                var enabled = ConfigBool(config, "Enabled", false);
                var intervalSeconds = Math.Clamp(ConfigInt(config, "PollIntervalSeconds", 0), 0, 86400);
                var syncRequested = await _store.TryConsumeStateSignalAsync("gamestores-sync-request");
                var confirmRequested = await _store.TryConsumeStateSignalAsync("gamestores-confirm-request");
                var claimRequest = await _store.TryConsumeStateSignalJsonAsync("gamestores-claim-request");
                var requirePlayerClaim = ConfigBool(config, "RequirePlayerClaim", true);
                var due = enabled &&
                    intervalSeconds > 0 &&
                    (DateTimeOffset.UtcNow - lastPollUtc).TotalSeconds >= intervalSeconds;

                if (claimRequest is { ValueKind: System.Text.Json.JsonValueKind.Object } claim)
                {
                    var result = await _delivery.ClaimForPlayerAsync(new GameStoresClaimRequest(
                        String(claim, "steamId", "steam", "SteamId", "SteamID"),
                        String(claim, "name", "Name", "playerName", "PlayerName"),
                        String(claim, "runtimeKey", "RuntimeKey", "key", "Key")), "player-claim", stoppingToken);
                    await _store.AppendStateAsync("gamestores-shop", new
                    {
                        utc = DateTimeOffset.UtcNow,
                        at = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                        status = "worker-claim-finished",
                        result
                    });
                }

                if (syncRequested || due)
                {
                    lastPollUtc = DateTimeOffset.UtcNow;
                    var result = requirePlayerClaim && !syncRequested
                        ? await _delivery.SyncPendingAsync("poll-sync-only", stoppingToken)
                        : await _delivery.ProcessAsync(syncRequested ? "panel-sync" : "poll", stoppingToken);
                    await _store.AppendStateAsync("gamestores-shop", new
                    {
                        utc = DateTimeOffset.UtcNow,
                        at = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                        status = "worker-sync-finished",
                        result
                    });
                }

                if (confirmRequested)
                {
                    var result = await _delivery.ConfirmDeliveredAsync("panel-confirm", stoppingToken);
                    await _store.AppendStateAsync("gamestores-shop", new
                    {
                        utc = DateTimeOffset.UtcNow,
                        at = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                        status = "worker-confirm-finished",
                        result
                    });
                }
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "GameStores worker tick failed.");
                try
                {
                    await _store.AppendStateAsync("gamestores-shop", new
                    {
                        utc = DateTimeOffset.UtcNow,
                        at = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                        status = "worker-error",
                        message = ex.Message
                    });
                }
                catch
                {
                    // The worker must not crash the web host because state logging failed.
                }
            }
        }
    }

    private static bool ConfigBool(System.Text.Json.JsonElement element, string name, bool fallback)
    {
        if (element.ValueKind != System.Text.Json.JsonValueKind.Object)
        {
            return fallback;
        }

        foreach (var property in element.EnumerateObject())
        {
            if (!property.Name.Equals(name, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            return property.Value.ValueKind switch
            {
                System.Text.Json.JsonValueKind.True => true,
                System.Text.Json.JsonValueKind.False => false,
                System.Text.Json.JsonValueKind.String when bool.TryParse(property.Value.GetString(), out var parsed) => parsed,
                _ => fallback
            };
        }

        return fallback;
    }

    private static int ConfigInt(System.Text.Json.JsonElement element, string name, int fallback)
    {
        if (element.ValueKind != System.Text.Json.JsonValueKind.Object)
        {
            return fallback;
        }

        foreach (var property in element.EnumerateObject())
        {
            if (!property.Name.Equals(name, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            if (property.Value.ValueKind == System.Text.Json.JsonValueKind.Number &&
                property.Value.TryGetInt32(out var number))
            {
                return number;
            }

            if (property.Value.ValueKind == System.Text.Json.JsonValueKind.String &&
                int.TryParse(property.Value.GetString(), out var parsed))
            {
                return parsed;
            }
        }

        return fallback;
    }

    private static string String(System.Text.Json.JsonElement element, params string[] names)
    {
        if (element.ValueKind != System.Text.Json.JsonValueKind.Object)
        {
            return "";
        }

        foreach (var property in element.EnumerateObject())
        {
            if (!names.Any(name => property.Name.Equals(name, StringComparison.OrdinalIgnoreCase)))
            {
                continue;
            }

            return property.Value.ValueKind switch
            {
                System.Text.Json.JsonValueKind.String => property.Value.GetString()?.Trim() ?? "",
                System.Text.Json.JsonValueKind.Number => property.Value.ToString().Trim(),
                _ => ""
            };
        }

        return "";
    }
}
