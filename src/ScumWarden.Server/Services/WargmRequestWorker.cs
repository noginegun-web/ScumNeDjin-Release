using System.Text.Json;

namespace ScumWarden.Server.Services;

public sealed class WargmRequestWorker : BackgroundService
{
    private readonly WargmShopService _wargm;
    private readonly WargmDeliveryService _delivery;
    private readonly PluginConfigStore _store;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private DateTimeOffset _nextPoll = DateTimeOffset.MinValue;

    public WargmRequestWorker(WargmShopService wargm, WargmDeliveryService delivery, PluginConfigStore store)
    {
        _wargm = wargm;
        _delivery = delivery;
        _store = store;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                var syncRequested = await _store.TryConsumeStateSignalAsync("wargm-sync-request");
                var confirmRequested = await _store.TryConsumeStateSignalAsync("wargm-confirm-request");
                var pollDue = PollDue();

                if (syncRequested || pollDue)
                {
                    await RunSyncAsync(syncRequested ? "game-request" : "poll", stoppingToken);
                }

                if (confirmRequested)
                {
                    await RunConfirmAsync("game-request", stoppingToken);
                }
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception ex)
            {
                await _store.AppendStateAsync("wargm-shop", new
                {
                    utc = DateTimeOffset.UtcNow,
                    status = "worker-error",
                    message = Trim(ex.Message, 600)
                });
            }

            await Task.Delay(1000, stoppingToken);
        }
    }

    private bool PollDue()
    {
        var config = _store.GetConfig("wargm-shop");
        if (!Bool(config, "Enabled", false))
        {
            return false;
        }

        var interval = Int(config, "PollIntervalSeconds", 0);
        if (interval <= 0)
        {
            return false;
        }

        var now = DateTimeOffset.UtcNow;
        if (now < _nextPoll)
        {
            return false;
        }

        _nextPoll = now.AddSeconds(Math.Clamp(interval, 5, 3600));
        return true;
    }

    private async Task RunSyncAsync(string source, CancellationToken cancellationToken)
    {
        if (!await _gate.WaitAsync(0, cancellationToken))
        {
            await _store.AppendStateAsync("wargm-shop", new
            {
                utc = DateTimeOffset.UtcNow,
                status = "sync-skipped-busy",
                source
            });
            return;
        }

        try
        {
            await _store.AppendStateAsync("wargm-shop", new
            {
                utc = DateTimeOffset.UtcNow,
                status = "sync-started",
                source
            });
            var result = await _wargm.SyncPendingAsync(cancellationToken);
            var deliveryResult = await _delivery.DeliverPendingAsync(source, cancellationToken);
            var confirmResult = await _wargm.ConfirmDeliveredAsync(cancellationToken);
            await _store.AppendStateAsync("wargm-shop", new
            {
                utc = DateTimeOffset.UtcNow,
                status = "sync-finished",
                source,
                result,
                deliveryResult,
                confirmResult
            });
        }
        finally
        {
            _gate.Release();
        }
    }

    private async Task RunConfirmAsync(string source, CancellationToken cancellationToken)
    {
        if (!await _gate.WaitAsync(0, cancellationToken))
        {
            await _store.AppendStateAsync("wargm-shop", new
            {
                utc = DateTimeOffset.UtcNow,
                status = "confirm-skipped-busy",
                source
            });
            return;
        }

        try
        {
            await _store.AppendStateAsync("wargm-shop", new
            {
                utc = DateTimeOffset.UtcNow,
                status = "confirm-started",
                source
            });
            var result = await _wargm.ConfirmDeliveredAsync(cancellationToken);
            await _store.AppendStateAsync("wargm-shop", new
            {
                utc = DateTimeOffset.UtcNow,
                status = "confirm-finished",
                source,
                result
            });
        }
        finally
        {
            _gate.Release();
        }
    }

    private static bool Bool(JsonElement element, string name, bool fallback)
    {
        if (!TryGet(element, name, out var value))
        {
            return fallback;
        }

        if (value.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            return value.GetBoolean();
        }

        return bool.TryParse(value.ToString(), out var parsed) ? parsed : fallback;
    }

    private static int Int(JsonElement element, string name, int fallback)
    {
        if (!TryGet(element, name, out var value))
        {
            return fallback;
        }

        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number))
        {
            return number;
        }

        return int.TryParse(value.ToString(), out number) ? number : fallback;
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

    private static string Trim(string value, int max) => value.Length <= max ? value : value[..max];
}
