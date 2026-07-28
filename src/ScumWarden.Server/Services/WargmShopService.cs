using System.Globalization;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace ScumWarden.Server.Services;

public sealed class WargmShopService
{
    private static readonly Regex SteamIdRegex = new(@"\b\d{17}\b", RegexOptions.Compiled);
    private readonly HttpClient _http;
    private readonly PluginConfigStore _store;

    public WargmShopService(HttpClient http, PluginConfigStore store)
    {
        _http = http;
        _store = store;
        _http.BaseAddress ??= new Uri("https://api.wargm.ru/v1/");
        _http.Timeout = TimeSpan.FromSeconds(20);
    }

    public async Task<object> SyncPendingAsync(CancellationToken cancellationToken = default)
    {
        var config = _store.GetConfig("wargm-shop");
        if (!Bool(config, "Enabled", false))
        {
            return new { synced = false, reason = "Магазин Wargm выключен в настройках.", pending = 0 };
        }

        var projectId = Int(config, "ProjectId", 0);
        var apiKey = Str(config, "ApiKey");
        if (projectId <= 0 || string.IsNullOrWhiteSpace(apiKey))
        {
            return new { synced = false, reason = "Не настроены ProjectId или API key Wargm.", pending = 0 };
        }

        var operations = await FetchPendingOperationsAsync(config, projectId, apiKey, cancellationToken);
        var rows = new List<Dictionary<string, object?>>();
        var skipped = new List<object>();
        foreach (var operation in operations)
        {
            var rule = MatchRule(config, operation);
            if (rule is null)
            {
                skipped.Add(new { operation.operationId, operation.offerId, operation.title, reason = "Нет подходящего правила выдачи." });
                continue;
            }

            var row = BuildPendingRow(operation, rule.Value);
            if (row is null)
            {
                skipped.Add(new { operation.operationId, operation.offerId, operation.title, reason = "Правило не содержит выдаваемых данных." });
                continue;
            }

            rows.Add(row);
        }

        await _store.SetStateAsync("wargm-pending", rows);
        await _store.AppendStateAsync("wargm-shop", new
        {
            utc = DateTimeOffset.UtcNow,
            status = "synced",
            fetched = operations.Count,
            pending = rows.Count,
            skipped = skipped.Count,
            skippedPreview = skipped.Take(10).ToArray()
        });

        return new
        {
            synced = true,
            fetched = operations.Count,
            pending = rows.Count,
            skipped = skipped.Count,
            skippedPreview = skipped.Take(10).ToArray()
        };
    }

    public async Task<object> ConfirmDeliveredAsync(CancellationToken cancellationToken = default)
    {
        var config = _store.GetConfig("wargm-shop");
        if (!Bool(config, "Enabled", false))
        {
            return new { confirmed = false, reason = "Магазин Wargm выключен в настройках.", scanned = 0, acked = 0 };
        }

        var projectId = Int(config, "ProjectId", 0);
        var apiKey = Str(config, "ApiKey");
        if (projectId <= 0 || string.IsNullOrWhiteSpace(apiKey))
        {
            return new { confirmed = false, reason = "Не настроены ProjectId или API key Wargm.", scanned = 0, acked = 0 };
        }

        var delivered = _store.GetState("wargm-delivered");
        if (delivered.ValueKind != JsonValueKind.Array)
        {
            return new { confirmed = true, scanned = 0, acked = 0, failed = 0 };
        }

        var confirmedKeys = ConfirmedWargmKeys();
        var endpoint = Bool(config, "UseClaimInsteadOfSuccess", false) ? "shop/operation_claim" : "shop/operation_success";
        var scanned = 0;
        var acked = 0;
        var failed = new List<object>();

        foreach (var row in delivered.EnumerateArray().TakeLast(200))
        {
            scanned++;
            var key = Str(row, "key");
            var operationId = First(Str(row, "operationId", "OperationId", "id", "Id"), OperationIdFromKey(key));
            if (operationId.Length == 0)
            {
                continue;
            }

            var confirmKey = key.Length > 0 ? key : "operationId:" + operationId;
            if (confirmedKeys.Contains(confirmKey) || confirmedKeys.Contains("operationId:" + operationId))
            {
                continue;
            }

            if (await ConfirmOperationAsync(config, endpoint, projectId, apiKey, operationId, cancellationToken))
            {
                acked++;
                confirmedKeys.Add(confirmKey);
                confirmedKeys.Add("operationId:" + operationId);
                await _store.AppendStateAsync("wargm-confirmed", new
                {
                    utc = DateTimeOffset.UtcNow,
                    status = "confirmed",
                    key = confirmKey,
                    operationId,
                    endpoint,
                    steamId = Str(row, "steamId"),
                    name = Str(row, "name"),
                    title = Str(row, "title")
                });
                await _store.AppendStateAsync("wargm-shop", new
                {
                    utc = DateTimeOffset.UtcNow,
                    status = "confirmed",
                    key = confirmKey,
                    operationId,
                    endpoint
                });
            }
            else
            {
                failed.Add(new { key = confirmKey, operationId, endpoint });
                await _store.AppendStateAsync("wargm-shop", new
                {
                    utc = DateTimeOffset.UtcNow,
                    status = "confirm-failed",
                    key = confirmKey,
                    operationId,
                    endpoint
                });
            }
        }

        return new
        {
            confirmed = true,
            endpoint,
            scanned,
            acked,
            failed = failed.Count,
            failedPreview = failed.Take(10).ToArray()
        };
    }

    private async Task<List<WargmOperation>> FetchPendingOperationsAsync(JsonElement config, int projectId, string apiKey, CancellationToken cancellationToken)
    {
        var query = new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase)
        {
            ["client"] = $"{projectId}:{apiKey.Trim()}",
            ["numeric_string"] = "true",
            ["status"] = "pending",
            ["claimed"] = "0",
            ["type"] = "shop"
        };

        var filter = NormalizeDeliveryFilter(Str(config, "DeliveryFilter"));
        if (filter.Length > 0)
        {
            query["delivery"] = filter;
        }

        var serverId = Int(config, "WargmServerId", 0);
        if (serverId > 0)
        {
            query["server_id"] = serverId.ToString(CultureInfo.InvariantCulture);
        }

        var url = "shop/operations?" + string.Join("&", query
            .Where(pair => !string.IsNullOrWhiteSpace(pair.Value))
            .Select(pair => $"{Uri.EscapeDataString(pair.Key)}={Uri.EscapeDataString(pair.Value!)}"));

        using var response = await _http.GetAsync(url, cancellationToken);
        var json = await response.Content.ReadAsStringAsync(cancellationToken);
        if (!response.IsSuccessStatusCode)
        {
            await _store.AppendStateAsync("wargm-shop", new
            {
                utc = DateTimeOffset.UtcNow,
                status = "api-error",
                httpStatus = (int)response.StatusCode,
                message = Trim(json, 500)
            });
            return new List<WargmOperation>();
        }

        using var doc = JsonDocument.Parse(json);
        return EnumerateOperationArray(doc.RootElement)
            .Select(ParseOperation)
            .Where(operation => operation is not null)
            .Select(operation => operation!)
            .ToList();
    }

    private async Task<bool> ConfirmOperationAsync(JsonElement config, string endpoint, int projectId, string apiKey, string operationId, CancellationToken cancellationToken)
    {
        var query = new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase)
        {
            ["client"] = $"{projectId}:{apiKey.Trim()}",
            ["operation_id"] = operationId
        };

        var serverId = Int(config, "WargmServerId", 0);
        if (serverId > 0)
        {
            query["server_id"] = serverId.ToString(CultureInfo.InvariantCulture);
        }

        var url = endpoint + "?" + string.Join("&", query
            .Where(pair => !string.IsNullOrWhiteSpace(pair.Value))
            .Select(pair => $"{Uri.EscapeDataString(pair.Key)}={Uri.EscapeDataString(pair.Value!)}"));

        using var response = await _http.GetAsync(url, cancellationToken);
        var json = await response.Content.ReadAsStringAsync(cancellationToken);
        if (!response.IsSuccessStatusCode)
        {
            await _store.AppendStateAsync("wargm-shop", new
            {
                utc = DateTimeOffset.UtcNow,
                status = "confirm-http-error",
                endpoint,
                operationId,
                httpStatus = (int)response.StatusCode,
                message = Trim(json, 500)
            });
            return false;
        }

        if (string.IsNullOrWhiteSpace(json))
        {
            return true;
        }

        try
        {
            using var doc = JsonDocument.Parse(json);
            if (LooksLikeApiError(doc.RootElement))
            {
                await _store.AppendStateAsync("wargm-shop", new
                {
                    utc = DateTimeOffset.UtcNow,
                    status = "confirm-api-error",
                    endpoint,
                    operationId,
                    message = Trim(json, 500)
                });
                return false;
            }
        }
        catch (JsonException)
        {
            await _store.AppendStateAsync("wargm-shop", new
            {
                utc = DateTimeOffset.UtcNow,
                status = "confirm-json-warning",
                endpoint,
                operationId,
                message = Trim(json, 500)
            });
        }

        return true;
    }

    private static Dictionary<string, object?>? BuildPendingRow(WargmOperation operation, JsonElement rule)
    {
        var mode = NormalizeDeliveryMode(Str(rule, "DeliveryMode"));
        var row = new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase)
        {
            ["operationId"] = operation.operationId,
            ["offerId"] = operation.offerId,
            ["title"] = operation.title,
            ["steamId"] = operation.steamId,
            ["recipientName"] = operation.recipientName,
            ["delivery"] = operation.delivery,
            ["quantity"] = operation.quantity,
            ["createdAtUtc"] = operation.createdAtUtc == default ? null : operation.createdAtUtc,
            ["delivered"] = false,
            ["mode"] = mode.ToLowerInvariant()
        };

        switch (mode)
        {
            case "Money":
            case "Gold":
                row["mode"] = mode.Equals("Gold", StringComparison.OrdinalIgnoreCase) ? "gold" : "money";
                row["amount"] = ResolveAmount(rule, operation);
                return Convert.ToInt64(row["amount"], CultureInfo.InvariantCulture) != 0 ? row : null;
            case "Vehicle":
                row["mode"] = "vehicle";
                row["vehicleId"] = VehicleCatalog.NormalizeVehicleId(Str(rule, "VehicleAsset"));
                return string.IsNullOrWhiteSpace(Convert.ToString(row["vehicleId"], CultureInfo.InvariantCulture)) ? null : row;
            case "Fame":
            case "SetFame":
                row["mode"] = mode.Equals("SetFame", StringComparison.OrdinalIgnoreCase) ? "setfame" : "fame";
                row["amount"] = ResolveAmount(rule, operation);
                return Convert.ToInt64(row["amount"], CultureInfo.InvariantCulture) != 0 ? row : null;
            case "Attributes":
                row["mode"] = "attributes";
                row["strength"] = Int(rule, "Strength", 0);
                row["constitution"] = Int(rule, "Constitution", 0);
                row["dexterity"] = Int(rule, "Dexterity", 0);
                row["intelligence"] = Int(rule, "Intelligence", 0);
                return row;
            case "Skill":
                row["mode"] = "skill";
                row["skill"] = Str(rule, "SkillName");
                row["level"] = Int(rule, "SkillLevel", 0);
                row["experience"] = Int(rule, "SkillExperience", 0);
                return string.IsNullOrWhiteSpace(Convert.ToString(row["skill"], CultureInfo.InvariantCulture)) ? null : row;
            case "AllSkills":
                row["mode"] = "allskills";
                row["level"] = Math.Max(1, Int(rule, "SkillLevel", 4));
                row["experience"] = Int(rule, "SkillExperience", 0);
                return row;
            case "Vip":
                row["mode"] = "vip";
                row["durationDays"] = Int(rule, "DurationDays", 0);
                row["tier"] = Str(rule, "Tier");
                return row;
            case "CargoDropPlayer":
                row["mode"] = "cargodropplayer";
                return row;
            case "PlayerCommand":
            case "ServerCommand":
                row["mode"] = "command";
                row["command"] = Str(rule, "CommandTemplate");
                return string.IsNullOrWhiteSpace(Convert.ToString(row["command"], CultureInfo.InvariantCulture)) ? null : row;
            default:
                row["mode"] = "item";
                var itemsSpec = ResolveItemsSpec(rule, operation);
                row["itemsSpec"] = itemsSpec;
                var first = itemsSpec.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).FirstOrDefault() ?? "";
                var parts = first.Split('|');
                row["itemId"] = parts.ElementAtOrDefault(0) ?? "";
                row["quantity"] = int.TryParse(parts.ElementAtOrDefault(1), NumberStyles.Integer, CultureInfo.InvariantCulture, out var qty) ? qty : 1;
                return itemsSpec.Length > 0 ? row : null;
        }
    }

    private static JsonElement? MatchRule(JsonElement config, WargmOperation operation)
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

            var offer = Str(rule, "MatchOfferId");
            var item = Str(rule, "MatchItemId", "MatchObjectId");
            var title = Str(rule, "MatchTitleContains");
            if (offer.Length > 0 && string.Equals(offer, operation.offerId, StringComparison.OrdinalIgnoreCase))
            {
                return rule.Clone();
            }

            if (item.Length > 0 && string.Equals(item, operation.itemId, StringComparison.OrdinalIgnoreCase))
            {
                return rule.Clone();
            }

            if (title.Length > 0 && operation.title.Contains(title, StringComparison.OrdinalIgnoreCase))
            {
                return rule.Clone();
            }
        }

        return null;
    }

    private static string ResolveItemsSpec(JsonElement rule, WargmOperation operation)
    {
        if (TryGet(rule, "Items", out var items) && items.ValueKind == JsonValueKind.Array)
        {
            var parts = items.EnumerateArray()
                .Select(item => (id: CleanItemToken(Str(item, "ItemId")), qty: Math.Max(1, Int(item, "Quantity", 1))))
                .Where(item => item.id.Length > 0)
                .Select(item => $"{item.id}|{item.qty}")
                .ToArray();
            if (parts.Length > 0)
            {
                return string.Join(';', parts);
            }
        }

        var itemId = CleanItemToken(Str(rule, "ItemId"));
        if (itemId.Length == 0)
        {
            itemId = CleanItemToken(operation.itemId);
        }

        var quantity = Int(rule, "Quantity", 0);
        if (quantity <= 0)
        {
            quantity = Math.Max(1, operation.quantity);
        }

        return itemId.Length > 0 ? $"{itemId}|{Math.Clamp(quantity, 1, 1000)}" : "";
    }

    private static long ResolveAmount(JsonElement rule, WargmOperation operation)
    {
        var amount = Int(rule, "Amount", 0);
        return amount > 0 ? amount : Math.Max(0, operation.quantity);
    }

    private static IEnumerable<JsonElement> EnumerateOperationArray(JsonElement root)
    {
        if (root.ValueKind == JsonValueKind.Array)
        {
            return root.EnumerateArray().ToArray();
        }

        if (root.ValueKind != JsonValueKind.Object)
        {
            return Array.Empty<JsonElement>();
        }

        foreach (var propertyName in new[] { "data", "items", "operations", "result", "response", "responce" })
        {
            if (TryGet(root, propertyName, out var value) && value.ValueKind == JsonValueKind.Array)
            {
                return value.EnumerateArray().ToArray();
            }

            if (TryGet(root, propertyName, out value) && value.ValueKind == JsonValueKind.Object)
            {
                var nested = EnumerateOperationArray(value).ToArray();
                if (nested.Length > 0)
                {
                    return nested;
                }
            }
        }

        return root.EnumerateObject()
            .Where(property => property.Value.ValueKind == JsonValueKind.Object && LooksLikeOperation(property.Value))
            .Select(property => property.Value)
            .ToArray();
    }

    private HashSet<string> ConfirmedWargmKeys()
    {
        var rows = _store.GetState("wargm-confirmed");
        var keys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (rows.ValueKind != JsonValueKind.Array)
        {
            return keys;
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
                keys.Add("operationId:" + operationId);
            }
        }

        return keys;
    }

    private static string OperationIdFromKey(string key)
    {
        var text = (key ?? "").Trim();
        foreach (var prefix in new[] { "operationId:", "OperationId:", "id:", "Id:" })
        {
            if (text.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            {
                return text[prefix.Length..].Trim();
            }
        }

        return "";
    }

    private static bool LooksLikeOperation(JsonElement element)
    {
        var operationId = Str(element, "operation_id", "id", "operationId");
        if (operationId.Length == 0)
        {
            return false;
        }

        return Str(element, "offer_id", "goods_id", "product_id", "offerId", "productId").Length > 0 ||
               Str(element, "title", "name", "offer_title", "product_name", "goods_name", "offerTitle", "productName").Length > 0 ||
               Str(element, "status").Equals("pending", StringComparison.OrdinalIgnoreCase);
    }

    private static bool LooksLikeApiError(JsonElement element)
    {
        if (element.ValueKind != JsonValueKind.Object)
        {
            return false;
        }

        if (TryGet(element, "success", out var success))
        {
            if (success.ValueKind == JsonValueKind.False)
            {
                return true;
            }

            if (success.ValueKind == JsonValueKind.Number && success.TryGetInt32(out var successNumber) && successNumber == 0)
            {
                return true;
            }
        }

        var status = Str(element, "status", "state");
        if (status.Equals("error", StringComparison.OrdinalIgnoreCase) ||
            status.Equals("fail", StringComparison.OrdinalIgnoreCase) ||
            status.Equals("failed", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        if (Str(element, "error", "errors").Length > 0)
        {
            return true;
        }

        foreach (var envelopeName in new[] { "response", "responce", "result", "data" })
        {
            if (TryGet(element, envelopeName, out var nested) && LooksLikeApiError(nested))
            {
                return true;
            }
        }

        return false;
    }

    private static WargmOperation? ParseOperation(JsonElement element)
    {
        if (element.ValueKind != JsonValueKind.Object)
        {
            return null;
        }

        var offer = TryGet(element, "offer", out var offerElement) && offerElement.ValueKind == JsonValueKind.Object
            ? offerElement
            : default;
        var user = TryGet(element, "user", out var userElement) && userElement.ValueKind == JsonValueKind.Object
            ? userElement
            : default;

        var offerId = First(Str(element, "offer_id", "goods_id", "product_id", "offerId", "productId"), Str(offer, "id", "offer_id", "offerId"));
        var title = First(Str(element, "title", "name", "offer_title", "product_name", "goods_name", "offerTitle", "productName"), Str(offer, "title", "name"));
        var itemId = First(Str(element, "item", "item_id", "itemId", "asset", "asset_id", "assetId"), Str(offer, "item", "item_id", "itemId", "asset", "asset_id", "assetId"));
        var steam = First(
            Str(element, "steam_id", "user_steam_id", "steamId", "userSteamId", "steam64", "steam_id64", "steamId64"),
            Str(user, "steam_id", "steamId", "steam64", "steam_id64", "steamId64"));
        steam = NormalizeSteamId(steam.Length > 0 ? steam : ExtractSteamId(element));
        var recipient = First(
            Str(element, "recipient", "recipientName", "target", "player", "nickname", "player_name", "playerName", "username", "userName"),
            Str(user, "name", "nickname", "username", "display_name", "displayName"));

        return new WargmOperation(
            operationId: Str(element, "operation_id", "id", "operationId"),
            offerId: offerId,
            title: title,
            itemId: itemId,
            recipientName: recipient,
            steamId: steam,
            delivery: Str(element, "delivery"),
            quantity: Math.Max(1, Int(element, "quantity", Int(element, "qty", Int(element, "count", 1)))),
            createdAtUtc: DateTimeOffset.UtcNow);
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

    private static string NormalizeDeliveryMode(string? mode)
    {
        var normalized = (mode ?? "").Trim();
        if (normalized.Equals("money", StringComparison.OrdinalIgnoreCase)) return "Money";
        if (normalized.Equals("gold", StringComparison.OrdinalIgnoreCase)) return "Gold";
        if (normalized.Equals("vehicle", StringComparison.OrdinalIgnoreCase)) return "Vehicle";
        if (normalized.Equals("fame", StringComparison.OrdinalIgnoreCase) || normalized.Equals("changefame", StringComparison.OrdinalIgnoreCase) || normalized.Equals("famepoints", StringComparison.OrdinalIgnoreCase)) return "Fame";
        if (normalized.Equals("setfame", StringComparison.OrdinalIgnoreCase) || normalized.Equals("setfamepoints", StringComparison.OrdinalIgnoreCase)) return "SetFame";
        if (normalized.Equals("attributes", StringComparison.OrdinalIgnoreCase) || normalized.Equals("attribute", StringComparison.OrdinalIgnoreCase) || normalized.Equals("stats", StringComparison.OrdinalIgnoreCase)) return "Attributes";
        if (normalized.Equals("skill", StringComparison.OrdinalIgnoreCase)) return "Skill";
        if (normalized.Equals("allskills", StringComparison.OrdinalIgnoreCase) || normalized.Equals("allskill", StringComparison.OrdinalIgnoreCase) || normalized.Equals("skillpack", StringComparison.OrdinalIgnoreCase) || normalized.Equals("skillspack", StringComparison.OrdinalIgnoreCase)) return "AllSkills";
        if (normalized.Equals("vip", StringComparison.OrdinalIgnoreCase) || normalized.Equals("vipprivilege", StringComparison.OrdinalIgnoreCase) || normalized.Equals("subscription", StringComparison.OrdinalIgnoreCase)) return "Vip";
        if (normalized.Equals("cargodrop", StringComparison.OrdinalIgnoreCase) || normalized.Equals("cargodropplayer", StringComparison.OrdinalIgnoreCase) || normalized.Equals("airdrop", StringComparison.OrdinalIgnoreCase)) return "CargoDropPlayer";
        if (normalized.Equals("playercommand", StringComparison.OrdinalIgnoreCase) || normalized.Equals("player-command", StringComparison.OrdinalIgnoreCase)) return "PlayerCommand";
        if (normalized.Equals("servercommand", StringComparison.OrdinalIgnoreCase) || normalized.Equals("server-command", StringComparison.OrdinalIgnoreCase)) return "ServerCommand";
        return "SpawnItem";
    }

    private static string NormalizeDeliveryFilter(string? mode)
    {
        var normalized = (mode ?? "").Trim();
        return normalized.Equals("api", StringComparison.OrdinalIgnoreCase) ||
               normalized.Equals("admin", StringComparison.OrdinalIgnoreCase) ||
               normalized.Equals("online", StringComparison.OrdinalIgnoreCase)
            ? normalized.ToLowerInvariant()
            : "";
    }

    private static string NormalizeSteamId(string? value)
    {
        var match = SteamIdRegex.Match(value ?? "");
        return match.Success ? match.Value : "";
    }

    private static string ExtractSteamId(JsonElement element) => NormalizeSteamId(element.GetRawText());
    private static string First(params string[] values) => values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? "";
    private static string CleanSpec(string value) => (value ?? "").Replace("|", "", StringComparison.Ordinal).Replace(";", "", StringComparison.Ordinal).Trim();
    private static string CleanItemToken(string value)
    {
        var itemId = CleanSpec(value);
        return LooksLikeScumItemId(itemId) ? itemId : "";
    }

    private static bool LooksLikeScumItemId(string value)
    {
        var text = value.Trim();
        return text.Length > 0 &&
               text.Any(char.IsLetter) &&
               text.All(ch => char.IsLetterOrDigit(ch) || ch is '_' or '-' or '.');
    }
    private static string Trim(string value, int max) => value.Length <= max ? value : value[..max];

    private sealed record WargmOperation(
        string operationId,
        string offerId,
        string title,
        string itemId,
        string recipientName,
        string steamId,
        string delivery,
        int quantity,
        DateTimeOffset createdAtUtc);
}
