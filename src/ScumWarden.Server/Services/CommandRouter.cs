using System.Text.Json;

namespace ScumWarden.Server.Services;

public sealed class CommandRouter
{
    private readonly FileBridgeExecutor _bridge;
    private readonly LocalRconService _localRcon;
    private readonly WardenEventStore _events;

    public CommandRouter(FileBridgeExecutor bridge, LocalRconService localRcon, WardenEventStore events)
    {
        _bridge = bridge;
        _localRcon = localRcon;
        _events = events;
    }

    public async Task<CommandResult> GetPlayersAsync()
    {
        var result = await TraceAsync("list_players", () => _bridge.ExecuteAsync("list_players", new { }));
        if (result.Ok)
        {
            return result;
        }

        using var doc = JsonDocument.Parse("""{"players":[],"count":0,"bridgeOnline":false}""");
        return new CommandResult(true, "list_players", result.Source, result.Message, doc.RootElement.Clone(), new[] { result.Message });
    }

    public async Task<CommandResult> GetPlayerDetailsAsync(string? steamId, string? name) =>
        await TraceAsync("player_details", () => _bridge.ExecuteAsync("player_details", new { steamId, name }));

    public async Task<CommandResult> GetPlayerDetailsAsync(string? steamId, string? name, string? runtimeKey) =>
        await TraceAsync("player_details", () => _bridge.ExecuteAsync("player_details", new { steamId, name, runtimeKey }));

    public async Task<CommandResult> QueueStatusAsync() =>
        await TraceAsync("queue_status", () => _bridge.ExecuteAsync("queue_status", new { }, 2000));

    public async Task<CommandResult> ExecuteRawAsync(CommandRequest request)
    {
        var command = NormalizeAdminCommand(request.Command);
        if (string.IsNullOrWhiteSpace(command))
        {
            return new CommandResult(false, "admin_exec", "router", "Команда не задана.");
        }

        var local = await _localRcon.ExecuteAsync(command, request.TimeoutMs);
        if (local is not null)
        {
            return TraceResult("admin_exec", local with { Command = "admin_exec" });
        }

        var bridgeResult = await _bridge.ExecuteAsync("admin_exec", new
        {
            commandText = command,
            args = request.Args ?? new Dictionary<string, JsonElement>()
        }, request.TimeoutMs);
        return TraceResult("admin_exec", bridgeResult);
    }

    public async Task<CommandResult> SendChatAsync(ChatRequest request)
    {
        if (string.IsNullOrWhiteSpace(request.Message))
        {
            return new CommandResult(false, "chat", "router", "Сообщение не задано.");
        }

        var hasTarget = !string.IsNullOrWhiteSpace(request.TargetSteamId) ||
            !string.IsNullOrWhiteSpace(request.TargetName) ||
            !string.IsNullOrWhiteSpace(request.TargetRuntimeKey) ||
            !string.IsNullOrWhiteSpace(request.RuntimeKey);
        var bridgeCommand = hasTarget
            ? "chat_target"
            : "chat_broadcast";

        if (hasTarget)
        {
            var targetFailure = await ValidateLivePlayerTargetAsync(
                bridgeCommand,
                request.TargetSteamId,
                request.TargetName,
                FirstText(request.TargetRuntimeKey, request.RuntimeKey),
                requireSteam: false);
            if (targetFailure is not null)
            {
                return targetFailure;
            }
        }

        return await NativeThenBridgeAsync(bridgeCommand, "/api/chat", request,
            () => _bridge.ExecuteAsync(bridgeCommand, request));
    }

    public async Task<CommandResult> TeleportAsync(TeleportRequest request)
    {
        var targetFailure = await ValidateLivePlayerTargetAsync("teleport_player", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("teleport_player", "/api/player/teleport", request,
            () => _bridge.ExecuteAsync("teleport_player", request));
    }

    public async Task<CommandResult> AdminPlayerActionAsync(string action, PlayerActionRequest request)
    {
        var targetFailure = await ValidateLivePlayerTargetAsync(
            action.Equals("ban", StringComparison.OrdinalIgnoreCase) ? "ban_player" : "kick_player",
            request.SteamId,
            request.Name,
            request.RuntimeKey,
            requireSteam: false);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        var target = ResolveTargetToken(request.SteamId, request.Name);
        if (target is null)
        {
            return new CommandResult(false, action, "router", "Укажите SteamID или имя игрока.");
        }

        var command = action.Equals("ban", StringComparison.OrdinalIgnoreCase)
            ? $"#Ban {target} {request.Reason ?? ""}".Trim()
            : $"#Kick {target} {request.Reason ?? ""}".Trim();

        var payload = new
        {
            commandText = command,
            request.SteamId,
            request.Name,
            reason = request.Reason,
            request.RuntimeKey
        };

        return await NativeThenBridgeAsync("admin_exec", $"/api/player/{action.ToLowerInvariant()}", payload,
            () => _bridge.ExecuteAsync("admin_exec", payload));
    }

    public async Task<CommandResult> ChangeMoneyAsync(MoneyRequest request)
    {
        if (!HasAnyPlayerTarget(request.SteamId, request.Name, request.RuntimeKey))
        {
            return new CommandResult(false, "change_money", "router", "Укажите SteamID, имя игрока или runtimeKey.");
        }

        var payload = new
        {
            request.SteamId,
            request.Name,
            request.Amount,
            currency = string.IsNullOrWhiteSpace(request.Currency) ? "Normal" : request.Currency,
            request.RuntimeKey
        };

        var targetFailure = await ValidateLivePlayerTargetAsync("change_money", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        var local = await _localRcon.ExecuteMoneyAsync(request);
        if (local is not null)
        {
            return TraceResult("change_money", local with { Command = "change_money" });
        }

        return await NativeThenBridgeAsync("change_money", "/api/player/change-money", payload,
            () => _bridge.ExecuteAsync("change_money", payload));
    }

    public async Task<CommandResult> SetFameAsync(MoneyRequest request)
    {
        if (!HasAnyPlayerTarget(request.SteamId, request.Name, request.RuntimeKey))
        {
            return new CommandResult(false, "set_fame", "router", "Укажите SteamID, имя игрока или runtimeKey.");
        }

        var payload = new
        {
            request.SteamId,
            request.Name,
            request.Amount,
            request.RuntimeKey
        };

        var targetFailure = await ValidateLivePlayerTargetAsync("set_fame", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("set_fame", "/api/player/set-fame", payload,
            () => _bridge.ExecuteAsync("set_fame", payload));
    }

    public async Task<CommandResult> ChangeFameAsync(MoneyRequest request)
    {
        if (!HasAnyPlayerTarget(request.SteamId, request.Name, request.RuntimeKey))
        {
            return new CommandResult(false, "change_fame", "router", "Укажите SteamID, имя игрока или runtimeKey.");
        }

        var payload = new
        {
            request.SteamId,
            request.Name,
            request.Amount,
            request.RuntimeKey
        };

        var targetFailure = await ValidateLivePlayerTargetAsync("change_fame", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        var local = await _localRcon.ExecuteFameAsync(request);
        if (local is not null)
        {
            return TraceResult("change_fame", local with { Command = "change_fame" });
        }

        return await NativeThenBridgeAsync("change_fame", "/api/player/change-fame", payload,
            () => _bridge.ExecuteAsync("change_fame", payload, 25000), 25000);
    }

    public async Task<CommandResult> ClearInventoryAsync(PlayerActionRequest request)
    {
        var targetFailure = await ValidateLivePlayerTargetAsync("clear_inventory", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("clear_inventory", "/api/player/clear-inventory", request,
            () => _bridge.ExecuteAsync("clear_inventory", request));
    }

    public async Task<CommandResult> DeleteInventoryItemAsync(InventoryItemDeleteRequest request)
    {
        var entityId = FirstText(request.EntityId, request.ItemEntityId, request.RuntimeId);
        if (string.IsNullOrWhiteSpace(entityId))
        {
            return new CommandResult(false, "delete_inventory_item", "router", "EntityID предмета не указан.");
        }

        var payload = new
        {
            request.SteamId,
            request.Name,
            request.RuntimeKey,
            entityId,
            itemId = request.ItemId ?? "",
            itemClass = FirstText(request.ItemClass, request.ClassPath),
            classPath = FirstText(request.ClassPath, request.ItemClass),
            itemEntitySetup = FirstText(request.ItemEntitySetup, request.AssetPath),
            assetPath = FirstText(request.AssetPath, request.ItemEntitySetup)
        };

        var targetFailure = await ValidateLivePlayerTargetAsync("delete_inventory_item", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("delete_inventory_item", "/api/player/inventory/delete", payload,
            () => _bridge.ExecuteAsync("delete_inventory_item", payload, 25000), 25000);
    }

    public async Task<CommandResult> DestroyInventoryEntitiesAsync(InventoryEntitiesRequest request)
    {
        var entityIds = (request.EntityIds ?? Array.Empty<string>())
            .Select(id => (id ?? string.Empty).Trim())
            .Where(id => id.Length > 0)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
        var entitiesText = string.IsNullOrWhiteSpace(request.EntitiesText)
            ? string.Join(' ', entityIds)
            : request.EntitiesText.Trim();
        if (string.IsNullOrWhiteSpace(entitiesText))
        {
            return new CommandResult(false, "destroy_inventory_entities", "router", "EntityID list is empty.");
        }

        var payload = new
        {
            request.SteamId,
            request.Name,
            request.RuntimeKey,
            entityIds,
            entitiesText
        };

        var targetFailure = await ValidateLivePlayerTargetAsync("destroy_inventory_entities", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("destroy_inventory_entities", "/api/player/destroy-inventory-entities", payload,
            () => _bridge.ExecuteAsync("admin_exec", new
            {
                request.SteamId,
                request.Name,
                request.RuntimeKey,
                commandText = "ScumNeDjinDestroyEntities " + entitiesText
            }, 25000), 25000);
    }

    public async Task<CommandResult> DeliverItemBatchAsync(ItemBatchRequest request)
    {
        var items = NormalizeGrantItems(request).ToArray();
        if (items.Length == 0)
        {
            return new CommandResult(false, "deliver_items_batch", "router", "Предметы для выдачи не указаны.");
        }

        var itemsSpec = string.Join(';', items.Select(item =>
            $"{CleanItemsSpecToken(item.ItemId)}|{Math.Clamp(item.Quantity, 1, 100)}"));
        var payload = new
        {
            request.SteamId,
            request.Name,
            request.RuntimeKey,
            itemsSpec,
            Items = items.Select(item => new { item.ItemId, item.Quantity }).ToArray()
        };

        var targetFailure = await ValidateLivePlayerTargetAsync("deliver_items_batch", request.SteamId, request.Name, request.RuntimeKey, requireLocation: true);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        var local = await _localRcon.ExecuteItemBatchAsync(request, items);
        if (local is not null)
        {
            return TraceResult("deliver_items_batch", local with { Command = "deliver_items_batch" });
        }

        return await NativeThenBridgeAsync("deliver_items_batch", "/api/player/item-batch", payload,
            () => _bridge.ExecuteAsync("deliver_items_batch", payload, 60000), 60000);
    }

    public async Task<CommandResult> EquipItemAsync(ItemGrantRequest request)
    {
        if (string.IsNullOrWhiteSpace(request.ItemId))
        {
            return new CommandResult(false, "equip_item", "router", "ID предмета не указан.");
        }

        if (string.IsNullOrWhiteSpace(request.SteamId) &&
            string.IsNullOrWhiteSpace(request.Name) &&
            string.IsNullOrWhiteSpace(request.RuntimeKey))
        {
            return new CommandResult(false, "equip_item", "router", "Укажите игрока для экипировки предмета.");
        }

        var payload = new
        {
            request.SteamId,
            request.Name,
            request.RuntimeKey,
            request.ItemId,
            quantity = Math.Clamp(request.Quantity, 1, 10)
        };

        var targetFailure = await ValidateLivePlayerTargetAsync("equip_item", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("equip_item", "/api/player/equip-item", payload,
            () => _bridge.ExecuteAsync("equip_item", payload, 15000), 15000);
    }

    public async Task<CommandResult> PrewarmItemsAsync(ItemPrewarmRequest request)
    {
        var itemsSpec = NormalizePrewarmSpec(request);
        if (string.IsNullOrWhiteSpace(itemsSpec))
        {
            return new CommandResult(false, "prewarm_items", "router", "Предметы для проверки не указаны.");
        }

        var payload = new
        {
            itemsSpec
        };

        return await NativeThenBridgeAsync("prewarm_items", "/api/items/prewarm", payload,
            () => _bridge.ExecuteAsync("prewarm_items", payload, request.TimeoutMs ?? 30000), request.TimeoutMs ?? 30000);
    }

    public async Task<CommandResult> SpawnVehicleAsync(VehicleSpawnRequest request)
    {
        var payload = request with { VehicleId = VehicleCatalog.NormalizeVehicleId(request.VehicleId) };
        var targetFailure = await ValidateLivePlayerTargetAsync("spawn_vehicle", payload.SteamId, payload.Name, payload.RuntimeKey, requireLocation: true);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        var local = await _localRcon.ExecuteVehicleSpawnAsync(payload);
        if (local is not null)
        {
            return TraceResult("spawn_vehicle", local with { Command = "spawn_vehicle" });
        }

        return await NativeThenBridgeAsync("spawn_vehicle", "/api/player/spawn-vehicle", payload,
            () => _bridge.ExecuteAsync("spawn_vehicle", payload, 25000), 25000);
    }

    public async Task<CommandResult> SpawnActorAsync(ActorSpawnRequest request)
    {
        if (string.IsNullOrWhiteSpace(request.ActorClass))
        {
            return new CommandResult(false, "spawn_actor", "router", "Класс актора не указан.");
        }

        if (string.IsNullOrWhiteSpace(request.SteamId) &&
            string.IsNullOrWhiteSpace(request.Name) &&
            string.IsNullOrWhiteSpace(request.RuntimeKey))
        {
            return new CommandResult(false, "spawn_actor", "router", "Укажите игрока для создания актора.");
        }

        var payload = new
        {
            request.SteamId,
            request.Name,
            request.RuntimeKey,
            request.ActorClass
        };

        var targetFailure = await ValidateLivePlayerTargetAsync("spawn_actor", request.SteamId, request.Name, request.RuntimeKey, requireLocation: true);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("spawn_actor", "/api/player/spawn-actor", payload,
            () => _bridge.ExecuteAsync("spawn_actor", payload, 25000), 25000);
    }

    public async Task<CommandResult> ZombieInfectAsync(ZombieInfectRequest request)
    {
        if (string.IsNullOrWhiteSpace(request.SteamId) &&
            string.IsNullOrWhiteSpace(request.Name) &&
            string.IsNullOrWhiteSpace(request.RuntimeKey))
        {
            return new CommandResult(false, "zombie_infect", "router", "Укажите игрока для заражения.");
        }

        var payload = new
        {
            request.SteamId,
            request.Name,
            request.RuntimeKey,
            request.MeshPath,
            Gender = FirstText(request.Gender, "Female"),
            TransformMode = FirstText(request.TransformMode, "setgender"),
            request.Message,
            request.Announce,
            request.Whisper,
            request.SuitFallback
        };

        var targetFailure = await ValidateLivePlayerTargetAsync("zombie_transform", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("zombie_transform", "/api/player/turn-zombie", payload,
            () => _bridge.ExecuteAsync("zombie_transform", payload, 25000), 25000);
    }

    public async Task<CommandResult> DestroyVehicleAsync(PlayerActionRequest request)
    {
        var vehicleRef = string.IsNullOrWhiteSpace(request.EntityId) ? request.Reason : request.EntityId;
        if (string.IsNullOrWhiteSpace(vehicleRef))
        {
            return new CommandResult(false, "destroy_vehicle_ref", "router", "ID транспорта для удаления не указан.");
        }

        var payload = new
        {
            request.SteamId,
            request.Name,
            request.RuntimeKey,
            vehicleRef,
            request.EntityId,
            reason = string.IsNullOrWhiteSpace(request.Reason) || request.Reason == vehicleRef ? "panel" : request.Reason
        };

        var local = await _localRcon.ExecuteDestroyVehicleAsync(vehicleRef);
        if (local is not null)
        {
            return TraceResult("destroy_vehicle_ref", local with { Command = "destroy_vehicle_ref" });
        }

        return await NativeThenBridgeAsync("destroy_vehicle_ref", "/api/vehicle/destroy", payload,
            () => _bridge.ExecuteAsync("destroy_vehicle_ref", payload, 10000), 10000);
    }

    public async Task<CommandResult> SetAttributesAsync(CharacterAttributesRequest request)
    {
        var strength = AttributeValue(request, "strength", "Strength", request.Strength);
        var constitution = AttributeValue(request, "constitution", "Constitution", request.Constitution);
        var dexterity = AttributeValue(request, "dexterity", "Dexterity", request.Dexterity);
        var intelligence = AttributeValue(request, "intelligence", "Intelligence", request.Intelligence);
        var payload = new
        {
            request.SteamId,
            request.Name,
            request.RuntimeKey,
            strength,
            constitution,
            dexterity,
            intelligence
        };

        var targetFailure = await ValidateLivePlayerTargetAsync("set_attributes", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("set_attributes", "/api/player/set-attributes", payload,
            () => _bridge.ExecuteAsync("set_attributes", payload, 15000), 15000);
    }

    public async Task<CommandResult> SetSkillAsync(CharacterSkillRequest request)
    {
        var targetFailure = await ValidateLivePlayerTargetAsync("set_skill", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("set_skill", "/api/player/set-skill", request,
            () => _bridge.ExecuteAsync("set_skill", request));
    }

    public async Task<CommandResult> ApplyCharacterPackAsync(CharacterPackRequest request)
    {
        var pack = string.IsNullOrWhiteSpace(request.Pack) ? request.PackName : request.Pack;
        var payload = new
        {
            request.SteamId,
            request.Name,
            request.RuntimeKey,
            pack = string.IsNullOrWhiteSpace(pack) ? "fullstats" : pack
        };

        var targetFailure = await ValidateLivePlayerTargetAsync("apply_character_pack", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("apply_character_pack", "/api/player/apply-character-pack", payload,
            () => _bridge.ExecuteAsync("apply_character_pack", payload, 15000), 15000);
    }

    public async Task<CommandResult> RunPlayerPluginCommandAsync(PlayerPluginCommandRequest request, int? timeoutMs = null)
    {
        if (string.IsNullOrWhiteSpace(request.Message))
        {
            return new CommandResult(false, "player_plugin_command", "router", "Команда плагина не задана.");
        }

        if (string.IsNullOrWhiteSpace(request.SteamId) &&
            string.IsNullOrWhiteSpace(request.Name) &&
            string.IsNullOrWhiteSpace(request.RuntimeKey))
        {
            return new CommandResult(false, "player_plugin_command", "router", "Укажите игрока для выполнения команды плагина.");
        }

        var targetFailure = await ValidateLivePlayerTargetAsync("player_plugin_command", request.SteamId, request.Name, request.RuntimeKey);
        if (targetFailure is not null)
        {
            return targetFailure;
        }

        return await NativeThenBridgeAsync("player_plugin_command", "/api/plugin-command", request,
            () => _bridge.ExecuteAsync("player_plugin_command", request, timeoutMs), timeoutMs);
    }

    public async Task<CommandResult> ExecuteBridgeAsync(string command, object args, int? timeoutMs = null) =>
        await TraceAsync(command, () => _bridge.ExecuteAsync(command, args, timeoutMs));

    private async Task<CommandResult> TraceAsync(string command, Func<Task<CommandResult>> action)
    {
        var result = await action();
        return TraceResult(command, result);
    }

    private CommandResult TraceResult(string command, CommandResult result)
    {
        _events.Add(new WardenEvent("command", DateTimeOffset.UtcNow, result.Message, new Dictionary<string, object?>
        {
            ["command"] = command,
            ["ok"] = result.Ok,
            ["source"] = result.Source
        }));
        return result;
    }

    private async Task<CommandResult> NativeThenBridgeAsync(
        string command,
        string nativePath,
        object payload,
        Func<Task<CommandResult>> bridgeAction,
        int? timeoutMs = null)
    {
        var bridge = await bridgeAction();
        return TraceResult(command, bridge);
    }

    private async Task<CommandResult?> ValidateLivePlayerTargetAsync(
        string command,
        string? steamId,
        string? name,
        string? runtimeKey,
        bool requireLocation = false,
        bool requireSteam = true)
    {
        if (!HasAnyPlayerTarget(steamId, name, runtimeKey))
        {
            return new CommandResult(false, command, "router", "Укажите SteamID, имя игрока или runtimeKey.");
        }

        var result = await _bridge.ExecuteAsync("resolve_player_target", new
        {
            steamId,
            name,
            runtimeKey,
            reason = command,
            requireLocation,
            requireSteam
        }, requireLocation ? 8000 : 5000);

        if (result.Ok)
        {
            return null;
        }

        return TraceResult(command, result with { Command = command });
    }

    private static string NormalizeAdminCommand(string? command)
    {
        var trimmed = (command ?? "").Trim();
        if (trimmed.Length == 0)
        {
            return "";
        }

        return trimmed.StartsWith('#') ? trimmed : "#" + trimmed;
    }

    private static string? ResolveTargetToken(string? steamId, string? name)
    {
        if (!string.IsNullOrWhiteSpace(steamId))
        {
            return steamId.Trim();
        }

        return string.IsNullOrWhiteSpace(name) ? null : $"\"{name.Trim()}\"";
    }

    private static bool HasAnyPlayerTarget(string? steamId, string? name, string? runtimeKey) =>
        !string.IsNullOrWhiteSpace(steamId) ||
        !string.IsNullOrWhiteSpace(name) ||
        !string.IsNullOrWhiteSpace(runtimeKey);

    private static string FirstText(params string?[] values) =>
        values.Select(value => value?.Trim() ?? "")
            .FirstOrDefault(value => value.Length > 0) ?? "";

    private static IEnumerable<ItemGrantRequest> NormalizeGrantItems(ItemBatchRequest request)
    {
        if (request.Items is { Count: > 0 })
        {
            foreach (var item in request.Items)
            {
                if (LooksLikeScumItemId(item.ItemId))
                {
                    yield return item;
                }
            }
        }

        if (LooksLikeScumItemId(request.ItemId))
        {
            yield return new ItemGrantRequest(request.SteamId, request.Name, request.ItemId!.Trim(), request.Quantity, false, request.RuntimeKey);
        }
    }

    private static string CleanItemsSpecToken(string? value) =>
        (value ?? "").Replace("|", "", StringComparison.Ordinal).Replace(";", "", StringComparison.Ordinal).Trim();

    private static string NormalizePrewarmSpec(ItemPrewarmRequest request)
    {
        if (!string.IsNullOrWhiteSpace(request.ItemsSpec))
        {
            return request.ItemsSpec;
        }

        var items = new List<string>();
        if (request.Items is { Count: > 0 })
        {
            items.AddRange(request.Items
                .Where(LooksLikeScumItemId)
                .Select(item => $"{CleanItemsSpecToken(item)}|1"));
        }

        if (LooksLikeScumItemId(request.ItemId))
        {
            items.Add($"{CleanItemsSpecToken(request.ItemId)}|{Math.Clamp(request.Quantity, 1, 100)}");
        }

        return string.Join(';', items);
    }

    private static bool LooksLikeScumItemId(string? value)
    {
        var text = (value ?? "").Trim();
        return text.Length > 0 &&
               text.Any(char.IsLetter) &&
               text.All(ch => char.IsLetterOrDigit(ch) || ch is '_' or '-' or '.');
    }

    private static double AttributeValue(CharacterAttributesRequest request, string lowerKey, string upperKey, double? fallback)
    {
        if (fallback.HasValue)
        {
            return fallback.Value;
        }

        if (request.Attributes is not null)
        {
            if (request.Attributes.TryGetValue(lowerKey, out var lower))
            {
                return lower;
            }

            if (request.Attributes.TryGetValue(upperKey, out var upper))
            {
                return upper;
            }
        }

        return 0;
    }
}

public static class CharacterCatalog
{
    public static readonly object[] Skills =
    {
        new { key = "endurance", name = "Выносливость", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "running", name = "Бег", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "brawling", name = "Рукопашный бой", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "melee weapons", name = "Холодное оружие", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "archery", name = "Луки", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "rifles", name = "Винтовки", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "handguns", name = "Пистолеты", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "thievery", name = "Воровство", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "demolition", name = "Подрывное дело", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "motorcycling", name = "Мотоциклы", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "driving", name = "Вождение", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "throwing", name = "Метание", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "stealth", name = "Скрытность", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "awareness", name = "Внимательность", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "camouflage", name = "Маскировка", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "engineering", name = "Инженерия", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "sniping", name = "Снайперская стрельба", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "survival", name = "Выживание", levels = new[] { "нет", "базовый", "средний", "продвинутый" } },
        new { key = "medical", name = "Медицина", levels = new[] { "нет", "базовый", "средний", "продвинутый" } }
    };
}
