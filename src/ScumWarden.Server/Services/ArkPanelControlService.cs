using System.Net;
using System.Text.RegularExpressions;
using Microsoft.Extensions.Options;
using ScumWarden.Server.Configuration;

namespace ScumWarden.Server.Services;

public sealed class ArkPanelControlService
{
    private static readonly Regex CsrfRegex = new(
        @"name\s*=\s*[""']CSRF_TOKEN[""']\s+content\s*=\s*[""']([^""']+)[""']",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);

    private readonly IOptionsMonitor<WardenOptions> _options;

    public ArkPanelControlService(IOptionsMonitor<WardenOptions> options)
    {
        _options = options;
    }

    public object Status()
    {
        var config = LoadConfig();
        return new
        {
            enabled = config.Enabled,
            configured = config.Configured,
            baseUrl = config.BaseUrl,
            serverId = config.ServerId,
            authMode = config.Cookie.Length > 0 ? "cookie" : "login",
            message = config.Configured
                ? "Команды управления сервером будут отправляться через панель хостинга."
                : "Панель хостинга не настроена в ScumNeDjin/nedjin.ini."
        };
    }

    public async Task<object> RunActionAsync(string? action)
    {
        var clean = (action ?? "").Trim().ToLowerInvariant();
        if (clean is not ("start" or "stop" or "restart"))
        {
            throw new InvalidOperationException("Разрешены только start, stop или restart.");
        }

        var config = LoadConfig();
        if (!config.Configured)
        {
            throw new InvalidOperationException("Панель хостинга не настроена для управления сервером.");
        }

        var cookieJar = new CookieContainer();
        using var handler = new HttpClientHandler
        {
            CookieContainer = cookieJar,
            AutomaticDecompression = DecompressionMethods.GZip | DecompressionMethods.Deflate
        };
        using var http = new HttpClient(handler)
        {
            Timeout = TimeSpan.FromSeconds(75)
        };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("ScumNeDjin/1.0");
        http.DefaultRequestHeaders.TryAddWithoutValidation("X-Requested-With", "XMLHttpRequest");

        if (config.Cookie.Length > 0)
        {
            http.DefaultRequestHeaders.TryAddWithoutValidation("Cookie", config.Cookie);
        }
        else
        {
            var loginPage = await http.GetStringAsync($"{config.BaseUrl}/account/login");
            var csrf = ExtractCsrf(loginPage);
            if (csrf.Length == 0)
            {
                throw new InvalidOperationException("Не удалось получить CSRF страницы авторизации хостинга.");
            }

            using var loginForm = new FormUrlEncodedContent(new Dictionary<string, string>
            {
                ["email"] = config.Email,
                ["password"] = config.Password,
                ["google_code_auth"] = "",
                ["remember_auth_user"] = "remember-me"
            });
            http.DefaultRequestHeaders.Remove("X-CSRF-TOKEN");
            http.DefaultRequestHeaders.TryAddWithoutValidation("X-CSRF-TOKEN", csrf);
            var loginResponse = await http.PostAsync($"{config.BaseUrl}/account/login/ajax", loginForm);
            var loginBody = await loginResponse.Content.ReadAsStringAsync();
            if (!loginResponse.IsSuccessStatusCode || !loginBody.Contains(@"""status"":""success""", StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException("Панель хостинга отклонила авторизацию.");
            }

            var consolePage = await http.GetStringAsync($"{config.BaseUrl}/servers/control/console/{Uri.EscapeDataString(config.ServerId)}/");
            var consoleCsrf = ExtractCsrf(consolePage);
            if (consoleCsrf.Length > 0)
            {
                http.DefaultRequestHeaders.Remove("X-CSRF-TOKEN");
                http.DefaultRequestHeaders.TryAddWithoutValidation("X-CSRF-TOKEN", consoleCsrf);
            }
        }

        var response = await http.GetAsync($"{config.BaseUrl}/servers/control/action/{Uri.EscapeDataString(config.ServerId)}/{clean}");
        if (!response.IsSuccessStatusCode)
        {
            throw new InvalidOperationException($"Панель хостинга вернула HTTP {(int)response.StatusCode}.");
        }

        return new
        {
            ok = true,
            action = clean,
            serverId = config.ServerId,
            statusCode = (int)response.StatusCode,
            message = clean switch
            {
                "start" => "Команда запуска отправлена в панель хостинга.",
                "stop" => "Команда остановки отправлена в панель хостинга. Если панель ScumNeDjin работает внутри SCUMServer.exe, она станет недоступна после остановки.",
                _ => "Команда перезапуска отправлена в панель хостинга."
            }
        };
    }

    private static string ExtractCsrf(string html)
    {
        var match = CsrfRegex.Match(html ?? "");
        return match.Success ? match.Groups[1].Value : "";
    }

    private ArkPanelConfig LoadConfig()
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var path in CandidateIniPaths())
        {
            if (!File.Exists(path)) continue;
            foreach (var line in File.ReadLines(path))
            {
                var trimmed = line.Trim();
                if (trimmed.Length == 0 || trimmed.StartsWith(';') || trimmed.StartsWith('#')) continue;
                var index = trimmed.IndexOf('=');
                if (index <= 0) continue;
                values[trimmed[..index].Trim()] = trimmed[(index + 1)..].Trim();
            }
            break;
        }

        var enabled = Bool(values, "ark_panel_enabled", false);
        var baseUrl = Text(values, "ark_panel_url", "https://panel.ark-hoster.ru").TrimEnd('/');
        var email = Text(values, "ark_panel_email", "");
        var password = Text(values, "ark_panel_password", "");
        var serverId = Text(values, "ark_panel_server_id", "");
        var cookie = Text(values, "ark_panel_cookie", "");
        return new ArkPanelConfig(enabled, baseUrl, email, password, serverId, cookie);
    }

    private IEnumerable<string> CandidateIniPaths()
    {
        var options = _options.CurrentValue;
        if (!string.IsNullOrWhiteSpace(options.ServerRoot))
        {
            yield return Path.Combine(options.ServerRoot, "ScumNeDjin", "nedjin.ini");
            yield return Path.Combine(options.ServerRoot, "SCUM", "Binaries", "Win64", "ScumNeDjin", "nedjin.ini");
        }

        if (!string.IsNullOrWhiteSpace(options.BridgePath))
        {
            yield return Path.Combine(options.BridgePath, "nedjin.ini");
            var parent = Directory.GetParent(options.BridgePath);
            if (parent is not null)
            {
                yield return Path.Combine(parent.FullName, "ScumNeDjin", "nedjin.ini");
            }
        }

        yield return Path.Combine(Directory.GetCurrentDirectory(), "ScumNeDjin", "nedjin.ini");
        yield return Path.Combine(AppContext.BaseDirectory, "ScumNeDjin", "nedjin.ini");
    }

    private static string Text(Dictionary<string, string> values, string key, string fallback) =>
        values.TryGetValue(key, out var value) && !string.IsNullOrWhiteSpace(value) ? value.Trim() : fallback;

    private static bool Bool(Dictionary<string, string> values, string key, bool fallback)
    {
        var value = Text(values, key, fallback ? "true" : "false").ToLowerInvariant();
        return value is "1" or "true" or "yes" or "on";
    }

    private sealed record ArkPanelConfig(
        bool Enabled,
        string BaseUrl,
        string Email,
        string Password,
        string ServerId,
        string Cookie)
    {
        public bool Configured => Enabled &&
                                  BaseUrl.Length > 0 &&
                                  ServerId.Length > 0 &&
                                  ((Email.Length > 0 && Password.Length > 0) || Cookie.Length > 0);
    }
}
