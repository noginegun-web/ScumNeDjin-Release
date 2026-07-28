using System.Text.RegularExpressions;
using System.Globalization;
using System.Text;
using Microsoft.Extensions.Options;
using ScumWarden.Server.Configuration;

namespace ScumWarden.Server.Services;

public sealed class ScumLogService
{
    private const int CrashProblemCacheSeconds = 30;

    private static readonly Regex ScumChatRegex = new(
        @"^(?:\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\s*)?'(?<player>[^']+)'\s+'(?<channel>[^:']+):\s*(?<msg>.*)'$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly Regex ScumLoginRegex = new(
        @"^(?:\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\s*)?'(?<player>[^']+)'\s+(?<state>logged in|logged out|connected|disconnected)",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);
    private static readonly Regex PlainKillRegex = new(
        @"LogSCUM:\s+'(?<victim>[^']+)'\s+was killed by\s+'(?<killer>[^']+)'",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);
    private static readonly Regex ScumDiedRegex = new(
        @"^(?:\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\s*)?Died:\s*(?<victim>.+?)(?:\s*\([^)]*\))?,\s*Killer:\s*(?<killer>.+?)(?:\s*\([^)]*\))?\s+Weapon:\s*(?<weapon>.+?)(?:\s+S:\[.*?Distance:\s*(?<distance>[0-9.,]+)\s*m.*\])?$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);
    private static readonly Regex ScumAdminCommandRegex = new(
        @"^(?:\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\s*)?'(?<player>[^']+)'\s+Command:\s+'(?<command>.*?)'\s*$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);
    private static readonly Regex ScumActionLogRegex = new(
        @"^(?:\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\s*)?\[(?<category>Log(?:Trap|BaseBuilding|Minigame|Chest|Vehicle|Crafting|Economy|Bank|Squad|GameEvent))\]\s*(?<content>.+)$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);
    private static readonly Regex ScumLocationRegex = new(
        @"Location:\s*X=(?<x>[-0-9.,]+)\s+Y=(?<y>[-0-9.,]+)\s+Z=(?<z>[-0-9.,]+)",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);
    private static readonly Regex NameDbSteamRegex = new(
        @"(?<name>.+?)\s*\((?<databaseId>\d+),\s*(?<steamId>\d{16,20})\)",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly Regex SteamDbNameRegex = new(
        @"(?<steamId>\d{16,20})\s*\((?<databaseId>\d+),\s*(?<name>.+?)\)",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly Regex ChatRegex = new(@"(?:Chat|CHAT).*?(?<name>[A-Za-z0-9_\-\[\] ]{2,32})\s*[:>]\s*(?<message>.+)$", RegexOptions.Compiled);
    private static readonly Regex KillRegex = new(@"(?<killer>[A-Za-z0-9_\-\[\] ]{2,32}).*?(?:killed|Kill).*?(?<victim>[A-Za-z0-9_\-\[\] ]{2,32})", RegexOptions.Compiled | RegexOptions.IgnoreCase);
    private static readonly Regex SteamRegex = new(@"(?<steam>\d{16,20})", RegexOptions.Compiled);
    private static readonly Regex GlobalStatsPlayersRegex = new(
        @"\|\s*C:\s*\d+.*?,\s*P:\s*(?<count>\d+)\s*\(",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    private readonly IOptionsMonitor<WardenOptions> _options;
    private readonly WardenEventStore _events;
    private readonly Dictionary<string, long> _positions = new(StringComparer.OrdinalIgnoreCase);
    private readonly object _problemScanGate = new();
    private CrashProblemStatus? _lastProblemScan;
    private DateTimeOffset _lastProblemScanUtc = DateTimeOffset.MinValue;

    public ScumLogService(IOptionsMonitor<WardenOptions> options, WardenEventStore events)
    {
        _options = options;
        _events = events;
    }

    public StoreStatus GetStatus()
    {
        if (_options.CurrentValue.ClientTestMode)
        {
            return new StoreStatus(true, "client-test", "Client test mode: simulated SCUM logs are available.");
        }

        var paths = LogDirectories().ToArray();
        var online = paths.Any(Directory.Exists);
        return online
            ? new StoreStatus(true, string.Join(" | ", paths.Where(Directory.Exists)), "Log directories exist.")
            : new StoreStatus(false, string.Join(" | ", paths), "Log directories not found.");
    }

    public void PollOnce()
    {
        if (_options.CurrentValue.ClientTestMode)
        {
            return;
        }

        foreach (var path in LogDirectories())
        {
            if (!Directory.Exists(path))
            {
                continue;
            }

            foreach (var file in Directory.EnumerateFiles(path, "*.log").OrderByDescending(File.GetLastWriteTimeUtc).Take(12))
            {
                ReadNewLines(file);
            }
        }
    }

    public IReadOnlyList<string> ReadServerLines(int limit)
    {
        if (_options.CurrentValue.ClientTestMode)
        {
            return ClientTestSimulator.ServerLogLines().Take(Math.Clamp(limit, 1, 1000)).ToArray();
        }

        var path = _options.CurrentValue.LogDirectory;
        if (!Directory.Exists(path))
        {
            return Array.Empty<string>();
        }

        var newest = Directory.EnumerateFiles(path, "*.log")
            .OrderByDescending(File.GetLastWriteTimeUtc)
            .FirstOrDefault();
        return newest is null ? Array.Empty<string>() : Tail(newest, limit);
    }

    public IReadOnlyList<string> ReadRuntimeLines(int limit)
    {
        if (_options.CurrentValue.ClientTestMode)
        {
            return ClientTestSimulator.RuntimeLogLines().Take(Math.Clamp(limit, 1, 1000)).ToArray();
        }

        var path = RuntimeLogPaths(_options.CurrentValue).FirstOrDefault(File.Exists);
        return path is not null ? Tail(path, limit) : Array.Empty<string>();
    }

    public IReadOnlyList<string> ReadUe4ssLines(int limit)
    {
        if (_options.CurrentValue.ClientTestMode)
        {
            return Array.Empty<string>();
        }

        var path = GetUe4ssLogPath();
        return path is not null && File.Exists(path) ? Tail(path, limit) : Array.Empty<string>();
    }

    public string? GetWin64Directory()
    {
        var options = _options.CurrentValue;
        if (!string.IsNullOrWhiteSpace(options.BridgePath))
        {
            var bridgeDirectory = Directory.GetParent(options.BridgePath)?.FullName;
            if (!string.IsNullOrWhiteSpace(bridgeDirectory) && Directory.Exists(bridgeDirectory))
            {
                return bridgeDirectory;
            }
        }

        if (!string.IsNullOrWhiteSpace(options.RuntimeLogPath))
        {
            var runtimeDirectory = Directory.GetParent(options.RuntimeLogPath)?.FullName;
            var parent = !string.IsNullOrWhiteSpace(runtimeDirectory)
                ? Directory.GetParent(runtimeDirectory)?.FullName
                : null;
            if (!string.IsNullOrWhiteSpace(parent) && Directory.Exists(parent))
            {
                return parent;
            }

            if (!string.IsNullOrWhiteSpace(runtimeDirectory) && Directory.Exists(runtimeDirectory))
            {
                return runtimeDirectory;
            }
        }

        if (!string.IsNullOrWhiteSpace(options.ServerRoot))
        {
            foreach (var candidate in new[]
            {
                Path.Combine(options.ServerRoot, "SCUM", "Binaries", "Win64"),
                Path.Combine(options.ServerRoot, "Binaries", "Win64"),
                options.ServerRoot
            })
            {
                if (Directory.Exists(candidate))
                {
                    return candidate;
                }
            }
        }

        return null;
    }

    public string? GetUe4ssLogPath()
    {
        var win64 = GetWin64Directory();
        if (string.IsNullOrWhiteSpace(win64))
        {
            return null;
        }

        var path = Path.Combine(win64, "UE4SS.log");
        return File.Exists(path) ? path : null;
    }

    public IReadOnlyList<CrashArtifact> GetRecentCrashArtifacts(int maxAgeHours = 72)
    {
        var win64 = GetWin64Directory();
        if (string.IsNullOrWhiteSpace(win64) || !Directory.Exists(win64))
        {
            return Array.Empty<CrashArtifact>();
        }

        var cutoff = DateTimeOffset.UtcNow.AddHours(-Math.Clamp(maxAgeHours, 1, 24 * 14));
        try
        {
            return Directory.EnumerateFiles(win64, "crash_*", SearchOption.TopDirectoryOnly)
                .Select(path =>
                {
                    try
                    {
                        var info = new FileInfo(path);
                        return new CrashArtifact(
                            Path.GetExtension(path).Equals(".dmp", StringComparison.OrdinalIgnoreCase) ? "dump" : "crash-file",
                            path,
                            info.Length,
                            new DateTimeOffset(info.LastWriteTimeUtc, TimeSpan.Zero));
                    }
                    catch
                    {
                        return null;
                    }
                })
                .Where(artifact => artifact is not null && artifact.ModifiedUtc >= cutoff)
                .Cast<CrashArtifact>()
                .OrderByDescending(artifact => artifact.ModifiedUtc)
                .Take(10)
                .ToArray();
        }
        catch
        {
            return Array.Empty<CrashArtifact>();
        }
    }

    public CrashProblemStatus DetectRecentProblem(int maxAgeHours = 72)
    {
        if (_options.CurrentValue.ClientTestMode)
        {
            return CrashProblemStatus.None();
        }

        var now = DateTimeOffset.UtcNow;
        lock (_problemScanGate)
        {
            if (_lastProblemScan is not null && (now - _lastProblemScanUtc).TotalSeconds < CrashProblemCacheSeconds)
            {
                return _lastProblemScan;
            }

            _lastProblemScan = BuildProblemStatus(maxAgeHours);
            _lastProblemScanUtc = now;
            return _lastProblemScan;
        }
    }

    private CrashProblemStatus BuildProblemStatus(int maxAgeHours)
    {
        var serverLines = ReadServerLines(600);
        var ue4ssLines = ReadUe4ssLines(600);
        var runtimeLines = ReadRuntimeLines(300);
        var artifacts = GetRecentCrashArtifacts(maxAgeHours);
        var markers = serverLines.Concat(ue4ssLines).Concat(runtimeLines)
            .Where(IsCrashOrUe4ssProblemLine)
            .Reverse()
            .Take(40)
            .Reverse()
            .ToArray();

        var latestArtifact = artifacts.FirstOrDefault();
        var hasFatal = markers.Any(line =>
            line.Contains("Fatal error", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("Unhandled Exception", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("EXCEPTION_ACCESS_VIOLATION", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("crashdump", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("UE4SS.dll", StringComparison.OrdinalIgnoreCase));
        var hasUe4ssIssue = markers.Any(line =>
            line.Contains("UE4SS", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("lua error", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("stack overflow", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("recursion", StringComparison.OrdinalIgnoreCase));

        if (!hasFatal && !hasUe4ssIssue && latestArtifact is null)
        {
            return CrashProblemStatus.None();
        }

        var detectedAt = latestArtifact?.ModifiedUtc ?? LatestReadableLogTimeUtc();
        var severity = hasFatal || latestArtifact is not null ? "fatal" : "warning";
        var title = severity == "fatal"
            ? "Обнаружен след краша SCUM/UE4SS"
            : "Обнаружены ошибки UE4SS/моста в логах";
        var message = severity == "fatal"
            ? "В логах или crash-файлах найден признак падения предыдущего запуска. Скачайте диагностический архив и прикрепите его в Discord-тикет."
            : "В логах найден признак ошибки UE4SS/Lua. Если сервер нестабилен, скачайте диагностический архив и прикрепите его в Discord-тикет.";
        var source = string.Join(" + ", new[]
        {
            serverLines.Count > 0 ? "SCUM.log" : "",
            ue4ssLines.Count > 0 ? "UE4SS.log" : "",
            runtimeLines.Count > 0 ? "nedjin_bridge.log" : "",
            artifacts.Count > 0 ? "crash_*" : ""
        }.Where(value => value.Length > 0));

        return new CrashProblemStatus(
            true,
            severity,
            title,
            message,
            source,
            detectedAt,
            markers,
            artifacts.ToArray());
    }

    private DateTimeOffset? LatestReadableLogTimeUtc()
    {
        var candidates = new List<string>();
        var newestServer = NewestServerLogPath();
        if (!string.IsNullOrWhiteSpace(newestServer))
        {
            candidates.Add(newestServer);
        }

        var runtime = _options.CurrentValue.RuntimeLogPath;
        if (!string.IsNullOrWhiteSpace(runtime))
        {
            candidates.Add(runtime);
        }

        var ue4ss = GetUe4ssLogPath();
        if (!string.IsNullOrWhiteSpace(ue4ss))
        {
            candidates.Add(ue4ss);
        }

        return candidates
            .Where(File.Exists)
            .Select(path => new DateTimeOffset(File.GetLastWriteTimeUtc(path), TimeSpan.Zero))
            .OrderByDescending(value => value)
            .Cast<DateTimeOffset?>()
            .FirstOrDefault();
    }

    private string? NewestServerLogPath()
    {
        var path = _options.CurrentValue.LogDirectory;
        if (!Directory.Exists(path))
        {
            return null;
        }

        try
        {
            return Directory.EnumerateFiles(path, "*.log")
                .OrderByDescending(File.GetLastWriteTimeUtc)
                .FirstOrDefault();
        }
        catch
        {
            return null;
        }
    }

    private static bool IsCrashOrUe4ssProblemLine(string line)
    {
        var text = line ?? "";
        return text.Contains("Fatal error", StringComparison.OrdinalIgnoreCase) ||
            text.Contains("Unhandled Exception", StringComparison.OrdinalIgnoreCase) ||
            text.Contains("EXCEPTION_ACCESS_VIOLATION", StringComparison.OrdinalIgnoreCase) ||
            text.Contains("crashdump", StringComparison.OrdinalIgnoreCase) ||
            text.Contains("crash_", StringComparison.OrdinalIgnoreCase) ||
            text.Contains("stack overflow", StringComparison.OrdinalIgnoreCase) ||
            text.Contains("recursion", StringComparison.OrdinalIgnoreCase) ||
            text.Contains("UE4SS.dll", StringComparison.OrdinalIgnoreCase) ||
            text.Contains("UE4SS error", StringComparison.OrdinalIgnoreCase) ||
            text.Contains("lua error", StringComparison.OrdinalIgnoreCase);
    }

    public object InferOnlinePlayersFromLog(int limit = 2000)
    {
        if (_options.CurrentValue.ClientTestMode)
        {
            return ClientTestSimulator.PlayersEnvelope(_options.CurrentValue);
        }

        var rows = new Dictionary<string, PlayerSnapshot>(StringComparer.OrdinalIgnoreCase);
        var latestPlayerCount = -1;

        foreach (var line in ReadServerLines(limit))
        {
            var stats = GlobalStatsPlayersRegex.Match(line);
            if (stats.Success && int.TryParse(stats.Groups["count"].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsedCount))
            {
                latestPlayerCount = parsedCount;
            }

            if (!TryParseScumLoginLine(line, out var login))
            {
                continue;
            }

            var key = !string.IsNullOrWhiteSpace(login.SteamId) ? login.SteamId : login.Name;
            if (string.IsNullOrWhiteSpace(key))
            {
                continue;
            }

            rows[key] = new PlayerSnapshot(login.SteamId, login.Name, login.DatabaseId, login.IsJoin, DateTimeOffset.UtcNow);
        }

        var online = rows.Values
            .Where(player => player.Online)
            .Reverse()
            .Take(latestPlayerCount >= 0 ? latestPlayerCount : rows.Count)
            .Select(player => new
            {
                name = player.Name,
                steamId = player.SteamId,
                online = true,
                status = "online",
                source = "server-log",
                userProfileId = player.DatabaseId > 0 ? player.DatabaseId.ToString(CultureInfo.InvariantCulture) : "",
                serverUserProfileId = player.DatabaseId > 0 ? player.DatabaseId.ToString(CultureInfo.InvariantCulture) : "",
                runtimeKey = "",
                runtimeReady = false
            })
            .ToArray();

        return new
        {
            command = "players_snapshot",
            source = "server-log",
            message = "Список игроков восстановлен по SCUM.log; live runtime bridge не запрошен.",
            payload = new
            {
                players = online,
                count = online.Length,
                logPlayerCount = latestPlayerCount,
                runtimeReady = false
            },
            warnings = Array.Empty<string>(),
            completedAtUtc = DateTimeOffset.UtcNow
        };
    }

    private void ReadNewLines(string file)
    {
        try
        {
            var length = new FileInfo(file).Length;
            var start = _positions.TryGetValue(file, out var pos) ? pos : Math.Max(0, length - 128 * 1024);
            if (length < start)
            {
                start = 0;
            }

            var wasKnown = _positions.ContainsKey(file);
            using var stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            var encoding = DetectLogEncoding(stream);
            if (start > 0 && IsUnicodeEncoding(encoding) && start % 2 != 0)
            {
                start--;
            }

            stream.Seek(start, SeekOrigin.Begin);
            using var reader = new StreamReader(stream, encoding, detectEncodingFromByteOrderMarks: true);
            if (!wasKnown && start > 0)
            {
                _ = reader.ReadLine();
            }

            while (!reader.EndOfStream)
            {
                var line = reader.ReadLine();
                if (!string.IsNullOrWhiteSpace(line))
                {
                    ParseLine(line);
                }
            }

            _positions[file] = stream.Position;
        }
        catch
        {
            // Log tailing must never break the control panel.
        }
    }

    private void ParseLine(string line)
    {
        var cleanLine = CleanLogLine(line);
        if (string.IsNullOrWhiteSpace(cleanLine))
        {
            return;
        }

        if (IsScumKillJsonDetail(cleanLine))
        {
            return;
        }

        if (TryParseScumChatLine(cleanLine, out var chat))
        {
            _events.Add(new WardenEvent("chat", DateTimeOffset.UtcNow, chat.Message, new Dictionary<string, object?>
            {
                ["player"] = chat.Name,
                ["name"] = chat.Name,
                ["steamId"] = chat.SteamId,
                ["channel"] = chat.Channel
            }, line));
            return;
        }

        if (TryParseScumLoginLine(cleanLine, out var login))
        {
            _events.Add(new WardenEvent("login", DateTimeOffset.UtcNow, cleanLine, new Dictionary<string, object?>
            {
                ["player"] = login.Name,
                ["name"] = login.Name,
                ["steamId"] = login.SteamId,
                ["databaseId"] = login.DatabaseId,
                ["state"] = login.IsJoin ? "joined" : "left"
            }, line));
            return;
        }

        var scumDied = ScumDiedRegex.Match(cleanLine);
        if (scumDied.Success)
        {
            var killer = ParsePlayerToken(scumDied.Groups["killer"].Value);
            var victim = ParsePlayerToken(scumDied.Groups["victim"].Value);
            var distance = scumDied.Groups["distance"].Success ? CleanValue(scumDied.Groups["distance"].Value + " m") : "";
            _events.Add(new WardenEvent("kill", DateTimeOffset.UtcNow, cleanLine, new Dictionary<string, object?>
            {
                ["killer"] = killer.Name,
                ["killerSteamId"] = killer.SteamId,
                ["victim"] = victim.Name,
                ["victimSteamId"] = victim.SteamId,
                ["weapon"] = CleanValue(scumDied.Groups["weapon"].Value),
                ["distance"] = distance
            }, line));
            return;
        }

        var plainKill = PlainKillRegex.Match(cleanLine);
        if (plainKill.Success)
        {
            var killer = ParsePlayerToken(plainKill.Groups["killer"].Value);
            var victim = ParsePlayerToken(plainKill.Groups["victim"].Value);
            _events.Add(new WardenEvent("kill", DateTimeOffset.UtcNow, cleanLine, new Dictionary<string, object?>
            {
                ["killer"] = killer.Name,
                ["killerSteamId"] = killer.SteamId,
                ["victim"] = victim.Name,
                ["victimSteamId"] = victim.SteamId
            }, line));
            return;
        }

        var adminCommand = ScumAdminCommandRegex.Match(cleanLine);
        if (adminCommand.Success)
        {
            var player = ParsePlayerToken(adminCommand.Groups["player"].Value);
            var command = adminCommand.Groups["command"].Value.Trim();
            _events.Add(new WardenEvent("command", DateTimeOffset.UtcNow, command, new Dictionary<string, object?>
            {
                ["player"] = player.Name,
                ["name"] = player.Name,
                ["steamId"] = player.SteamId,
                ["databaseId"] = player.DatabaseId,
                ["command"] = command,
                ["source"] = "server-log"
            }, line));
            return;
        }

        var action = ScumActionLogRegex.Match(cleanLine);
        if (action.Success)
        {
            var category = action.Groups["category"].Value.Trim();
            var content = action.Groups["content"].Value.Trim();
            var actor = ParseActionActor(content);
            var data = new Dictionary<string, object?>
            {
                ["category"] = category,
                ["content"] = content,
                ["player"] = actor.Name,
                ["name"] = actor.Name,
                ["steamId"] = actor.SteamId,
                ["databaseId"] = actor.DatabaseId,
                ["source"] = "server-log"
            };

            var location = ScumLocationRegex.Match(content);
            if (location.Success)
            {
                data["x"] = NormalizeNumberText(location.Groups["x"].Value);
                data["y"] = NormalizeNumberText(location.Groups["y"].Value);
                data["z"] = NormalizeNumberText(location.Groups["z"].Value);
            }

            _events.Add(new WardenEvent("action", DateTimeOffset.UtcNow, cleanLine, data, line));
            return;
        }

        var lower = cleanLine.ToLowerInvariant();
        var legacyChat = ChatRegex.Match(cleanLine);
        if (legacyChat.Success && IsLikelyLegacyChatLine(cleanLine, lower))
        {
            _events.Add(new WardenEvent("chat", DateTimeOffset.UtcNow, legacyChat.Groups["message"].Value.Trim(), new Dictionary<string, object?>
            {
                ["player"] = legacyChat.Groups["name"].Value.Trim(),
                ["steamId"] = SteamRegex.Match(cleanLine).Groups["steam"].Value
            }, cleanLine));
            return;
        }

        if (!IsServerSettingLine(cleanLine, lower) && (lower.Contains("kill") || lower.Contains("killed")))
        {
            var match = KillRegex.Match(cleanLine);
            if (match.Success)
            {
                _events.Add(new WardenEvent("kill", DateTimeOffset.UtcNow, cleanLine, new Dictionary<string, object?>
                {
                    ["killer"] = match.Groups["killer"].Value.Trim(),
                    ["victim"] = match.Groups["victim"].Value.Trim()
                }, cleanLine));
                return;
            }
        }

        if (!IsServerSettingLine(cleanLine, lower)
            && SteamRegex.IsMatch(cleanLine)
            && (lower.Contains("login") || lower.Contains("logged in") || lower.Contains("logged out") || lower.Contains("connected") || lower.Contains("disconnect")))
        {
            _events.Add(new WardenEvent("login", DateTimeOffset.UtcNow, cleanLine, new Dictionary<string, object?>
            {
                ["steamId"] = SteamRegex.Match(cleanLine).Groups["steam"].Value
            }, cleanLine));
            return;
        }

        if (lower.Contains("currency") || lower.Contains("economy") || lower.Contains("gold"))
        {
            _events.Add(new WardenEvent("economy", DateTimeOffset.UtcNow, cleanLine, new Dictionary<string, object?>(), cleanLine));
        }
    }

    private static bool IsLikelyLegacyChatLine(string line, string lower)
    {
        if (IsServerSettingLine(line, lower))
        {
            return false;
        }

        var trimmed = CleanLogLine(line);
        return SteamRegex.IsMatch(trimmed)
            || trimmed.StartsWith("Chat:", StringComparison.OrdinalIgnoreCase)
            || trimmed.StartsWith("CHAT:", StringComparison.OrdinalIgnoreCase)
            || trimmed.StartsWith("[Chat]", StringComparison.OrdinalIgnoreCase)
            || trimmed.Contains(" chat ", StringComparison.OrdinalIgnoreCase)
            || trimmed.Contains(" CHAT ", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsServerSettingLine(string line, string lower) =>
        lower.StartsWith("logconfig:", StringComparison.OrdinalIgnoreCase)
        || lower.Contains("setting cvar", StringComparison.OrdinalIgnoreCase)
        || line.Contains("[[", StringComparison.Ordinal);

    private static bool IsScumKillJsonDetail(string line) =>
        line.Contains("\"Killer\"", StringComparison.OrdinalIgnoreCase)
        && line.Contains("\"Victim\"", StringComparison.OrdinalIgnoreCase);

    private IEnumerable<string> LogDirectories()
    {
        var options = _options.CurrentValue;
        yield return options.LogDirectory;
        yield return options.SaveFilesLogDirectory;

        var inferred = Path.Combine(options.ServerRoot, "SCUM", "Saved", "SaveFiles", "Logs");
        if (!string.Equals(inferred, options.SaveFilesLogDirectory, StringComparison.OrdinalIgnoreCase))
        {
            yield return inferred;
        }
    }

    private static IEnumerable<string> RuntimeLogPaths(WardenOptions options)
    {
        if (!string.IsNullOrWhiteSpace(options.RuntimeLogPath))
        {
            yield return options.RuntimeLogPath;
        }

        if (!string.IsNullOrWhiteSpace(options.RuntimeDataRoot))
        {
            yield return Path.Combine(options.RuntimeDataRoot, "logs", "nedjin.log");
        }

        if (!string.IsNullOrWhiteSpace(options.ServerRoot))
        {
            yield return Path.Combine(options.ServerRoot, "SCUM", "Saved", "ScumNeDjin", "logs", "nedjin.log");
            yield return Path.Combine(options.ServerRoot, "Saved", "ScumNeDjin", "logs", "nedjin.log");
        }

        if (!string.IsNullOrWhiteSpace(options.BridgePath))
        {
            var win64 = Directory.GetParent(options.BridgePath)?.FullName;
            var scumRoot = !string.IsNullOrWhiteSpace(win64)
                ? Directory.GetParent(win64)?.Parent?.FullName
                : null;
            if (!string.IsNullOrWhiteSpace(scumRoot))
            {
                yield return Path.Combine(scumRoot, "Saved", "ScumNeDjin", "logs", "nedjin.log");
            }

            yield return Path.Combine(options.BridgePath, "nedjin.log");
        }
    }

    private static bool TryParseScumChatLine(string line, out ParsedChat chat)
    {
        chat = default;
        var match = ScumChatRegex.Match(CleanLogLine(line));
        if (!match.Success)
        {
            return false;
        }

        var player = ParsePlayerToken(match.Groups["player"].Value);
        if (string.IsNullOrWhiteSpace(player.Name))
        {
            return false;
        }

        var message = match.Groups["msg"].Value.Trim();
        if (message.Length == 0)
        {
            return false;
        }

        chat = new ParsedChat(player.SteamId, player.Name, match.Groups["channel"].Value.Trim(), message);
        return true;
    }

    private static bool TryParseScumLoginLine(string line, out ParsedLogin login)
    {
        login = default;
        var match = ScumLoginRegex.Match(CleanLogLine(line));
        if (!match.Success)
        {
            return false;
        }

        var player = ParsePlayerToken(match.Groups["player"].Value);
        if (string.IsNullOrWhiteSpace(player.Name) && string.IsNullOrWhiteSpace(player.SteamId))
        {
            return false;
        }

        var state = match.Groups["state"].Value;
        login = new ParsedLogin(player.SteamId, player.Name, player.DatabaseId,
            state.Contains("logged in", StringComparison.OrdinalIgnoreCase) ||
            state.Contains("connected", StringComparison.OrdinalIgnoreCase));
        return true;
    }

    private static ParsedPlayer ParsePlayerToken(string token)
    {
        var work = (token ?? "").Trim();
        var firstSpace = work.IndexOf(' ');
        if (firstSpace > 0 && System.Net.IPAddress.TryParse(work[..firstSpace].Trim(), out _))
        {
            work = work[(firstSpace + 1)..].Trim();
        }

        var colon = work.IndexOf(':');
        if (colon <= 0)
        {
            return new ParsedPlayer("", work.Trim(), 0);
        }

        var steamId = work[..colon].Trim();
        var remainder = work[(colon + 1)..].Trim();
        var databaseId = 0;
        var tail = Regex.Match(remainder, @"^(?<name>.*?)(?:\((?<db>\d+)\))?$", RegexOptions.CultureInvariant);
        if (tail.Success)
        {
            remainder = tail.Groups["name"].Value.Trim();
            if (tail.Groups["db"].Success)
            {
                _ = int.TryParse(tail.Groups["db"].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out databaseId);
            }
        }

        return new ParsedPlayer(steamId, remainder, databaseId);
    }

    private static ParsedPlayer ParseActionActor(string line)
    {
        foreach (var marker in new[] { "User:", "Owner:", "Burier:", "Unburier:", "Overtaker:", "New owner:", "Old owner:" })
        {
            var index = line.IndexOf(marker, StringComparison.OrdinalIgnoreCase);
            if (index < 0)
            {
                continue;
            }

            var token = line[(index + marker.Length)..].Trim();
            var stop = token.IndexOf('.');
            if (stop >= 0)
            {
                token = token[..stop].Trim();
            }

            var parsed = ParseScumPerson(token);
            if (!string.IsNullOrWhiteSpace(parsed.Name) || !string.IsNullOrWhiteSpace(parsed.SteamId))
            {
                return parsed;
            }
        }

        return new ParsedPlayer("", "", 0);
    }

    private static ParsedPlayer ParseScumPerson(string token)
    {
        token = (token ?? "").Trim();
        if (token.Length == 0 || token.Equals("N/A", StringComparison.OrdinalIgnoreCase) ||
            token.Equals("-1()", StringComparison.OrdinalIgnoreCase) || token.Equals("0(World)", StringComparison.OrdinalIgnoreCase))
        {
            return new ParsedPlayer("", token, 0);
        }

        var nameDbSteam = NameDbSteamRegex.Match(token);
        if (nameDbSteam.Success)
        {
            _ = int.TryParse(nameDbSteam.Groups["databaseId"].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var databaseId);
            return new ParsedPlayer(nameDbSteam.Groups["steamId"].Value.Trim(), nameDbSteam.Groups["name"].Value.Trim(), databaseId);
        }

        var steamDbName = SteamDbNameRegex.Match(token);
        if (steamDbName.Success)
        {
            _ = int.TryParse(steamDbName.Groups["databaseId"].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var databaseId);
            return new ParsedPlayer(steamDbName.Groups["steamId"].Value.Trim(), steamDbName.Groups["name"].Value.Trim(), databaseId);
        }

        return ParsePlayerToken(token);
    }

    private static string NormalizeNumberText(string value) =>
        (value ?? "").Trim().Replace(',', '.');

    private static string CleanValue(string value) =>
        Regex.Replace((value ?? "").Trim().Trim('"', '\'', '[', ']'), "\\s+", " ");

    private static string CleanLogLine(string line) =>
        (line ?? string.Empty).Replace("\0", string.Empty).Trim().TrimStart('\uFEFF');

    private static IEnumerable<string> ReadLogLines(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        var encoding = DetectLogEncoding(stream);
        stream.Position = 0;
        using var reader = new StreamReader(stream, encoding, detectEncodingFromByteOrderMarks: true);
        while (!reader.EndOfStream)
        {
            var line = reader.ReadLine();
            if (line is not null)
            {
                yield return line;
            }
        }
    }

    private static Encoding DetectLogEncoding(Stream stream)
    {
        Span<byte> bom = stackalloc byte[4];
        stream.Position = 0;
        var read = stream.Read(bom);
        stream.Position = 0;
        if (read >= 2 && bom[0] == 0xFF && bom[1] == 0xFE)
        {
            return Encoding.Unicode;
        }

        if (read >= 2 && bom[0] == 0xFE && bom[1] == 0xFF)
        {
            return Encoding.BigEndianUnicode;
        }

        if (read >= 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)
        {
            return Encoding.UTF8;
        }

        return LooksLikeUtf16Le(stream) ? Encoding.Unicode : Encoding.UTF8;
    }

    private static bool LooksLikeUtf16Le(Stream stream)
    {
        stream.Position = 0;
        Span<byte> sample = stackalloc byte[512];
        var read = stream.Read(sample);
        stream.Position = 0;
        if (read < 8)
        {
            return false;
        }

        var zeroOddBytes = 0;
        var checkedOddBytes = 0;
        for (var i = 1; i < read; i += 2)
        {
            checkedOddBytes++;
            if (sample[i] == 0)
            {
                zeroOddBytes++;
            }
        }

        return checkedOddBytes > 0 && zeroOddBytes >= checkedOddBytes / 3;
    }

    private static bool IsUnicodeEncoding(Encoding encoding) =>
        encoding.CodePage == Encoding.Unicode.CodePage || encoding.CodePage == Encoding.BigEndianUnicode.CodePage;

    private readonly record struct ParsedPlayer(string SteamId, string Name, int DatabaseId);
    private readonly record struct ParsedChat(string SteamId, string Name, string Channel, string Message);
    private readonly record struct ParsedLogin(string SteamId, string Name, int DatabaseId, bool IsJoin);
    private readonly record struct PlayerSnapshot(string SteamId, string Name, int DatabaseId, bool Online, DateTimeOffset TimestampUtc);

    private static IReadOnlyList<string> Tail(string path, int limit)
    {
        try
        {
            return ReadLogLines(path)
                .Reverse()
                .Take(Math.Clamp(limit, 1, 2000))
                .Reverse()
                .Select(CleanLogLine)
                .Where(line => !string.IsNullOrWhiteSpace(line))
                .ToArray();
        }
        catch
        {
            return Array.Empty<string>();
        }
    }
}

public sealed record CrashProblemStatus(
    bool HasProblem,
    string Severity,
    string Title,
    string Message,
    string Source,
    DateTimeOffset? DetectedAtUtc,
    IReadOnlyList<string> Markers,
    IReadOnlyList<CrashArtifact> Artifacts)
{
    public static CrashProblemStatus None() => new(
        false,
        "ok",
        "",
        "",
        "",
        null,
        Array.Empty<string>(),
        Array.Empty<CrashArtifact>());
}

public sealed record CrashArtifact(string Kind, string Path, long SizeBytes, DateTimeOffset ModifiedUtc);

public sealed class LogWatcherService : BackgroundService
{
    private readonly ScumLogService _logs;
    private readonly IOptionsMonitor<WardenOptions> _options;

    public LogWatcherService(ScumLogService logs, IOptionsMonitor<WardenOptions> options)
    {
        _logs = logs;
        _options = options;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            _logs.PollOnce();
            await Task.Delay(Math.Max(250, _options.CurrentValue.LogPollMilliseconds), stoppingToken);
        }
    }
}
