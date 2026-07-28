using System.Text.Json;
using System.Text.Json.Serialization;

namespace ScumWarden.Server.Services;

public static class WardenJson
{
    public static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = false,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };
}

public sealed record ApiEnvelope([property: JsonPropertyName("ok")] bool IsOk, object? Data = null, string? Error = null)
{
    public static ApiEnvelope Ok(object? data) => new(true, data);
    public static ApiEnvelope Fail(string error) => new(false, null, error);
    public static ApiEnvelope FromCommand(CommandResult result) =>
        result.Ok ? Ok(result.ToResponse()) : Fail(result.Message);
}

public sealed record CommandResult(
    bool Ok,
    string Command,
    string Source,
    string Message,
    JsonElement? Payload = null,
    IReadOnlyList<string>? Warnings = null)
{
    public object ToResponse() => new
    {
        command = Command,
        source = Source,
        message = Message,
        payload = Payload,
        warnings = Warnings,
        completedAtUtc = DateTimeOffset.UtcNow
    };

    public object? DataOrDefault(string propertyName)
    {
        if (Payload is not { ValueKind: JsonValueKind.Object } payload)
        {
            return Array.Empty<object>();
        }

        return payload.TryGetProperty(propertyName, out var prop)
            ? JsonSerializer.Deserialize<object>(prop.GetRawText(), WardenJson.Options)
            : Array.Empty<object>();
    }
}

public sealed record BridgeStatus(
    bool Online,
    string BridgePath,
    DateTimeOffset? HeartbeatUtc,
    double? HeartbeatAgeSeconds,
    string Message);

public sealed record StoreStatus(bool Online, string Path, string Message);

public sealed record CommandRequest(string? Command, Dictionary<string, JsonElement>? Args = null, int? TimeoutMs = null);
public sealed record AdminCommandProbeRequest(string? Verbs = null, string? CommandText = null, int? TimeoutMs = null);
public sealed record ChatRequest(string Message, string Channel = "server", string? TargetSteamId = null, string? TargetName = null, string? TargetRuntimeKey = null, string? RuntimeKey = null);
public sealed record TeleportRequest(string? SteamId, string? Name, double X, double Y, double Z, string? RuntimeKey = null);
public sealed record PlayerActionRequest(string? SteamId, string? Name, string? Reason = null, string? RuntimeKey = null, string? EntityId = null);
public sealed record ZombieInfectRequest(
    string? SteamId,
    string? Name,
    string? RuntimeKey = null,
    string? MeshPath = null,
    string? Gender = null,
    string? TransformMode = null,
    string? Message = null,
    bool? Announce = null,
    bool? Whisper = null,
    bool? SuitFallback = null);
public sealed record InventoryEntitiesRequest(string? SteamId, string? Name, string? RuntimeKey = null, IReadOnlyList<string>? EntityIds = null, string? EntitiesText = null);
public sealed record InventoryItemDeleteRequest(
    string? SteamId,
    string? Name,
    string? RuntimeKey = null,
    string? EntityId = null,
    string? ItemEntityId = null,
    string? RuntimeId = null,
    string? ItemId = null,
    string? ItemClass = null,
    string? ClassPath = null,
    string? ItemEntitySetup = null,
    string? AssetPath = null);
public sealed record MoneyRequest(string? SteamId, string? Name, long Amount, string Mode = "add", string Currency = "Normal", string? RuntimeKey = null);
public sealed record ItemGrantRequest(string? SteamId, string? Name, string ItemId, int Quantity = 1, bool Equip = false, string? RuntimeKey = null);
public sealed record ItemBatchRequest(string? SteamId, string? Name, IReadOnlyList<ItemGrantRequest>? Items, string? RuntimeKey = null, string? ItemId = null, int Quantity = 1)
{
    public static ItemBatchRequest FromSingle(ItemGrantRequest request) =>
        new(request.SteamId, request.Name, new[] { request }, request.RuntimeKey);
}
public sealed record ItemPrewarmRequest(IReadOnlyList<string>? Items = null, string? ItemsSpec = null, string? ItemId = null, int Quantity = 1, int? TimeoutMs = null);
public sealed record VehicleSpawnRequest(string? SteamId, string? Name, string VehicleId, string? RuntimeKey = null, string? Alias = null, int Minutes = 10, int Charge = 0);
public sealed record ActorSpawnRequest(string? SteamId, string? Name, string ActorClass, string? RuntimeKey = null);
public sealed record CharacterAttributesRequest(
    string? SteamId,
    string? Name,
    Dictionary<string, double>? Attributes = null,
    double? Strength = null,
    double? Constitution = null,
    double? Dexterity = null,
    double? Intelligence = null,
    string? RuntimeKey = null);
public sealed record CharacterSkillRequest(string? SteamId, string? Name, string Skill, int Level = 0, int Experience = 0, string? RuntimeKey = null);
public sealed record CharacterPackRequest(string? SteamId, string? Name, string? Pack = null, string? PackName = null, string? RuntimeKey = null);
public sealed record HomeSetRequest(string? SteamId, string? Name, string? Label = null, string? RuntimeKey = null);
public sealed record HomeTeleportRequest(string? SteamId, string? Name, double X, double Y, double Z, string? RuntimeKey = null, string? Label = null);
public sealed record FastTravelRequest(string? SteamId, string? Name, string? Alias = null, double? X = null, double? Y = null, double? Z = null, string? RuntimeKey = null);
public sealed record SectorScanRequest(string? Sector, string? SteamId = null, string? Name = null, string? RuntimeKey = null);
public sealed record PrivateMessageRequest(string? SteamId, string? Name, string Message, string? RuntimeKey = null);
public sealed record PlayerPluginCommandRequest(string? SteamId, string? Name, string Message, string? RuntimeKey = null);
public sealed record WargmManualDeliverRequest(
    string? SteamId,
    string? Name,
    string? Mode,
    IReadOnlyList<ItemGrantRequest>? Items = null,
    string? ItemId = null,
    int Quantity = 1,
    string? VehicleId = null,
    long Amount = 0,
    string? Skill = null,
    string? SkillName = null,
    int Level = 0,
    int Experience = 0,
    int SkillExperience = 0,
    double Strength = 0,
    double Constitution = 0,
    double Dexterity = 0,
    double Intelligence = 0,
    string? Command = null,
    string? RuntimeKey = null);
public sealed record PluginCommandRequest(string Command, JsonElement? Args = null, int? TimeoutMs = null);
public sealed record ScheduledEventRunRequest(int? Index = null, bool Force = false, int? TimeoutMs = null);
public sealed record PluginConfigSaveRequest(string Name, JsonElement Config);
public sealed record PluginStateRequest(string? Name, bool Enabled, string? Key = null);
public sealed record ServerControlRequest(string? Action);
public sealed record ServerConfigSaveRequest(string Name, string Content);
public sealed record LogClearRequest(string? Target = null, IReadOnlyList<string>? Targets = null);
public sealed record WelcomeTimerResetRequest(string? SteamId);
public sealed record DiscordTestRequest(string? Route = null, string? Message = null);
public sealed record DiscordSecretRequest(string? BotToken = null, string? Token = null);

public sealed record WardenEvent(
    string Type,
    DateTimeOffset TimestampUtc,
    string Message,
    Dictionary<string, object?> Data,
    string? Raw = null);
