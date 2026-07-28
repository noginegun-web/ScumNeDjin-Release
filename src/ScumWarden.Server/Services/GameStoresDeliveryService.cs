using System.Globalization;
using System.Text.Json;

namespace ScumWarden.Server.Services;

public sealed class GameStoresDeliveryService
{
    private static readonly string[] AllSkillKeys =
    {
        "endurance",
        "running",
        "brawling",
        "melee weapons",
        "archery",
        "rifles",
        "handguns",
        "thievery",
        "demolition",
        "motorcycling",
        "driving",
        "throwing",
        "stealth",
        "awareness",
        "camouflage",
        "engineering",
        "sniping",
        "survival",
        "medical"
    };

    private readonly CommandRouter _router;
    private readonly GameStoresShopService _shop;
    private readonly PluginConfigStore _store;
    private readonly GameStoresDeliveryJournal _journal;
    // Pending orders are file-backed and the game command is not idempotent. Keep a full
    // sync/deliver/confirm operation atomic inside this single panel host.
    private readonly SemaphoreSlim _operationGate = new(1, 1);

    public GameStoresDeliveryService(CommandRouter router, GameStoresShopService shop, PluginConfigStore store)
    {
        _router = router;
        _shop = shop;
        _store = store;
        _journal = new GameStoresDeliveryJournal(store);
    }

    public Task<object> ProcessAsync(string source = "manual", CancellationToken cancellationToken = default) =>
        SerializeOperationAsync<object>(async () =>
        {
            var sync = await SyncPendingCoreAsync(source, cancellationToken);
            var delivery = await DeliverPendingCoreAsync(source, targetPlayer: null, manualClaim: false, cancellationToken);
            var confirm = await ConfirmDeliveredCoreAsync(source, cancellationToken);
            return new { sync, delivery, confirm };
        }, cancellationToken);

    public Task<object> SyncPendingAsync(string source = "manual", CancellationToken cancellationToken = default) =>
        SerializeOperationAsync<object>(() => SyncPendingCoreAsync(source, cancellationToken), cancellationToken);

    private async Task<object> SyncPendingCoreAsync(string source, CancellationToken cancellationToken)
    {
        var players = (await GetOnlinePlayersAsync())
            .Where(player => !string.IsNullOrWhiteSpace(player.SteamId))
            .ToArray();
        return await SyncPendingForPlayersAsync(source, players, cancellationToken);
    }

    public Task<object> ClaimForPlayerAsync(GameStoresClaimRequest request, string source = "player-claim", CancellationToken cancellationToken = default) =>
        SerializeOperationAsync<object>(() => ClaimForPlayerCoreAsync(request, source, cancellationToken), cancellationToken);

    private async Task<object> ClaimForPlayerCoreAsync(GameStoresClaimRequest request, string source, CancellationToken cancellationToken)
    {
        var config = _store.GetConfig("gamestores-shop");
        if (!Bool(config, "Enabled", false))
        {
            await AppendShopAsync("claim-failed", "", "", request.SteamId ?? "", request.Name ?? "", "Магазин GameStores выключен в настройках.");
            return new { claimed = false, reason = "Магазин GameStores выключен в настройках.", issued = 0 };
        }

        if (!HasCredentials(config))
        {
            await AppendShopAsync("claim-failed", "", "", request.SteamId ?? "", request.Name ?? "", "Не настроены ShopId/SecretKey GameStores.");
            return new { claimed = false, reason = "Не настроены ShopId/SecretKey GameStores.", issued = 0 };
        }

        var onlinePlayers = await GetOnlinePlayersAsync();
        var target = MatchOnlinePlayer(onlinePlayers, new WargmManualDeliverRequest(
            SteamId: request.SteamId,
            Name: request.Name,
            Mode: "item",
            RuntimeKey: request.RuntimeKey));
        if (target is null || string.IsNullOrWhiteSpace(target.SteamId))
        {
            await AppendShopAsync("claim-failed", "", "", request.SteamId ?? "", request.Name ?? "", "Игрок не найден среди live-игроков, выдача не отправлена в пустоту.");
            return new { claimed = false, reason = "Игрок не найден среди live-игроков.", issued = 0 };
        }

        await AppendShopAsync("claim-started", "", "", target.SteamId, target.Name, "Игрок запросил выдачу покупок GameStores.");
        var sync = await SyncPendingForPlayersAsync(source, new[] { target }, cancellationToken);
        var delivery = await DeliverPendingCoreAsync(source, target, manualClaim: true, cancellationToken);
        object? confirm = null;
        if (delivery.Issued > 0)
        {
            confirm = await ConfirmDeliveredCoreAsync(source, cancellationToken);
        }

        var status = delivery.Issued > 0 ? "claim-finished" : "claim-empty";
        var message = delivery.Issued > 0
            ? $"Покупки GameStores выданы: {delivery.Issued}."
            : "Покупок GameStores для выдачи не найдено.";
        await AppendShopAsync(status, "", "", target.SteamId, target.Name, message);
        return new { claimed = true, source, player = target, sync, delivery, confirm };
    }

    private async Task<object> SyncPendingForPlayersAsync(string source, GameStoresPlayerRef[] players, CancellationToken cancellationToken)
    {
        var config = _store.GetConfig("gamestores-shop");
        if (!Bool(config, "Enabled", false))
        {
            return new { synced = false, reason = "Магазин GameStores выключен в настройках.", players = 0, pending = 0 };
        }

        if (!HasCredentials(config))
        {
            return new { synced = false, reason = "Не настроены ShopId/SecretKey GameStores.", players = 0, pending = 0 };
        }

        var pending = ExistingPendingRows();
        var knownKeys = pending.Select(row => Str(row, "key")).Where(key => key.Length > 0).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var settledKeys = SettledKeys();
        await ReconcileDeliveredJournalAsync(source);

        var fetched = 0;
        var matched = 0;
        var noRule = 0;
        var apiFailures = 0;
        var skippedSettled = 0;

        foreach (var player in players)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = await _shop.GetItemsAsync(config, player.SteamId, cancellationToken);
            if (!result.Ok)
            {
                apiFailures++;
                await AppendShopAsync("api-error", "", "", player.SteamId, player.Name, result.Message);
                continue;
            }

            foreach (var item in result.Items)
            {
                fetched++;
                var key = ItemKey(config, item);
                var journal = _journal.Inspect(key);
                if (settledKeys.Contains(key) || journal.State != GameStoresJournalState.None || knownKeys.Contains(key))
                {
                    skippedSettled++;
                    continue;
                }

                var row = BuildPendingRow(config, item, player, key);
                if (row is null)
                {
                    noRule++;
                    await AppendShopAsync("no-rule", key, item.Id, player.SteamId, player.Name,
                        $"Нет правила выдачи GameStores для товара: {First(item.Name, item.ItemId, item.Command, item.Id)}.");
                    continue;
                }

                matched++;
                knownKeys.Add(key);
                pending.Add(row.Value.Clone());
            }
        }

        await _store.SetStateAsync("gamestores-pending", pending);
        await _store.AppendStateAsync("gamestores-shop", new
        {
            utc = DateTimeOffset.UtcNow,
            status = "synced",
            source,
            players = players.Length,
            fetched,
            matched,
            noRule,
            apiFailures,
            skippedSettled,
            pending = pending.Count
        });

        return new
        {
            synced = true,
            source,
            players = players.Length,
            fetched,
            matched,
            noRule,
            apiFailures,
            skippedSettled,
            pending = pending.Count
        };
    }

    public Task<object> DeliverPendingAsync(string source = "manual", CancellationToken cancellationToken = default) =>
        SerializeOperationAsync<object>(async () =>
            await DeliverPendingCoreAsync(source, targetPlayer: null, manualClaim: false, cancellationToken), cancellationToken);

    private async Task<DeliverySummary> DeliverPendingCoreAsync(string source, GameStoresPlayerRef? targetPlayer, bool manualClaim, CancellationToken cancellationToken)
    {
        var config = _store.GetConfig("gamestores-shop");
        if (!Bool(config, "Enabled", false))
        {
            return new DeliverySummary(false, source, 0, 0, 0, 0, 0, "Магазин GameStores выключен в настройках.");
        }

        if (!manualClaim && !Bool(config, "AutoDeliverPending", true))
        {
            return new DeliverySummary(false, source, 0, 0, 0, 0, 0, "Автовыдача GameStores выключена.");
        }

        await ReconcileDeliveredJournalAsync(source);
        var rows = _store.GetState("gamestores-pending");
        if (rows.ValueKind != JsonValueKind.Array)
        {
            return new DeliverySummary(true, source, 0, 0, 0, 0, 0, "");
        }

        var now = DateTimeOffset.UtcNow;
        var safeLimit = manualClaim
            ? Math.Clamp(Int(config, "ManualClaimMaxPerRun", Int(config, "AutoDeliveryMaxPerRun", 5)), 1, 100)
            : Math.Clamp(Int(config, "AutoDeliveryMaxPerRun", 5), 1, 100);
        var retryDelaySeconds = Math.Clamp(Int(config, "RetryDelaySeconds", 60), 0, 86400);
        var maxAttempts = Math.Max(0, Int(config, "MaxDeliveryAttempts", 0));
        var deliveryDelaySeconds = Math.Clamp(Int(config, "DeliveryDelaySeconds", 0), 0, 86400);
        var deliverOnlyOnline = Bool(config, "DeliverOnlyToOnlinePlayers", true);
        var allowCommandDelivery = Bool(config, "AllowUnsafeCommandDelivery", false);
        var attempts = AttemptIndex();
        var settledKeys = SettledKeys();
        var onlinePlayers = targetPlayer is not null
            ? new[] { targetPlayer }
            : deliverOnlyOnline
            ? await GetOnlinePlayersAsync()
            : Array.Empty<GameStoresPlayerRef>();

        var scanned = 0;
        var issued = 0;
        var failed = 0;
        var skipped = 0;
        var remaining = new List<JsonElement>();

        foreach (var row in rows.EnumerateArray())
        {
            cancellationToken.ThrowIfCancellationRequested();
            scanned++;
            var key = First(Str(row, "key"), RowKey(row, scanned));
            var bucketId = Str(row, "bucketId", "id", "Id");
            var journal = _journal.Inspect(key);
            if (journal.State == GameStoresJournalState.Delivered)
            {
                await ReconcileDeliveredJournalAsync(source, journal.Entry);
                skipped++;
                continue;
            }

            if (settledKeys.Contains(key) || (!string.IsNullOrWhiteSpace(bucketId) && settledKeys.Contains("id:" + bucketId)))
            {
                skipped++;
                continue;
            }

            // A pre-dispatch marker may have survived a process crash, a failed
            // transport, or a damaged write. The game grant is not idempotent, so
            // retain the pending row for manual reconciliation instead of retrying.
            if (journal.State != GameStoresJournalState.None)
            {
                failed++;
                remaining.Add(row.Clone());
                continue;
            }

            if (issued >= safeLimit)
            {
                skipped++;
                remaining.Add(row.Clone());
                continue;
            }

            var request = RequestFromPending(row);
            if (targetPlayer is not null && !SamePlayer(targetPlayer, request))
            {
                remaining.Add(row.Clone());
                continue;
            }

            if (targetPlayer is not null)
            {
                request = request with
                {
                    SteamId = First(targetPlayer.SteamId, request.SteamId),
                    Name = First(targetPlayer.Name, request.Name),
                    RuntimeKey = First(targetPlayer.RuntimeKey, request.RuntimeKey)
                };
            }

            if (deliveryDelaySeconds > 0 && TryTime(row, out var createdAt) && (now - createdAt).TotalSeconds < deliveryDelaySeconds)
            {
                skipped++;
                remaining.Add(row.Clone());
                continue;
            }

            if (attempts.TryGetValue(key, out var attempt))
            {
                if (attempt.HasUnresolvedDispatch)
                {
                    var legacyEntry = _journal.CreateEntry(key, bucketId, request.SteamId ?? "", request.Name ?? "", source, "managed");
                    _journal.TryWrite(legacyEntry, GameStoresJournalState.Uncertain, "legacy-dispatch-without-journal");
                    failed++;
                    remaining.Add(row.Clone());
                    await AppendShopAsync("delivery-blocked", key, bucketId, request.SteamId ?? "", request.Name ?? "",
                        "Предыдущая выдача GameStores не имеет завершённого journal-маркера; повторная выдача заблокирована до сверки.");
                    continue;
                }

                if (maxAttempts > 0 && attempt.Count >= maxAttempts)
                {
                    failed++;
                    remaining.Add(row.Clone());
                    await AppendAttemptAsync(key, row, request, "max-attempts", $"Достигнут лимит попыток GameStores: {attempt.Count}.");
                    continue;
                }

                if (retryDelaySeconds > 0 && attempt.LastAttemptUtc is { } lastAttempt &&
                    (now - lastAttempt).TotalSeconds < retryDelaySeconds)
                {
                    skipped++;
                    remaining.Add(row.Clone());
                    continue;
                }
            }

            if (deliverOnlyOnline)
            {
                var live = MatchOnlinePlayer(onlinePlayers, request);
                if (live is null)
                {
                    skipped++;
                    remaining.Add(row.Clone());
                    await AppendShopAsync("offline", key, bucketId, request.SteamId ?? "", request.Name ?? "", "Игрок не найден среди подключённых, выдача отложена.");
                    continue;
                }

                request = request with
                {
                    SteamId = First(live.SteamId, request.SteamId),
                    Name = First(live.Name, request.Name),
                    RuntimeKey = First(live.RuntimeKey, request.RuntimeKey)
                };
            }

            var journalEntry = _journal.CreateEntry(key, bucketId, request.SteamId ?? "", request.Name ?? "", source, "managed");
            if (!_journal.TryWrite(journalEntry, GameStoresJournalState.Reserved) ||
                !_journal.TryWrite(journalEntry, GameStoresJournalState.Dispatching))
            {
                failed++;
                remaining.Add(row.Clone());
                await AppendShopAsync("delivery-blocked", key, bucketId, request.SteamId ?? "", request.Name ?? "",
                    "Не удалось создать неизменяемый journal-маркер GameStores; выдача не отправлена.");
                continue;
            }

            await AppendAttemptAsync(key, row, request, "delivering", "");
            CommandResult result;
            try
            {
                result = await DeliverRequestAsync(request, allowCommandDelivery);
            }
            catch (OperationCanceledException)
            {
                _journal.TryWrite(journalEntry, GameStoresJournalState.Uncertain, "delivery-cancelled-after-dispatch-marker");
                throw;
            }
            catch (Exception ex)
            {
                _journal.TryWrite(journalEntry, GameStoresJournalState.Uncertain, "delivery-exception-after-dispatch-marker");
                failed++;
                remaining.Add(row.Clone());
                await AppendAttemptAsync(key, row, request, "uncertain", ex.GetType().Name);
                await AppendShopAsync("delivery-uncertain", key, bucketId, request.SteamId ?? "", request.Name ?? "",
                    "Выдача GameStores завершилась исключением после dispatching-маркера; повторная выдача заблокирована до сверки.");
                continue;
            }

            if (result.Ok)
            {
                if (!_journal.TryWrite(journalEntry, GameStoresJournalState.Delivered))
                {
                    // The grant may already have reached the game, but the durable
                    // terminal marker did not. Dispatching remains and blocks retry.
                    failed++;
                    remaining.Add(row.Clone());
                    await AppendAttemptAsync(key, row, request, "uncertain", "Не удалось сохранить delivered journal-маркер.");
                    await AppendShopAsync("delivery-uncertain", key, bucketId, request.SteamId ?? "", request.Name ?? "",
                        "Игра подтвердила выдачу, но durable journal-маркер не сохранён; повторная выдача заблокирована до сверки.");
                    continue;
                }

                issued++;
                settledKeys.Add(key);
                if (!string.IsNullOrWhiteSpace(bucketId))
                {
                    settledKeys.Add("id:" + bucketId);
                }
                await _store.AppendStateAsync("gamestores-delivered", DeliveredRecord(key, row, request, result, source));
                await AppendShopAsync("delivered", key, bucketId, request.SteamId ?? "", request.Name ?? "", result.Message);
            }
            else
            {
                _journal.TryWrite(journalEntry, GameStoresJournalState.Uncertain, "delivery-result-not-ok");
                failed++;
                remaining.Add(row.Clone());
                await AppendAttemptAsync(key, row, request, "uncertain", result.Message);
                await AppendShopAsync("delivery-uncertain", key, bucketId, request.SteamId ?? "", request.Name ?? "",
                    "Выдача GameStores не подтвердилась; повторная выдача заблокирована до сверки. " + result.Message);
            }
        }

        await _store.SetStateAsync("gamestores-pending", remaining);
        return new DeliverySummary(true, source, scanned, issued, failed, skipped, remaining.Count, "");
    }

    public Task<object> ConfirmDeliveredAsync(string source = "manual", CancellationToken cancellationToken = default) =>
        SerializeOperationAsync<object>(() => ConfirmDeliveredCoreAsync(source, cancellationToken), cancellationToken);

    private async Task<object> ConfirmDeliveredCoreAsync(string source, CancellationToken cancellationToken)
    {
        var config = _store.GetConfig("gamestores-shop");
        if (!Bool(config, "Enabled", false))
        {
            return new { confirmed = false, reason = "Магазин GameStores выключен в настройках.", scanned = 0, acked = 0 };
        }

        if (!HasCredentials(config))
        {
            return new { confirmed = false, reason = "Не настроены ShopId/SecretKey GameStores; подтверждение выдачи отложено.", scanned = 0, acked = 0 };
        }

        await ReconcileDeliveredJournalAsync(source);
        var delivered = DeliveredRowsForConfirmation();
        if (delivered.Count == 0)
        {
            return new { confirmed = true, scanned = 0, acked = 0, failed = 0 };
        }

        var confirmedKeys = ConfirmedKeys();
        var scanned = 0;
        var acked = 0;
        var failed = 0;

        foreach (var row in delivered.TakeLast(500))
        {
            cancellationToken.ThrowIfCancellationRequested();
            scanned++;
            var bucketId = Str(row, "bucketId", "id", "Id");
            if (bucketId.Length == 0)
            {
                continue;
            }

            var key = First(Str(row, "key"), "id:" + bucketId);
            if (confirmedKeys.Contains(key) || confirmedKeys.Contains("id:" + bucketId))
            {
                continue;
            }

            var result = await _shop.ConfirmGivenAsync(config, bucketId, cancellationToken);
            if (result.Ok)
            {
                acked++;
                confirmedKeys.Add(key);
                confirmedKeys.Add("id:" + bucketId);
                await _store.AppendStateAsync("gamestores-confirmed", new
                {
                    utc = DateTimeOffset.UtcNow,
                    status = "confirmed",
                    source,
                    key,
                    bucketId,
                    steamId = Str(row, "steamId"),
                    name = Str(row, "name")
                });
                await AppendShopAsync("confirmed", key, bucketId, Str(row, "steamId"), Str(row, "name"), "GameStores order marked as given.");
            }
            else
            {
                failed++;
                await AppendShopAsync("confirm-failed", key, bucketId, Str(row, "steamId"), Str(row, "name"), result.Message);
            }
        }

        return new { confirmed = true, source, scanned, acked, failed };
    }

    private async Task<T> SerializeOperationAsync<T>(Func<Task<T>> operation, CancellationToken cancellationToken)
    {
        await _operationGate.WaitAsync(cancellationToken);
        try
        {
            using var lease = await _journal.AcquireOperationLeaseAsync(cancellationToken);
            return await operation();
        }
        finally
        {
            _operationGate.Release();
        }
    }

    private async Task<CommandResult> DeliverRequestAsync(WargmManualDeliverRequest request, bool allowCommandDelivery)
    {
        var mode = NormalizeMode(request.Mode);
        var skillExperience = request.SkillExperience != 0 ? request.SkillExperience : request.Experience;
        return mode switch
        {
            "money" or "currency" or "balance" => await _router.ChangeMoneyAsync(
                new MoneyRequest(request.SteamId, request.Name, request.Amount, Currency: "Normal", RuntimeKey: request.RuntimeKey)),
            "gold" => await _router.ChangeMoneyAsync(
                new MoneyRequest(request.SteamId, request.Name, request.Amount, Currency: "Gold", RuntimeKey: request.RuntimeKey)),
            "fame" or "changefame" or "famepoint" or "famepoints" or "changefamepoints" => await _router.ChangeFameAsync(
                new MoneyRequest(request.SteamId, request.Name, request.Amount, RuntimeKey: request.RuntimeKey)),
            "setfame" or "setfamepoints" => await _router.SetFameAsync(
                new MoneyRequest(request.SteamId, request.Name, request.Amount, RuntimeKey: request.RuntimeKey)),
            "vehicle" or "spawnvehicle" => await _router.SpawnVehicleAsync(
                new VehicleSpawnRequest(request.SteamId, request.Name, VehicleCatalog.NormalizeVehicleId(request.VehicleId), request.RuntimeKey)),
            "skill" or "setskill" => await _router.SetSkillAsync(
                new CharacterSkillRequest(request.SteamId, request.Name, First(request.SkillName, request.Skill), request.Level, skillExperience, request.RuntimeKey)),
            "allskills" or "allskill" or "skills" or "skillpack" or "skillspack" => await ApplyAllSkillsAsync(request, skillExperience),
            "attributes" or "attribute" or "stats" => await _router.SetAttributesAsync(
                new CharacterAttributesRequest(request.SteamId, request.Name, Strength: request.Strength, Constitution: request.Constitution, Dexterity: request.Dexterity, Intelligence: request.Intelligence, RuntimeKey: request.RuntimeKey)),
            "command" or "admincommand" or "playercommand" or "servercommand" => allowCommandDelivery
                ? await _router.ExecuteRawAsync(new CommandRequest(ApplyCommandTemplate(request.Command, request)))
                : new CommandResult(false, "gamestores_command", "gamestores", "Command delivery is disabled by AllowUnsafeCommandDelivery=false."),
            "vip" or "vipprivilege" or "vipprivileges" or "subscription" => new CommandResult(false, "gamestores_vip", "gamestores", "VIP delivery must be handled by a dedicated rule; backend skipped it safely."),
            "cargodrop" or "cargodropplayer" or "airdrop" or "scheduledcargodrop" => new CommandResult(false, "gamestores_cargodrop", "gamestores", "Cargo drop delivery needs live player coordinates; backend skipped it safely."),
            _ => await _router.DeliverItemBatchAsync(new ItemBatchRequest(request.SteamId, request.Name, NormalizeItems(request), request.RuntimeKey))
        };
    }

    private async Task<CommandResult> ApplyAllSkillsAsync(WargmManualDeliverRequest request, int experience)
    {
        var level = request.Level <= 0 ? 4 : request.Level;
        var messages = new List<string>();
        foreach (var skill in AllSkillKeys)
        {
            var result = await _router.SetSkillAsync(new CharacterSkillRequest(request.SteamId, request.Name, skill, level, experience, request.RuntimeKey));
            messages.Add($"{skill}: {result.Message}");
            if (!result.Ok)
            {
                return new CommandResult(false, "set_all_skills", result.Source, $"Не удалось выставить все навыки: {skill}: {result.Message}", Warnings: messages);
            }
        }

        return new CommandResult(true, "set_all_skills", "router", $"Все навыки выставлены на уровень {level}.", Warnings: messages);
    }

    private async Task<GameStoresPlayerRef[]> GetOnlinePlayersAsync()
    {
        var result = await _router.GetPlayersAsync();
        if (!result.Ok || result.Payload is not { ValueKind: JsonValueKind.Object } payload ||
            !payload.TryGetProperty("players", out var players) || players.ValueKind != JsonValueKind.Array)
        {
            return Array.Empty<GameStoresPlayerRef>();
        }

        return players.EnumerateArray()
            .Select(player => new GameStoresPlayerRef(
                Str(player, "steamId", "steam", "SteamId", "SteamID"),
                Str(player, "name", "Name", "playerName", "PlayerName"),
                Str(player, "runtimeKey", "RuntimeKey", "key", "Key")))
            .Where(player => player.SteamId.Length > 0 || player.Name.Length > 0 || player.RuntimeKey.Length > 0)
            .ToArray();
    }

    private static GameStoresPlayerRef? MatchOnlinePlayer(IEnumerable<GameStoresPlayerRef> players, WargmManualDeliverRequest request)
    {
        var steam = (request.SteamId ?? "").Trim();
        var runtimeKey = (request.RuntimeKey ?? "").Trim();
        var name = (request.Name ?? "").Trim();
        if (runtimeKey.Length > 0)
        {
            return players.FirstOrDefault(player => player.RuntimeKey.Equals(runtimeKey, StringComparison.OrdinalIgnoreCase));
        }

        if (steam.Length > 0)
        {
            return players.FirstOrDefault(player => player.SteamId.Equals(steam, StringComparison.OrdinalIgnoreCase));
        }

        return name.Length == 0
            ? null
            : players.FirstOrDefault(player => player.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
    }

    private static bool SamePlayer(GameStoresPlayerRef player, WargmManualDeliverRequest request)
    {
        var steam = (request.SteamId ?? "").Trim();
        if (player.SteamId.Length > 0 && steam.Length > 0)
        {
            return player.SteamId.Equals(steam, StringComparison.OrdinalIgnoreCase);
        }

        var runtimeKey = (request.RuntimeKey ?? "").Trim();
        if (player.RuntimeKey.Length > 0 && runtimeKey.Length > 0)
        {
            return player.RuntimeKey.Equals(runtimeKey, StringComparison.OrdinalIgnoreCase);
        }

        var name = (request.Name ?? "").Trim();
        return player.Name.Length > 0 && name.Length > 0 &&
               player.Name.Equals(name, StringComparison.OrdinalIgnoreCase);
    }

    private JsonElement? BuildPendingRow(JsonElement config, GameStoresItem item, GameStoresPlayerRef player, string key)
    {
        var rule = MatchRule(config, item);
        if (rule is null && !Bool(config, "AllowDirectItemIdDelivery", false))
        {
            return null;
        }

        var mode = NormalizeMode(rule is null ? "SpawnItem" : Str(rule.Value, "DeliveryMode", "deliveryMode", "mode"));
        var row = new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase)
        {
            ["key"] = key,
            ["bucketId"] = item.Id,
            ["productId"] = item.ItemId,
            ["title"] = item.Name,
            ["type"] = item.Type,
            ["gameStoresCommand"] = item.Command,
            ["steamId"] = player.SteamId,
            ["recipientName"] = player.Name,
            ["runtimeKey"] = player.RuntimeKey,
            ["createdAtUtc"] = DateTimeOffset.UtcNow,
            ["delivered"] = false,
            ["mode"] = mode
        };

        var outputRule = rule ?? default;
        switch (mode)
        {
            case "money":
            case "currency":
            case "balance":
            case "gold":
            case "fame":
            case "changefame":
            case "setfame":
            case "setfamepoints":
                row["amount"] = ResolveAmount(outputRule, item);
                break;
            case "vehicle":
            case "spawnvehicle":
                row["vehicleId"] = VehicleCatalog.NormalizeVehicleId(Str(outputRule, "VehicleAsset", "vehicleAsset", "VehicleId", "vehicleId"));
                if (string.IsNullOrWhiteSpace(Convert.ToString(row["vehicleId"], CultureInfo.InvariantCulture))) return null;
                break;
            case "skill":
            case "setskill":
                row["skill"] = Str(outputRule, "SkillName", "skillName", "Skill", "skill");
                row["level"] = Int(outputRule, "SkillLevel", 0);
                row["experience"] = Int(outputRule, "SkillExperience", 0);
                if (string.IsNullOrWhiteSpace(Convert.ToString(row["skill"], CultureInfo.InvariantCulture))) return null;
                break;
            case "allskills":
            case "skills":
            case "skillpack":
                row["level"] = Math.Max(1, Int(outputRule, "SkillLevel", 4));
                row["experience"] = Int(outputRule, "SkillExperience", 0);
                break;
            case "attributes":
            case "attribute":
            case "stats":
                row["strength"] = Double(outputRule, "Strength", 0);
                row["constitution"] = Double(outputRule, "Constitution", 0);
                row["dexterity"] = Double(outputRule, "Dexterity", 0);
                row["intelligence"] = Double(outputRule, "Intelligence", 0);
                break;
            case "command":
            case "admincommand":
            case "playercommand":
            case "servercommand":
                row["command"] = Str(outputRule, "CommandTemplate", "commandTemplate", "Command", "command");
                if (string.IsNullOrWhiteSpace(Convert.ToString(row["command"], CultureInfo.InvariantCulture))) return null;
                break;
            default:
                var spec = ResolveItemsSpec(config, outputRule, item);
                if (spec.Length == 0) return null;
                row["mode"] = "item";
                row["itemsSpec"] = spec;
                var first = spec.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).FirstOrDefault() ?? "";
                var parts = first.Split('|', StringSplitOptions.TrimEntries);
                row["itemId"] = parts.ElementAtOrDefault(0) ?? "";
                row["quantity"] = int.TryParse(parts.ElementAtOrDefault(1), NumberStyles.Integer, CultureInfo.InvariantCulture, out var quantity) ? quantity : 1;
                break;
        }

        return JsonSerializer.SerializeToElement(row, WardenJson.Options);
    }

    private static JsonElement? MatchRule(JsonElement config, GameStoresItem item)
    {
        if (!TryGet(config, "Rules", out var rules) || rules.ValueKind != JsonValueKind.Array)
        {
            return null;
        }

        foreach (var rule in rules.EnumerateArray())
        {
            if (!Bool(rule, "Enabled", true))
            {
                continue;
            }

            var bucketId = Str(rule, "MatchBucketId", "matchBucketId", "MatchId", "matchId");
            var productId = Str(rule, "MatchProductId", "matchProductId", "MatchItemId", "matchItemId", "MatchObjectId", "matchObjectId");
            var title = Str(rule, "MatchTitleContains", "matchTitleContains");
            var command = Str(rule, "MatchCommandContains", "matchCommandContains");
            if (bucketId.Length > 0 && bucketId.Equals(item.Id, StringComparison.OrdinalIgnoreCase))
            {
                return rule.Clone();
            }

            if (productId.Length > 0 &&
                (productId.Equals(item.ItemId, StringComparison.OrdinalIgnoreCase) ||
                 productId.Equals(item.Id, StringComparison.OrdinalIgnoreCase)))
            {
                return rule.Clone();
            }

            if (title.Length > 0 && item.Name.Contains(title, StringComparison.OrdinalIgnoreCase))
            {
                return rule.Clone();
            }

            if (command.Length > 0 && item.Command.Contains(command, StringComparison.OrdinalIgnoreCase))
            {
                return rule.Clone();
            }
        }

        return null;
    }

    private static string ResolveItemsSpec(JsonElement config, JsonElement? rule, GameStoresItem item)
    {
        if (rule is { } ruleElement && TryGet(ruleElement, "Items", out var items) && items.ValueKind == JsonValueKind.Array)
        {
            var parts = items.EnumerateArray()
                .Select(entry => (id: CleanItemToken(Str(entry, "ItemId", "itemId")), qty: Math.Max(1, Int(entry, "Quantity", 1))))
                .Where(entry => entry.id.Length > 0)
                .Select(entry => $"{entry.id}|{entry.qty}")
                .ToArray();
            if (parts.Length > 0)
            {
                return string.Join(';', parts);
            }
        }

        var itemId = rule is { } value ? CleanItemToken(Str(value, "ItemId", "itemId")) : "";
        if (itemId.Length == 0 && Bool(config, "AllowDirectItemIdDelivery", false))
        {
            itemId = CleanItemToken(item.ItemId);
        }

        var quantity = rule is { } quantityRule ? Int(quantityRule, "Quantity", 0) : 0;
        if (quantity <= 0)
        {
            quantity = Math.Max(1, ParseInt(item.Amount, 1));
        }

        return itemId.Length > 0 ? $"{itemId}|{Math.Clamp(quantity, 1, 1000)}" : "";
    }

    private static long ResolveAmount(JsonElement? rule, GameStoresItem item)
    {
        var amount = rule is { } value ? Long(value, "Amount", 0) : 0;
        return amount != 0 ? amount : ParseLong(item.Amount, 0);
    }

    private WargmManualDeliverRequest RequestFromPending(JsonElement row)
    {
        var mode = NormalizeMode(Str(row, "mode", "DeliveryMode", "deliveryMode"));
        var items = ItemsFromSpec(Str(row, "itemsSpec", "ItemsSpec"));
        var itemId = Str(row, "itemId", "ItemId");
        if (items.Count == 0 && itemId.Length > 0)
        {
            items.Add(new ItemGrantRequest(null, null, itemId, Math.Clamp(Int(row, "quantity", 1), 1, 100)));
        }

        return new WargmManualDeliverRequest(
            SteamId: Str(row, "steamId", "SteamId", "steam", "SteamID"),
            Name: First(Str(row, "name", "Name"), Str(row, "recipientName", "RecipientName")),
            Mode: mode,
            Items: items,
            ItemId: itemId,
            Quantity: Math.Clamp(Int(row, "quantity", 1), 1, 100),
            VehicleId: VehicleCatalog.NormalizeVehicleId(Str(row, "vehicleId", "VehicleId", "VehicleAsset", "vehicleAsset")),
            Amount: Long(row, "amount", 0),
            Skill: Str(row, "skill", "Skill"),
            SkillName: Str(row, "skillName", "SkillName"),
            Level: Int(row, "level", Int(row, "skillLevel", Int(row, "SkillLevel", 0))),
            Experience: Int(row, "experience", Int(row, "skillExperience", Int(row, "SkillExperience", 0))),
            SkillExperience: Int(row, "skillExperience", Int(row, "SkillExperience", 0)),
            Strength: Double(row, "strength", Double(row, "Strength", 0)),
            Constitution: Double(row, "constitution", Double(row, "Constitution", 0)),
            Dexterity: Double(row, "dexterity", Double(row, "Dexterity", 0)),
            Intelligence: Double(row, "intelligence", Double(row, "Intelligence", 0)),
            Command: Str(row, "command", "Command", "commandTemplate", "CommandTemplate"),
            RuntimeKey: Str(row, "runtimeKey", "RuntimeKey"));
    }

    private List<JsonElement> ExistingPendingRows()
    {
        var rows = _store.GetState("gamestores-pending");
        if (rows.ValueKind != JsonValueKind.Array)
        {
            return new List<JsonElement>();
        }

        var settled = SettledKeys();
        return rows.EnumerateArray()
            .Where(row =>
            {
                var key = Str(row, "key");
                var id = Str(row, "bucketId", "id", "Id");
                return (key.Length == 0 || !settled.Contains(key)) &&
                       (id.Length == 0 || !settled.Contains("id:" + id));
            })
            .Select(row => row.Clone())
            .ToList();
    }

    private async Task ReconcileDeliveredJournalAsync(string fallbackSource, GameStoresJournalEntry? singleEntry = null)
    {
        var entries = singleEntry is null
            ? _journal.DeliveredEntries()
            : new[] { singleEntry };
        if (entries.Count == 0)
        {
            return;
        }

        var known = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var existing = _store.GetState("gamestores-delivered");
        if (existing.ValueKind == JsonValueKind.Array)
        {
            foreach (var row in existing.EnumerateArray())
            {
                AddDeliveredIdentity(known, Str(row, "key"), Str(row, "bucketId", "id", "Id"));
            }
        }

        foreach (var entry in entries)
        {
            if (!AddDeliveredIdentity(known, entry.Key, entry.BucketId))
            {
                continue;
            }

            await _store.AppendStateAsync("gamestores-delivered", new
            {
                utc = entry.Utc,
                source = First(entry.Source, fallbackSource),
                key = entry.Key,
                bucketId = entry.BucketId,
                steamId = entry.SteamId,
                name = entry.Name,
                status = "delivered",
                journalRecovered = true,
                message = "Recovered from immutable GameStores delivery journal."
            });
        }
    }

    private List<JsonElement> DeliveredRowsForConfirmation()
    {
        var rows = new List<JsonElement>();
        var known = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var delivered = _store.GetState("gamestores-delivered");
        if (delivered.ValueKind == JsonValueKind.Array)
        {
            foreach (var row in delivered.EnumerateArray())
            {
                var clone = row.Clone();
                if (AddDeliveredIdentity(known, Str(clone, "key"), Str(clone, "bucketId", "id", "Id")))
                {
                    rows.Add(clone);
                }
            }
        }

        foreach (var entry in _journal.DeliveredEntries())
        {
            if (!AddDeliveredIdentity(known, entry.Key, entry.BucketId))
            {
                continue;
            }

            rows.Add(JsonSerializer.SerializeToElement(new
            {
                utc = entry.Utc,
                source = entry.Source,
                key = entry.Key,
                bucketId = entry.BucketId,
                steamId = entry.SteamId,
                name = entry.Name,
                status = "delivered",
                journalRecovered = true
            }, WardenJson.Options));
        }

        return rows;
    }

    private static bool AddDeliveredIdentity(HashSet<string> known, string key, string bucketId)
    {
        var identities = new List<string>();
        if (!string.IsNullOrWhiteSpace(key))
        {
            identities.Add("key:" + key.Trim());
        }

        if (!string.IsNullOrWhiteSpace(bucketId))
        {
            identities.Add("id:" + bucketId.Trim());
        }

        if (identities.Count == 0 || identities.Any(known.Contains))
        {
            return false;
        }

        foreach (var identity in identities)
        {
            known.Add(identity);
        }

        return true;
    }

    private HashSet<string> SettledKeys()
    {
        var keys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        AddStateKeys(keys, "gamestores-delivered");
        AddStateKeys(keys, "gamestores-confirmed");
        foreach (var entry in _journal.DeliveredEntries())
        {
            if (entry.Key.Length > 0)
            {
                keys.Add(entry.Key);
            }

            if (entry.BucketId.Length > 0)
            {
                keys.Add("id:" + entry.BucketId);
            }
        }
        return keys;
    }

    private HashSet<string> ConfirmedKeys()
    {
        var keys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        AddStateKeys(keys, "gamestores-confirmed");
        return keys;
    }

    private void AddStateKeys(HashSet<string> keys, string stateName)
    {
        var rows = _store.GetState(stateName);
        if (rows.ValueKind != JsonValueKind.Array)
        {
            return;
        }

        foreach (var row in rows.EnumerateArray())
        {
            var key = Str(row, "key");
            if (key.Length > 0)
            {
                keys.Add(key);
            }

            var id = Str(row, "bucketId", "id", "Id");
            if (id.Length > 0)
            {
                keys.Add("id:" + id);
            }
        }
    }

    private Dictionary<string, AttemptInfo> AttemptIndex()
    {
        var rows = _store.GetState("gamestores-attempts");
        var result = new Dictionary<string, AttemptInfo>(StringComparer.OrdinalIgnoreCase);
        if (rows.ValueKind != JsonValueKind.Array)
        {
            return result;
        }

        foreach (var row in rows.EnumerateArray().TakeLast(1000))
        {
            var key = Str(row, "key");
            if (key.Length == 0)
            {
                continue;
            }

            if (!result.TryGetValue(key, out var info))
            {
                info = new AttemptInfo();
                result[key] = info;
            }

            var status = Str(row, "status");
            if (status.Length == 0 || status.Equals("delivering", StringComparison.OrdinalIgnoreCase))
            {
                info.Count++;
            }

            if (status.Equals("delivering", StringComparison.OrdinalIgnoreCase) ||
                status.Equals("claim-delivering", StringComparison.OrdinalIgnoreCase))
            {
                info.HasUnresolvedDispatch = true;
            }

            if ((status.Length == 0 || status.Equals("delivering", StringComparison.OrdinalIgnoreCase) ||
                    status.Equals("retry", StringComparison.OrdinalIgnoreCase) ||
                    status.Equals("failed", StringComparison.OrdinalIgnoreCase)) &&
                TryTime(row, out var utc))
            {
                info.LastAttemptUtc = utc;
            }
        }

        return result;
    }

    private async Task AppendAttemptAsync(string key, JsonElement row, WargmManualDeliverRequest request, string status, string message)
    {
        await _store.AppendStateAsync("gamestores-attempts", new
        {
            utc = DateTimeOffset.UtcNow,
            key,
            bucketId = Str(row, "bucketId", "id", "Id"),
            productId = Str(row, "productId", "itemId"),
            title = Str(row, "title", "name"),
            steamId = request.SteamId,
            name = request.Name,
            mode = NormalizeMode(request.Mode),
            status,
            message
        });
    }

    private async Task AppendShopAsync(string status, string key, string bucketId, string steamId, string name, string message)
    {
        await _store.AppendStateAsync("gamestores-shop", new
        {
            utc = DateTimeOffset.UtcNow,
            at = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
            status,
            key,
            bucketId,
            steamId,
            name,
            message
        });
    }

    private static object DeliveredRecord(string key, JsonElement row, WargmManualDeliverRequest request, CommandResult result, string source) => new
    {
        utc = DateTimeOffset.UtcNow,
        source,
        key,
        bucketId = Str(row, "bucketId", "id", "Id"),
        productId = Str(row, "productId", "itemId"),
        title = Str(row, "title", "name"),
        steamId = request.SteamId,
        name = request.Name,
        mode = NormalizeMode(request.Mode),
        itemId = request.ItemId,
        itemsSpec = string.Join(';', NormalizeItems(request).Select(item => $"{item.ItemId}|{item.Quantity}")),
        vehicleId = VehicleCatalog.NormalizeVehicleId(request.VehicleId),
        amount = request.Amount,
        quantity = request.Quantity,
        status = "delivered",
        message = result.Message
    };

    private static string ItemKey(JsonElement config, GameStoresItem item)
    {
        var shopId = First(Str(config, "ShopId", "StoreId", "shopId", "storeId"), "0");
        var serverId = First(Str(config, "ServerId", "GameStoresServerId", "serverId", "gameStoresServerId"), "0");
        return $"gamestores:{shopId}:{serverId}:id:{item.Id}";
    }

    private static string RowKey(JsonElement row, int index)
    {
        var id = Str(row, "bucketId", "id", "Id");
        if (id.Length > 0)
        {
            return "id:" + id;
        }

        return "fallback:" + index.ToString(CultureInfo.InvariantCulture);
    }

    private static IReadOnlyList<ItemGrantRequest> NormalizeItems(WargmManualDeliverRequest request)
    {
        var items = new List<ItemGrantRequest>();
        if (request.Items is { Count: > 0 })
        {
            items.AddRange(request.Items
                .Where(item => LooksLikeScumItemId(item.ItemId))
                .Select(item => new ItemGrantRequest(
                    request.SteamId,
                    request.Name,
                    item.ItemId.Trim(),
                    Math.Clamp(item.Quantity, 1, 100),
                    false,
                    request.RuntimeKey)));
        }

        if (!string.IsNullOrWhiteSpace(request.ItemId) && LooksLikeScumItemId(request.ItemId))
        {
            items.Add(new ItemGrantRequest(
                request.SteamId,
                request.Name,
                request.ItemId.Trim(),
                Math.Clamp(request.Quantity, 1, 100),
                false,
                request.RuntimeKey));
        }

        return items
            .GroupBy(item => item.ItemId, StringComparer.OrdinalIgnoreCase)
            .Select(group => group.First())
            .ToArray();
    }

    private static List<ItemGrantRequest> ItemsFromSpec(string spec)
    {
        var result = new List<ItemGrantRequest>();
        foreach (var entry in (spec ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var parts = entry.Split('|', StringSplitOptions.TrimEntries);
            var itemId = parts.ElementAtOrDefault(0) ?? "";
            if (!LooksLikeScumItemId(itemId))
            {
                continue;
            }

            var quantity = int.TryParse(parts.ElementAtOrDefault(1), NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed)
                ? parsed
                : 1;
            result.Add(new ItemGrantRequest(null, null, itemId, Math.Clamp(quantity, 1, 100)));
        }

        return result;
    }

    private static string ApplyCommandTemplate(string? command, WargmManualDeliverRequest request) =>
        (command ?? "")
            .Replace("{steamId}", request.SteamId ?? "", StringComparison.OrdinalIgnoreCase)
            .Replace("{name}", request.Name ?? "", StringComparison.OrdinalIgnoreCase);

    private static string NormalizeMode(string? mode)
    {
        var normalized = (mode ?? "item").Trim().ToLowerInvariant()
            .Replace("_", "", StringComparison.Ordinal)
            .Replace("-", "", StringComparison.Ordinal)
            .Replace(" ", "", StringComparison.Ordinal)
            .Replace(".", "", StringComparison.Ordinal);
        return normalized.Length == 0 ? "item" : normalized;
    }

    private static bool HasCredentials(JsonElement config) =>
        !string.IsNullOrWhiteSpace(Str(config, "ShopId", "StoreId", "shopId", "storeId")) &&
        !string.IsNullOrWhiteSpace(Str(config, "SecretKey", "secretKey", "ApiKey", "apiKey"));

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

    private static int Int(JsonElement? element, string name, int fallback) =>
        element is { } value ? Int(value, name, fallback) : fallback;

    private static int Int(JsonElement element, string name, int fallback)
    {
        if (!TryGet(element, name, out var value))
        {
            return fallback;
        }

        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var n))
        {
            return n;
        }

        return int.TryParse(Str(element, name), NumberStyles.Integer, CultureInfo.InvariantCulture, out n) ? n : fallback;
    }

    private static long Long(JsonElement element, string name, long fallback)
    {
        if (!TryGet(element, name, out var value))
        {
            return fallback;
        }

        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out var n))
        {
            return n;
        }

        return long.TryParse(Str(element, name), NumberStyles.Integer, CultureInfo.InvariantCulture, out n) ? n : fallback;
    }

    private static double Double(JsonElement? element, string name, double fallback) =>
        element is { } value ? Double(value, name, fallback) : fallback;

    private static double Double(JsonElement element, string name, double fallback)
    {
        if (!TryGet(element, name, out var value))
        {
            return fallback;
        }

        if (value.ValueKind == JsonValueKind.Number && value.TryGetDouble(out var n))
        {
            return n;
        }

        return double.TryParse(Str(element, name), NumberStyles.Float, CultureInfo.InvariantCulture, out n) ? n : fallback;
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

        return bool.TryParse(Str(element, name), out var result) ? result : fallback;
    }

    private static bool TryTime(JsonElement row, out DateTimeOffset utc)
    {
        foreach (var name in new[] { "utc", "timestampUtc", "createdAtUtc" })
        {
            var raw = Str(row, name);
            if (DateTimeOffset.TryParse(raw, CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal, out utc))
            {
                return true;
            }
        }

        utc = default;
        return false;
    }

    private static int ParseInt(string value, int fallback) =>
        int.TryParse((value ?? "").Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed)
            ? parsed
            : fallback;

    private static long ParseLong(string value, long fallback) =>
        long.TryParse((value ?? "").Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed)
            ? parsed
            : fallback;

    private static string First(params string?[] values) =>
        values.Select(value => value?.Trim() ?? "")
            .FirstOrDefault(value => value.Length > 0) ?? "";

    private static string CleanItemToken(string value)
    {
        var itemId = (value ?? "").Replace("|", "", StringComparison.Ordinal).Replace(";", "", StringComparison.Ordinal).Trim();
        return LooksLikeScumItemId(itemId) ? itemId : "";
    }

    private static bool LooksLikeScumItemId(string? value)
    {
        var text = (value ?? "").Trim();
        return text.Length > 0 &&
               text.Any(char.IsLetter) &&
               text.All(ch => char.IsLetterOrDigit(ch) || ch is '_' or '-' or '.');
    }

    private sealed class AttemptInfo
    {
        public int Count { get; set; }
        public DateTimeOffset? LastAttemptUtc { get; set; }
        public bool HasUnresolvedDispatch { get; set; }
    }

    private sealed record DeliverySummary(bool Delivered, string Source, int Scanned, int Issued, int Failed, int Skipped, int Remaining, string Reason);

    private sealed record GameStoresPlayerRef(string SteamId, string Name, string RuntimeKey);
}

public sealed record GameStoresClaimRequest(string? SteamId = null, string? Name = null, string? RuntimeKey = null);
