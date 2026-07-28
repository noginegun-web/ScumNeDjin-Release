using System.Globalization;
using System.Text.Json;
using ScumWarden.Server.Configuration;

namespace ScumWarden.Server.Services;

public static class ClientTestSimulator
{
    public static CommandResult Execute(string command, object? args, JsonSerializerOptions json, WardenOptions options)
    {
        var payload = command switch
        {
            "list_players" => PlayersPayload(options),
            "player_details" => PlayerDetailsPayload(options, args),
            "queue_status" => new { available = true, depth = 0, pending = 0, busy = false, source = "client-test" },
            "delete_inventory_item" => new { deleted = true, route = "client-test", entityId = Text(args, "entityId") },
            "destroy_inventory_entities" => new { deleted = true, route = "client-test", entitiesText = Text(args, "entitiesText") },
            "clear_inventory" => new { cleared = true, route = "client-test" },
            "deliver_items_batch" => new { delivered = true, route = "client-test", itemsSpec = Text(args, "itemsSpec") },
            "equip_item" => new { equipped = true, route = "client-test", itemId = Text(args, "itemId") },
            "spawn_vehicle" => new { spawned = true, route = "client-test", entityId = "190110", vehicleId = Text(args, "vehicleId") },
            "destroy_vehicle_ref" => new { destroyed = true, route = "client-test", vehicleRef = Text(args, "vehicleRef") },
            "change_money" => new { changed = true, route = "client-test", amount = Number(args, "Amount", "amount"), currency = Text(args, "currency", "Normal") },
            "set_fame" => new { changed = true, route = "client-test", amount = Number(args, "Amount", "amount") },
            "set_attributes" => new { changed = true, route = "client-test", strength = Number(args, "strength"), constitution = Number(args, "constitution"), dexterity = Number(args, "dexterity"), intelligence = Number(args, "intelligence") },
            "set_skill" => new { changed = true, route = "client-test", skill = Text(args, "Skill"), level = Number(args, "Level", "level") },
            "apply_character_pack" => new { applied = true, route = "client-test", pack = Text(args, "pack", "fullstats") },
            "chat_broadcast" or "chat_target" => new { sent = true, route = "client-test", channel = Text(args, "Channel", "server"), message = Text(args, "Message") },
            "admin_exec" => new { queued = true, route = "client-test", commandText = Text(args, "commandText") },
            "player_plugin_command" => new { executed = true, route = "client-test", message = Text(args, "Message") },
            "cleanup_rentals" => new { cleaned = true, route = "client-test", removed = 1 },
            "prewarm_items" => new { checkedItems = true, route = "client-test", itemsSpec = Text(args, "itemsSpec") },
            _ => new { accepted = true, route = "client-test", command }
        };

        var jsonPayload = JsonSerializer.SerializeToElement(payload, json);
        return new CommandResult(true, command, "client-test", MessageFor(command), jsonPayload);
    }

    public static object PlayersEnvelope(WardenOptions options) => new
    {
        command = "players_snapshot",
        source = "client-test",
        message = "Клиентский тестовый режим: игроки сгенерированы локально.",
        payload = PlayersPayload(options),
        warnings = Array.Empty<string>(),
        completedAtUtc = DateTimeOffset.UtcNow
    };

    public static object[] Players(WardenOptions options) => new object[]
    {
        Player(options.ClientTestPlayerName, "76561198000000001", "1001", "client-local", 125000, 42, 280, -140240, 251120, 6200),
        Player("DsProject", "76561198000000002", "1002", "client-ds", 76500, 8, 120, -140640, 251520, 6200),
        Player("WILD-Test", "76561198000000003", "1003", "client-wild", 18800, 0, 48, -141020, 250940, 6200)
    };

    public static object Inventory(string? steamId, string? name) => new
    {
        source = "client-test inventory",
        rows = new object[]
        {
            InventoryRow("76561198000000001", 1001, 1001, 310001, "Weapon_MK18", "BlueprintGeneratedClass /Game/Items/Weapons/MK18/Weapon_MK18.Weapon_MK18_C", "", 0, "hands"),
            InventoryRow("76561198000000001", 1001, 1001, 310002, "Improvised_Backpack", "BlueprintGeneratedClass /Game/Items/Equipment/Backpacks/Improvised_Backpack.Improvised_Backpack_C", "", 0, "back"),
            InventoryRow("76561198000000001", 1001, 310002, 310003, "Emergency_bandage_Big", "BlueprintGeneratedClass /Game/Items/Medical/Emergency_bandage_Big.Emergency_bandage_Big_C", "", 1, "backpack"),
            InventoryRow("76561198000000001", 1001, 310002, 310004, "CannedGoulash", "BlueprintGeneratedClass /Game/Items/Food/CannedGoulash.CannedGoulash_C", "", 1, "backpack"),
            InventoryRow("76561198000000001", 1001, 310002, 310005, "Cal_556x45mm_Ammobox", "BlueprintGeneratedClass /Game/Items/Ammo/Cal_556x45mm_Ammobox.Cal_556x45mm_Ammobox_C", "", 1, "backpack")
        },
        quickSlots = new object[]
        {
            new { steamId = "76561198000000001", slotIndex = 1, itemClass = "Weapon_MK18", itemId = "Weapon_MK18", label = "MK18" },
            new { steamId = "76561198000000001", slotIndex = 2, itemClass = "Emergency_bandage_Big", itemId = "Emergency_bandage_Big", label = "Bandage" }
        },
        generatedAtUtc = DateTimeOffset.UtcNow
    };

    public static object Wallet(string? steamId, string? name) => new
    {
        source = "client-test wallet",
        rows = new object[]
        {
            new { steamId = steamId ?? "76561198000000001", name = name ?? "ClientTester", walletBalance = 125000, money = 125000, gold = 42, famePoints = 280, fame = 280 }
        },
        generatedAtUtc = DateTimeOffset.UtcNow
    };

    public static object Attributes(string? steamId, string? name) => new
    {
        source = "client-test attributes",
        rows = new object[]
        {
            new { steamId = steamId ?? "76561198000000001", name = name ?? "ClientTester", strength = 8.0, constitution = 7.5, dexterity = 7.0, intelligence = 6.5, rifles = 3, medical = 3, driving = 3 }
        },
        generatedAtUtc = DateTimeOffset.UtcNow
    };

    public static object Skills(string? steamId, string? name) => new
    {
        source = "client-test skills",
        readOnly = true,
        steamId = steamId ?? "76561198000000001",
        name = name ?? "ClientTester",
        skills = new object[]
        {
            new { steamId = steamId ?? "76561198000000001", playerName = name ?? "ClientTester", name = "Rifles", skillName = "Rifles", level = 3.0, experience = 1450.0 },
            new { steamId = steamId ?? "76561198000000001", playerName = name ?? "ClientTester", name = "Medical", skillName = "Medical", level = 3.0, experience = 810.0 },
            new { steamId = steamId ?? "76561198000000001", playerName = name ?? "ClientTester", name = "Driving", skillName = "Driving", level = 3.0, experience = 620.0 }
        },
        generatedAtUtc = DateTimeOffset.UtcNow
    };

    public static IReadOnlyList<Dictionary<string, object?>> Squads() => new[]
    {
        Row(("name", "WILD"), ("leader", "ClientTester"), ("members", 3), ("score", 1250)),
        Row(("name", "DsProject"), ("leader", "DsProject"), ("members", 2), ("score", 840))
    };

    public static IReadOnlyList<Dictionary<string, object?>> Flags() => new[]
    {
        Row(("name", "WILD Base"), ("x", -140240), ("y", 251120), ("z", 6200), ("owner", "ClientTester"))
    };

    public static IReadOnlyList<Dictionary<string, object?>> Vehicles() => new[]
    {
        Row(("entityId", "190110"), ("assetName", "BPC_Rager"), ("displayName", "Rager"), ("owner", "ClientTester"), ("rental", true)),
        Row(("entityId", "190111"), ("assetName", "BPC_Laika"), ("displayName", "Laika"), ("owner", "DsProject"), ("rental", false))
    };

    public static IReadOnlyList<Dictionary<string, object?>> Economy() => new[]
    {
        Row(("name", "ClientTester"), ("steamId", "76561198000000001"), ("money", 125000), ("gold", 42), ("fame", 280)),
        Row(("name", "DsProject"), ("steamId", "76561198000000002"), ("money", 76500), ("gold", 8), ("fame", 120))
    };

    public static IReadOnlyList<string> ServerLogLines() => new[]
    {
        "2026.05.14-12.00.00: 'ClientTester' 'Global: Проверка клиентского стенда'",
        "2026.05.14-12.01.00: 'DsProject' logged in. UserProfileID: 1002",
        "LogSCUM: 'WILD-Test' was killed by 'ClientTester'"
    };

    public static IReadOnlyList<string> RuntimeLogLines() => new[]
    {
        "[client-test] Panel started in local client test mode.",
        "[client-test] Bridge commands are simulated and do not touch a SCUM client process.",
        "[client-test] Plugin config saving is real; game actions are dry-run."
    };

    public static void SeedEvents(WardenEventStore events, WardenOptions options)
    {
        events.Add(new WardenEvent("chat", DateTimeOffset.UtcNow.AddMinutes(-4), "Проверка клиентского стенда", new Dictionary<string, object?>
        {
            ["player"] = options.ClientTestPlayerName,
            ["name"] = options.ClientTestPlayerName,
            ["steamId"] = "76561198000000001",
            ["channel"] = "Global"
        }));
        events.Add(new WardenEvent("login", DateTimeOffset.UtcNow.AddMinutes(-3), $"{options.ClientTestPlayerName} вошёл в тестовую сессию", new Dictionary<string, object?>
        {
            ["player"] = options.ClientTestPlayerName,
            ["name"] = options.ClientTestPlayerName,
            ["steamId"] = "76561198000000001",
            ["state"] = "joined"
        }));
        events.Add(new WardenEvent("kill", DateTimeOffset.UtcNow.AddMinutes(-2), "[WILD] ClientTester победил WILD-Test", new Dictionary<string, object?>
        {
            ["killer"] = options.ClientTestPlayerName,
            ["killerSteamId"] = "76561198000000001",
            ["victim"] = "WILD-Test",
            ["victimSteamId"] = "76561198000000003"
        }));
        events.Add(new WardenEvent("economy", DateTimeOffset.UtcNow.AddMinutes(-1), "Начисление денег в клиентском тесте", new Dictionary<string, object?>
        {
            ["player"] = options.ClientTestPlayerName,
            ["amount"] = 1000,
            ["currency"] = "Normal"
        }));
    }

    private static object PlayersPayload(WardenOptions options) => new
    {
        players = Players(options),
        count = 3,
        bridgeOnline = true,
        runtimeReady = true,
        clientTestMode = true
    };

    private static object PlayerDetailsPayload(WardenOptions options, object? args)
    {
        var name = Text(args, "name", options.ClientTestPlayerName);
        var steamId = Text(args, "steamId", "76561198000000001");
        return new
        {
            player = Player(name, steamId, "1001", "client-local", 125000, 42, 280, -140240, 251120, 6200),
            wallet = Wallet(steamId, name),
            attributes = Attributes(steamId, name),
            inventory = Inventory(steamId, name)
        };
    }

    private static object Player(string name, string steamId, string profileId, string runtimeKey, long money, long gold, long fame, double x, double y, double z) => new
    {
        name,
        steamId,
        online = true,
        status = "online",
        source = "client-test",
        userProfileId = profileId,
        serverUserProfileId = profileId,
        profileId,
        runtimeKey,
        runtimeReady = true,
        walletBalance = money,
        money,
        gold,
        famePoints = fame,
        fame,
        x,
        y,
        z
    };

    private static object InventoryRow(string steamId, long playerEntityId, long containerEntityId, long entityId, string itemId, string itemClass, string itemEntitySetup, int depth, string slot) => new
    {
        steamId,
        playerEntityId,
        containerEntityId,
        entityId,
        itemId,
        itemClass,
        itemEntitySetup,
        depth,
        slot,
        label = itemId
    };

    private static Dictionary<string, object?> Row(params (string Key, object? Value)[] values)
    {
        var row = new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase);
        foreach (var (key, value) in values)
        {
            row[key] = value;
        }
        return row;
    }

    private static string MessageFor(string command) => command switch
    {
        "list_players" => "Клиентский тестовый список игроков сформирован.",
        "chat_broadcast" or "chat_target" => "Клиентский тест: сообщение принято без отправки в SCUM.",
        "admin_exec" => "Клиентский тест: admin-команда принята без отправки в SCUM.",
        "delete_inventory_item" or "destroy_inventory_entities" or "clear_inventory" => "Клиентский тест: удаление отмечено как выполненное.",
        "deliver_items_batch" or "equip_item" => "Клиентский тест: выдача предметов отмечена как выполненная.",
        "spawn_vehicle" or "destroy_vehicle_ref" => "Клиентский тест: действие с транспортом отмечено как выполненное.",
        _ => "Клиентский тест: действие выполнено в dry-run режиме."
    };

    private static string Text(object? args, string key, string fallback = "") =>
        Value(args, key) switch
        {
            JsonElement { ValueKind: JsonValueKind.String } text => text.GetString() ?? fallback,
            JsonElement element when element.ValueKind is JsonValueKind.Number or JsonValueKind.True or JsonValueKind.False => element.ToString(),
            string text => text,
            null => fallback,
            var value => Convert.ToString(value, CultureInfo.InvariantCulture) ?? fallback
        };

    private static long Number(object? args, params string[] keys)
    {
        foreach (var key in keys)
        {
            var value = Value(args, key);
            if (value is JsonElement element)
            {
                if (element.ValueKind == JsonValueKind.Number && element.TryGetInt64(out var parsed))
                {
                    return parsed;
                }

                if (element.ValueKind == JsonValueKind.String && long.TryParse(element.GetString(), NumberStyles.Integer, CultureInfo.InvariantCulture, out parsed))
                {
                    return parsed;
                }
            }

            if (value is not null && long.TryParse(Convert.ToString(value, CultureInfo.InvariantCulture), NumberStyles.Integer, CultureInfo.InvariantCulture, out var converted))
            {
                return converted;
            }
        }

        return 0;
    }

    private static object? Value(object? args, string key)
    {
        if (args is null)
        {
            return null;
        }

        var type = args.GetType();
        var prop = type.GetProperty(key) ??
            type.GetProperties().FirstOrDefault(p => string.Equals(p.Name, key, StringComparison.OrdinalIgnoreCase));
        return prop?.GetValue(args);
    }
}
