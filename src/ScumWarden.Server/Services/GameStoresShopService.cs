using System.Globalization;
using System.Text.Json;

namespace ScumWarden.Server.Services;

public sealed class GameStoresShopService
{
    private readonly HttpClient _http;
    private readonly PluginConfigStore _store;

    public GameStoresShopService(HttpClient http, PluginConfigStore store)
    {
        _http = http;
        _store = store;
        _http.Timeout = TimeSpan.FromSeconds(20);
    }

    public async Task<GameStoresItemsResult> GetItemsAsync(JsonElement config, string steamId, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(steamId))
        {
            return new GameStoresItemsResult(false, Array.Empty<GameStoresItem>(), "SteamID is empty.", null);
        }

        var credentials = ReadCredentials(config);
        if (!credentials.Ok)
        {
            return new GameStoresItemsResult(false, Array.Empty<GameStoresItem>(), credentials.Message, null);
        }

        var query = credentials.QueryBase();
        query["items"] = "true";
        query["steam_id"] = steamId.Trim();
        var response = await GetJsonAsync(query, cancellationToken);
        if (!response.Ok)
        {
            await AppendLogAsync("api-error", response.Message, response.Code);
            return new GameStoresItemsResult(false, Array.Empty<GameStoresItem>(), response.Message, response.Code);
        }

        try
        {
            using var doc = JsonDocument.Parse(response.Body);
            var root = doc.RootElement;
            var code = IntOrNull(root, "code");
            if (code == 104)
            {
                return new GameStoresItemsResult(true, Array.Empty<GameStoresItem>(), "Bucket is empty.", code);
            }

            if (!LooksSuccessful(root))
            {
                var message = First(Str(root, "message"), "GameStores API returned an error.");
                await AppendLogAsync("api-error", message, code);
                return new GameStoresItemsResult(false, Array.Empty<GameStoresItem>(), message, code);
            }

            var items = EnumerateDataArray(root)
                .Select(ParseItem)
                .Where(item => !string.IsNullOrWhiteSpace(item.Id))
                .ToArray();
            return new GameStoresItemsResult(true, items, "OK", code);
        }
        catch (JsonException ex)
        {
            await AppendLogAsync("api-json-error", ex.Message, null);
            return new GameStoresItemsResult(false, Array.Empty<GameStoresItem>(), "GameStores returned invalid JSON.", null);
        }
    }

    public async Task<GameStoresApiResult> ConfirmGivenAsync(JsonElement config, string bucketItemId, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(bucketItemId))
        {
            return new GameStoresApiResult(false, "GameStores bucket item id is empty.", null);
        }

        var credentials = ReadCredentials(config);
        if (!credentials.Ok)
        {
            return new GameStoresApiResult(false, credentials.Message, null);
        }

        var query = credentials.QueryBase();
        query["gived"] = "true";
        query["id"] = bucketItemId.Trim();
        var response = await GetJsonAsync(query, cancellationToken);
        if (!response.Ok)
        {
            await AppendLogAsync("confirm-http-error", response.Message, response.Code);
            return new GameStoresApiResult(false, response.Message, response.Code);
        }

        if (string.IsNullOrWhiteSpace(response.Body))
        {
            return new GameStoresApiResult(true, "OK", null);
        }

        try
        {
            using var doc = JsonDocument.Parse(response.Body);
            var root = doc.RootElement;
            var code = IntOrNull(root, "code");
            if (LooksSuccessful(root))
            {
                return new GameStoresApiResult(true, "OK", code);
            }

            var message = First(Str(root, "message"), "GameStores confirm returned an error.");
            await AppendLogAsync("confirm-api-error", message, code);
            return new GameStoresApiResult(false, message, code);
        }
        catch (JsonException)
        {
            return new GameStoresApiResult(true, "GameStores confirm returned non-JSON success body.", null);
        }
    }

    private async Task<GameStoresHttpResult> GetJsonAsync(Dictionary<string, string> query, CancellationToken cancellationToken)
    {
        var url = "https://gamestores.app/api/?" + string.Join("&", query
            .Where(pair => !string.IsNullOrWhiteSpace(pair.Value))
            .Select(pair => $"{Uri.EscapeDataString(pair.Key)}={Uri.EscapeDataString(pair.Value)}"));
        try
        {
            using var response = await _http.GetAsync(url, cancellationToken);
            var body = await response.Content.ReadAsStringAsync(cancellationToken);
            return response.IsSuccessStatusCode
                ? new GameStoresHttpResult(true, body, "OK", (int)response.StatusCode)
                : new GameStoresHttpResult(false, body, $"GameStores HTTP {(int)response.StatusCode}.", (int)response.StatusCode);
        }
        catch (TaskCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return new GameStoresHttpResult(false, "", "GameStores request timed out.", null);
        }
        catch (HttpRequestException ex)
        {
            return new GameStoresHttpResult(false, "", ex.Message, null);
        }
    }

    private GameStoresCredentials ReadCredentials(JsonElement config)
    {
        var shopId = First(Str(config, "ShopId", "StoreId", "shopId", "storeId"), "0");
        var secret = Str(config, "SecretKey", "secretKey", "ApiKey", "apiKey");
        var serverId = First(Str(config, "ServerId", "GameStoresServerId", "serverId", "gameStoresServerId"), "0");
        if (shopId == "0" || string.IsNullOrWhiteSpace(secret))
        {
            return new GameStoresCredentials(false, shopId, secret, serverId, "Не настроены ShopId или SecretKey GameStores.");
        }

        return new GameStoresCredentials(true, shopId, secret, serverId, "OK");
    }

    private async Task AppendLogAsync(string status, string message, int? code)
    {
        await _store.AppendStateAsync("gamestores-shop", new
        {
            utc = DateTimeOffset.UtcNow,
            status,
            code,
            message = Trim(message, 500)
        });
    }

    private static IEnumerable<JsonElement> EnumerateDataArray(JsonElement root)
    {
        if (root.ValueKind == JsonValueKind.Array)
        {
            return root.EnumerateArray().ToArray();
        }

        if (root.ValueKind != JsonValueKind.Object)
        {
            return Array.Empty<JsonElement>();
        }

        foreach (var name in new[] { "data", "items", "result", "response", "responce" })
        {
            if (TryGet(root, name, out var value))
            {
                if (value.ValueKind == JsonValueKind.Array)
                {
                    return value.EnumerateArray().ToArray();
                }

                if (value.ValueKind == JsonValueKind.Object)
                {
                    var nested = EnumerateDataArray(value).ToArray();
                    if (nested.Length > 0)
                    {
                        return nested;
                    }
                }
            }
        }

        return Array.Empty<JsonElement>();
    }

    private static GameStoresItem ParseItem(JsonElement element) => new(
        Id: Str(element, "id", "Id"),
        Name: Str(element, "name", "Name", "title", "Title"),
        Amount: Str(element, "amount", "Amount", "quantity", "Quantity", "count", "Count"),
        Type: Str(element, "type", "Type"),
        Command: GameStoresCommand(element),
        ItemId: Str(element, "item_id", "itemId", "ItemId", "product_id", "productId"));

    private static string GameStoresCommand(JsonElement element)
    {
        var direct = Str(element, "command", "Command", "commands", "Commands");
        if (direct.Length > 0)
        {
            return direct;
        }

        if (TryGet(element, "data", out var data) && data.ValueKind == JsonValueKind.Object)
        {
            return Str(data, "command", "Command", "commands", "Commands");
        }

        return "";
    }

    private static bool LooksSuccessful(JsonElement root)
    {
        var status = Str(root, "result", "status", "state");
        if (status.Equals("success", StringComparison.OrdinalIgnoreCase) ||
            status.Equals("ok", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        var code = IntOrNull(root, "code");
        if (code is null or 100 or 107)
        {
            var message = Str(root, "message", "error", "errors");
            return message.Length == 0 || status.Length == 0;
        }

        return false;
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

    private static int? IntOrNull(JsonElement element, string name)
    {
        if (!TryGet(element, name, out var value))
        {
            return null;
        }

        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var n))
        {
            return n;
        }

        return int.TryParse(Str(element, name), NumberStyles.Integer, CultureInfo.InvariantCulture, out n) ? n : null;
    }

    private static string First(params string?[] values) =>
        values.Select(value => value?.Trim() ?? "")
            .FirstOrDefault(value => value.Length > 0) ?? "";

    private static string Trim(string value, int max) => value.Length <= max ? value : value[..max];

    private sealed record GameStoresCredentials(bool Ok, string ShopId, string SecretKey, string ServerId, string Message)
    {
        public Dictionary<string, string> QueryBase() => new(StringComparer.OrdinalIgnoreCase)
        {
            ["shop_id"] = ShopId,
            ["secret"] = SecretKey,
            ["server"] = string.IsNullOrWhiteSpace(ServerId) ? "0" : ServerId
        };
    }

    private sealed record GameStoresHttpResult(bool Ok, string Body, string Message, int? Code);
}

public sealed record GameStoresItem(
    string Id,
    string Name,
    string Amount,
    string Type,
    string Command,
    string ItemId);

public sealed record GameStoresItemsResult(bool Ok, IReadOnlyList<GameStoresItem> Items, string Message, int? Code);
public sealed record GameStoresApiResult(bool Ok, string Message, int? Code);
