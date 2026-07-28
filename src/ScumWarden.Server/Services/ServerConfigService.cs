using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Extensions.Options;
using ScumWarden.Server.Configuration;

namespace ScumWarden.Server.Services;

public sealed class ServerConfigService
{
    private const int MaxConfigBytes = 2 * 1024 * 1024;
    private static readonly Regex SafeFileName = new(@"^[A-Za-z0-9_.-]{1,96}$", RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly string[] KnownConfigNames =
    [
        "ServerSettings.ini",
        "Engine.ini",
        "Game.ini",
        "GameUserSettings.ini",
        "EconomyOverride.json",
        "AdminUsers.ini",
        "BannedUsers.ini",
        "WhitelistedUsers.ini",
        "ExclusiveUsers.ini",
        "SilencedUsers.ini",
        "ServerSettingsAdminUsers.ini",
        "RaidTimes.json",
        "Notifications.json",
        "Input.ini"
    ];

    private readonly IOptionsMonitor<WardenOptions> _options;

    public ServerConfigService(IOptionsMonitor<WardenOptions> options)
    {
        _options = options;
    }

    public object GetServerInfo()
    {
        var name = GetServerName();
        var root = ResolveScumRoot();
        return new
        {
            name,
            server = name,
            configRoot = root is null ? "" : Path.Combine(root, "Saved", "Config", "WindowsServer"),
            source = ServerNameSource()
        };
    }

    public string GetServerName()
    {
        var settings = ServerConfigPath("ServerSettings.ini", allowKnownMissing: true);
        var fromIni = settings is not null && File.Exists(settings)
            ? ReadIniValue(settings, "scum.ServerName")
            : "";
        if (!string.IsNullOrWhiteSpace(fromIni)) return fromIni.Trim();

        var fromLog = ReadServerNameFromLog();
        if (!string.IsNullOrWhiteSpace(fromLog)) return fromLog.Trim();

        return string.IsNullOrWhiteSpace(_options.CurrentValue.ServerName)
            ? "SCUM hosted"
            : _options.CurrentValue.ServerName;
    }

    public object ListConfigs()
    {
        var root = ServerConfigRoot();
        var rows = new List<object>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        if (Directory.Exists(root))
        {
            foreach (var file in Directory.EnumerateFiles(root, "*.*", SearchOption.TopDirectoryOnly)
                         .Where(IsSupportedConfigPath)
                         .OrderBy(Path.GetFileName, StringComparer.OrdinalIgnoreCase))
            {
                rows.Add(FileRow(file, exists: true));
                seen.Add(Path.GetFileName(file));
            }
        }

        foreach (var name in KnownConfigNames)
        {
            if (seen.Contains(name)) continue;
            var path = Path.Combine(root, name);
            rows.Add(FileRow(path, exists: File.Exists(path)));
        }

        return new
        {
            root,
            files = rows,
            message = "Редактор меняет только SCUM/Saved/Config/WindowsServer. Настройки модулей ScumNeDjin не трогаются."
        };
    }

    public object ReadConfig(string? name)
    {
        var clean = CleanConfigName(string.IsNullOrWhiteSpace(name) ? "ServerSettings.ini" : name);
        var path = ServerConfigPath(clean, allowKnownMissing: true)
            ?? throw new InvalidOperationException("Недопустимое имя конфига.");
        if (!File.Exists(path))
        {
            throw new FileNotFoundException("Файл конфига не найден.", clean);
        }

        var info = new FileInfo(path);
        if (info.Length > MaxConfigBytes)
        {
            throw new InvalidOperationException("Файл слишком большой для встроенного редактора.");
        }

        return new
        {
            name = clean,
            content = File.ReadAllText(path),
            sizeBytes = info.Length,
            modifiedUtc = info.LastWriteTimeUtc,
            requiresRestart = true,
            path
        };
    }

    public async Task<object> SaveConfigAsync(ServerConfigSaveRequest request)
    {
        var clean = CleanConfigName(request.Name);
        var path = ServerConfigPath(clean, allowKnownMissing: true)
            ?? throw new InvalidOperationException("Недопустимое имя конфига.");
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);

        var content = request.Content ?? "";
        if (content.Length > MaxConfigBytes)
        {
            throw new InvalidOperationException("Файл слишком большой для сохранения через панель.");
        }
        if (string.IsNullOrWhiteSpace(content))
        {
            throw new InvalidOperationException("Пустой SCUM config нельзя сохранить через панель.");
        }

        if (Path.GetExtension(clean).Equals(".json", StringComparison.OrdinalIgnoreCase))
        {
            JsonDocument.Parse(content);
        }

        string? backupPath = null;
        if (File.Exists(path))
        {
            backupPath = CreateBackupPath(path);
        }

        await WriteConfigAtomicallyAsync(path, content, backupPath);

        return new
        {
            saved = true,
            name = clean,
            backup = backupPath is null ? "" : Path.GetFileName(backupPath),
            requiresRestart = true,
            message = "Конфиг сохранён. Большинство SCUM-настроек вступает в силу после рестарта сервера."
        };
    }

    private static string CreateBackupPath(string path) =>
        path + ".backup." + DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() + "." + Guid.NewGuid().ToString("N") + ".bak";

    private static async Task WriteConfigAtomicallyAsync(string path, string content, string? backupPath)
    {
        var tempPath = path + ".tmp." + Environment.ProcessId + "." + Guid.NewGuid().ToString("N");
        try
        {
            var bytes = Encoding.UTF8.GetBytes(content);
            await using (var stream = new FileStream(
                tempPath,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                bufferSize: 4096,
                FileOptions.WriteThrough))
            {
                await stream.WriteAsync(bytes);
                await stream.FlushAsync();
                stream.Flush(flushToDisk: true);
            }

            if (File.Exists(path))
            {
                if (string.IsNullOrWhiteSpace(backupPath) || File.Exists(backupPath))
                {
                    throw new IOException("Не удалось подготовить уникальную резервную копию конфига.");
                }

                File.Replace(tempPath, path, backupPath, ignoreMetadataErrors: true);
            }
            else
            {
                File.Move(tempPath, path);
            }
        }
        finally
        {
            try
            {
                if (File.Exists(tempPath)) File.Delete(tempPath);
            }
            catch
            {
            }
        }
    }

    private string ServerNameSource()
    {
        var settings = ServerConfigPath("ServerSettings.ini", allowKnownMissing: true);
        if (settings is not null && File.Exists(settings) && !string.IsNullOrWhiteSpace(ReadIniValue(settings, "scum.ServerName")))
        {
            return "ServerSettings.ini";
        }

        return string.IsNullOrWhiteSpace(ReadServerNameFromLog()) ? "WardenOptions" : "SCUM.log";
    }

    private static object FileRow(string path, bool exists)
    {
        var info = exists ? new FileInfo(path) : null;
        return new
        {
            name = Path.GetFileName(path),
            exists,
            sizeBytes = info?.Length ?? 0,
            modifiedUtc = info?.LastWriteTimeUtc,
            type = Path.GetExtension(path).Equals(".json", StringComparison.OrdinalIgnoreCase) ? "json" : "ini",
            requiresRestart = true
        };
    }

    private string ServerConfigRoot()
    {
        var root = ResolveScumRoot();
        return root is null
            ? Path.Combine(AppContext.BaseDirectory, "SCUM", "Saved", "Config", "WindowsServer")
            : Path.Combine(root, "Saved", "Config", "WindowsServer");
    }

    private string? ServerConfigPath(string name, bool allowKnownMissing)
    {
        var clean = CleanConfigName(name);
        if (!allowKnownMissing && !File.Exists(Path.Combine(ServerConfigRoot(), clean))) return null;
        if (!KnownConfigNames.Contains(clean, StringComparer.OrdinalIgnoreCase) &&
            !File.Exists(Path.Combine(ServerConfigRoot(), clean)))
        {
            return null;
        }

        return Path.GetFullPath(Path.Combine(ServerConfigRoot(), clean));
    }

    private static string CleanConfigName(string name)
    {
        var clean = Path.GetFileName((name ?? "").Trim());
        if (!SafeFileName.IsMatch(clean) || !IsSupportedConfigName(clean))
        {
            throw new InvalidOperationException("Недопустимое имя конфига.");
        }

        return clean;
    }

    private static bool IsSupportedConfigPath(string path) => IsSupportedConfigName(Path.GetFileName(path));

    private static bool IsSupportedConfigName(string name)
    {
        var ext = Path.GetExtension(name);
        return ext.Equals(".ini", StringComparison.OrdinalIgnoreCase) ||
               ext.Equals(".json", StringComparison.OrdinalIgnoreCase);
    }

    private string? ResolveScumRoot()
    {
        var options = _options.CurrentValue;
        if (!string.IsNullOrWhiteSpace(options.ServerRoot))
        {
            var serverRoot = Path.GetFullPath(options.ServerRoot);
            foreach (var candidate in new[] { Path.Combine(serverRoot, "SCUM"), serverRoot })
            {
                if (Directory.Exists(Path.Combine(candidate, "Saved")) ||
                    Directory.Exists(Path.Combine(candidate, "Binaries", "Win64")))
                {
                    return candidate;
                }
            }
        }

        if (string.IsNullOrWhiteSpace(options.BridgePath)) return null;
        var bridge = new DirectoryInfo(Path.GetFullPath(options.BridgePath));
        var win64 = string.Equals(bridge.Name, "nedjin_bridge", StringComparison.OrdinalIgnoreCase)
            ? bridge.Parent
            : bridge;
        if (win64 is null) return null;

        return string.Equals(win64.Name, "Win64", StringComparison.OrdinalIgnoreCase)
            ? win64.Parent?.Parent?.FullName
            : null;
    }

    private static string ReadIniValue(string path, string key)
    {
        foreach (var line in File.ReadLines(path))
        {
            var trimmed = line.Trim();
            if (trimmed.Length == 0 || trimmed.StartsWith(';') || trimmed.StartsWith('#')) continue;
            var index = trimmed.IndexOf('=');
            if (index <= 0) continue;
            if (string.Equals(trimmed[..index].Trim(), key, StringComparison.OrdinalIgnoreCase))
            {
                return trimmed[(index + 1)..].Trim();
            }
        }

        return "";
    }

    private string ReadServerNameFromLog()
    {
        var root = ResolveScumRoot();
        if (root is null) return "";
        var logPath = Path.Combine(root, "Saved", "Logs", "SCUM.log");
        if (!File.Exists(logPath)) return "";

        var name = "";
        foreach (var line in Tail(logPath, 1200))
        {
            const string marker = "scum.ServerName:";
            var index = line.IndexOf(marker, StringComparison.OrdinalIgnoreCase);
            if (index < 0) continue;
            var tail = line[(index + marker.Length)..];
            var end = tail.IndexOf("]]", StringComparison.Ordinal);
            name = (end >= 0 ? tail[..end] : tail).Trim();
        }

        return name;
    }

    private static IEnumerable<string> Tail(string path, int maxLines)
    {
        var queue = new Queue<string>();
        foreach (var line in File.ReadLines(path))
        {
            queue.Enqueue(line);
            while (queue.Count > maxLines) queue.Dequeue();
        }

        return queue;
    }
}
