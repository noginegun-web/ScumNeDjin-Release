using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text;
using Microsoft.Extensions.Options;
using ScumWarden.Server.Configuration;

namespace ScumWarden.Server.Services;

public sealed class PluginConfigStore
{
    private readonly JsonSerializerOptions _json;
    private readonly string _configRoot;
    private readonly string _baseConfigRoot;
    private readonly string _stateRoot;
    private readonly string? _legacyConfigRoot;
    private readonly string? _legacyBaseConfigRoot;
    private readonly string? _legacyStateRoot;
    private readonly SemaphoreSlim _stateGate = new(1, 1);
    private const int JsonReadAttempts = 6;
    private const int JsonReadRetryDelayMilliseconds = 25;

    // Durable transactional state (for example the GameStores delivery journal) must
    // live next to module state, but it must not inherit the legacy .live overlay
    // semantics used by editable plugin configs.
    public string StateRootPath => _stateRoot;

    // Keep this map in step with PLAYER_COMMAND_ALIAS_CONFIG_FIELDS in the Lua bridge.
    // The panel must not persist an alias which the bridge would route to two commands.
    private static readonly PlayerCommandAliasDefinition[] PlayerCommandAliasConfigFields =
    {
        new("Help", new[] { "Help", "help" }),
        new("Info", new[] { "Info", "info" }),
        new("Hello", new[] { "Hello", "hello" }),
        new("SetHome", new[] { "SetHome", "setHome", "sethome" }),
        new("Home", new[] { "Home", "home" }),
        new("Homes", new[] { "Homes", "homes" }),
        new("DeleteHome", new[] { "DeleteHome", "DelHome", "deleteHome", "delHome", "delhome" }),
        new("PrivateMessage", new[] { "PrivateMessage", "Pm", "PM", "privateMessage", "pm" }),
        new("Reply", new[] { "Reply", "reply" }),
        new("PrivateMessageHistory", new[] { "PrivateMessageHistory", "PmHistory", "PMHistory", "privateMessageHistory", "pmHistory", "pmhistory" }),
        new("SectorScan", new[] { "SectorScan", "Scan", "sectorScan", "scan" }),
        new("Armory", new[] { "Armory", "armory" }),
        new("ArmoryBack", new[] { "ArmoryBack", "armoryBack", "armoryback" }),
        new("FastTravel", new[] { "Travel", "FastTravel", "travel", "fastTravel" }),
        new("BaseLoot", new[] { "BaseLoot", "Loot", "baseLoot", "loot" }),
        new("Rent", new[] { "Rent", "Rental", "VehicleRental", "rent", "rental", "vehicleRental" }),
        new("InventoryDelete", new[] { "InventoryDelete", "InvDelete", "InvDel", "inventoryDelete", "invDelete", "invDel", "invdel" }),
        new("WelcomePack", new[] { "WelcomePack", "Starter", "welcomePack", "starter" }),
        new("DailyPack", new[] { "DailyPack", "Daily", "dailyPack", "daily" }),
        new("Battlepass", new[] { "Battlepass", "BattlePass", "Bp", "BP", "battlepass", "battlePass", "bp" }),
        new("Dlc", new[] { "Dlc", "DLC", "Deluxe", "dlc", "deluxe" }),
        new("GameStores", new[] { "GameStores", "Gamestores", "GameStoresShop", "Pay", "gameStores", "gamestores", "gameStoresShop", "pay" }),
        new("MoneyTransfer", new[] { "MoneyTransfer", "SendMoney", "moneyTransfer", "sendMoney" }),
        new("Quest", new[] { "Quest", "Quests", "quest", "quests" }),
        new("ItemUpgrade", new[] { "ItemUpgrade", "Upgrade", "itemUpgrade", "upgrade" }),
        new("Vip", new[] { "Vip", "VIP", "vip" }),
        new("DiscordTest", new[] { "DiscordTest", "discordTest", "discordtest" }),
        new("Wargm", new[] { "Wargm", "WargmShop", "wargm", "wargmShop" }),
        new("Streak", new[] { "Streak", "streak" }),
        new("Bounty", new[] { "Bounty", "bounty" }),
        new("HunterTop", new[] { "HunterTop", "hunterTop", "huntertop" }),
        new("Language", new[] { "Language", "Lang", "language", "lang" }),
        new("Editor", new[] { "Editor", "Ed", "editor", "ed" })
    };

    public PluginConfigStore(IOptions<WardenOptions> options, JsonSerializerOptions json)
    {
        _json = new JsonSerializerOptions(json)
        {
            PropertyNamingPolicy = null,
            DictionaryKeyPolicy = null
        };
        var roots = ResolveDataRoots(options.Value);
        _stateRoot = Path.Combine(roots.PrimaryRoot, "state");
        _baseConfigRoot = Path.Combine(roots.PrimaryRoot, "configs");
        _configRoot = Path.Combine(_stateRoot, "configs");
        if (!string.IsNullOrWhiteSpace(roots.LegacyRoot) &&
            !string.Equals(
                Path.GetFullPath(roots.LegacyRoot),
                Path.GetFullPath(roots.PrimaryRoot),
                StringComparison.OrdinalIgnoreCase))
        {
            _legacyStateRoot = Path.Combine(roots.LegacyRoot, "state");
            _legacyBaseConfigRoot = Path.Combine(roots.LegacyRoot, "configs");
            _legacyConfigRoot = Path.Combine(_legacyStateRoot, "configs");
        }
        Directory.CreateDirectory(_baseConfigRoot);
        Directory.CreateDirectory(_stateRoot);
        Directory.CreateDirectory(_configRoot);
        EnsureDefaults();
    }

    public IReadOnlyList<object> ListModules()
    {
        return ModuleCatalog.All.Select(m =>
        {
            // GET /api/plugins must not create or normalize module config files
            // simply because the panel is opened.
            var config = GetConfigReadOnly(m.Key);
            return new
            {
                m.Key,
                m.Name,
                m.Description,
                m.Category,
                config,
                enabled = IsEnabled(config)
            };
        }).ToArray();
    }

    public JsonElement GetConfig(string name) => GetConfig(name, persistChanges: true);

    public JsonElement GetConfigReadOnly(string name) => GetConfig(name, persistChanges: false);

    private JsonElement GetConfig(string name, bool persistChanges)
    {
        var path = GetReadableConfigPath(name);
        var defaults = JsonSerializer.SerializeToNode(ModuleCatalog.DefaultFor(name), _json);
        if (path is null)
        {
            if (!persistChanges)
            {
                using var missingDocument = JsonDocument.Parse((defaults ?? new JsonObject()).ToJsonString(_json));
                return missingDocument.RootElement.Clone();
            }

            path = GetConfigPath(name);
            WriteTextFileAtomic(path, JsonSerializer.Serialize(ModuleCatalog.DefaultFor(name), _json));
        }

        var writePath = GetConfigPath(name);
        var text = ReadTextFileLive(path);
        JsonNode? current;
        try
        {
            current = JsonNode.Parse(text);
        }
        catch (JsonException)
        {
            current = defaults?.DeepClone();
        }

        if (current is not JsonObject currentObject || defaults is not JsonObject defaultObject)
        {
            current = defaults?.DeepClone() ?? new JsonObject();
            if (persistChanges)
            {
                WriteTextFileAtomic(writePath, current.ToJsonString(_json));
            }
        }
        else if (MergeMissingDefaults(currentObject, defaultObject) || HardenExistingConfig(name, currentObject))
        {
            if (persistChanges)
            {
                WriteTextFileAtomic(writePath, currentObject.ToJsonString(_json));
            }
            current = currentObject;
        }

        using var doc = JsonDocument.Parse((current ?? new JsonObject()).ToJsonString(_json));
        return doc.RootElement.Clone();
    }

    public async Task<object> SaveConfigAsync(PluginConfigSaveRequest request)
    {
        var path = GetConfigPath(request.Name);
        var node = JsonNode.Parse(request.Config.GetRawText()) as JsonObject;
        if (node is not null)
        {
            NormalizeEnabledFlag(node);
            if (string.Equals(SafeName(request.Name), "command-aliases", StringComparison.Ordinal))
            {
                ValidatePlayerCommandAliases(node);
            }
            if (string.Equals(SafeName(request.Name), "help-response", StringComparison.Ordinal) ||
                string.Equals(SafeName(request.Name), "info-response", StringComparison.Ordinal))
            {
                ValidateHelpResponse(node);
            }
            await WriteTextFileAtomicAsync(path, node.ToJsonString(_json));
        }
        else
        {
            await WriteTextFileAtomicAsync(path, JsonSerializer.Serialize(request.Config, _json));
        }
        return new { saved = true, name = request.Name };
    }

    public async Task<object> ResetConfigAsync(string name)
    {
        var path = GetConfigPath(name);
        await WriteTextFileAtomicAsync(path, JsonSerializer.Serialize(ModuleCatalog.DefaultFor(name), _json));
        return new { reset = true, name, config = GetConfig(name) };
    }

    public Task<object> DeleteConfigAsync(string name)
    {
        var path = GetConfigPath(name);
        DeleteTextFileLive(path);

        return Task.FromResult<object>(new
        {
            deleted = true,
            name,
            builtInModule = ModuleCatalog.All.Any(module => string.Equals(module.Key, name, StringComparison.OrdinalIgnoreCase)),
            message = "Настройки модуля удалены. Встроенный модуль будет создан заново с настройками по умолчанию при следующем чтении."
        });
    }

    public async Task<object> SetEnabledAsync(PluginStateRequest request)
    {
        var name = string.IsNullOrWhiteSpace(request.Name) ? request.Key : request.Name;
        if (string.IsNullOrWhiteSpace(name))
        {
            return new { saved = false, error = "Ключ модуля не указан." };
        }

        var existing = GetConfig(name);
        var node = JsonNode.Parse(existing.GetRawText()) as JsonObject ?? new JsonObject();
        NormalizeEnabledFlag(node);
        node["Enabled"] = request.Enabled;
        await WriteTextFileAtomicAsync(GetConfigPath(name), node.ToJsonString(_json));
        return new { saved = true, name, request.Enabled };
    }

    public JsonElement GetState(string name)
    {
        var path = GetReadableStatePath(name);
        if (path is null)
        {
            path = GetStatePath(name);
            WriteTextFileAtomic(path, "[]");
        }

        using var doc = JsonDocument.Parse(ReadTextFileLive(path));
        return doc.RootElement.Clone();
    }

    public JsonElement GetStateOrDefault(string name, string fallbackJson)
    {
        var path = GetReadableStatePath(name);
        var text = path is not null ? ReadTextFileLive(path) : fallbackJson;
        if (string.IsNullOrWhiteSpace(text) || text.Trim() == "[]")
        {
            text = fallbackJson;
        }

        try
        {
            using var doc = JsonDocument.Parse(text);
            return doc.RootElement.Clone();
        }
        catch (JsonException)
        {
            using var doc = JsonDocument.Parse(fallbackJson);
            return doc.RootElement.Clone();
        }
    }

    public async Task<JsonElement> SetStateAsync(string name, object rows)
    {
        var path = GetStatePath(name);
        await _stateGate.WaitAsync();
        try
        {
            await WriteTextFileAtomicAsync(path, JsonSerializer.Serialize(rows, _json));
            using var doc = JsonDocument.Parse(ReadTextFileLive(path));
            return doc.RootElement.Clone();
        }
        finally
        {
            _stateGate.Release();
        }
    }

    public async Task<JsonElement> AppendStateAsync(string name, object record)
    {
        var path = GetStatePath(name);
        await _stateGate.WaitAsync();
        try
        {
            var rows = JsonSerializer.Deserialize<List<JsonElement>>(FileExistsLive(path) ? ReadTextFileLive(path) : "[]", _json) ?? new();
            var element = JsonSerializer.SerializeToElement(record, _json);
            rows.Add(element);
            if (rows.Count > 5000)
            {
                rows.RemoveRange(0, rows.Count - 5000);
            }

            await WriteTextFileAtomicAsync(path, JsonSerializer.Serialize(rows, _json));
            return element.Clone();
        }
        finally
        {
            _stateGate.Release();
        }
    }

    public async Task<object> ClearStatesAsync(IEnumerable<string> names)
    {
        var targets = names
            .Where(name => !string.IsNullOrWhiteSpace(name))
            .Select(name => CanonicalStateName(name.Trim()))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();

        await _stateGate.WaitAsync();
        try
        {
            var cleared = new List<object>();
            foreach (var name in targets)
            {
                var path = GetStatePath(name);
                var before = LiveFileLength(path);
                WriteTextFileAtomic(path, "[]");
                cleared.Add(new
                {
                    name = SafeName(name),
                    bytesBefore = before,
                    bytesAfter = LiveFileLength(path)
                });
            }

            return new
            {
                cleared,
                count = cleared.Count
            };
        }
        finally
        {
            _stateGate.Release();
        }
    }

    public async Task<object> TrimStateAsync(string name, int keepLast)
    {
        var path = GetStatePath(name);
        var safeKeep = Math.Clamp(keepLast, 0, 5000);
        await _stateGate.WaitAsync();
        try
        {
            var before = LiveFileLength(path);
            var rows = JsonSerializer.Deserialize<List<JsonElement>>(FileExistsLive(path) ? ReadTextFileLive(path) : "[]", _json) ?? new();
            if (safeKeep <= 0)
            {
                rows.Clear();
            }
            else if (rows.Count > safeKeep)
            {
                rows.RemoveRange(0, rows.Count - safeKeep);
            }

            WriteTextFileAtomic(path, JsonSerializer.Serialize(rows, _json));
            return new
            {
                name = SafeName(name),
                kept = rows.Count,
                bytesBefore = before,
                bytesAfter = LiveFileLength(path)
            };
        }
        finally
        {
            _stateGate.Release();
        }
    }

    public async Task<bool> TryConsumeStateSignalAsync(string name)
    {
        var path = GetStatePath(name);
        var legacyPath = GetLegacyStatePath(name);
        await _stateGate.WaitAsync();
        try
        {
            var exists = FileExistsLive(path) || (legacyPath is not null && FileExistsLive(legacyPath));
            if (!exists)
            {
                return false;
            }

            DeleteTextFileLive(path);
            if (legacyPath is not null)
            {
                DeleteTextFileLive(legacyPath);
            }
            return true;
        }
        finally
        {
            _stateGate.Release();
        }
    }

    public async Task<JsonElement?> TryConsumeStateSignalJsonAsync(string name)
    {
        var path = GetStatePath(name);
        var legacyPath = GetLegacyStatePath(name);
        await _stateGate.WaitAsync();
        try
        {
            var readPath = FileExistsLive(path)
                ? path
                : legacyPath is not null && FileExistsLive(legacyPath)
                    ? legacyPath
                    : null;
            if (readPath is null)
            {
                return null;
            }

            var text = ReadTextFileLive(readPath);
            DeleteTextFileLive(path);
            if (legacyPath is not null)
            {
                DeleteTextFileLive(legacyPath);
            }

            if (string.IsNullOrWhiteSpace(text))
            {
                return null;
            }

            try
            {
                using var doc = JsonDocument.Parse(text);
                return doc.RootElement.Clone();
            }
            catch (JsonException)
            {
                return null;
            }
        }
        finally
        {
            _stateGate.Release();
        }
    }

    public async Task<object> ResetWelcomeTimerAsync(WelcomeTimerResetRequest request)
    {
        var path = Path.Combine(_stateRoot, "welcome-pack-timers.json");
        if (string.IsNullOrWhiteSpace(request.SteamId))
        {
            await _stateGate.WaitAsync();
            try
            {
                await WriteTextFileAtomicAsync(path, "[]");
                return new { reset = "all" };
            }
            finally
            {
                _stateGate.Release();
            }
        }

        await _stateGate.WaitAsync();
        try
        {
            var timers = JsonSerializer.Deserialize<List<Dictionary<string, object?>>>(FileExistsLive(path) ? ReadTextFileLive(path) : "[]", _json) ?? new();
            timers.RemoveAll(t => t.TryGetValue("steamId", out var value) && string.Equals(Convert.ToString(value), request.SteamId, StringComparison.OrdinalIgnoreCase));
            await WriteTextFileAtomicAsync(path, JsonSerializer.Serialize(timers, _json));
            return new { reset = request.SteamId };
        }
        finally
        {
            _stateGate.Release();
        }
    }

    private string GetConfigPath(string name) => Path.Combine(_configRoot, SafeName(name) + ".json");
    private string GetBaseConfigPath(string name) => Path.Combine(_baseConfigRoot, SafeName(name) + ".json");
    private string? GetLegacyConfigPath(string name) => _legacyConfigRoot is null ? null : Path.Combine(_legacyConfigRoot, SafeName(name) + ".json");
    private string? GetLegacyBaseConfigPath(string name) => _legacyBaseConfigRoot is null ? null : Path.Combine(_legacyBaseConfigRoot, SafeName(name) + ".json");
    private string? GetReadableConfigPath(string name)
    {
        foreach (var path in new[]
        {
            GetConfigPath(name),
            GetLegacyConfigPath(name),
            GetBaseConfigPath(name),
            GetLegacyBaseConfigPath(name)
        })
        {
            if (path is not null && FileExistsLive(path))
            {
                return path;
            }
        }

        return null;
    }

    private string GetStatePath(string name) => Path.Combine(_stateRoot, SafeName(CanonicalStateName(name)) + ".json");
    private string? GetLegacyStatePath(string name) => _legacyStateRoot is null ? null : Path.Combine(_legacyStateRoot, SafeName(CanonicalStateName(name)) + ".json");
    private string? GetReadableStatePath(string name)
    {
        var primary = GetStatePath(name);
        if (FileExistsLive(primary))
        {
            return primary;
        }

        var legacy = GetLegacyStatePath(name);
        return legacy is not null && FileExistsLive(legacy) ? legacy : null;
    }

    private static Task WriteTextFileAtomicAsync(string path, string text) =>
        Task.Run(() => WriteTextFileAtomic(path, text));

    private static bool FileExistsLive(string path)
    {
        if (FileExistsLiveImmediate(path))
        {
            return true;
        }

        // Lua and this service both publish JSON through a temporary file. Do not
        // mistake the tiny replacement window for an absent state/config file.
        if (!HasPendingAtomicReplacement(path))
        {
            return false;
        }

        for (var attempt = 1; attempt < JsonReadAttempts; attempt++)
        {
            Thread.Sleep(JsonReadRetryDelayMilliseconds);
            if (FileExistsLiveImmediate(path))
            {
                return true;
            }
        }

        return false;
    }

    private static string ReadTextFileLive(string path)
    {
        string? incompleteText = null;
        Exception? lastError = null;
        for (var attempt = 1; attempt <= JsonReadAttempts; attempt++)
        {
            var livePath = path + ".live";
            var candidate = File.Exists(livePath) ? livePath : path;
            try
            {
                var text = File.ReadAllText(candidate);
                if (LooksLikeCompleteJson(text))
                {
                    return text;
                }

                incompleteText = text;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                lastError = ex;
            }

            if (attempt < JsonReadAttempts)
            {
                Thread.Sleep(JsonReadRetryDelayMilliseconds);
            }
        }

        if (incompleteText is not null)
        {
            // Keep the existing malformed-config handling intact after a full retry window.
            return incompleteText;
        }

        throw new IOException($"Unable to read JSON file after {JsonReadAttempts} attempts: {path}", lastError);
    }

    private static bool FileExistsLiveImmediate(string path) => File.Exists(path + ".live") || File.Exists(path);

    private static bool HasPendingAtomicReplacement(string path)
    {
        if (File.Exists(path + ".tmp"))
        {
            return true;
        }

        var directory = Path.GetDirectoryName(path);
        var fileName = Path.GetFileName(path);
        if (string.IsNullOrWhiteSpace(directory) || string.IsNullOrWhiteSpace(fileName))
        {
            return false;
        }

        try
        {
            return Directory.EnumerateFiles(directory, fileName + ".tmp.*").Any();
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    private static bool LooksLikeCompleteJson(string text)
    {
        var trimmed = (text ?? string.Empty).Trim();
        return trimmed.Length >= 2 &&
               ((trimmed[0] == '{' && trimmed[^1] == '}') ||
                (trimmed[0] == '[' && trimmed[^1] == ']'));
    }

    private static void DeleteTextFileLive(string path)
    {
        try { if (File.Exists(path + ".live")) File.Delete(path + ".live"); } catch { }
        try { if (File.Exists(path)) File.Delete(path); } catch { }
    }

    private static void WriteTextFileAtomic(string path, string text)
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var content = text ?? string.Empty;
        var tempPath = CreateAtomicTempPath(path);
        try
        {
            File.WriteAllText(tempPath, content);
            ReplaceFileAtomically(tempPath, path);
            TryDeleteFile(path + ".live");
            return;
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
        finally
        {
            TryDeleteFile(tempPath);
        }

        // A running Lua bridge can temporarily hold the base JSON open. Keep the
        // old file intact and publish a complete overlay which both C# and Lua read first.
        WriteLiveFileAtomically(path + ".live", content);
    }

    private static string CreateAtomicTempPath(string path) =>
        path + ".tmp." + Environment.ProcessId + "." + Guid.NewGuid().ToString("N");

    private static void ReplaceFileAtomically(string tempPath, string destinationPath)
    {
        if (File.Exists(destinationPath))
        {
            try { File.SetAttributes(destinationPath, FileAttributes.Normal); } catch { }
        }

        File.Move(tempPath, destinationPath, overwrite: true);
    }

    private static void WriteLiveFileAtomically(string livePath, string text)
    {
        var tempPath = CreateAtomicTempPath(livePath);
        try
        {
            File.WriteAllText(tempPath, text);
            ReplaceFileAtomically(tempPath, livePath);
        }
        finally
        {
            TryDeleteFile(tempPath);
        }
    }

    private static void TryDeleteFile(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch
        {
        }
    }

    private sealed record DataRoots(string PrimaryRoot, string? LegacyRoot);
    private sealed record PlayerCommandAliasDefinition(string CommandKey, string[] Fields);

    private static DataRoots ResolveDataRoots(WardenOptions options)
    {
        var legacyRoot = ResolveLegacyRoot(options);
        if (!string.IsNullOrWhiteSpace(options.RuntimeDataRoot))
        {
            return new DataRoots(Path.GetFullPath(options.RuntimeDataRoot), legacyRoot);
        }

        var scumRoot = ResolveScumRoot(options);
        if (!string.IsNullOrWhiteSpace(scumRoot))
        {
            return new DataRoots(Path.Combine(scumRoot, "Saved", "ScumNeDjin"), legacyRoot);
        }

        if (!string.IsNullOrWhiteSpace(legacyRoot))
        {
            return new DataRoots(legacyRoot, null);
        }

        return new DataRoots(Path.Combine(AppContext.BaseDirectory, "data"), null);
    }

    private static string? ResolveLegacyRoot(WardenOptions options)
    {
        var win64 = ResolveWin64FromBridge(options.BridgePath);
        return string.IsNullOrWhiteSpace(win64) ? null : Path.Combine(win64, "ScumNeDjin");
    }

    private static string? ResolveScumRoot(WardenOptions options)
    {
        if (!string.IsNullOrWhiteSpace(options.ServerRoot))
        {
            var serverRoot = Path.GetFullPath(options.ServerRoot);
            foreach (var candidate in new[]
            {
                Path.Combine(serverRoot, "SCUM"),
                serverRoot
            })
            {
                if (Directory.Exists(Path.Combine(candidate, "Saved")) ||
                    Directory.Exists(Path.Combine(candidate, "Binaries", "Win64")) ||
                    Directory.Exists(Path.Combine(candidate, "SCUM", "Saved")))
                {
                    return candidate.EndsWith(Path.DirectorySeparatorChar + "SCUM", StringComparison.OrdinalIgnoreCase)
                        ? candidate
                        : Path.Combine(candidate, "SCUM");
                }
            }
        }

        var win64 = ResolveWin64FromBridge(options.BridgePath);
        if (string.IsNullOrWhiteSpace(win64))
        {
            return null;
        }

        var win64Dir = new DirectoryInfo(win64);
        if (!string.Equals(win64Dir.Name, "Win64", StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        var binaries = win64Dir.Parent;
        var scum = binaries?.Parent;
        return scum?.FullName;
    }

    private static string? ResolveWin64FromBridge(string bridgePath)
    {
        if (string.IsNullOrWhiteSpace(bridgePath))
        {
            return null;
        }

        var fullBridge = Path.GetFullPath(bridgePath);
        var bridgeInfo = new DirectoryInfo(fullBridge);
        var win64 = string.Equals(bridgeInfo.Name, "nedjin_bridge", StringComparison.OrdinalIgnoreCase)
            ? bridgeInfo.Parent
            : bridgeInfo;
        return win64?.FullName;
    }

    private void EnsureDefaults()
    {
        foreach (var module in ModuleCatalog.All)
        {
            var overlayPath = GetConfigPath(module.Key);
            var basePath = GetBaseConfigPath(module.Key);
            if (!FileExistsLive(overlayPath) && !FileExistsLive(basePath))
            {
                WriteTextFileAtomic(basePath, JsonSerializer.Serialize(ModuleCatalog.DefaultFor(module.Key), _json));
            }
            else
            {
                _ = GetConfig(module.Key);
            }
        }
    }

    private static bool MergeMissingDefaults(JsonObject current, JsonObject defaults)
    {
        var changed = false;
        foreach (var pair in defaults)
        {
            var existingKey = FindExistingKey(current, pair.Key);
            if (existingKey is null)
            {
                current[pair.Key] = pair.Value?.DeepClone();
                changed = true;
                continue;
            }

            if (current[existingKey] is JsonObject currentChild && pair.Value is JsonObject defaultChild)
            {
                changed |= MergeMissingDefaults(currentChild, defaultChild);
            }
        }

        return changed;
    }

    private static bool HardenExistingConfig(string name, JsonObject current)
    {
        if (!string.Equals(name, "zombie-infection", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var changed = false;
        var enabledKey = FindExistingKey(current, "Enabled");
        if (enabledKey is null)
        {
            current["Enabled"] = false;
            changed = true;
        }
        else if (current[enabledKey] is JsonValue enabledValue &&
            enabledValue.TryGetValue<bool>(out var enabled) &&
            enabled)
        {
            current[enabledKey] = false;
            changed = true;
        }

        var transformModeKey = FindExistingKey(current, "TransformMode");
        if (transformModeKey is null)
        {
            current["TransformMode"] = "mesh";
            changed = true;
        }
        else if (current[transformModeKey] is JsonValue transformModeValue &&
            transformModeValue.TryGetValue<string>(out var transformMode) &&
            string.Equals(transformMode, "scum-appearance", StringComparison.OrdinalIgnoreCase))
        {
            current[transformModeKey] = "mesh";
            changed = true;
        }

        if (FindExistingKey(current, "AllowMeshFallback") is null)
        {
            current["AllowMeshFallback"] = false;
            changed = true;
        }

        if (FindExistingKey(current, "MeshApplyMode") is null)
        {
            current["MeshApplyMode"] = "property";
            changed = true;
        }

        if (FindExistingKey(current, "UnsafePostMeshRefresh") is null)
        {
            current["UnsafePostMeshRefresh"] = false;
            changed = true;
        }

        var unsafeAnimKey = FindExistingKey(current, "UnsafeAnimBlueprint");
        if (unsafeAnimKey is null)
        {
            current["UnsafeAnimBlueprint"] = false;
            changed = true;
            unsafeAnimKey = "UnsafeAnimBlueprint";
        }

        var unsafeAnimEnabled = false;
        if (current[unsafeAnimKey] is JsonValue unsafeAnimValue &&
            unsafeAnimValue.TryGetValue<bool>(out var parsedUnsafeAnim))
        {
            unsafeAnimEnabled = parsedUnsafeAnim;
        }

        if (!unsafeAnimEnabled)
        {
            current["ApplyZombieAnimation"] = false;
            changed = true;
        }

        return changed;
    }

    private static string? FindExistingKey(JsonObject current, string key)
    {
        if (current.ContainsKey(key))
        {
            return key;
        }

        foreach (var existing in current.Select(pair => pair.Key))
        {
            if (string.Equals(existing, key, StringComparison.OrdinalIgnoreCase))
            {
                return existing;
            }
        }

        return null;
    }

    private static void ValidatePlayerCommandAliases(JsonObject config)
    {
        // A disabled config is ignored by Lua, which falls back to the compiled defaults.
        if (!JsonBool(config, true, "Enabled", "enabled"))
        {
            return;
        }

        var defaults = JsonSerializer.SerializeToNode(ModuleCatalog.DefaultFor("command-aliases")) as JsonObject ?? new JsonObject();
        var aliasesByCommand = new Dictionary<string, IReadOnlyList<string>>(StringComparer.Ordinal);
        foreach (var definition in PlayerCommandAliasConfigFields)
        {
            var (_, aliases) = ReadConfiguredPlayerCommandAliases(defaults, definition, includeBangAliases: true);
            aliasesByCommand[definition.CommandKey] = aliases;
        }

        var includeBangAliases = JsonBool(config, true, "IncludeBangAliases", "includeBangAliases");
        foreach (var definition in PlayerCommandAliasConfigFields)
        {
            var (present, aliases) = ReadConfiguredPlayerCommandAliases(config, definition, includeBangAliases);
            // This matches the Lua bridge: an explicitly empty field leaves the default command active.
            if (present && aliases.Count > 0)
            {
                aliasesByCommand[definition.CommandKey] = aliases;
            }
        }

        var owners = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var definition in PlayerCommandAliasConfigFields)
        {
            foreach (var alias in aliasesByCommand[definition.CommandKey])
            {
                if (owners.TryGetValue(alias, out var owner) &&
                    !string.Equals(owner, definition.CommandKey, StringComparison.Ordinal))
                {
                    throw new InvalidOperationException(
                        $"Конфликт псевдонимов команд: {alias}: {owner} и {definition.CommandKey}. Изменения не сохранены.");
                }

                owners[alias] = definition.CommandKey;
            }
        }
    }

    private static (bool Present, IReadOnlyList<string> Aliases) ReadConfiguredPlayerCommandAliases(
        JsonObject config,
        PlayerCommandAliasDefinition definition,
        bool includeBangAliases)
    {
        var aliases = new List<string>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var present = false;

        foreach (var field in definition.Fields)
        {
            if (!config.TryGetPropertyValue(field, out var raw))
            {
                continue;
            }

            present = true;
            if (raw is JsonArray array)
            {
                foreach (var item in array)
                {
                    AddPlayerCommandAlias(aliases, seen, JsonString(item), includeBangAliases);
                }
            }
            else
            {
                var text = JsonString(raw);
                if (!string.IsNullOrWhiteSpace(text))
                {
                    foreach (var token in text.Split(new[] { ',', ';', ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
                    {
                        AddPlayerCommandAlias(aliases, seen, token, includeBangAliases);
                    }
                }
            }
        }

        return (present, aliases);
    }

    private static void AddPlayerCommandAlias(
        ICollection<string> aliases,
        ISet<string> seen,
        string? value,
        bool includeBangAliases)
    {
        var direct = NormalizePlayerCommandAlias(value);
        if (string.IsNullOrWhiteSpace(direct) || !seen.Add(direct))
        {
            return;
        }

        aliases.Add(direct);
        if (includeBangAliases && direct.StartsWith("/", StringComparison.Ordinal))
        {
            var bang = "!" + direct[1..];
            if (seen.Add(bang))
            {
                aliases.Add(bang);
            }
        }
    }

    private static string NormalizePlayerCommandAlias(string? value)
    {
        var text = string.Concat((value ?? string.Empty).Where(ch => !char.IsWhiteSpace(ch))).ToLowerInvariant();
        if (text.Length == 0)
        {
            return string.Empty;
        }

        return text[0] is '/' or '!' ? text : "/" + text;
    }

    private static string? JsonString(JsonNode? node)
    {
        return node is JsonValue value && value.TryGetValue<string>(out var text) ? text : null;
    }

    private void ValidateHelpResponse(JsonObject config)
    {
        var maxLines = RequireHelpResponseInteger(config, "MaxLines", 24, 1, 40, "Максимум строк");
        var maxLineBytes = RequireHelpResponseInteger(config, "MaxLineBytes", 220, 16, 220, "Максимум байт на строку");
        _ = RequireHelpResponseInteger(config, "LineDelayMs", 140, 40, 1000, "Пауза между строками");

        var bridgeMaxLineBytes = ReadBridgeMaxChatMessageBytes();
        if (maxLineBytes > bridgeMaxLineBytes)
        {
            throw new InvalidOperationException($"Максимум байт на строку не может быть выше текущего лимита bridge: {bridgeMaxLineBytes}.");
        }

        ValidateHelpResponseText(config, "Text", "Русская версия", maxLines, maxLineBytes);
        ValidateHelpResponseText(config, "EnglishText", "Английская версия", maxLines, maxLineBytes);
    }

    private int ReadBridgeMaxChatMessageBytes()
    {
        try
        {
            var bridge = GetConfigReadOnly("bridge-safety");
            if (bridge.ValueKind == JsonValueKind.Object &&
                (bridge.TryGetProperty("MaxChatMessageBytes", out var value) || bridge.TryGetProperty("maxChatMessageBytes", out value)) &&
                TryGetJsonInteger(value, out var parsed))
            {
                return Math.Clamp(parsed, 16, 220);
            }
        }
        catch
        {
            // A malformed optional bridge config must not weaken the safe default.
        }

        return 220;
    }

    private static int RequireHelpResponseInteger(JsonObject config, string canonicalKey, int fallback, int minimum, int maximum, string label)
    {
        var actualKey = FindExistingKey(config, canonicalKey);
        if (actualKey is null)
        {
            config[canonicalKey] = fallback;
            return fallback;
        }

        if (!TryGetJsonInteger(config[actualKey], out var value) || value < minimum || value > maximum)
        {
            throw new InvalidOperationException($"{label}: укажите целое значение от {minimum} до {maximum}.");
        }

        if (!string.Equals(actualKey, canonicalKey, StringComparison.Ordinal))
        {
            config.Remove(actualKey);
        }
        config[canonicalKey] = value;
        return value;
    }

    private static bool TryGetJsonInteger(JsonNode? node, out int value)
    {
        value = 0;
        if (node is not JsonValue jsonValue)
        {
            return false;
        }

        if (jsonValue.TryGetValue<int>(out value))
        {
            return true;
        }

        if (jsonValue.TryGetValue<long>(out var longValue) && longValue is >= int.MinValue and <= int.MaxValue)
        {
            value = (int)longValue;
            return true;
        }

        if (jsonValue.TryGetValue<string>(out var textValue) && int.TryParse(textValue, out value))
        {
            return true;
        }

        return false;
    }

    private static bool TryGetJsonInteger(JsonElement element, out int value)
    {
        value = 0;
        if (element.ValueKind == JsonValueKind.Number && element.TryGetInt32(out value))
        {
            return true;
        }

        return element.ValueKind == JsonValueKind.String && int.TryParse(element.GetString(), out value);
    }

    private static void ValidateHelpResponseText(JsonObject config, string canonicalKey, string label, int maxLines, int maxLineBytes)
    {
        var actualKey = FindExistingKey(config, canonicalKey);
        if (actualKey is null)
        {
            config[canonicalKey] = string.Empty;
            return;
        }

        if (JsonString(config[actualKey]) is not { } rawText)
        {
            throw new InvalidOperationException($"{label}: текст должен быть строкой.");
        }

        var normalized = rawText.Replace("\r\n", "\n", StringComparison.Ordinal).Replace('\r', '\n');
        var lines = normalized.Split('\n')
            .Select(line => line.Trim())
            .Where(line => line.Length > 0)
            .ToArray();
        if (lines.Length > maxLines)
        {
            throw new InvalidOperationException($"{label}: максимум {maxLines} строк.");
        }

        for (var index = 0; index < lines.Length; index++)
        {
            var bytes = Encoding.UTF8.GetByteCount(lines[index]);
            if (bytes > maxLineBytes)
            {
                throw new InvalidOperationException($"{label}: строка {index + 1} содержит {bytes}/{maxLineBytes} байт.");
            }
        }

        if (!string.Equals(actualKey, canonicalKey, StringComparison.Ordinal))
        {
            config.Remove(actualKey);
        }
        config[canonicalKey] = normalized;
    }

    private static bool JsonBool(JsonObject config, bool fallback, params string[] keys)
    {
        foreach (var key in keys)
        {
            if (!config.TryGetPropertyValue(key, out var node) || node is not JsonValue value)
            {
                continue;
            }

            if (value.TryGetValue<bool>(out var boolValue))
            {
                return boolValue;
            }

            if (value.TryGetValue<string>(out var textValue) && bool.TryParse(textValue, out var parsedValue))
            {
                return parsedValue;
            }
        }

        return fallback;
    }

    private static void NormalizeEnabledFlag(JsonObject current)
    {
        var actual = FindExistingKey(current, "Enabled") ?? FindExistingKey(current, "enabled");
        if (actual is null)
        {
            return;
        }

        bool? enabled = null;
        if (current[actual] is JsonValue value)
        {
            if (value.TryGetValue<bool>(out var boolValue))
            {
                enabled = boolValue;
            }
            else if (value.TryGetValue<string>(out var textValue) &&
                bool.TryParse(textValue, out var parsedValue))
            {
                enabled = parsedValue;
            }
        }

        var duplicateKeys = current.Select(pair => pair.Key)
            .Where(key => !string.Equals(key, "Enabled", StringComparison.Ordinal) &&
                string.Equals(key, "enabled", StringComparison.OrdinalIgnoreCase))
            .ToArray();
        foreach (var key in duplicateKeys)
        {
            current.Remove(key);
        }

        if (enabled.HasValue)
        {
            current["Enabled"] = enabled.Value;
        }
    }

    private static string CanonicalStateName(string value)
    {
        return SafeName(value) switch
        {
            "vehicle-rental" => "vehicle-rentals",
            "welcome-pack" => "welcome-pack-claims",
            "home-system" => "homes",
            "fast-travel" => "fast-travel-trips",
            _ => value
        };
    }

    private static string SafeName(string value)
    {
        var safe = new string(value.Where(ch => char.IsLetterOrDigit(ch) || ch is '-' or '_').ToArray());
        return string.IsNullOrWhiteSpace(safe) ? "module" : safe.ToLowerInvariant();
    }

    private static bool IsEnabled(JsonElement config)
    {
        if (config.TryGetProperty("Enabled", out var upper) && upper.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            return upper.GetBoolean();
        }

        if (config.TryGetProperty("enabled", out var lower) && lower.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            return lower.GetBoolean();
        }

        return true;
    }

    private static long LiveFileLength(string path)
    {
        try
        {
            var livePath = path + ".live";
            if (File.Exists(livePath))
            {
                return new FileInfo(livePath).Length;
            }

            return File.Exists(path) ? new FileInfo(path).Length : 0;
        }
        catch
        {
            return 0;
        }
    }
}

public sealed record ModuleDescriptor(string Key, string Name, string Category, string Description);

public static class ModuleCatalog
{
    public static readonly ModuleDescriptor[] All =
    {
        new("welcome-pack", "Стартовый набор", "players", "Стартовый набор через защищённую выдачу предметов."),
        new("daily-pack", "Ежедневный набор", "players", "Ежедневная выдача предметов с отдельными VIP-бонусами."),
        new("battlepass", "Battlepass", "players", "Автоматическая 30-дневная серия наград после прогрузки игрока."),
        new("vip-system", "VIP игроки", "players", "VIP-участники, права и отдельные лимиты для модулей."),
        new("character-packs", "Наборы персонажа", "players", "Панельные пресеты атрибутов, навыков и командных пакетов."),
        new("fast-travel", "Быстрое перемещение", "players", "Платные маршруты с задержкой и отменой при движении."),
        new("home-system", "Дом игрока", "players", "Именные дома, лимиты и возврат к точкам."),
        new("base-loot-collector", "Сбор лута базы", "players", "Команда /loot собирает лежащий лут в сундуки своего флага с проверкой отряда."),
        new("vehicle-rental", "Аренда транспорта", "vehicles", "Аренда транспорта, срок действия и возврат."),
        new("money-transfer", "Переводы денег", "commerce", "Игроки переводят валюту друг другу командой /sendmoney или /send."),
        new("item-upgrade", "Апгрейд предметов", "commerce", "Платная замена предмета в руках на настроенный улучшенный вариант."),
        new("wargm-shop", "Магазин Wargm", "commerce", "Очередь внешних покупок и ручная выдача."),
        new("gamestores-shop", "Магазин GameStores", "commerce", "Интеграция с GameStores: получение корзины, безопасная выдача и подтверждение покупок."),
        new("bounty-hunt", "Охота за наградой", "events", "Серии убийств, bounty-цели и награды."),
        new("server-kill-feed", "Лента убийств", "events", "Глобальные сообщения об убийствах."),
        new("scheduled-events", "Планировщик заданий", "events", "Задания сервера по времени или интервалу: cargo drop, world events, admin-команды и наборы предметов в точках карты."),
        new("zone-robot-schedule", "Роботы по зонам", "events", "Планировщик включения и отключения зонных команд для роботов/событий."),
        new("panel-quests", "Квесты сервера", "events", "Квестовая доска панели с наградами, объявлениями и безопасным claim-режимом."),
        new("sector-scan", "Сканер сектора", "intel", "Платная проверка сектора."),
        new("private-messages", "Личные сообщения", "chat", "Личные сообщения, ответы и история."),
        new("help-response", "Ответ команды /help", "chat", "Настраиваемая многострочная справка для игроков с безопасным возвратом к динамической помощи."),
        new("info-response", "Ответ команды /info", "chat", "Настраиваемое многострочное описание команд с безопасным возвратом к динамической информации."),
        new("command-aliases", "Команды игроков", "chat", "Настраивает слова вызова игровых команд: /help можно заменить на /helper, /rent на /arenda. Применяется после рестарта SCUM-сервера."),
        new("discord-log", "Журнал Discord", "integrations", "Пересылка чата, входов, убийств и событий в Discord.")
    };

    public static object DefaultFor(string key) => key.ToLowerInvariant() switch
    {
        "welcome-pack" => new { Enabled = true, RequiredPermission = "", CooldownHours = 30000, SerializeClaimsGlobally = true, InterItemDelayMs = 1500, SuccessMessage = "Стартовый набор выдан.", RentalVehicleMinutes = 1440, RentalVehicleSalePenalty = 25000, RentalVehicles = new[] { new { Enabled = true, Alias = "starter-vehicle", DisplayName = "Стартовый транспорт", AssetName = "BPC_Dirtbike", Minutes = 240, MissingVehiclePenalty = 25000, SuccessMessage = "Стартпак выдан. Транспорт стартового набора выдан на 4 часа." } }, VipItems = Array.Empty<object>(), VipSettings = new { Enabled = true, RequiredPermission = "vip.active", CooldownHours = 30000, MoneyAmount = 0, GoldAmount = 0, FameAmount = 0, SuccessMessage = "VIP бонус стартового набора выдан." }, Items = new[] { new { ItemId = "Tactical_Jacket_01_03", Quantity = 1 }, new { ItemId = "MilitaryPants_03", Quantity = 1 }, new { ItemId = "Military_Backpack_01_03", Quantity = 1 }, new { ItemId = "Military_Helmet_01_03", Quantity = 1 }, new { ItemId = "Bulletproof_Vest_01", Quantity = 1 }, new { ItemId = "Tactical_Gloves_01_01", Quantity = 1 }, new { ItemId = "CombatBoots", Quantity = 1 }, new { ItemId = "Weapon_AK47", Quantity = 1 }, new { ItemId = "Cal_7_62x39mm", Quantity = 1 }, new { ItemId = "Magazine_RPK", Quantity = 1 } } },
        "daily-pack" => new { Name = "Ежедневный пак", Description = "Ежедневная выдача предметов игроку через безопасную очередь WelcomePack.", Enabled = true, CooldownHours = 24, RequiredPermission = "", SuccessMessage = "Ежедневный пак выдан.", VipItems = new[] { new { ItemId = "Apple_2", Quantity = 1 } }, VipSettings = new { Enabled = true, RequiredPermission = "daily-pack.vip", CooldownHours = 12, MoneyAmount = 5000, GoldAmount = 5, FameAmount = 50, SuccessMessage = "VIP бонус ежедневного пака выдан." }, Items = new[] { new { ItemId = "Apple_2", Quantity = 2 }, new { ItemId = "CannedGoulash", Quantity = 1 }, new { ItemId = "Emergency_bandage_Big", Quantity = 1 } } },
        "battlepass" => BattlepassDefault(),
        "vip-system" => new { Enabled = true, DefaultTier = "vip", DefaultDurationDays = 30, ExtendExisting = true, BasePermissions = new[] { "vip.active", "welcome-pack.vip", "sethome.vip", "daily-pack.vip", "battlepass.vip", "sector-scan.vip", "vehicle-rental.vip", "base-loot.vip" }, TierPermissions = new { premium = new[] { "vip.premium" } }, Members = Array.Empty<object>(), Features = new { WelcomePack = new { Enabled = true, Items = Array.Empty<object>() }, DailyPack = new { Enabled = true, CooldownHours = 12, Items = Array.Empty<object>() }, Battlepass = new { Enabled = true }, HomeSystem = new { Enabled = true, MaxHomes = 5 }, SectorScan = new { Enabled = true, Free = true, CooldownSeconds = 30 }, VehicleRental = new { Enabled = true, DiscountPercent = 25, DefaultMinutes = 30, MaxMinutes = 180, SpawnCooldownSeconds = 60 }, BaseLootCollector = new { Enabled = true, RadiusCm = 7000, CooldownSeconds = 60, MaxItemsPerRun = 150 } }, Wargm = new { Enabled = true, DefaultDurationDays = 30, SuccessMessage = "VIP активирован." } },
        "character-packs" => new { Enabled = true, RequiredPermission = "", DefaultAttributes = new { Strength = 8, Constitution = 8, Dexterity = 8, Intelligence = 8 }, SkillPresets = new[] { new { Name = "rifles-core", Skill = "rifles", Level = 3, Experience = 0, Description = "Быстрый preset для стрелкового набора." }, new { Name = "medical-core", Skill = "medical", Level = 3, Experience = 0, Description = "Быстрый preset для медицины." }, new { Name = "driving-core", Skill = "driving", Level = 3, Experience = 0, Description = "Быстрый preset для вождения." } }, Packs = new[] { new { Name = "fullstats", Aliases = Array.Empty<string>(), Description = "Панельный пресет для установки базовых атрибутов живому игроку по SteamID.", Enabled = true, Permission = "", CooldownHours = 0, SuccessMessage = "Атрибуты персонажа обновлены.", Actions = new[] { new { Type = "PlayerCommand", Value = "SetAttributes 8 8 8 8", StopOnFailure = true } } } } },
        "fast-travel" => new { Enabled = true, TransferCooldownMinutes = 60, RatePerMeter = 0.5, FixedFare = 0, TeleportDelaySeconds = 5, TravelConfirmationSeconds = 90, CancelMoveDistance = 150, SuccessArrivalDistance = 1500, Outposts = new[] { new { DisplayName = "A0 Trader", CommandAlias = "a0", CenterZone = new[] { -621973.438, -557260.438 }, ArrivalPoint = new[] { -621973.438, -557260.438, 50d }, Price = 0 }, new { DisplayName = "B4 Trader", CommandAlias = "b4", CenterZone = new[] { 571278.25, -224427.219 }, ArrivalPoint = new[] { 571278.25, -224427.219, 50d }, Price = 0 }, new { DisplayName = "C2 Trader", CommandAlias = "c2", CenterZone = new[] { -153247.156, 289822.219 }, ArrivalPoint = new[] { -153247.156, 289822.219, 50d }, Price = 0 }, new { DisplayName = "Z3 Trader", CommandAlias = "z3", CenterZone = new[] { 24006.01, -676488.25 }, ArrivalPoint = new[] { 24006.01, -676488.25, 50d }, Price = 0 } } },
        "home-system" => new { Enabled = true, DefaultMaxHomes = 2, VipMaxHomes = 5, TeleportDelaySeconds = 0, TeleportCooldownMinutes = 60, CancelMoveDistance = 150, SuccessArrivalDistance = 600, VipPermission = "sethome.vip" },
        "base-loot-collector" => BaseLootCollectorDefault(),
        "vehicle-rental" => new { Enabled = true, DefaultRentalMinutes = 10, MinRentalMinutes = 10, MaxRentalMinutes = 60, ChargePenaltyOnMissingVehicle = true, EnableRuntimeVehicleReturnDestroy = true, EnableRuntimeVehicleExpireDestroy = true, EnableRuntimeVehicleTrackAfterSpawn = true, EnableRuntimeVehicleGenericClassFallback = false, CaptureVehiclesBeforeSpawn = false, ExpireDestroyMaxAttempts = 6, RuntimeDestroyResolveCooldownSeconds = 45, CleanupMaxPerRun = 1, RefreshPlayerBeforeVehicleSpawn = false, GlobalSpawnCooldownSeconds = 60, PlayerSpawnCooldownSeconds = 600, DefaultMissingVehiclePenalty = 25000, WarningMinutesBeforeExpiry = new[] { 5, 1 }, Vehicles = new[] { new { Alias = "rager", DisplayName = "Rager", AssetName = "BPC_Rager", PricePer10Minutes = 15000, InitialCharge = 5000, DefaultMinutes = 10, MaxMinutes = 120, MissingVehiclePenalty = 50000 }, new { Alias = "wolfswaggen", DisplayName = "WolfsWagen", AssetName = "BPC_WolfsWagen", PricePer10Minutes = 12000, InitialCharge = 4000, DefaultMinutes = 10, MaxMinutes = 120, MissingVehiclePenalty = 50000 }, new { Alias = "laika", DisplayName = "Laika", AssetName = "BPC_Laika", PricePer10Minutes = 8000, InitialCharge = 4000, DefaultMinutes = 10, MaxMinutes = 120, MissingVehiclePenalty = 50000 }, new { Alias = "Dirtbike", DisplayName = "Транспорт", AssetName = "BPC_Dirtbike", PricePer10Minutes = 2500, InitialCharge = 1000, DefaultMinutes = 10, MaxMinutes = 120, MissingVehiclePenalty = 25000 } }, VipVehicles = Array.Empty<object>() },
        "money-transfer" => MoneyTransferDefault(),
        "item-upgrade" => ItemUpgradeDefault(),
        "wargm-shop" => new { Enabled = false, ProjectId = 0, ApiKey = "", WargmServerId = 0, PollIntervalSeconds = 0, AutoDeliverPending = true, AutoDeliveryMaxPerRun = 5, DeliveryDelaySeconds = 0, RetryDelaySeconds = 60, MaxDeliveryAttempts = 0, DeliveryFilter = "", RequireRecipientName = false, DeliverOnlyToOnlinePlayers = true, AnnounceBeforeDelivery = true, AnnouncementTemplate = "[Wargm] {player}, покупка \"{offerTitle}\" готовится к выдаче: {delivery}.", BroadcastDeliveries = false, AllowUnsafeCommandDelivery = false, UnsafeCommandInterItemDelayMs = 5000, UnsafeCommandInterOperationCooldownSeconds = 90, UnsafeCommandMaxItemsPerOperation = 128, UnsafeCommandMaxQuantityPerItem = 10, VehicleGlobalSpawnCooldownSeconds = 20, VehiclePlayerSpawnCooldownSeconds = 60, UseClaimInsteadOfSuccess = false, Rules = new[] { new { Enabled = true, MatchOfferId = "", MatchItemId = "", MatchTitleContains = "Starter Pack", DeliveryMode = "SpawnItem", DeliveryLabel = "Starter Pack", ItemId = "CannedGoulash", Items = Array.Empty<object>(), VehicleAsset = "", Quantity = 1, Amount = 0, SkillName = "", SkillLevel = 0, SkillExperience = 0, DurationDays = 0, Tier = "", Strength = 0, Constitution = 0, Dexterity = 0, Intelligence = 0, CommandTemplate = "", SuccessMessage = "Покупка Wargm выдана." } } },
        "gamestores-shop" => new { Enabled = false, ShopId = 0, SecretKey = "", ServerId = 0, PollIntervalSeconds = 0, RequirePlayerClaim = true, PlayerClaimCommand = "/pay", ClaimCooldownSeconds = 10, ManualClaimMaxPerRun = 5, AutoDeliverPending = true, AutoDeliveryMaxPerRun = 5, DeliveryDelaySeconds = 0, RetryDelaySeconds = 60, MaxDeliveryAttempts = 0, DeliverOnlyToOnlinePlayers = true, AllowDirectItemIdDelivery = false, AllowUnsafeCommandDelivery = false, HttpTimeoutSeconds = 20, Rules = new[] { new { Enabled = true, MatchBucketId = "", MatchProductId = "", MatchItemId = "", MatchTitleContains = "Starter Pack", MatchCommandContains = "", DeliveryMode = "SpawnItem", DeliveryLabel = "Starter Pack", ItemId = "CannedGoulash", Items = Array.Empty<object>(), VehicleAsset = "", Quantity = 1, Amount = 0, SkillName = "", SkillLevel = 0, SkillExperience = 0, DurationDays = 0, Tier = "", Strength = 0, Constitution = 0, Dexterity = 0, Intelligence = 0, CommandTemplate = "", SuccessMessage = "Покупка GameStores выдана." } } },
        "bounty-hunt" => new { Enabled = true, BroadcastThreshold = 3, HeroThreshold = 5, MaxBoardEntries = 5, LeaderKillRewardMode = "None", LeaderKillRewardAmount = 0, BroadcastLeaderKillReward = true },
        "server-kill-feed" => new { Enabled = true, Prefix = "WILD", Source = "SaveFilesLogs", UseRuntimeHooks = false, AnnounceSuicides = true, IncludeDistance = true, IncludeWeapon = true, FanoutToPlayers = false, DuplicateWindowSeconds = 4 },
        "scheduled-events" => ScheduledEventsDefault(),
        "zone-robot-schedule" => ZoneRobotScheduleDefault(),
        "panel-quests" => PanelQuestsDefault(),
        "sector-scan" => new { Enabled = true, RequiredPermission = "", ScanCost = 1000, CooldownSeconds = 120, IncludeSelf = true, IncludePlayerNames = true, MaxListedNames = 5, SuccessMessage = "[Скан] Квадрат {sector}: найдено {count}. {names}", EmptyMessage = "[Скан] Квадрат {sector} пуст.", NotEnoughMoneyMessage = "[Скан] Нужно {cost} денег. Сейчас не хватает.", CooldownMessage = "[Скан] Подожди ещё {seconds} сек." },
        "private-messages" => new { Enabled = true, UsePermission = false, AllowPermission = "pm.use", UseCooldown = true, CooldownTimeSeconds = 3, EnableLogging = true, EnableHistory = true },
        "help-response" => HelpResponseDefault(),
        "info-response" => InfoResponseDefault(),
        "command-aliases" => CommandAliasesDefault(),
        "discord-log" => new { Enabled = false, TransportMode = "Webhook", ServerLabel = "", DefaultWebhookUrl = "", PresenceWebhookUrl = "", ChatWebhookUrl = "", CombatWebhookUrl = "", SystemWebhookUrl = "", GuildId = "", DefaultChannelId = "", PresenceChannelId = "", ChatChannelId = "", CombatChannelId = "", SystemChannelId = "", Username = "WILD", AvatarUrl = "", NotifyOnLoad = true, NotifyPlayerConnected = true, NotifyPlayerDisconnected = true, NotifyPlayerRespawned = false, NotifyPlayerChat = true, NotifyPlayerKills = true, NotifyPlayerDeaths = true, IncludeSteamId = true, IgnoreSlashCommands = true, IgnoredChatPrefixes = new[] { "/" }, ForwardLocalChat = true, ForwardGlobalChat = true, ForwardSquadChat = true, ForwardAdminChat = false, MinPostIntervalMs = 800, MaxQueueLength = 200, DuplicateWindowSeconds = 15, HttpTimeoutSeconds = 10 },
        _ => new { enabled = false }
    };

    private static object CommandAliasesDefault() => new
    {
        Name = "Команды игроков",
        Description = "Позволяет поменять слова вызова команд. Пишите без /, по одной команде на строку. Изменения применяются после рестарта SCUM-сервера.",
        Enabled = true,
        IncludeBangAliases = true,
        Help = new[] { "help" },
        Info = new[] { "info" },
        Hello = new[] { "hello" },
        Language = new[] { "lang", "language" },
        SetHome = new[] { "sethome" },
        Home = new[] { "home" },
        Homes = new[] { "homes" },
        DeleteHome = new[] { "delhome" },
        PrivateMessage = new[] { "pm" },
        Reply = new[] { "reply", "r" },
        PrivateMessageHistory = new[] { "pmhistory" },
        SectorScan = new[] { "scan" },
        FastTravel = new[] { "travel" },
        BaseLoot = new[] { "loot", "collectloot", "sortloot" },
        Rent = new[] { "rent" },
        WelcomePack = new[] { "starter", "welcomepack" },
        DailyPack = new[] { "daily", "dailypack", "daily-pack" },
        Battlepass = new[] { "battlepass", "bp" },
        Dlc = new[] { "dlc", "dls", "длс" },
        GameStores = new[] { "pay", "gamestores", "gs" },
        MoneyTransfer = new[] { "sendmoney", "send" },
        ItemUpgrade = new[] { "upgrade", "up" },
        Quest = new[] { "quest", "quests" },
        Streak = new[] { "streak" },
        Bounty = new[] { "bounty" },
        HunterTop = new[] { "huntertop" },
        Wargm = new[] { "wargm" },
        InventoryDelete = new[] { "invdel", "deleteitem" },
        Vip = new[] { "vip" },
        DiscordTest = new[] { "discordtest" },
        Armory = Array.Empty<string>(),
        ArmoryBack = Array.Empty<string>(),
        Editor = Array.Empty<string>()
    };

    private static object HelpResponseDefault() => new
    {
        Name = "Ответ команды /help",
        Description = "Если включён свой текст, каждая непустая строка отправляется игроку отдельным сообщением чата. Пустой язык автоматически использует русскую версию; выключенный модуль сохраняет стандартную динамическую справку.",
        Enabled = true,
        UseCustomText = false,
        Text = "",
        EnglishText = "",
        LineDelayMs = 140,
        MaxLines = 24,
        MaxLineBytes = 220
    };

    private static object InfoResponseDefault() => new
    {
        Name = "Ответ команды /info",
        Description = "Если включён свой текст, каждая непустая строка отправляется игроку отдельным сообщением чата. Пустой английский текст использует русскую версию; выключенный модуль сохраняет стандартное динамическое описание команд.",
        Enabled = true,
        UseCustomText = false,
        Text = "",
        EnglishText = "",
        LineDelayMs = 160,
        MaxLines = 24,
        MaxLineBytes = 220
    };

    private static object BattlepassDefault() => new
    {
        Name = "Battlepass",
        Description = "Автоматическая 30-дневная серия наград за реальные дни входа. Выдача начинается через минуту после входа, чтобы персонаж успел прогрузиться.",
        Enabled = true,
        MaxDays = 30,
        DelayAfterJoinSeconds = 60,
        PollIntervalMs = 5000,
        RetryWindowSeconds = 300,
        InterItemDelayMs = 1500,
        RequiredPermission = "",
        SuccessMessage = "Battlepass награда {day}/{maxDays} выдана.",
        VipSettings = new
        {
            Enabled = true,
            RequiredPermission = "battlepass.vip",
            SuccessMessage = "VIP Battlepass награда {day}/{maxDays} выдана."
        },
        Rewards = Enumerable.Range(1, 30).Select(day => new
        {
            Enabled = true,
            Day = day,
            MoneyAmount = day switch
            {
                1 => 3000,
                2 => 5000,
                3 => 1000,
                _ => 1000 + day * 250
            },
            GoldAmount = day == 7 || day == 14 || day == 21 || day == 30 ? 1 : 0,
            FameAmount = day % 5 == 0 ? 25 : 0,
            ItemsText = day switch
            {
                2 => "Apple_2|2",
                5 => "CannedGoulash|1",
                10 => "Emergency_bandage_Big|1",
                20 => "Apple_2|2;CannedGoulash|1",
                30 => "MRE_Cheeseburger|1;Emergency_bandage_Big|1",
                _ => ""
            },
            Message = "Battlepass награда {day}/{maxDays} получена."
        }).ToArray(),
        VipRewards = Enumerable.Range(1, 30).Select(day => new
        {
            Enabled = true,
            Day = day,
            MoneyAmount = day switch
            {
                1 => 5000,
                2 => 8000,
                3 => 3000,
                _ => 2500 + day * 400
            },
            GoldAmount = day % 5 == 0 ? 2 : 0,
            FameAmount = day % 3 == 0 ? 50 : 0,
            ItemsText = day switch
            {
                2 => "Apple_2|4",
                5 => "CannedGoulash|2",
                10 => "Emergency_bandage_Big|2",
                20 => "Apple_2|4;CannedGoulash|2",
                30 => "MRE_Cheeseburger|2;Emergency_bandage_Big|2",
                _ => ""
            },
            Message = "VIP Battlepass награда {day}/{maxDays} получена."
        }).ToArray()
    };

    private static object MoneyTransferDefault() => new
    {
        Name = "Переводы денег",
        Description = "Игроки переводят деньги друг другу командой /sendmoney <ник|SteamID> <сумма> или /send <ник|SteamID> <сумма>.",
        Enabled = true,
        RequiredPermission = "",
        Currency = "Normal",
        MinAmount = 100,
        MaxAmount = 100000,
        DailyLimit = 500000,
        CooldownSeconds = 30,
        FeePercent = 0,
        FeeFixed = 0,
        RequireBothOnline = true,
        AllowSelfTransfer = false,
        SuccessMessage = "Перевод отправлен: ${amount} игроку {target}.",
        ReceivedMessage = "Получен перевод ${amount} от {sender}.",
        UsageMessage = "Формат: /sendmoney <ник|SteamID> <сумма>."
    };

    private static object ItemUpgradeDefault() => new
    {
        Name = "Апгрейд предметов",
        Description = "Заменяет предмет в руках на настроенный ResultItemId после оплаты. Включайте только для проверенных пар предметов.",
        Enabled = false,
        AllowReplacement = false,
        AllowSameItemId = true,
        RequiredPermission = "",
        CooldownSeconds = 60,
        RequireItemInHands = true,
        SpawnResultNearPlayer = true,
        SuccessMessage = "Апгрейд выполнен: {source} -> {result}.",
        Rules = new[]
        {
            new
            {
                Enabled = false,
                Alias = "barrett-light",
                DisplayName = "Лёгкий Barrett",
                SourceItemId = "Weapon_M82A1",
                ResultItemId = "Weapon_M82A1",
                Weight = 1,
                Health = 0,
                Uses = 0,
                Dirtiness = 0,
                AmmoCount = 0,
                CashValue = 0,
                KeyCardSector = "",
                CostMoney = 10000,
                CostGold = 0,
                CostFame = 0,
                RequiredCostItemId = "",
                RequiredCostItemQuantity = 0,
                SuccessMessage = "Предмет улучшен. Заберите новую версию рядом с собой."
            }
        }
    };

    private static object BaseLootCollectorDefault() => new
    {
        Name = "Сбор лута базы",
        Description = "Команда /loot собирает лежащий рядом лут в сундуки того же флага. Доступ по умолчанию только участникам отряда владельца флага.",
        Enabled = false,
        RequiredPermission = "",
        CommandAliases = new[] { "loot", "collectloot", "sortloot" },
        RequireSquadOwnedFlag = true,
        AllowFlagOwnerWithoutSquad = true,
        AllowAdminsBypass = false,
        RadiusCm = 5000,
        FlagZoneShape = "square",
        FlagZoneHalfExtentCm = 5000,
        CollectWholeFlagZone = true,
        EnableSphereOverlapScan = true,
        SphereOverlapObjectTypes = "0;1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30;31",
        SphereOverlapMaxActors = 1000,
        SphereOverlapWorkBudgetMs = 120,
        AllowRuntimeFlagFallback = false,
        AllowGlobalInventoryUserScan = false,
        CooldownSeconds = 120,
        MaxItemsPerRun = 80,
        MaxChestsPerRun = 40,
        MaxScannedItems = 5000,
        MaxScannedChests = 160,
        CombinedItemScanLimit = 5000,
        CollectAllOnSingleCommand = true,
        PreferChestItemScanOnly = false,
        BatchItemsPerTick = 1,
        BatchDelayMs = 200,
        MoveWorkBudgetMs = 90,
        SingleItemMoveBudgetMs = 30,
        ScanWorkBudgetMs = 0,
        ChestScanWorkBudgetMs = 0,
        ItemFindAllBudgetMs = 1200,
        ItemScanStepBudgetMs = 45,
        ItemScanDelayMs = 200,
        ItemScanBatchItems = 1,
        ItemScanVerboseSamples = false,
        InventoryLocationMaxAttempts = 16,
        RequireOnFloorPresence = true,
        NameContains = true,
        DefaultChestName = "Loot",
        FallbackToDefaultChest = true,
        DatabaseMode = "native-dll",
        AllowSqliteExeFallback = false,
        SqliteExe = "",
        DatabasePath = "..\\..\\Saved\\SaveFiles\\SCUM.db",
        DryRun = false,
        RelocateBeforeMove = false,
        AllowUnsafeDropAroundFallback = false,
        ControlledRelocate = true,
        ControlledRelocateDelayMs = 1200,
        ControlledRelocateMaxPerRun = 25,
        RelocateIfFartherThanCm = 650,
        RelocateZOffset = 25,
        InventoryLocationOccupancyBatch = true,
        EnableNativeStoreRpc = false,
        NativeStoreRpcExclusive = true,
        NativeStoreRpcVerifyDelayMs = 180,
        NativeStoreRpcMaxPerRun = 1,
        SuccessMessage = "[Лут] Перемещено: {Moved}. Пропущено: {Skipped}.",
        EmptyMessage = "[Лут] Подходящего лута рядом с флагом не найдено.",
        NoFlagMessage = "[Лут] Встань внутри зоны своего флага.",
        AccessDeniedMessage = "[Лут] Сбор доступен только участникам отряда владельца флага.",
        DbUnavailableMessage = "[Лут] Проверка владельца флага недоступна: native SQLite DLL не прочитала SCUM.db.",
        VipSettings = new
        {
            Enabled = true,
            RequiredPermission = "base-loot.vip",
            RadiusCm = 7000,
            CooldownSeconds = 60,
            MaxItemsPerRun = 150
        },
        Rules = new[]
        {
            new { Enabled = true, Name = "Оружие", MatchContains = "weapon;rifle;shotgun;pistol;bow;sword;knife;m82;ak47", ChestName = "Weapons" },
            new { Enabled = true, Name = "Патроны", MatchContains = "ammo;cal;bullet;magazine;shell;arrow", ChestName = "Ammo" },
            new { Enabled = true, Name = "Медицина", MatchContains = "bandage;medical;vitamin;painkiller;antibiotic;firstaid", ChestName = "Medical" },
            new { Enabled = true, Name = "Еда и вода", MatchContains = "apple;food;can;water;drink;mre;meat;goulash", ChestName = "Food" },
            new { Enabled = true, Name = "Одежда", MatchContains = "jacket;pants;boots;helmet;vest;backpack;gloves;shirt", ChestName = "Gear" }
        }
    };

    private static object ZoneRobotScheduleDefault() => new
    {
        Name = "Роботы по зонам",
        Description = "Расписание команд для зон. Используйте только проверенные SCUM admin-команды конкретного сервера.",
        Enabled = false,
        PollIntervalMs = 15000,
        Rules = new[]
        {
            new
            {
                Enabled = false,
                Name = "B2 ночной режим",
                Zone = "B2",
                ScheduleTimesOn = "21:00",
                ScheduleTimesOff = "06:00",
                CommandTemplateOn = "Announce Роботы включены в зоне {Zone}.",
                CommandTemplateOff = "Announce Роботы отключены в зоне {Zone}.",
                AnnouncementOn = "[Зоны] Роботы включены в {Zone}.",
                AnnouncementOff = "[Зоны] Роботы отключены в {Zone}."
            }
        }
    };

    private static object PanelQuestsDefault() => new
    {
        Name = "Квесты сервера",
        Description = "Панельная квестовая доска. Безопасный режим выдаёт награду по /quest claim и не удаляет предметы из инвентаря без доказанного парсера.",
        Enabled = false,
        RequiredPermission = "",
        ClaimCooldownHours = 24,
        AnnounceOnClaim = true,
        ListMessage = "Активные квесты: {quests}",
        Quests = new[]
        {
            new
            {
                Enabled = true,
                Alias = "north",
                Title = "Северный заказ",
                Mode = "Claim",
                Description = "Тестовый ежедневный квест панели.",
                TargetItemId = "",
                TargetCount = 0,
                RewardMoney = 5000,
                RewardGold = 0,
                RewardFame = 0,
                RewardItemsText = "Apple_2|2",
                SuccessMessage = "Квест выполнен: {title}.",
                Announcement = "[Квест] {player} получил награду за {title}."
            }
        }
    };

    private static object ZombieInfectionDefault() => new
    {
        Enabled = false,
        TransformMode = "mesh",
        AllowMeshFallback = false,
        ZombieMeshPath = "/Game/ConZ_Files/Characters/Zombies2/Models/Male_Zombie/Male_Zombie_Mid/SK_Dressed_Mid_01_V1",
        ZombieMeshCandidates = new[]
        {
            "/Game/ConZ_Files/Characters/Zombies2/Models/Male_Zombie/Male_Zombie_Mid/SK_Dressed_Mid_01_V1",
            "/Game/ConZ_Files/Characters/Zombies2/Models/Male_Zombie/Male_Zombie_Muscle/SK_Stripped_Muscle_01",
            "/Game/ConZ_Files/Characters/Zombies2/Models/Military_Zombie/Military_Zombie_01/SK_Military_Zombie_01",
            "/Game/ConZ_Files/Characters/Zombies2/Models/Hospital_Zombie/Hospital_Zombie_03/SK_Dr_Zombie_03",
            "/Game/ConZ_Files/Characters/Zombies2/Models/Female_Zombie/Female_Zombie_Muscle/F_Stripped_Muscle_04/SK_Female_Zombie_04_LOD0"
        },
        ZombieAnimBlueprintPath = "/Game/ConZ_Files/Characters/Zombies2/Blueprints/ABP_Zombie.ABP_Zombie_C",
        ApplyZombieAnimation = false,
        MeshApplyMode = "property",
        UnsafePostMeshRefresh = false,
        UnsafeAnimBlueprint = false,
        MeshComponentMode = "primary",
        AnnounceToServer = true,
        AnnounceMessage = "[Зомби] {player} превращён в зомби.",
        WhisperToPlayer = true,
        PlayerMessage = "Ты превращён в зомби.",
        CooldownSeconds = 60
    };

    private static object ScheduledEventsDefault() => new
    {
        Enabled = false,
        Name = "Планировщик заданий",
        Description = "Задания сервера по времени или интервалу: cargo drop, world events, admin-команды и наборы предметов в точках карты.",
        Instructions = new[]
        {
            "Включите модуль, добавьте или выберите задание, затем нажмите Сохранить изменения.",
            "ScheduleTimes исполняется воркером по локальному времени сервера: 12:00, 18:30. Если ScheduleTimes пустой, работает IntervalMinutes.",
            "Mode=Airdrop вызывает настоящий SCUM cargo drop командой ScheduleWorldEvent BP_CargoDropEvent X={X} Y={Y} Z={Z}.",
            "Mode=WorldEvent запускает выбранный класс события через ScheduleWorldEvent {WorldEventClass} X={X} Y={Y} Z={Z}.",
            "Mode=SpawnItems выдаёт набор в точку карты. Формат ItemsText: ItemId|Количество;ItemId2|Количество.",
            "Mode=Command выполняет одну проверенную admin-команду без символа #."
        },
        WorldEventClasses = new[] { "BP_CargoDropEvent", "BP_EncounterCargoDropEvent", "BP_DeathmatchGameEvent", "BP_TeamDeathmatchGameEvent", "BP_CTFGameEvent", "BP_DropZoneGameEvent" },
        MaxRunsPerTick = 1,
        PollIntervalMs = 15000,
        Jobs = new[]
        {
            new { Enabled = false, Name = "Cargo drop B2 в 12:00 и 18:00", Mode = "Airdrop", ScheduleTimes = "12:00, 18:00", IntervalMinutes = 60, RunOnStartup = false, PointGroup = "airdrop", ItemSet = "", WorldEventClass = "BP_CargoDropEvent", CommandTemplate = "ScheduleWorldEvent BP_CargoDropEvent X={X} Y={Y} Z={Z}", Announcement = "[Ивент] Cargo drop вызван в районе {Point}.", MaxItemsPerRun = 30, PointCount = 1 },
            new { Enabled = false, Name = "World event DropZone C2 в 20:00", Mode = "WorldEvent", ScheduleTimes = "20:00", IntervalMinutes = 180, RunOnStartup = false, PointGroup = "world-event", ItemSet = "", WorldEventClass = "BP_DropZoneGameEvent", CommandTemplate = "ScheduleWorldEvent {WorldEventClass} X={X} Y={Y} Z={Z}", Announcement = "[Ивент] {WorldEventClass} запущен в районе {Point}.", MaxItemsPerRun = 1, PointCount = 1 },
            new { Enabled = false, Name = "Тайник с лутом в 21:00", Mode = "SpawnItems", ScheduleTimes = "21:00", IntervalMinutes = 120, RunOnStartup = false, PointGroup = "stash", ItemSet = "basic", WorldEventClass = "", CommandTemplate = "", Announcement = "[Ивент] Тайник {ItemSet} появился в районе {Point}.", MaxItemsPerRun = 30, PointCount = 1 },
            new { Enabled = false, Name = "Команда по расписанию", Mode = "Command", ScheduleTimes = "03:55", IntervalMinutes = 1440, RunOnStartup = false, PointGroup = "", ItemSet = "", WorldEventClass = "", CommandTemplate = "Announce Плановая проверка сервера.", Announcement = "", MaxItemsPerRun = 1, PointCount = 1 }
        },
        Points = new[]
        {
            new { Name = "B2 cargo", Group = "airdrop", X = -107205d, Y = -250108d, Z = 27875.398d, Radius = 0d },
            new { Name = "C2 world event", Group = "world-event", X = -153247d, Y = 289822d, Z = 200d, Radius = 150d },
            new { Name = "C2 лес", Group = "stash", X = -153247d, Y = 289822d, Z = 200d, Radius = 150d }
        },
        ItemSets = new[]
        {
            new { Name = "basic", Weight = 1, ItemsText = "Apple_2|2;CannedGoulash|1;Emergency_bandage_Big|1" },
            new { Name = "cargo", Weight = 1, ItemsText = "MRE_TunaSalad|2;Water_05l|2;Emergency_bandage_Big|2" }
        }
    };
}
