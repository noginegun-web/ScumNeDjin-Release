using System.Globalization;
using System.Text.Json;

namespace ScumWarden.Server.Services;

public sealed class WargmDeliveryService
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
    private readonly PluginConfigStore _store;

    public WargmDeliveryService(CommandRouter router, PluginConfigStore store)
    {
        _router = router;
        _store = store;
    }

    public async Task<CommandResult> DeliverManualAsync(WargmManualDeliverRequest request, string source = "manual")
    {
        var resolvedRequest = await ResolveOnlineTargetAsync(request);
        var result = await DeliverRequestAsync(resolvedRequest, allowCommandDelivery: true);
        await _store.AppendStateAsync("wargm-shop", new
        {
            utc = DateTimeOffset.UtcNow,
            source,
            status = result.Ok ? "manual-delivered" : "manual-failed",
            steamId = resolvedRequest.SteamId,
            name = resolvedRequest.Name,
            runtimeKey = resolvedRequest.RuntimeKey,
            mode = NormalizeMode(resolvedRequest.Mode),
            ok = result.Ok,
            message = result.Message
        });
        return result;
    }

    public async Task<object> DeliverPendingAsync(string source = "auto", CancellationToken cancellationToken = default)
    {
        var config = _store.GetConfig("wargm-shop");
        if (!Bool(config, "Enabled", false))
        {
            return new { delivered = false, reason = "Магазин Wargm выключен в настройках.", scanned = 0, issued = 0 };
        }

        if (!Bool(config, "AutoDeliverPending", true))
        {
            return new { delivered = false, reason = "Автовыдача Wargm выключена.", scanned = 0, issued = 0 };
        }

        var pending = _store.GetState("wargm-pending");
        if (pending.ValueKind != JsonValueKind.Array)
        {
            return new { delivered = true, scanned = 0, issued = 0, failed = 0, skipped = 0 };
        }

        var now = DateTimeOffset.UtcNow;
        var safeLimit = Math.Clamp(Int(config, "AutoDeliveryMaxPerRun", 5), 1, 100);
        var retryDelaySeconds = Math.Clamp(Int(config, "RetryDelaySeconds", 60), 0, 86400);
        var maxAttempts = Math.Max(0, Int(config, "MaxDeliveryAttempts", 0));
        var deliveryDelaySeconds = Math.Clamp(Int(config, "DeliveryDelaySeconds", 0), 0, 86400);
        var requireRecipientName = Bool(config, "RequireRecipientName", false);
        var deliverOnlyOnline = Bool(config, "DeliverOnlyToOnlinePlayers", true);
        var allowCommandDelivery = Bool(config, "AllowUnsafeCommandDelivery", false);

        var settledKeys = SettledWargmKeys();
        var attempts = AttemptIndex();
        var onlinePlayers = deliverOnlyOnline
            ? await GetOnlinePlayersAsync()
            : Array.Empty<WargmPlayerRef>();

        var scanned = 0;
        var issued = 0;
        var failed = 0;
        var skipped = 0;
        var remaining = new List<JsonElement>();
        var previews = new List<object>();

        foreach (var row in pending.EnumerateArray())
        {
            cancellationToken.ThrowIfCancellationRequested();
            scanned++;
            var key = WargmOperationKey(row, scanned);
            var operationId = Str(row, "operationId", "OperationId", "id", "Id");
            if (IsSettled(settledKeys, key, operationId))
            {
                skipped++;
                continue;
            }

            if (issued >= safeLimit)
            {
                remaining.Add(row.Clone());
                skipped++;
                continue;
            }

            var request = RequestFromPending(row);
            if (requireRecipientName && string.IsNullOrWhiteSpace(request.Name) && string.IsNullOrWhiteSpace(request.SteamId))
            {
                failed++;
                remaining.Add(row.Clone());
                await AppendAttemptAsync(key, row, request, "failed", "Получатель Wargm не найден в операции.");
                continue;
            }

            if (deliveryDelaySeconds > 0 && TryTime(row, out var createdAt) && (now - createdAt).TotalSeconds < deliveryDelaySeconds)
            {
                skipped++;
                remaining.Add(row.Clone());
                continue;
            }

            if (attempts.TryGetValue(key, out var attempt))
            {
                if (maxAttempts > 0 && attempt.Count >= maxAttempts)
                {
                    failed++;
                    remaining.Add(row.Clone());
                    await AppendShopAsync(key, row, request, "max-attempts", $"Достигнут лимит попыток Wargm: {attempt.Count}.");
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
                    await AppendShopAsync(key, row, request, "offline", "Игрок не найден среди подключённых, выдача отложена.");
                    continue;
                }

                request = request with
                {
                    SteamId = First(live.SteamId, request.SteamId),
                    Name = First(live.Name, request.Name),
                    RuntimeKey = First(live.RuntimeKey, request.RuntimeKey)
                };
            }

            await AppendAttemptAsync(key, row, request, "delivering", "");
            var result = await DeliverRequestAsync(request, allowCommandDelivery);
            if (result.Ok)
            {
                issued++;
                settledKeys.Add(key);
                if (operationId.Length > 0)
                {
                    settledKeys.Add("operation-id:" + operationId);
                    settledKeys.Add("operationId:" + operationId);
                }

                await _store.AppendStateAsync("wargm-delivered", DeliveredRecord(key, row, request, result, source));
                await AppendShopAsync(key, row, request, "delivered", result.Message);
                previews.Add(new { key, operationId, request.SteamId, request.Name, mode = NormalizeMode(request.Mode) });
            }
            else
            {
                failed++;
                remaining.Add(row.Clone());
                await AppendAttemptAsync(key, row, request, "retry", result.Message);
                await AppendShopAsync(key, row, request, "retry", result.Message);
            }
        }

        await _store.SetStateAsync("wargm-pending", remaining);
        return new
        {
            delivered = true,
            source,
            scanned,
            issued,
            failed,
            skipped,
            remaining = remaining.Count,
            preview = previews.Take(10).ToArray()
        };
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
                : new CommandResult(false, "wargm_command", "wargm", "Command delivery is disabled by AllowUnsafeCommandDelivery=false."),
            "vip" or "vipprivilege" or "vipprivileges" or "subscription" => new CommandResult(false, "wargm_vip", "wargm", "VIP delivery must be handled by the Lua /wargm route; backend auto-delivery skipped it safely."),
            "cargodrop" or "cargodropplayer" or "airdrop" or "scheduledcargodrop" => new CommandResult(false, "wargm_cargodrop", "wargm", "Cargo drop delivery needs live player coordinates; backend auto-delivery skipped it safely."),
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

    private async Task<WargmPlayerRef[]> GetOnlinePlayersAsync()
    {
        var result = await _router.GetPlayersAsync();
        if (!result.Ok || result.Payload is not { ValueKind: JsonValueKind.Object } payload ||
            !payload.TryGetProperty("players", out var players) || players.ValueKind != JsonValueKind.Array)
        {
            return Array.Empty<WargmPlayerRef>();
        }

        return players.EnumerateArray()
            .Select(player => new WargmPlayerRef(
                Str(player, "steamId", "steam", "SteamId", "SteamID"),
                Str(player, "name", "Name", "playerName", "PlayerName"),
                Str(player, "runtimeKey", "RuntimeKey", "key", "Key")))
            .Where(player => player.SteamId.Length > 0 || player.Name.Length > 0 || player.RuntimeKey.Length > 0)
            .ToArray();
    }

    private async Task<WargmManualDeliverRequest> ResolveOnlineTargetAsync(WargmManualDeliverRequest request)
    {
        if (string.IsNullOrWhiteSpace(request.SteamId) &&
            string.IsNullOrWhiteSpace(request.Name) &&
            string.IsNullOrWhiteSpace(request.RuntimeKey))
        {
            return request;
        }

        var live = MatchOnlinePlayer(await GetOnlinePlayersAsync(), request);
        if (live is null)
        {
            return request;
        }

        return request with
        {
            SteamId = First(live.SteamId, request.SteamId),
            Name = First(live.Name, request.Name),
            RuntimeKey = First(live.RuntimeKey, request.RuntimeKey)
        };
    }

    private static WargmPlayerRef? MatchOnlinePlayer(IEnumerable<WargmPlayerRef> players, WargmManualDeliverRequest request)
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
            Command: Str(row, "command", "Command", "commandTemplate", "CommandTemplate"));
    }

    private async Task AppendAttemptAsync(string key, JsonElement row, WargmManualDeliverRequest request, string status, string message)
    {
        await _store.AppendStateAsync("wargm-attempts", new
        {
            utc = DateTimeOffset.UtcNow,
            key,
            operationId = Str(row, "operationId", "OperationId", "id", "Id"),
            offerId = Str(row, "offerId", "OfferId"),
            title = Str(row, "title", "Title"),
            steamId = request.SteamId,
            name = request.Name,
            mode = NormalizeMode(request.Mode),
            status,
            message
        });
    }

    private async Task AppendShopAsync(string key, JsonElement row, WargmManualDeliverRequest request, string status, string message)
    {
        await _store.AppendStateAsync("wargm-shop", new
        {
            utc = DateTimeOffset.UtcNow,
            key,
            operationId = Str(row, "operationId", "OperationId", "id", "Id"),
            offerId = Str(row, "offerId", "OfferId"),
            title = Str(row, "title", "Title"),
            steamId = request.SteamId,
            name = request.Name,
            mode = NormalizeMode(request.Mode),
            status,
            message
        });
    }

    private static object DeliveredRecord(string key, JsonElement row, WargmManualDeliverRequest request, CommandResult result, string source) => new
    {
        utc = DateTimeOffset.UtcNow,
        source,
        key,
        operationId = Str(row, "operationId", "OperationId", "id", "Id"),
        offerId = Str(row, "offerId", "OfferId"),
        title = Str(row, "title", "Title"),
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

    private Dictionary<string, WargmAttemptInfo> AttemptIndex()
    {
        var rows = _store.GetState("wargm-attempts");
        var result = new Dictionary<string, WargmAttemptInfo>(StringComparer.OrdinalIgnoreCase);
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
                info = new WargmAttemptInfo();
                result[key] = info;
            }

            if (!Str(row, "status").Equals("retry-wait", StringComparison.OrdinalIgnoreCase))
            {
                info.Count++;
            }

            if (TryTime(row, out var utc))
            {
                info.LastAttemptUtc = utc;
            }
        }

        return result;
    }

    private HashSet<string> SettledWargmKeys()
    {
        var keys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        AddStateKeys(keys, "wargm-delivered");
        AddStateKeys(keys, "wargm-confirmed");
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

            var operationId = Str(row, "operationId", "OperationId", "id", "Id");
            if (operationId.Length > 0)
            {
                keys.Add("operation-id:" + operationId);
                keys.Add("operationId:" + operationId);
            }
        }
    }

    private static bool IsSettled(HashSet<string> settledKeys, string key, string operationId) =>
        settledKeys.Contains(key) ||
        (operationId.Length > 0 && (settledKeys.Contains("operation-id:" + operationId) || settledKeys.Contains("operationId:" + operationId)));

    private static string WargmOperationKey(JsonElement row, int index)
    {
        var operationId = Str(row, "operationId", "OperationId", "id", "Id");
        if (operationId.Length > 0)
        {
            return "operation-id:" + operationId;
        }

        var steamId = Str(row, "steamId", "SteamId", "steam", "SteamID");
        var offerId = Str(row, "offerId", "OfferId");
        var title = Str(row, "title", "Title");
        var created = Str(row, "createdAtUtc", "utc", "timestampUtc");
        return string.Join(':', "fallback", steamId, offerId, title, created, index.ToString(CultureInfo.InvariantCulture));
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

        if (!string.IsNullOrWhiteSpace(request.ItemId))
        {
            if (LooksLikeScumItemId(request.ItemId))
            {
                items.Add(new ItemGrantRequest(
                    request.SteamId,
                    request.Name,
                    request.ItemId.Trim(),
                    Math.Clamp(request.Quantity, 1, 100),
                    false,
                    request.RuntimeKey));
            }
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

    private static string First(params string?[] values) =>
        values.Select(value => value?.Trim() ?? "")
            .FirstOrDefault(value => value.Length > 0) ?? "";

    private static bool LooksLikeScumItemId(string? value)
    {
        var text = (value ?? "").Trim();
        return text.Length > 0 &&
               text.Any(char.IsLetter) &&
               text.All(ch => char.IsLetterOrDigit(ch) || ch is '_' or '-' or '.');
    }

    private sealed class WargmAttemptInfo
    {
        public int Count { get; set; }
        public DateTimeOffset? LastAttemptUtc { get; set; }
    }

    private sealed record WargmPlayerRef(string SteamId, string Name, string RuntimeKey);
}
