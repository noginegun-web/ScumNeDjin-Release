#include "http_server.h"
#include "bridge_client.h"
#include "config_key.h"
#include "managed_loader.h"
#include "path_utils.h"
#include "sqlite_db.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace nedjin {

namespace {

struct Request {
    std::string method;
    std::string path;
    std::string body;
};

std::atomic_bool g_wargmWorkerStarted{ false };
std::atomic_bool g_wargmSyncBusy{ false };
std::atomic_bool g_wargmDeliveryBusy{ false };
std::atomic_bool g_gameStoresWorkerStarted{ false };
std::atomic_bool g_gameStoresSyncBusy{ false };
std::atomic_bool g_gameStoresDeliveryBusy{ false };
// A pending GameStores order is file-backed and a game grant is not idempotent.
// Serialize the complete sync/deliver/confirm/claim transaction in this process.
// Recursive locking is required because composite operations invoke individual
// helpers on the same worker/request thread.
std::recursive_mutex g_gameStoresOperationMutex;
std::atomic_bool g_killLogWorkerStarted{ false };
std::atomic_bool g_discordLogWorkerStarted{ false };
std::atomic_bool g_scheduledEventsWorkerStarted{ false };
std::atomic_bool g_nativeFileCommandWorkerStarted{ false };
std::atomic_bool g_nativeMoneyFileCommandWorkerStarted{ false };
std::atomic_bool g_httpServerStarted{ false };
std::atomic<int> g_httpClientLimit{ 32 };
std::atomic<int> g_httpActiveClients{ 0 };
std::atomic<uint64_t> g_httpRejectedClients{ 0 };
std::atomic<uint64_t> g_httpThreadStartFailures{ 0 };
std::mutex g_nativeHttpStateMutex;
std::mutex g_arkPanelMutex;
std::mutex g_localRconMutex;
// The payment proof is a read -> live command -> read transaction.  A single
// process-wide lock keeps concurrent panel, shop and in-game money operations
// from invalidating an exact balance-delta observation.
std::mutex g_verifiedMoneyOperationMutex;
std::atomic<uint64_t> g_verifiedMoneyOperationSequence{ 0 };
std::atomic<int> g_localRconRequestId{ 1000 };

struct ArkPanelSession {
    std::string cookies;
    std::string csrf;
    uint64_t refreshedTick = 0;
};

ArkPanelSession g_arkPanelSession;

struct TimedJsonCache {
    std::string body;
    uint64_t tick = 0;
};

std::mutex g_playersCacheMutex;
TimedJsonCache g_playersCache;
std::mutex g_worldPersistenceCacheMutex;
TimedJsonCache g_worldPersistenceCache;
std::mutex g_globalStatsCacheMutex;
TimedJsonCache g_globalStatsCache;

struct HttpClientSlotGuard {
    ~HttpClientSlotGuard() {
        g_httpActiveClients.fetch_sub(1, std::memory_order_release);
    }
};

bool TryAcquireHttpClientSlot() {
    int active = g_httpActiveClients.load(std::memory_order_relaxed);
    const int limit = std::max(1, g_httpClientLimit.load(std::memory_order_relaxed));
    while (active < limit) {
        if (g_httpActiveClients.compare_exchange_weak(
                active,
                active + 1,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    g_httpRejectedClients.fetch_add(1, std::memory_order_relaxed);
    return false;
}

std::vector<std::string> TailLines(const std::wstring& path, int limit, size_t maxBytes);
std::string ToLowerAscii(std::string value);
std::string JsonRootStringField(const std::string& body, const std::string& key);
uintmax_t FileSizeOrZero(const std::wstring& path);
std::string HttpServerStatusJson();

int ConfigPort(const std::wstring& baseDir) {
    const auto config = ReadTextFile(JoinPath(JoinPath(baseDir, L"ScumNeDjin"), L"nedjin.ini"));
    const auto key = std::string("port=");
    auto pos = config.find(key);
    if (pos == std::string::npos) return 7084;
    pos += key.size();
    auto end = config.find_first_of("\r\n", pos);
    const auto raw = config.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    const int port = atoi(raw.c_str());
    return port > 0 ? port : 7084;
}

std::string ConfigApiKey(const std::wstring& baseDir) {
    const auto config = ReadTextFile(JoinPath(JoinPath(baseDir, L"ScumNeDjin"), L"nedjin.ini"));
    const auto key = std::string("api_key=");
    auto pos = config.find(key);
    if (pos == std::string::npos) {
#ifdef SCUM_NEDJIN_PUBLIC_REQUIRE_CONFIGURED_API_KEY
        return {};
#else
        return "12345";
#endif
    }
    pos += key.size();
    auto end = config.find_first_of("\r\n", pos);
    auto value = config.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
#ifdef SCUM_NEDJIN_PUBLIC_REQUIRE_CONFIGURED_API_KEY
    return PublicReleaseApiKeyOrEmpty(std::move(value));
#else
    return value;
#endif
}

std::string ConfigTextValue(const std::wstring& baseDir, const std::string& key, const std::string& fallback = {}) {
    const auto config = ReadTextFile(JoinPath(JoinPath(baseDir, L"ScumNeDjin"), L"nedjin.ini"));
    const auto needle = key + "=";
    auto pos = config.find(needle);
    if (pos == std::string::npos) return fallback;
    pos += needle.size();
    auto end = config.find_first_of("\r\n", pos);
    auto value = config.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value.empty() ? fallback : value;
}

bool ConfigBoolValue(const std::wstring& baseDir, const std::string& key, bool fallback = false) {
    auto value = ConfigTextValue(baseDir, key, fallback ? "true" : "false");
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

int ConfigIntValue(const std::wstring& baseDir, const std::string& key, int fallback, int minValue, int maxValue) {
    const auto value = ConfigTextValue(baseDir, key, std::to_string(fallback));
    const int parsed = std::atoi(value.c_str());
    if (parsed < minValue) return minValue;
    if (parsed > maxValue) return maxValue;
    return parsed;
}

std::vector<int> ConfigHttpPorts(const std::wstring& baseDir) {
    std::vector<int> ports;
    auto addPort = [&ports](int port) {
        if (port < 1 || port > 65535) return;
        if (std::find(ports.begin(), ports.end(), port) == ports.end()) {
            ports.push_back(port);
        }
    };

    addPort(ConfigPort(baseDir));

    auto extra = ConfigTextValue(baseDir, "http_extra_ports", "");
    for (char& ch : extra) {
        if (ch == ';' || ch == '|' || std::isspace(static_cast<unsigned char>(ch))) {
            ch = ',';
        }
    }
    std::stringstream ss(extra);
    std::string token;
    while (std::getline(ss, token, ',')) {
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.erase(token.begin());
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.pop_back();
        if (!token.empty()) {
            addPort(std::atoi(token.c_str()));
        }
    }

    if (ports.empty()) ports.push_back(7084);
    return ports;
}

std::string HeaderValue(const std::string& request, const std::string& key) {
    std::string lower = request;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string needle = key;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    needle += ":";
    auto pos = lower.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    auto end = request.find("\r\n", pos);
    auto value = request.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    return value;
}

Request ParseRequest(const std::string& raw) {
    Request req;
    const auto lineEnd = raw.find("\r\n");
    const auto firstLine = raw.substr(0, lineEnd);
    std::istringstream line(firstLine);
    line >> req.method >> req.path;
    const auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
        req.body = raw.substr(headerEnd + 4);
    }
    return req;
}

std::string ApiPath(const std::string& path) {
    const auto q = path.find('?');
    return q == std::string::npos ? path : path.substr(0, q);
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

std::string UrlDecode(std::string value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out.push_back(' ');
            continue;
        }
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = HexValue(value[i + 1]);
            const int lo = HexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

std::string UrlEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[(ch >> 4) & 0x0F]);
            out.push_back(kHex[ch & 0x0F]);
        }
    }
    return out;
}

std::string QueryValue(const std::string& path, const std::string& key) {
    const auto q = path.find('?');
    if (q == std::string::npos) return {};
    size_t pos = q + 1;
    while (pos < path.size()) {
        const auto amp = path.find('&', pos);
        const auto part = path.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const auto eq = part.find('=');
        const auto name = eq == std::string::npos ? part : part.substr(0, eq);
        if (name == key) {
            return UrlDecode(eq == std::string::npos ? "" : part.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

std::string FormValue(const std::string& body, const std::string& key) {
    size_t pos = 0;
    while (pos < body.size()) {
        const auto amp = body.find('&', pos);
        const auto part = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const auto eq = part.find('=');
        const auto name = UrlDecode(eq == std::string::npos ? part : part.substr(0, eq));
        if (name == key) {
            return UrlDecode(eq == std::string::npos ? "" : part.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

std::string MimeType(const std::wstring& path) {
    const auto ext = fs::path(path).extension().wstring();
    if (ext == L".html") return "text/html; charset=utf-8";
    if (ext == L".css") return "text/css; charset=utf-8";
    if (ext == L".js") return "application/javascript; charset=utf-8";
    if (ext == L".json") return "application/json; charset=utf-8";
    if (ext == L".png") return "image/png";
    if (ext == L".jpg" || ext == L".jpeg") return "image/jpeg";
    return "application/octet-stream";
}

std::string Envelope(bool ok, const std::string& dataOrError, bool rawData = true) {
    if (ok) {
        return std::string("{\"ok\":true,\"data\":") + (rawData ? dataOrError : ("\"" + JsonEscape(dataOrError) + "\"")) + ",\"error\":null}";
    }
    return std::string("{\"ok\":false,\"data\":null,\"error\":\"") + JsonEscape(dataOrError) + "\"}";
}

std::string EnvelopeFailureWithJsonData(const std::string& error, const std::string& dataJson) {
    const auto first = dataJson.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && (dataJson[first] == '{' || dataJson[first] == '[')) {
        return std::string("{\"ok\":false,\"data\":") + dataJson.substr(first) + ",\"error\":\"" + JsonEscape(error) + "\"}";
    }
    return Envelope(false, error);
}

std::string EmptyArray() {
    return "[]";
}

std::string TrimAscii(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string JsonValueOrString(std::string text) {
    text = TrimAscii(std::move(text));
    if (text.empty()) return "null";
    const char first = text.front();
    if (first == '{' || first == '[' || first == '"' || first == '-' || (first >= '0' && first <= '9')) {
        return text;
    }
    if (text == "true" || text == "false" || text == "null") return text;
    return std::string("\"") + JsonEscape(text) + "\"";
}

int QueryInt(const std::string& path, const std::string& key, int fallback, int minValue, int maxValue) {
    const auto raw = QueryValue(path, key);
    if (raw.empty()) return fallback;
    const int value = std::atoi(raw.c_str());
    return std::clamp(value, minValue, maxValue);
}

std::wstring ScumRoot(const std::wstring& baseDir) {
    return fs::path(baseDir).parent_path().parent_path().wstring();
}

std::wstring SavedLogPath(const std::wstring& baseDir) {
    return (fs::path(ScumRoot(baseDir)) / L"Saved" / L"Logs" / L"SCUM.log").wstring();
}

std::wstring SavedDbPath(const std::wstring& baseDir) {
    return (fs::path(ScumRoot(baseDir)) / L"Saved" / L"SaveFiles" / L"SCUM.db").wstring();
}

std::wstring SaveFilesLogsPath(const std::wstring& baseDir) {
    return (fs::path(ScumRoot(baseDir)) / L"Saved" / L"SaveFiles" / L"Logs").wstring();
}

std::wstring ServerConfigRootPath(const std::wstring& baseDir) {
    return (fs::path(ScumRoot(baseDir)) / L"Saved" / L"Config" / L"WindowsServer").wstring();
}

std::vector<std::string> KnownServerConfigNames() {
    return {
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
    };
}

bool IsSupportedServerConfigName(const std::string& name) {
    if (name.empty() || name.size() > 96) return false;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name.find(':') != std::string::npos) return false;
    for (const char ch : name) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.';
        if (!ok) return false;
    }
    const auto lower = ToLowerAscii(name);
    return lower.size() > 4 &&
        (lower.rfind(".ini") == lower.size() - 4 || lower.rfind(".json") == lower.size() - 5);
}

bool IsKnownServerConfigName(const std::string& name) {
    for (const auto& known : KnownServerConfigNames()) {
        if (ToLowerAscii(known) == ToLowerAscii(name)) return true;
    }
    return false;
}

std::string CleanServerConfigName(std::string name) {
    name = TrimAscii(std::move(name));
    std::replace(name.begin(), name.end(), '\\', '/');
    const auto slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    return IsSupportedServerConfigName(name) ? name : "";
}

std::wstring ServerConfigPath(const std::wstring& baseDir, const std::string& name) {
    const auto clean = CleanServerConfigName(name);
    if (clean.empty()) return {};
    return (fs::path(ServerConfigRootPath(baseDir)) / Utf8ToWide(clean)).wstring();
}

std::string IniValueFromText(const std::string& text, const std::string& key) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        line = TrimAscii(line);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        if (ToLowerAscii(TrimAscii(line.substr(0, eq))) == ToLowerAscii(key)) {
            return TrimAscii(line.substr(eq + 1));
        }
    }
    return {};
}

std::string ServerNameFromLog(const std::wstring& baseDir) {
    std::string name;
    for (const auto& line : TailLines(SavedLogPath(baseDir), 1200, 2 * 1024 * 1024)) {
        const auto lower = ToLowerAscii(line);
        const std::string marker = "scum.servername:";
        const auto pos = lower.find(marker);
        if (pos == std::string::npos) continue;
        auto value = line.substr(pos + marker.size());
        const auto end = value.find("]]");
        if (end != std::string::npos) value = value.substr(0, end);
        value = TrimAscii(value);
        if (!value.empty()) name = value;
    }
    return name;
}

std::string ServerDisplayName(const std::wstring& baseDir) {
    const auto settingsPath = ServerConfigPath(baseDir, "ServerSettings.ini");
    if (!settingsPath.empty() && FileExists(settingsPath)) {
        auto value = IniValueFromText(ReadTextFile(settingsPath), "scum.ServerName");
        if (!value.empty()) return value;
    }
    auto fromLog = ServerNameFromLog(baseDir);
    if (!fromLog.empty()) return fromLog;
    auto configured = ConfigTextValue(baseDir, "server_name", "");
    if (!configured.empty()) return configured;
    return "SCUM hosted";
}

std::string ServerConfigFileRowJson(const std::wstring& path, const std::string& name, bool exists) {
    std::ostringstream ss;
    const auto lower = ToLowerAscii(name);
    ss << "{\"name\":\"" << JsonEscape(name)
       << "\",\"exists\":" << (exists ? "true" : "false")
       << ",\"sizeBytes\":" << (exists ? FileSizeOrZero(path) : 0)
       << ",\"type\":\"" << (lower.rfind(".json") == lower.size() - 5 ? "json" : "ini") << "\""
       << ",\"requiresRestart\":true}";
    return ss.str();
}

std::string ServerConfigsListJson(const std::wstring& baseDir) {
    const auto root = ServerConfigRootPath(baseDir);
    std::vector<std::string> rows;
    std::vector<std::string> seen;
    try {
        if (fs::exists(root)) {
            for (const auto& entry : fs::directory_iterator(root)) {
                if (!entry.is_regular_file()) continue;
                const auto name = WideToUtf8(entry.path().filename().wstring());
                if (!IsSupportedServerConfigName(name)) continue;
                rows.push_back(ServerConfigFileRowJson(entry.path().wstring(), name, true));
                seen.push_back(ToLowerAscii(name));
            }
        }
    } catch (...) {
    }
    for (const auto& name : KnownServerConfigNames()) {
        if (std::find(seen.begin(), seen.end(), ToLowerAscii(name)) != seen.end()) continue;
        const auto path = ServerConfigPath(baseDir, name);
        rows.push_back(ServerConfigFileRowJson(path, name, FileExists(path)));
    }
    return std::string("{\"root\":\"") + JsonEscape(WideToUtf8(root)) +
        "\",\"message\":\"Редактор меняет только SCUM/Saved/Config/WindowsServer. Настройки модулей ScumNeDjin не трогаются.\",\"files\":[" +
        [&]() {
            std::ostringstream joined;
            for (size_t i = 0; i < rows.size(); ++i) {
                if (i > 0) joined << ",";
                joined << rows[i];
            }
            return joined.str();
        }() + "]}";
}

std::string ServerConfigReadEnvelope(const std::wstring& baseDir, const Request& req) {
    auto name = CleanServerConfigName(QueryValue(req.path, "name"));
    if (name.empty()) name = "ServerSettings.ini";
    const auto path = ServerConfigPath(baseDir, name);
    if (path.empty() || (!FileExists(path) && !IsKnownServerConfigName(name))) {
        return Envelope(false, "Недопустимый файл конфига.");
    }
    if (!FileExists(path)) return Envelope(false, "Файл конфига не найден.");
    const auto size = FileSizeOrZero(path);
    if (size > 2ULL * 1024ULL * 1024ULL) return Envelope(false, "Файл слишком большой для встроенного редактора.");
    std::string content;
    if (!ReadTextFileExact(path, content)) return Envelope(false, "Не удалось прочитать файл конфига.");
    return Envelope(true, std::string("{\"name\":\"") + JsonEscape(name) +
        "\",\"content\":\"" + JsonEscape(content) +
        "\",\"sizeBytes\":" + std::to_string(size) +
        ",\"requiresRestart\":true,\"path\":\"" + JsonEscape(WideToUtf8(path)) + "\"}");
}

std::wstring UniqueConfigBackupPath(const std::wstring& path) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto candidate = path + L".backup." + std::to_wstring(GetTickCount64()) +
            L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(attempt) + L".bak";
        if (!FileExists(candidate)) return candidate;
    }
    return {};
}

std::wstring UniqueServerConfigBackupPath(const std::wstring& path) {
    return UniqueConfigBackupPath(path);
}

std::string ServerConfigSaveEnvelope(const std::wstring& baseDir, const Request& req) {
    const auto name = CleanServerConfigName(JsonRootStringField(req.body, "name"));
    const auto content = JsonRootStringField(req.body, "content");
    if (name.empty()) return Envelope(false, "Недопустимое имя конфига.");
    const auto path = ServerConfigPath(baseDir, name);
    if (path.empty() || (!FileExists(path) && !IsKnownServerConfigName(name))) {
        return Envelope(false, "Разрешены только существующие или известные SCUM config файлы.");
    }
    if (content.size() > 2ULL * 1024ULL * 1024ULL) return Envelope(false, "Файл слишком большой для сохранения через панель.");
    if (TrimAscii(content).empty()) return Envelope(false, "Пустой SCUM config нельзя сохранить через панель.");
    const auto lower = ToLowerAscii(name);
    if (lower.rfind(".json") == lower.size() - 5) {
        const auto trimmed = TrimAscii(content);
        if (!trimmed.empty() && trimmed.front() != '{' && trimmed.front() != '[') {
            return Envelope(false, "JSON config должен начинаться с { или [.");
        }
    }
    std::string backupName;
    std::wstring backupPath;
    if (FileExists(path)) {
        backupPath = UniqueServerConfigBackupPath(path);
        if (backupPath.empty()) return Envelope(false, "Не удалось подготовить уникальную резервную копию конфига.");
        backupName = WideToUtf8(fs::path(backupPath).filename().wstring());
    }
    if (!WriteTextFileAtomicallyExact(path, content, backupPath)) return Envelope(false, "Не удалось атомарно сохранить файл конфига.");
    return Envelope(true, std::string("{\"saved\":true,\"name\":\"") + JsonEscape(name) +
        "\",\"backup\":\"" + JsonEscape(backupName) +
        "\",\"requiresRestart\":true,\"message\":\"Конфиг сохранён. Большинство SCUM-настроек вступает в силу после рестарта сервера.\"}");
}

std::wstring ProjectRuntimeRoot(const std::wstring& baseDir) {
    return (fs::path(ScumRoot(baseDir)) / L"Saved" / L"ScumNeDjin").wstring();
}

std::wstring ProjectLegacyRoot(const std::wstring& baseDir) {
    return JoinPath(baseDir, L"ScumNeDjin");
}

std::wstring ProjectRuntimeLogPath(const std::wstring& baseDir) {
    return (fs::path(ProjectRuntimeRoot(baseDir)) / L"logs" / L"nedjin.log").wstring();
}

std::wstring NativeHttpStatePath(const std::wstring& baseDir) {
    return (fs::path(ProjectRuntimeRoot(baseDir)) / L"state" / L"native_http.json").wstring();
}

std::wstring NativeHttpStatePathForPort(const std::wstring& baseDir, int port) {
    std::wstringstream name;
    name << L"native_http_" << port << L".json";
    return (fs::path(ProjectRuntimeRoot(baseDir)) / L"state" / name.str()).wstring();
}

std::wstring NativeHttpLegacyStatePath(const std::wstring& baseDir) {
    return JoinPath(JoinPath(baseDir, L"nedjin_bridge"), L"native_http.json");
}

std::wstring NativeHttpLegacyStatePathForPort(const std::wstring& baseDir, int port) {
    std::wstringstream name;
    name << L"native_http_" << port << L".json";
    return JoinPath(JoinPath(baseDir, L"nedjin_bridge"), name.str());
}

void WriteNativeHttpState(
    const std::wstring& baseDir,
    const std::string& status,
    int port,
    const std::string& message = {}) {
    std::lock_guard<std::mutex> lock(g_nativeHttpStateMutex);
    std::ostringstream ss;
    ss << "{\"utc\":\"" << UtcIsoNow()
       << "\",\"status\":\"" << JsonEscape(status)
       << "\",\"port\":" << port;
    if (!message.empty()) {
        ss << ",\"message\":\"" << JsonEscape(message) << "\"";
    }
    ss << "}";
    const auto body = ss.str();
    WriteTextFile(NativeHttpStatePath(baseDir), body);
    WriteTextFile(NativeHttpLegacyStatePath(baseDir), body);
    if (port > 0) {
        WriteTextFile(NativeHttpStatePathForPort(baseDir, port), body);
        WriteTextFile(NativeHttpLegacyStatePathForPort(baseDir, port), body);
    }
}

std::wstring ProjectLegacyRuntimeLogPath(const std::wstring& baseDir) {
    return JoinPath(JoinPath(baseDir, L"nedjin_bridge"), L"nedjin.log");
}

std::wstring ProjectReadableRuntimeLogPath(const std::wstring& baseDir) {
    const auto primary = ProjectRuntimeLogPath(baseDir);
    return FileExists(primary) ? primary : ProjectLegacyRuntimeLogPath(baseDir);
}

uintmax_t FileSizeOrZero(const std::wstring& path) {
    std::error_code ec;
    const auto size = fs::file_size(fs::path(path), ec);
    return ec ? 0 : size;
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsDigitsText(const std::string& value) {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string SqlTextLiteral(std::string value) {
    value = TrimAscii(value);
    if (value.size() > 128) value.resize(128);
    std::string out = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            out += "''";
        } else if (static_cast<unsigned char>(ch) >= 32) {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

std::string PlayerSqlFilter(const std::string& steamId, const std::string& name) {
    const auto cleanSteam = TrimAscii(steamId);
    if (IsDigitsText(cleanSteam) && cleanSteam.size() >= 16 && cleanSteam.size() <= 20) {
        return " AND up.user_id = " + SqlTextLiteral(cleanSteam) + " ";
    }

    const auto cleanName = TrimAscii(name);
    if (!cleanName.empty()) {
        return " AND lower(up.name) = lower(" + SqlTextLiteral(cleanName) + ") ";
    }

    return "";
}

std::string StateStorageKey(std::string key) {
    key = SafeConfigKey(key);
    if (key == "vehicle-rental" || key == "vehicle_rental") return "vehicle-rentals";
    if (key == "welcome-pack" || key == "welcome_pack") return "welcome-pack-claims";
    if (key == "home-system" || key == "home_system") return "homes";
    if (key == "fast-travel" || key == "fast_travel") return "fast-travel-trips";
    return key;
}

bool ContainsCi(const std::string& haystack, const std::string& needle) {
    return ToLowerAscii(haystack).find(ToLowerAscii(needle)) != std::string::npos;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        const auto end = text.find('\n', pos);
        auto line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return lines;
}

std::vector<std::string> TailLines(const std::wstring& path, int limit, size_t maxBytes = 2 * 1024 * 1024) {
    limit = std::clamp(limit, 1, 20000);
    std::ifstream file(fs::path(path), std::ios::binary | std::ios::ate);
    if (!file) return {};

    const auto endPos = file.tellg();
    if (endPos <= 0) return {};
    const auto fileSize = static_cast<size_t>(endPos);
    const auto readSize = std::min(fileSize, maxBytes);
    const auto startPos = static_cast<std::streamoff>(fileSize - readSize);
    file.seekg(startPos, std::ios::beg);

    std::string buffer(readSize, '\0');
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<size_t>(file.gcount()));
    if (startPos > 0) {
        const auto firstLine = buffer.find('\n');
        if (firstLine != std::string::npos) {
            buffer.erase(0, firstLine + 1);
        }
    }

    auto lines = SplitLines(buffer);
    if (static_cast<int>(lines.size()) > limit) {
        lines.erase(lines.begin(), lines.end() - limit);
    }
    return lines;
}

std::string ReadTailBytes(const std::wstring& path, size_t maxBytes, bool* truncated = nullptr) {
    if (truncated) *truncated = false;
    std::ifstream file(fs::path(path), std::ios::binary | std::ios::ate);
    if (!file) return {};

    const auto endPos = file.tellg();
    if (endPos <= 0) return {};
    const auto fileSize = static_cast<size_t>(endPos);
    const auto readSize = std::min(fileSize, std::max<size_t>(1, maxBytes));
    const auto startPos = static_cast<std::streamoff>(fileSize - readSize);
    if (truncated) *truncated = startPos > 0;
    file.seekg(startPos, std::ios::beg);

    std::string buffer(readSize, '\0');
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<size_t>(file.gcount()));
    return buffer;
}

std::string LinesJson(const std::vector<std::string>& lines) {
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) ss << ",";
        ss << "\"" << JsonEscape(lines[i]) << "\"";
    }
    ss << "]";
    return ss.str();
}

std::string TimestampFromLogLine(const std::string& line) {
    if (line.size() < 20 || line.front() != '[') return {};
    if (line[5] != '.' || line[8] != '.' || line[11] != '-' || line[14] != '.' || line[17] != '.') return {};
    return line.substr(1, 4) + "-" + line.substr(6, 2) + "-" + line.substr(9, 2) +
        "T" + line.substr(12, 2) + ":" + line.substr(15, 2) + ":" + line.substr(18, 2) + "Z";
}

std::string HumanLogMessage(const std::string& line) {
    const auto marker = line.find("]Log");
    if (marker == std::string::npos) return line;
    const auto payload = line.find(':', marker + 1);
    if (payload == std::string::npos || payload + 1 >= line.size()) return line.substr(marker + 1);
    auto out = line.substr(payload + 1);
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front()))) out.erase(out.begin());
    return out.empty() ? line : out;
}

std::string BuildLatestGlobalStatsJson(const std::wstring& baseDir) {
    const auto lines = TailLines(SavedLogPath(baseDir), 1200, 1024 * 1024);
    static const std::regex framePattern(
        R"(Global Stats:\s*([0-9.]+)ms\s*\(\s*([0-9.]+)FPS\),\s*([0-9.]+)ms\s*\(\s*([0-9.]+)FPS\),\s*([0-9.]+)ms\s*\(\s*([0-9.]+)FPS\))",
        std::regex_constants::icase);
    static const std::regex counterPattern(R"(([A-Z][A-Z]?)\s*:\s*(-?\d+))");

    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (!ContainsCi(*it, "Global Stats")) continue;

        std::smatch frameMatch;
        const bool hasFrame = std::regex_search(*it, frameMatch, framePattern);
        std::ostringstream counters;
        counters << "{";
        bool firstCounter = true;
        for (std::sregex_iterator counterIt(it->begin(), it->end(), counterPattern), end; counterIt != end; ++counterIt) {
            if (!firstCounter) counters << ",";
            firstCounter = false;
            counters << "\"" << JsonEscape((*counterIt)[1].str()) << "\":" << (*counterIt)[2].str();
        }
        counters << "}";

        std::ostringstream ss;
        ss << "{\"online\":true"
           << ",\"source\":\"SCUM.log Global Stats\""
           << ",\"timestampUtc\":\"" << JsonEscape(TimestampFromLogLine(*it)) << "\""
           << ",\"note\":\"Global Stats показывает время кадра сервера/FPS, это не ping игрока.\"";
        if (hasFrame) {
            ss << ",\"sample1\":{\"frameMs\":" << frameMatch[1].str() << ",\"fps\":" << frameMatch[2].str() << "}"
               << ",\"sample2\":{\"frameMs\":" << frameMatch[3].str() << ",\"fps\":" << frameMatch[4].str() << "}"
               << ",\"sample3\":{\"frameMs\":" << frameMatch[5].str() << ",\"fps\":" << frameMatch[6].str() << "}"
               << ",\"frameMs\":" << frameMatch[1].str()
               << ",\"fps\":" << frameMatch[2].str()
               << ",\"maxFrameMs\":" << frameMatch[5].str();
        }
        ss << ",\"counters\":" << counters.str()
           << ",\"line\":\"" << JsonEscape(*it) << "\"}";
        return ss.str();
    }

    return "{\"online\":false,\"source\":\"SCUM.log Global Stats\",\"note\":\"В хвосте SCUM.log пока нет строки Global Stats.\"}";
}

std::string LatestGlobalStatsJson(const std::wstring& baseDir) {
    // TailLines can read up to 1 MiB. /api/status and the performance widget
    // may request it together, so reuse a very short live-safe snapshot.
    const auto now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_globalStatsCacheMutex);
    if (!g_globalStatsCache.body.empty() && now - g_globalStatsCache.tick < 1000) {
        return g_globalStatsCache.body;
    }
    g_globalStatsCache.body = BuildLatestGlobalStatsJson(baseDir);
    g_globalStatsCache.tick = GetTickCount64();
    return g_globalStatsCache.body;
}

bool IsNoiseForEvents(const std::string& line) {
    return ContainsCi(line, "Setting CVar") ||
        ContainsCi(line, "ULevelStreaming") ||
        ContainsCi(line, "RequestLevel") ||
        ContainsCi(line, "CleanupWorld") ||
        ContainsCi(line, "Global Stats");
}

bool MatchesAny(const std::string& line, const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        if (ContainsCi(line, pattern)) return true;
    }
    return false;
}

std::string EventsFromLogJson(const std::wstring& baseDir, const std::string& type, const std::vector<std::string>& patterns) {
    std::vector<std::string> selected;
    const auto lines = TailLines(SavedLogPath(baseDir), 5000, 4 * 1024 * 1024);
    for (const auto& line : lines) {
        if (IsNoiseForEvents(line)) continue;
        if (!MatchesAny(line, patterns)) continue;

        std::ostringstream item;
        item << "{\"type\":\"" << JsonEscape(type)
             << "\",\"timestampUtc\":\"" << JsonEscape(TimestampFromLogLine(line))
             << "\",\"message\":\"" << JsonEscape(HumanLogMessage(line))
             << "\",\"raw\":\"" << JsonEscape(line) << "\"}";
        selected.push_back(item.str());
        if (selected.size() > 200) {
            selected.erase(selected.begin());
        }
    }

    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < selected.size(); ++i) {
        if (i) ss << ",";
        ss << selected[i];
    }
    ss << "]";
    return ss.str();
}

struct LogPlayerSession {
    std::string steamId;
    std::string name;
    std::string profileId;
    std::string loginUtc;
    std::string joinedUtc;
    std::string leftUtc;
    std::string lastSeenUtc;
    std::string lastLine;
    bool online = false;
};

struct CachedLogPlayerSnapshot {
    std::vector<LogPlayerSession> sessions;
    int playerCount = -1;
    uint64_t tick = 0;
};

std::mutex g_logPlayerSnapshotMutex;
CachedLogPlayerSnapshot g_logPlayerSnapshot;

CachedLogPlayerSnapshot ReadLogPlayerSnapshot(const std::wstring& baseDir, bool forceRefresh = false);

std::string ExtractAfterUntil(const std::string& text, const std::string& needle, const std::vector<std::string>& terminators) {
    auto pos = text.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    auto end = text.size();
    for (const auto& terminator : terminators) {
        const auto marker = text.find(terminator, pos);
        if (marker != std::string::npos && marker < end) {
            end = marker;
        }
    }
    return TrimAscii(UrlDecode(text.substr(pos, end - pos)));
}

LogPlayerSession* FindSession(
    std::vector<LogPlayerSession>& sessions,
    const std::string& steamId,
    const std::string& name,
    const std::string& profileId = {}) {
    if (!profileId.empty()) {
        for (auto& session : sessions) {
            if (session.profileId == profileId) return &session;
        }
    }

    if (!steamId.empty()) {
        for (auto& session : sessions) {
            if (session.steamId == steamId) return &session;
        }
    }

    const auto wantedName = ToLowerAscii(TrimAscii(name));
    if (!wantedName.empty()) {
        for (auto& session : sessions) {
            if (ToLowerAscii(session.name) == wantedName) return &session;
        }
    }

    return nullptr;
}

bool ParseLoginRequest(const std::string& line, std::string& steamId, std::string& name) {
    if (!ContainsCi(line, "Login request:")) return false;
    steamId = ExtractAfterUntil(line, "?user=", { "?", " " });
    name = ExtractAfterUntil(line, "?Name=", { " userId:", " platform:", "?platform:", "?" });
    return !steamId.empty() || !name.empty();
}

bool ParsePrisonerPossessed(const std::string& line, std::string& steamId, std::string& profileId, std::string& name) {
    static const std::regex pattern(R"(APrisoner::HandlePossessedBy:\s*(\d+),\s*(\d+),\s*([^\s]+))", std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(line, match, pattern)) return false;
    steamId = match[1].str();
    profileId = match[2].str();
    name = match[3].str();
    return !steamId.empty() || !profileId.empty() || !name.empty();
}

bool ParseScumLoggedIn(const std::string& line, std::string& steamId, std::string& profileId, std::string& name) {
    static const std::regex pattern(R"(LogSCUM:\s*'[^']*\s+(\d+):([^\(]+)\((\d+)\)'\s+logged in)", std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(line, match, pattern)) return false;
    steamId = TrimAscii(match[1].str());
    name = TrimAscii(match[2].str());
    profileId = TrimAscii(match[3].str());
    return !steamId.empty() || !profileId.empty() || !name.empty();
}

bool ParseJoinSucceeded(const std::string& line, std::string& name) {
    name = ExtractAfterUntil(line, "Join succeeded:", { "\r", "\n" });
    return !name.empty();
}

bool IsDisconnectLine(const std::string& line) {
    return ContainsCi(line, "Logout") ||
        ContainsCi(line, "logged out") ||
        ContainsCi(line, "Prisoner logging out") ||
        ContainsCi(line, "failed to respond to heartbeats") ||
        ContainsCi(line, "UNetConnection::Close") ||
        ContainsCi(line, "Connection timed out") ||
        ContainsCi(line, "Removed client connection") ||
        ContainsCi(line, "Destroying client connection") ||
        ContainsCi(line, "NotifyDisconnection");
}

int LatestServerLogPlayerCount(const std::wstring& baseDir) {
    return ReadLogPlayerSnapshot(baseDir).playerCount;
}

bool LineMentionsSession(const std::string& line, const LogPlayerSession& session) {
    return (!session.steamId.empty() && line.find(session.steamId) != std::string::npos) ||
        (!session.name.empty() && ContainsCi(line, session.name));
}

CachedLogPlayerSnapshot ReadLogPlayerSnapshot(const std::wstring& baseDir, bool forceRefresh) {
    constexpr uint64_t kLogPlayerSnapshotTtlMs = 5000;
    const auto nowTick = GetTickCount64();
    if (!forceRefresh) {
        std::lock_guard<std::mutex> lock(g_logPlayerSnapshotMutex);
        if (nowTick >= g_logPlayerSnapshot.tick &&
            nowTick - g_logPlayerSnapshot.tick < kLogPlayerSnapshotTtlMs &&
            (g_logPlayerSnapshot.playerCount >= 0 || !g_logPlayerSnapshot.sessions.empty())) {
            return g_logPlayerSnapshot;
        }
    }

    CachedLogPlayerSnapshot snapshot;
    snapshot.tick = nowTick;
    const auto lines = TailLines(SavedLogPath(baseDir), 3500, 4 * 1024 * 1024);
    static const std::regex pattern(R"(Global Stats:.*\|\s*C:\s*\d+\s*\(\s*\d+\s*\),\s*P:\s*(\d+))", std::regex_constants::icase);
    snapshot.playerCount = -1;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        std::smatch match;
        if (!std::regex_search(*it, match, pattern)) continue;
        const int count = std::atoi(match[1].str().c_str());
        if (count != 0) {
            snapshot.playerCount = count;
            break;
        }

        const auto statsIndex = static_cast<size_t>(std::distance(lines.begin(), it.base()) - 1);
        bool sawJoinSignal = false;
        for (size_t i = statsIndex + 1; i < lines.size(); ++i) {
            const auto& line = lines[i];
            std::string steamId;
            std::string profileId;
            std::string name;
            if (ParsePrisonerPossessed(line, steamId, profileId, name) ||
                ParseScumLoggedIn(line, steamId, profileId, name) ||
                ParseLoginRequest(line, steamId, name) ||
                ParseJoinSucceeded(line, name)) {
                sawJoinSignal = true;
                break;
            }
        }
        snapshot.playerCount = sawJoinSignal ? -1 : 0;
        break;
    }

    for (const auto& line : lines) {
        const auto timestamp = TimestampFromLogLine(line);

        std::string steamId;
        std::string name;
        std::string profileId;
        if (ParsePrisonerPossessed(line, steamId, profileId, name) ||
            ParseScumLoggedIn(line, steamId, profileId, name)) {
            auto* session = FindSession(snapshot.sessions, steamId, name, profileId);
            if (!session) {
                snapshot.sessions.push_back(LogPlayerSession{});
                session = &snapshot.sessions.back();
            }
            if (!steamId.empty()) session->steamId = steamId;
            if (!name.empty()) session->name = name;
            if (!profileId.empty()) session->profileId = profileId;
            session->online = true;
            if (session->joinedUtc.empty()) session->joinedUtc = timestamp;
            session->lastSeenUtc = timestamp;
            session->lastLine = line;
            continue;
        }

        if (ParseLoginRequest(line, steamId, name)) {
            auto* session = FindSession(snapshot.sessions, steamId, name);
            if (!session) {
                snapshot.sessions.push_back(LogPlayerSession{});
                session = &snapshot.sessions.back();
            }
            if (!steamId.empty()) session->steamId = steamId;
            if (!name.empty()) session->name = name;
            session->loginUtc = timestamp;
            session->lastSeenUtc = timestamp;
            session->lastLine = line;
            continue;
        }

        if (ParseJoinSucceeded(line, name)) {
            auto* session = FindSession(snapshot.sessions, {}, name);
            if (!session) {
                snapshot.sessions.push_back(LogPlayerSession{});
                session = &snapshot.sessions.back();
            }
            if (!name.empty()) session->name = name;
            session->online = true;
            session->joinedUtc = timestamp;
            session->leftUtc.clear();
            session->lastSeenUtc = timestamp;
            session->lastLine = line;
            continue;
        }

        if (IsDisconnectLine(line)) {
            for (auto& session : snapshot.sessions) {
                if (LineMentionsSession(line, session)) {
                    session.online = false;
                    session.leftUtc = timestamp;
                    session.lastSeenUtc = timestamp;
                    session.lastLine = line;
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_logPlayerSnapshotMutex);
        g_logPlayerSnapshot = snapshot;
    }
    return snapshot;
}

std::vector<LogPlayerSession> InferPlayerSessionsFromLog(const std::wstring& baseDir) {
    return ReadLogPlayerSnapshot(baseDir).sessions;
}

std::string PlayerSessionJson(const LogPlayerSession& session) {
    std::ostringstream ss;
    ss << "{\"name\":\"" << JsonEscape(session.name)
       << "\",\"steamId\":\"" << JsonEscape(session.steamId)
       << "\",\"profileId\":\"" << JsonEscape(session.profileId)
       << "\",\"online\":" << (session.online ? "true" : "false")
       << ",\"status\":\"" << (session.online ? "online" : "offline")
       << "\",\"source\":\"server-log\""
       << ",\"loginUtc\":\"" << JsonEscape(session.loginUtc)
       << "\",\"joinedUtc\":\"" << JsonEscape(session.joinedUtc)
       << "\",\"leftUtc\":\"" << JsonEscape(session.leftUtc)
       << "\",\"lastSeenUtc\":\"" << JsonEscape(session.lastSeenUtc)
       << "\",\"lastLine\":\"" << JsonEscape(session.lastLine) << "\"}";
    return ss.str();
}

std::string PlayerArrayJson(const std::vector<LogPlayerSession>& sessions, bool onlineOnly) {
    std::ostringstream ss;
    bool first = true;
    ss << "[";
    for (const auto& session : sessions) {
        if (onlineOnly && !session.online) continue;
        if (session.name.empty() && session.steamId.empty()) continue;
        if (!first) ss << ",";
        first = false;
        ss << PlayerSessionJson(session);
    }
    ss << "]";
    return ss.str();
}

int OnlineSessionCount(const std::vector<LogPlayerSession>& sessions) {
    int count = 0;
    for (const auto& session : sessions) {
        if (session.online && (!session.name.empty() || !session.steamId.empty())) ++count;
    }
    return count;
}

std::vector<LogPlayerSession> OnlineSessions(const std::vector<LogPlayerSession>& sessions) {
    std::vector<LogPlayerSession> online;
    for (const auto& session : sessions) {
        if (session.online && (!session.name.empty() || !session.steamId.empty())) {
            online.push_back(session);
        }
    }
    return online;
}

std::string SessionSortStamp(const LogPlayerSession& session) {
    if (!session.lastSeenUtc.empty()) return session.lastSeenUtc;
    if (!session.joinedUtc.empty()) return session.joinedUtc;
    return session.loginUtc;
}

std::vector<LogPlayerSession> TrimSessionsToLogCount(std::vector<LogPlayerSession> sessions, int logPlayerCount) {
    if (logPlayerCount < 0 || sessions.size() <= static_cast<size_t>(logPlayerCount)) return sessions;
    std::stable_sort(sessions.begin(), sessions.end(), [](const LogPlayerSession& a, const LogPlayerSession& b) {
        return SessionSortStamp(a) > SessionSortStamp(b);
    });
    sessions.resize(static_cast<size_t>(logPlayerCount));
    return sessions;
}

struct BridgeRuntimePlayer {
    std::string steamId;
    std::string name;
    std::string userProfileId;
    std::string serverUserProfileId;
    std::string x = "0";
    std::string y = "0";
    std::string z = "0";
    std::string runtimeKey;
    std::string source;
};

std::vector<std::string> ExtractPlayerObjects(const std::string& body) {
    std::vector<std::string> objects;
    const auto playersPos = body.find("\"players\"");
    if (playersPos == std::string::npos) return objects;
    const auto arrayStart = body.find('[', playersPos);
    if (arrayStart == std::string::npos) return objects;

    bool inString = false;
    bool escaped = false;
    int depth = 0;
    size_t objectStart = std::string::npos;
    for (size_t i = arrayStart + 1; i < body.size(); ++i) {
        const char ch = body[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\' && inString) {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            inString = !inString;
            continue;
        }
        if (inString) continue;
        if (ch == '{') {
            if (depth == 0) objectStart = i;
            ++depth;
            continue;
        }
        if (ch == '}') {
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(body.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
            continue;
        }
        if (ch == ']' && depth == 0) break;
    }
    return objects;
}

std::string ExtractPlayersArrayJson(const std::string& body) {
    const auto playersPos = body.find("\"players\"");
    if (playersPos == std::string::npos) return {};
    const auto arrayStart = body.find('[', playersPos);
    if (arrayStart == std::string::npos) return {};
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    for (size_t i = arrayStart; i < body.size(); ++i) {
        const char ch = body[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\' && inString) {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            inString = !inString;
            continue;
        }
        if (inString) continue;
        if (ch == '[') ++depth;
        if (ch == ']') {
            --depth;
            if (depth == 0) return body.substr(arrayStart, i - arrayStart + 1);
        }
    }
    return {};
}

void AppendJsonStringEscape(std::string& out, char ch) {
    switch (ch) {
    case '"': out.push_back('"'); break;
    case '\\': out.push_back('\\'); break;
    case '/': out.push_back('/'); break;
    case 'b': out.push_back('\b'); break;
    case 'f': out.push_back('\f'); break;
    case 'n': out.push_back('\n'); break;
    case 'r': out.push_back('\r'); break;
    case 't': out.push_back('\t'); break;
    default: out.push_back(ch); break;
    }
}

std::string FlatJsonStringValue(const std::string& object, const std::string& key) {
    const auto needle = "\"" + key + "\"";
    auto pos = object.find(needle);
    if (pos == std::string::npos) return {};
    pos = object.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = object.find('"', pos);
    if (pos == std::string::npos) return {};
    std::string out;
    bool esc = false;
    for (++pos; pos < object.size(); ++pos) {
        const auto ch = object[pos];
        if (esc) {
            AppendJsonStringEscape(out, ch);
            esc = false;
            continue;
        }
        if (ch == '\\') {
            esc = true;
            continue;
        }
        if (ch == '"') break;
        out.push_back(ch);
    }
    return out;
}

std::string FlatJsonNumberValue(const std::string& object, const std::string& key) {
    const auto needle = "\"" + key + "\"";
    auto pos = object.find(needle);
    if (pos == std::string::npos) return "0";
    pos = object.find(':', pos + needle.size());
    if (pos == std::string::npos) return "0";
    ++pos;
    while (pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) ++pos;
    const auto end = object.find_first_not_of("-0123456789.", pos);
    auto value = object.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    return value.empty() ? "0" : value;
}

std::vector<BridgeRuntimePlayer> BridgeRuntimePlayers(const std::string& body) {
    std::vector<BridgeRuntimePlayer> players;
    for (const auto& object : ExtractPlayerObjects(body)) {
        BridgeRuntimePlayer player;
        player.steamId = FlatJsonStringValue(object, "steamId");
        player.name = FlatJsonStringValue(object, "name");
        player.userProfileId = FlatJsonStringValue(object, "userProfileId");
        player.serverUserProfileId = FlatJsonStringValue(object, "serverUserProfileId");
        player.x = FlatJsonNumberValue(object, "x");
        player.y = FlatJsonNumberValue(object, "y");
        player.z = FlatJsonNumberValue(object, "z");
        player.runtimeKey = FlatJsonStringValue(object, "runtimeKey");
        player.source = FlatJsonStringValue(object, "source");
        players.push_back(player);
    }
    return players;
}

bool BridgePlayerIdentityLooksUnsafe(const std::string& body) {
    return body.find("FString:") != std::string::npos ||
        body.find("UObject:") != std::string::npos ||
        body.find("userdata:") != std::string::npos ||
        body.find("\"name\":\"Unknown\"") != std::string::npos ||
        body.find("\"name\": \"Unknown\"") != std::string::npos;
}

std::string RuntimeOnlyPlayerJson(const BridgeRuntimePlayer& runtime, size_t index) {
    const auto profileId = !runtime.serverUserProfileId.empty() ? runtime.serverUserProfileId : runtime.userProfileId;
    const auto safeName = (!runtime.name.empty() && ToLowerAscii(runtime.name) != "unknown")
        ? runtime.name
        : (std::string("Unknown live #") + std::to_string(index + 1));
    const bool hasIdentity =
        !runtime.steamId.empty() ||
        !profileId.empty() ||
        (!runtime.name.empty() && ToLowerAscii(runtime.name) != "unknown");

    std::ostringstream ss;
    ss << "{\"name\":\"" << JsonEscape(safeName)
       << "\",\"steamId\":\"" << JsonEscape(runtime.steamId)
       << "\",\"profileId\":\"" << JsonEscape(profileId)
       << "\",\"online\":true,\"status\":\"online\""
       << ",\"source\":\"ue4ss-live\""
       << ",\"identitySource\":\"" << (hasIdentity ? "ue4ss-runtime" : "runtime-only") << "\""
       << ",\"runtimeOnly\":" << (hasIdentity ? "false" : "true")
       << ",\"positionSource\":\"ue4ss\""
       << ",\"x\":" << (runtime.x.empty() ? "0" : runtime.x)
       << ",\"y\":" << (runtime.y.empty() ? "0" : runtime.y)
       << ",\"z\":" << (runtime.z.empty() ? "0" : runtime.z)
       << ",\"userProfileId\":\"" << JsonEscape(runtime.userProfileId)
       << "\",\"serverUserProfileId\":\"" << JsonEscape(runtime.serverUserProfileId)
       << "\",\"runtimeKey\":\"" << JsonEscape(runtime.runtimeKey)
       << "\"}";
    return ss.str();
}

std::string BridgePlayersWithLogIdentityJson(const std::wstring& baseDir, const std::string& bridgeBody) {
    auto sessions = OnlineSessions(InferPlayerSessionsFromLog(baseDir));
    const auto runtimePlayers = BridgeRuntimePlayers(bridgeBody);
    const int logPlayerCount = LatestServerLogPlayerCount(baseDir);
    if (logPlayerCount == 0) {
        if (!runtimePlayers.empty() && !BridgePlayerIdentityLooksUnsafe(bridgeBody)) {
            return {};
        }
        std::ostringstream empty;
        empty << "{\"ok\":true,\"source\":\"server-log\",\"command\":\"list_players\""
              << ",\"message\":\"SCUM.log подтверждает 0 игроков онлайн; устаревшие runtime-объекты скрыты\""
              << ",\"data\":{\"players\":[],\"count\":0"
              << ",\"runtimeCount\":" << runtimePlayers.size()
              << ",\"logPlayerCount\":0"
              << ",\"identityOverlay\":true"
              << ",\"staleRuntimeIgnored\":" << (runtimePlayers.empty() ? "false" : "true")
              << "}}";
        return empty.str();
    }
    sessions = TrimSessionsToLogCount(std::move(sessions), logPlayerCount);
    if (sessions.empty() && runtimePlayers.empty()) return {};

    const bool needsOverlay = BridgePlayerIdentityLooksUnsafe(bridgeBody) ||
        (!runtimePlayers.empty() && runtimePlayers.size() != sessions.size()) ||
        (logPlayerCount >= 0 && logPlayerCount != static_cast<int>(runtimePlayers.size()));
    if (!needsOverlay) return {};

    std::ostringstream ss;
    ss << "{\"ok\":true,\"source\":\"ue4ss+server-log\",\"command\":\"list_players\""
       << ",\"message\":\"Игроки сверены с серверным логом\""
       << ",\"data\":{\"players\":[";
    std::vector<bool> runtimeUsed(runtimePlayers.size(), false);
    const auto findRuntime = [&](const LogPlayerSession& session, size_t index) -> const BridgeRuntimePlayer* {
        if (!session.profileId.empty()) {
            for (size_t r = 0; r < runtimePlayers.size(); ++r) {
                if (runtimeUsed[r]) continue;
                const auto& runtime = runtimePlayers[r];
                if (runtime.userProfileId == session.profileId || runtime.serverUserProfileId == session.profileId) {
                    runtimeUsed[r] = true;
                    return &runtime;
                }
            }
        }
        if (!session.steamId.empty() || !session.name.empty()) {
            const auto wantedSteam = ToLowerAscii(session.steamId);
            const auto wantedName = ToLowerAscii(session.name);
            for (size_t r = 0; r < runtimePlayers.size(); ++r) {
                if (runtimeUsed[r]) continue;
                const auto& runtime = runtimePlayers[r];
                if (!wantedSteam.empty() && ToLowerAscii(runtime.steamId) == wantedSteam) {
                    runtimeUsed[r] = true;
                    return &runtime;
                }
                if (!wantedName.empty() && ToLowerAscii(runtime.name) == wantedName) {
                    runtimeUsed[r] = true;
                    return &runtime;
                }
            }
        }
        if (index < runtimePlayers.size() && !runtimeUsed[index]) {
            runtimeUsed[index] = true;
            return &runtimePlayers[index];
        }
        return nullptr;
    };

    size_t visibleCount = 0;
    for (size_t i = 0; i < sessions.size(); ++i) {
        if (visibleCount) ss << ",";
        const auto& session = sessions[i];
        const BridgeRuntimePlayer* runtime = findRuntime(session, i);
        ss << "{\"name\":\"" << JsonEscape(session.name)
           << "\",\"steamId\":\"" << JsonEscape(session.steamId)
           << "\",\"profileId\":\"" << JsonEscape(session.profileId)
           << "\",\"online\":true,\"status\":\"online\""
           << ",\"source\":\"ue4ss+server-log\""
           << ",\"identitySource\":\"server-log\""
           << ",\"positionSource\":\"" << (runtime ? "ue4ss" : "none") << "\""
           << ",\"x\":" << (runtime ? runtime->x : "0")
           << ",\"y\":" << (runtime ? runtime->y : "0")
           << ",\"z\":" << (runtime ? runtime->z : "0");
        if (runtime && !runtime->runtimeKey.empty()) {
            ss << ",\"runtimeKey\":\"" << JsonEscape(runtime->runtimeKey) << "\"";
        }
        ss << ",\"joinedUtc\":\"" << JsonEscape(session.joinedUtc)
           << "\",\"lastSeenUtc\":\"" << JsonEscape(session.lastSeenUtc)
           << "\"}";
        ++visibleCount;
    }

    for (size_t r = 0; r < runtimePlayers.size(); ++r) {
        if (runtimeUsed[r]) continue;
        if (visibleCount) ss << ",";
        ss << RuntimeOnlyPlayerJson(runtimePlayers[r], r);
        ++visibleCount;
    }
    ss << "],\"count\":" << visibleCount
       << ",\"runtimeCount\":" << runtimePlayers.size()
       << ",\"logPlayerCount\":" << logPlayerCount
       << ",\"identityOverlay\":true}}";
    return ss.str();
}

std::string LogPlayersJson(const std::wstring& baseDir, const std::string& bridgeMessage) {
    const auto sessions = InferPlayerSessionsFromLog(baseDir);
    const int logPlayerCount = LatestServerLogPlayerCount(baseDir);
    std::vector<LogPlayerSession> visibleSessions;
    if (logPlayerCount > 0) {
        visibleSessions = TrimSessionsToLogCount(OnlineSessions(sessions), logPlayerCount);
    } else if (logPlayerCount < 0) {
        visibleSessions = OnlineSessions(sessions);
    }
    const auto onlineCount = logPlayerCount == 0 ? 0 : static_cast<int>(visibleSessions.size());
    std::ostringstream ss;
    ss << "{\"command\":\"players_snapshot\""
       << ",\"source\":\"server-log\""
       << ",\"message\":\"Список игроков восстановлен по SCUM.log; live runtime bridge не запрошен.\""
       << ",\"payload\":{\"players\":" << PlayerArrayJson(visibleSessions, true)
       << ",\"count\":" << onlineCount
       << ",\"logPlayerCount\":" << logPlayerCount
       << ",\"recentSessions\":" << PlayerArrayJson(sessions, false)
       << ",\"bridgeOnline\":false}"
       << ",\"warnings\":[\"Живые координаты и прямые данные игрока требуют refresh=1/live=1.\",\"" << JsonEscape(bridgeMessage) << "\"]}";
    return ss.str();
}

std::string LivePlayersJson(const std::wstring& baseDir, int timeoutMs) {
    auto br = BridgeExecute(baseDir, "list_players", "{}", timeoutMs);
    if (br.ok && !br.body.empty()) {
        const auto overlaid = BridgePlayersWithLogIdentityJson(baseDir, br.body);
        return overlaid.empty() ? br.body : overlaid;
    }
    return LogPlayersJson(baseDir, br.message);
}

std::string PlayersJson(const std::wstring& baseDir, const Request& req) {
    const auto mode = ToLowerAscii(TrimAscii(QueryValue(req.path, "mode")));
    const auto live = ToLowerAscii(TrimAscii(QueryValue(req.path, "live")));
    const auto refresh = ToLowerAscii(TrimAscii(QueryValue(req.path, "refresh")));
    const bool forceRefresh = refresh == "1" || refresh == "true" || refresh == "yes";
    const bool wantsLogOnly = mode == "log" || live == "0" || live == "false" || live == "no";
    if (wantsLogOnly) {
        return LogPlayersJson(baseDir, "live runtime bridge disabled by caller");
    }

    constexpr uint64_t kPlayersCacheTtlMs = 12000;
    const auto nowTick = GetTickCount64();
    if (!forceRefresh) {
        std::lock_guard<std::mutex> lock(g_playersCacheMutex);
        if (!g_playersCache.body.empty() && nowTick >= g_playersCache.tick &&
            nowTick - g_playersCache.tick < kPlayersCacheTtlMs) {
            return g_playersCache.body;
        }
    }

    auto body = LivePlayersJson(baseDir, forceRefresh ? 2500 : 1800);
    {
        std::lock_guard<std::mutex> lock(g_playersCacheMutex);
        g_playersCache.body = body;
        g_playersCache.tick = nowTick;
    }
    return body;
}

std::string MapJsonWithPlayers(
    const std::string& playersJson,
    const std::string& playerSource,
    const std::string& flagsJson = "[]",
    const std::string& vehiclesJson = "[]") {
    std::ostringstream ss;
    ss << "{\"mapImageUrl\":\"\""
       << ",\"bounds\":{\"minX\":-905369.6875,\"maxX\":619646.5625,\"minY\":-904357.625,\"maxY\":619659.75,\"invertX\":true,\"invertY\":true,\"swapAxes\":false}"
       << ",\"players\":" << (playersJson.empty() ? "[]" : playersJson)
       << ",\"flags\":" << (flagsJson.empty() ? "[]" : flagsJson)
       << ",\"vehicles\":" << (vehiclesJson.empty() ? "[]" : vehiclesJson)
       << ",\"playerSource\":\"" << JsonEscape(playerSource) << "\"}";
    return ss.str();
}

std::string MapJson(const std::wstring& baseDir) {
    const auto sessions = InferPlayerSessionsFromLog(baseDir);
    const int logPlayerCount = LatestServerLogPlayerCount(baseDir);
    std::vector<LogPlayerSession> visibleSessions;
    if (logPlayerCount > 0) {
        visibleSessions = TrimSessionsToLogCount(OnlineSessions(sessions), logPlayerCount);
    } else if (logPlayerCount < 0) {
        visibleSessions = OnlineSessions(sessions);
    }
    return MapJsonWithPlayers(PlayerArrayJson(visibleSessions, true), "server-log fallback; positions require guarded runtime bridge");
}

std::string JsonFileOrNull(const std::wstring& path) {
    if (!FileExists(path)) return "null";
    auto text = ReadTextFile(path);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    if (text.empty()) return "null";
    if (text.front() == '{' || text.front() == '[') return text;
    return std::string("\"") + JsonEscape(text) + "\"";
}

bool ManagedLoaderPathIsAbsolute(const std::wstring& path) {
    return path.rfind(L"\\\\", 0) == 0 ||
        path.rfind(L"/", 0) == 0 ||
        (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'));
}

std::wstring ManagedLoaderConfiguredPath(
    const std::wstring& baseDir,
    const std::string& key,
    const std::wstring& fallbackRelative) {
    auto configured = Utf8ToWide(ConfigTextValue(baseDir, key, WideToUtf8(fallbackRelative)));
    if (configured.empty()) {
        configured = fallbackRelative;
    }
    return ManagedLoaderPathIsAbsolute(configured) ? configured : JoinPath(baseDir, configured);
}

std::string ManagedLoaderFileStatusJson(const std::wstring& path) {
    std::ostringstream ss;
    const bool exists = FileExists(path);
    ss << "{\"path\":\"" << JsonEscape(WideToUtf8(path))
       << "\",\"exists\":" << (exists ? "true" : "false")
       << ",\"sizeBytes\":" << (exists ? FileSizeOrZero(path) : 0)
       << "}";
    return ss.str();
}

std::string ManagedCommandQueueStatusJson(const std::wstring& baseDir) {
    const auto queueRoot = JoinPath(JoinPath(JoinPath(ProjectRuntimeRoot(baseDir), L"state"), L"managed"), L"commands");
    const auto pendingRoot = JoinPath(queueRoot, L"pending");
    const auto doneRoot = JoinPath(queueRoot, L"done");
    const auto failedRoot = JoinPath(queueRoot, L"failed");

    auto countJsonFiles = [](const std::wstring& root, bool results) {
        std::error_code ec;
        if (!fs::is_directory(fs::path(root), ec)) {
            return 0;
        }

        int count = 0;
        for (const auto& entry : fs::directory_iterator(fs::path(root), ec)) {
            if (ec) {
                break;
            }
            std::error_code fileEc;
            const auto fileName = entry.path().filename().wstring();
            const bool isResult = fileName.ends_with(L".result.json");
            if (entry.is_regular_file(fileEc) && entry.path().extension() == L".json" && isResult == results) {
                ++count;
            }
        }
        return count;
    };

    std::ostringstream ss;
    ss << "{\"schema\":\"scum-nedjin-managed-command-queue-status-v1\""
       << ",\"root\":\"" << JsonEscape(WideToUtf8(queueRoot)) << "\""
       << ",\"pending\":{\"path\":\"" << JsonEscape(WideToUtf8(pendingRoot))
       << "\",\"count\":" << countJsonFiles(pendingRoot, false)
       << ",\"resultCount\":" << countJsonFiles(pendingRoot, true) << "}"
       << ",\"done\":{\"path\":\"" << JsonEscape(WideToUtf8(doneRoot))
       << "\",\"count\":" << countJsonFiles(doneRoot, false)
       << ",\"resultCount\":" << countJsonFiles(doneRoot, true) << "}"
       << ",\"failed\":{\"path\":\"" << JsonEscape(WideToUtf8(failedRoot))
       << "\",\"count\":" << countJsonFiles(failedRoot, false)
       << ",\"resultCount\":" << countJsonFiles(failedRoot, true) << "}"
       << ",\"dispatcherEnabled\":false"
       << ",\"message\":\"Managed command queue is read-only; no dispatcher is enabled.\""
       << "}";
    return ss.str();
}

std::string ManagedLoaderStatusJson(const std::wstring& baseDir) {
    const bool enabled = ConfigBoolValue(baseDir, "managed_loader_enabled", false);
    const bool allowProbe = ConfigBoolValue(baseDir, "managed_loader_allow_in_process_probe", false);
    const bool allowApiProbe = ConfigBoolValue(baseDir, "managed_loader_allow_api_probe", false);
    const auto managedDir = JoinPath(JoinPath(baseDir, L"ScumNeDjin"), L"managed");
    const auto runtimeConfigPath = ManagedLoaderConfiguredPath(
        baseDir,
        "managed_loader_runtime_config",
        L"ScumNeDjin\\managed\\ScumManagedProbe.runtimeconfig.json");
    const auto assemblyPath = ManagedLoaderConfiguredPath(
        baseDir,
        "managed_loader_assembly",
        L"ScumNeDjin\\managed\\ScumManagedProbe.dll");
    const auto markerDir = ManagedLoaderConfiguredPath(
        baseDir,
        "managed_loader_marker_dir",
        L"..\\..\\Saved\\ScumNeDjin\\state");
    const auto savedStatusPath = JoinPath(JoinPath(ProjectRuntimeRoot(baseDir), L"state"), L"managed-loader-status.json");
    const auto bridgeStatusPath = JoinPath(JoinPath(baseDir, L"nedjin_bridge"), L"managed_loader.json");
    const bool safeToLoad = enabled && allowProbe;
    const bool safeToRunApiProbe = safeToLoad && allowApiProbe;
    const bool savedStatusActive = safeToLoad;
    const char* effectiveStatus = !enabled
        ? "disabled"
        : (!allowProbe ? "blocked_by_in_process_probe_flag" : (allowApiProbe ? "api_probe_ready" : "loader_ready"));
    const char* savedStatusIgnoredReason = savedStatusActive
        ? ""
        : (!enabled ? "managed loader disabled by nedjin.ini" : "managed loader blocked by safety flag");

    std::error_code ec;
    const bool managedDirExists = fs::is_directory(fs::path(managedDir), ec);

    std::ostringstream ss;
    ss << "{\"enabled\":" << (enabled ? "true" : "false")
       << ",\"allowInProcessProbe\":" << (allowProbe ? "true" : "false")
       << ",\"allowApiProbe\":" << (allowApiProbe ? "true" : "false")
       << ",\"safeToLoad\":" << (safeToLoad ? "true" : "false")
       << ",\"safeToRunApiProbe\":" << (safeToRunApiProbe ? "true" : "false")
       << ",\"effectiveStatus\":\"" << effectiveStatus << "\""
       << ",\"savedStatusActive\":" << (savedStatusActive ? "true" : "false")
       << ",\"savedStatusIgnoredReason\":\"" << JsonEscape(savedStatusIgnoredReason) << "\""
       << ",\"message\":\"" << (enabled
            ? (allowProbe ? "Managed loader is enabled and allowed to run." : "Managed loader enabled but blocked by safety flag.")
            : "Managed loader disabled by nedjin.ini.") << "\""
       << ",\"managedDir\":{\"path\":\"" << JsonEscape(WideToUtf8(managedDir))
       << "\",\"exists\":" << (managedDirExists ? "true" : "false") << "}"
       << ",\"runtimeConfig\":" << ManagedLoaderFileStatusJson(runtimeConfigPath)
       << ",\"assembly\":" << ManagedLoaderFileStatusJson(assemblyPath)
       << ",\"abstractions\":" << ManagedLoaderFileStatusJson(JoinPath(managedDir, L"ScumManaged.Abstractions.dll"))
       << ",\"sampleModule\":" << ManagedLoaderFileStatusJson(JoinPath(managedDir, L"ScumManaged.SampleModule.dll"))
       << ",\"nedjinStateModule\":" << ManagedLoaderFileStatusJson(JoinPath(managedDir, L"ScumManaged.NedjinStateModule.dll"))
       << ",\"hostApiModule\":" << ManagedLoaderFileStatusJson(JoinPath(managedDir, L"ScumManaged.HostApiModule.dll"))
       << ",\"moduleManifest\":" << ManagedLoaderFileStatusJson(JoinPath(managedDir, L"managed-modules.json"))
       << ",\"markerDir\":\"" << JsonEscape(WideToUtf8(markerDir)) << "\""
       << ",\"savedStatus\":" << JsonFileOrNull(savedStatusPath)
       << ",\"bridgeStatus\":" << JsonFileOrNull(bridgeStatusPath)
       << ",\"commandQueue\":" << ManagedCommandQueueStatusJson(baseDir)
       << "}";
    return ss.str();
}

std::string CommandTraceJson(const std::wstring& baseDir) {
    const auto bridgeDir = JoinPath(baseDir, L"nedjin_bridge");
    const auto runtimeLog = ProjectReadableRuntimeLogPath(baseDir);
    std::ostringstream ss;
    ss << "{\"nativeLoader\":" << JsonFileOrNull(JoinPath(bridgeDir, L"native_loader.json"))
       << ",\"http\":" << JsonFileOrNull(JoinPath(bridgeDir, L"native_http.json"))
       << ",\"ue4ssLoad\":" << JsonFileOrNull(JoinPath(bridgeDir, L"ue4ss_load.json"))
       << ",\"heartbeat\":" << JsonFileOrNull(JoinPath(bridgeDir, L"heartbeat.json"))
       << ",\"managedLoader\":" << ManagedLoaderStatusJson(baseDir)
       << ",\"bridgeLog\":" << LinesJson(TailLines(runtimeLog, 300, 512 * 1024))
       << ",\"runtimeLogPath\":\"" << JsonEscape(WideToUtf8(runtimeLog)) << "\""
       << "}";
    return ss.str();
}

std::string ArkPanelStatusJson(const std::wstring& baseDir);
std::string ArkPanelAdminEnvelope(const std::wstring& baseDir, const Request& req);
std::string WorldPersistenceStatusJson(const std::wstring& baseDir);

std::string StatusJson(const std::wstring& baseDir) {
    const auto dbPath = SavedDbPath(baseDir);
    const auto logPath = SavedLogPath(baseDir);
    const bool dbOnline = FileExists(dbPath);
    const bool logsOnline = FileExists(logPath);
    const auto serverName = ServerDisplayName(baseDir);

    std::ostringstream ss;
    ss << "{\"server\":\"" << JsonEscape(serverName) << "\""
       << ",\"serverName\":\"" << JsonEscape(serverName) << "\""
       << ",\"bridge\":" << BridgeStatusJson(baseDir)
       << ",\"managedLoader\":" << ManagedLoaderStatusJson(baseDir)
       << ",\"database\":{\"online\":" << (dbOnline ? "true" : "false")
       << ",\"path\":\"" << JsonEscape(WideToUtf8(dbPath))
       << "\",\"sizeBytes\":" << FileSizeOrZero(dbPath)
       << ",\"message\":\"" << (dbOnline ? "SCUM.db найден." : "SCUM.db не найден.") << "\"}"
       << ",\"logs\":{\"online\":" << (logsOnline ? "true" : "false")
       << ",\"path\":\"" << JsonEscape(WideToUtf8(logPath))
       << "\",\"sizeBytes\":" << FileSizeOrZero(logPath)
       << ",\"message\":\"" << (logsOnline ? "SCUM.log читается." : "SCUM.log не найден.") << "\"}"
       << ",\"worldPersistence\":" << WorldPersistenceStatusJson(baseDir)
       << ",\"httpServer\":" << HttpServerStatusJson()
       << ",\"performance\":" << LatestGlobalStatsJson(baseDir) << "}";
    return ss.str();
}

std::string RedactDiagnosticLine(std::string line) {
    try {
        static const std::regex secretPattern(R"((api[_-]?key|token|password|secret|botToken|webhook)\s*[:=]\s*[^\s,;"']+)",
            std::regex_constants::icase);
        static const std::regex wargmClientPattern(R"((client=\d+:)[^\s&]+)",
            std::regex_constants::icase);
        line = std::regex_replace(line, secretPattern, "$1=<redacted>");
        line = std::regex_replace(line, wargmClientPattern, "$1<redacted>");
    } catch (...) {
    }
    return line;
}

bool IsDiagnosticProblemLine(const std::string& line) {
    const auto lower = ToLowerAscii(line);
    return lower.find("error") != std::string::npos ||
        lower.find("failed") != std::string::npos ||
        lower.find("fatal") != std::string::npos ||
        lower.find("exception") != std::string::npos ||
        lower.find("crash") != std::string::npos ||
        lower.find("timeout") != std::string::npos ||
        lower.find("stale") != std::string::npos ||
        lower.find("unreadable") != std::string::npos ||
        lower.find("lua error") != std::string::npos ||
        lower.find("ue4ss error") != std::string::npos ||
        (lower.find("bridge") != std::string::npos &&
            (lower.find("stale") != std::string::npos ||
             lower.find("offline") != std::string::npos ||
             lower.find("failed") != std::string::npos ||
             lower.find("error") != std::string::npos ||
             lower.find("timeout") != std::string::npos ||
             lower.find("unreadable") != std::string::npos));
}

void AppendDiagnosticsSection(std::ostringstream& ss, const std::string& title, const std::vector<std::string>& lines) {
    ss << "\n===== " << title << " =====\n";
    if (lines.empty()) {
        ss << "(empty)\n";
        return;
    }
    for (const auto& line : lines) {
        ss << RedactDiagnosticLine(line) << "\n";
    }
}

std::string DiagnosticPackageText(const std::wstring& baseDir) {
    const auto bridgeDir = JoinPath(baseDir, L"nedjin_bridge");
    const auto runtimeLogPath = ProjectReadableRuntimeLogPath(baseDir);
    auto serverLines = TailLines(SavedLogPath(baseDir), 1000, 4 * 1024 * 1024);
    auto runtimeLines = TailLines(runtimeLogPath, 1000, 2 * 1024 * 1024);
    std::vector<std::string> problems;
    for (const auto& line : serverLines) {
        if (IsDiagnosticProblemLine(line)) problems.push_back(line);
        if (problems.size() >= 120) break;
    }
    if (problems.size() < 120) {
        for (const auto& line : runtimeLines) {
            if (IsDiagnosticProblemLine(line)) problems.push_back(line);
            if (problems.size() >= 120) break;
        }
    }

    std::vector<std::string> summary;
    summary.push_back(std::string("BaseDir: ") + WideToUtf8(baseDir));
    summary.push_back(std::string("ScumRoot: ") + WideToUtf8(ScumRoot(baseDir)));
    summary.push_back(std::string("DatabasePath: ") + WideToUtf8(SavedDbPath(baseDir)));
    summary.push_back(std::string("DatabaseSizeBytes: ") + std::to_string(FileSizeOrZero(SavedDbPath(baseDir))));
    summary.push_back(std::string("ServerLogPath: ") + WideToUtf8(SavedLogPath(baseDir)));
    summary.push_back(std::string("ServerLogSizeBytes: ") + std::to_string(FileSizeOrZero(SavedLogPath(baseDir))));
    summary.push_back(std::string("RuntimeRoot: ") + WideToUtf8(ProjectRuntimeRoot(baseDir)));
    summary.push_back(std::string("RuntimeLogPath: ") + WideToUtf8(runtimeLogPath));
    summary.push_back(std::string("RuntimeLogSizeBytes: ") + std::to_string(FileSizeOrZero(runtimeLogPath)));
    summary.push_back(std::string("StatusJson: ") + RedactDiagnosticLine(StatusJson(baseDir)));
    summary.push_back(std::string("CommandTraceJson: ") + RedactDiagnosticLine(CommandTraceJson(baseDir)));

    std::ostringstream ss;
    ss << "SCUM NeDjin diagnostics package\n";
    ss << "Generated UTC: " << UtcIsoNow() << "\n\n";
    ss << "Open a Discord ticket manually and attach this file:\n";
    ss << "Discord: https://discord.gg/kQwBDdzxEH\n\n";
    ss << "Important: secrets are redacted, but the package can still contain SteamID, player names, server paths, and gameplay logs.\n";
    AppendDiagnosticsSection(ss, "Summary", summary);
    AppendDiagnosticsSection(ss, "Detected problem lines", problems.empty()
        ? std::vector<std::string>{ "No obvious error lines detected in collected log tails." }
        : problems);
    AppendDiagnosticsSection(ss, "SCUM server log tail", serverLines);
    AppendDiagnosticsSection(ss, "NeDjin runtime bridge log tail", runtimeLines);
    return ss.str();
}

std::wstring PluginConfigPath(const std::wstring& baseDir, const std::string& key) {
    return JoinPath(JoinPath(ProjectRuntimeRoot(baseDir), L"configs"), Utf8ToWide(CanonicalPluginConfigKey(key) + ".json"));
}

std::wstring PluginConfigLegacyPath(const std::wstring& baseDir, const std::string& key) {
    return JoinPath(JoinPath(ProjectLegacyRoot(baseDir), L"configs"), Utf8ToWide(CanonicalPluginConfigKey(key) + ".json"));
}

std::wstring PluginConfigOverlayPath(const std::wstring& baseDir, const std::string& key) {
    return JoinPath(JoinPath(JoinPath(ProjectRuntimeRoot(baseDir), L"state"), L"configs"), Utf8ToWide(CanonicalPluginConfigKey(key) + ".json"));
}

std::wstring PluginConfigLegacyOverlayPath(const std::wstring& baseDir, const std::string& key) {
    return JoinPath(JoinPath(JoinPath(ProjectLegacyRoot(baseDir), L"state"), L"configs"), Utf8ToWide(CanonicalPluginConfigKey(key) + ".json"));
}

std::wstring PluginConfigPathForExactKey(const std::wstring& baseDir, const std::string& key) {
    return JoinPath(JoinPath(ProjectRuntimeRoot(baseDir), L"configs"), Utf8ToWide(SafeConfigKey(key) + ".json"));
}

std::wstring PluginConfigLegacyPathForExactKey(const std::wstring& baseDir, const std::string& key) {
    return JoinPath(JoinPath(ProjectLegacyRoot(baseDir), L"configs"), Utf8ToWide(SafeConfigKey(key) + ".json"));
}

std::wstring PluginConfigOverlayPathForExactKey(const std::wstring& baseDir, const std::string& key) {
    return JoinPath(JoinPath(JoinPath(ProjectRuntimeRoot(baseDir), L"state"), L"configs"), Utf8ToWide(SafeConfigKey(key) + ".json"));
}

std::wstring PluginConfigLegacyOverlayPathForExactKey(const std::wstring& baseDir, const std::string& key) {
    return JoinPath(JoinPath(JoinPath(ProjectLegacyRoot(baseDir), L"state"), L"configs"), Utf8ToWide(SafeConfigKey(key) + ".json"));
}

std::vector<std::wstring> PluginConfigLegacyAliasPaths(const std::wstring& baseDir, const std::string& key) {
    std::vector<std::wstring> paths;
    for (const auto& alias : LegacyPluginConfigAliases(key)) {
        paths.push_back(PluginConfigOverlayPathForExactKey(baseDir, alias));
        paths.push_back(PluginConfigLegacyOverlayPathForExactKey(baseDir, alias));
        paths.push_back(PluginConfigPathForExactKey(baseDir, alias));
        paths.push_back(PluginConfigLegacyPathForExactKey(baseDir, alias));
    }
    return paths;
}

bool PluginConfigPathExists(const std::wstring& path) {
    return FileExists(path) || FileExists(path + L".live");
}

bool TryReadPluginConfigJsonText(const std::wstring& path, std::string& text) {
    constexpr uintmax_t maxConfigJsonBytes = 2 * 1024 * 1024;
    text.clear();
    const auto livePath = path + L".live";
    const auto readPath = FileExists(livePath) ? livePath : path;
    if (!FileExists(readPath)) return false;
    if (FileSizeOrZero(readPath) > maxConfigJsonBytes) return false;
    std::string rawText;
    if (!ReadTextFileExact(readPath, rawText)) return false;
    text = TrimAscii(rawText);
    return IsValidJsonObject(text);
}

bool HasCanonicalPluginConfig(const std::wstring& baseDir, const std::string& key) {
    for (const auto& path : {
        PluginConfigOverlayPath(baseDir, key),
        PluginConfigLegacyOverlayPath(baseDir, key),
        PluginConfigPath(baseDir, key),
        PluginConfigLegacyPath(baseDir, key)
    }) {
        std::string text;
        if (TryReadPluginConfigJsonText(path, text)) return true;
    }
    return false;
}

bool HasLegacyPluginConfigAlias(const std::wstring& baseDir, const std::string& key) {
    const auto canonicalKey = CanonicalPluginConfigKey(key);
    for (const auto& path : PluginConfigLegacyAliasPaths(baseDir, canonicalKey)) {
        if (PluginConfigPathExists(path)) return true;
    }
    return false;
}

bool HasLegacyOnlyPluginConfigAlias(const std::wstring& baseDir, const std::string& key) {
    const auto canonicalKey = CanonicalPluginConfigKey(key);
    return !LegacyPluginConfigAliases(canonicalKey).empty() &&
        !HasCanonicalPluginConfig(baseDir, canonicalKey) &&
        HasLegacyPluginConfigAlias(baseDir, canonicalKey);
}

std::string PluginConfigMigrationRequiredMessage(const std::string& key) {
    return std::string("Найден только legacy-конфиг для ") + JsonEscape(CanonicalPluginConfigKey(key)) +
        ". Настройки сохранены без изменений; автоматическая миграция и запись заблокированы. "
        "Сначала создайте per-host backup и выполните отдельную проверяемую миграцию.";
}

// Preserve the currently effective overlay location during the Saved/legacy migration.
// A managed host may have published a .live recovery overlay while its base file was
// temporarily locked; updating that exact file keeps native and managed reads aligned.
std::wstring PluginConfigWritableOverlayPath(const std::wstring& baseDir, const std::string& key) {
    const auto primary = PluginConfigOverlayPath(baseDir, key);
    const auto primaryLive = primary + L".live";
    if (FileExists(primaryLive)) return primaryLive;
    if (FileExists(primary)) return primary;

    const auto legacy = PluginConfigLegacyOverlayPath(baseDir, key);
    const auto legacyLive = legacy + L".live";
    if (FileExists(legacyLive)) return legacyLive;
    if (FileExists(legacy)) return legacy;

    return primary;
}

bool WritePluginConfigOverlayAtomically(
    const std::wstring& baseDir,
    const std::string& key,
    const std::string& config,
    std::string& backupName) {
    backupName.clear();
    const auto target = PluginConfigWritableOverlayPath(baseDir, key);
    std::wstring backupPath;
    if (FileExists(target)) {
        backupPath = UniqueConfigBackupPath(target);
        if (backupPath.empty()) return false;
    }

    if (!WriteTextFileAtomicallyExact(target, config, backupPath)) return false;
    if (!backupPath.empty()) {
        backupName = WideToUtf8(fs::path(backupPath).filename().wstring());
    }
    return true;
}

std::wstring ModuleStatePath(const std::wstring& baseDir, const std::string& key) {
    return JoinPath(JoinPath(ProjectRuntimeRoot(baseDir), L"state"), Utf8ToWide(SafeConfigKey(key) + ".json"));
}

std::wstring ModuleLegacyStatePath(const std::wstring& baseDir, const std::string& key) {
    return JoinPath(JoinPath(ProjectLegacyRoot(baseDir), L"state"), Utf8ToWide(SafeConfigKey(key) + ".json"));
}

std::wstring ModuleReadableStatePath(const std::wstring& baseDir, const std::string& key) {
    const auto primary = ModuleStatePath(baseDir, key);
    return FileExists(primary) ? primary : ModuleLegacyStatePath(baseDir, key);
}

bool JsonKeyExistsCi(const std::string& json, const std::string& key) {
    const auto lower = ToLowerAscii(json);
    const auto needle = std::string("\"") + ToLowerAscii(key) + "\"";
    return lower.find(needle) != std::string::npos;
}

bool JsonBoolValueCi(const std::string& json, const std::string& key, bool fallback) {
    const auto lower = ToLowerAscii(json);
    const auto needle = std::string("\"") + ToLowerAscii(key) + "\"";
    const auto pos = lower.find(needle);
    if (pos == std::string::npos) return fallback;
    const auto colon = lower.find(':', pos + needle.size());
    if (colon == std::string::npos) return fallback;
    auto value = colon + 1;
    while (value < lower.size() && std::isspace(static_cast<unsigned char>(lower[value]))) ++value;
    if (lower.compare(value, 4, "true") == 0) return true;
    if (lower.compare(value, 5, "false") == 0) return false;
    return fallback;
}

bool ReplaceJsonBoolCi(std::string& json, const std::string& key, bool value) {
    auto lower = ToLowerAscii(json);
    const auto needle = std::string("\"") + ToLowerAscii(key) + "\"";
    const auto replacement = value ? std::string("true") : std::string("false");
    bool changed = false;
    size_t search = 0;
    while (true) {
        const auto pos = lower.find(needle, search);
        if (pos == std::string::npos) break;
        const auto colon = lower.find(':', pos + needle.size());
        if (colon == std::string::npos) break;
        auto valueStart = colon + 1;
        while (valueStart < lower.size() && std::isspace(static_cast<unsigned char>(lower[valueStart]))) ++valueStart;
        size_t valueLength = 0;
        if (lower.compare(valueStart, 4, "true") == 0) valueLength = 4;
        else if (lower.compare(valueStart, 5, "false") == 0) valueLength = 5;
        if (valueLength == 0) {
            search = colon + 1;
            continue;
        }
        json.replace(valueStart, valueLength, replacement);
        lower.replace(valueStart, valueLength, replacement);
        changed = true;
        search = valueStart + replacement.size();
    }
    return changed;
}

bool ReplaceJsonStringCi(std::string& json, const std::string& key, const std::string& value) {
    auto lower = ToLowerAscii(json);
    const auto needle = std::string("\"") + ToLowerAscii(key) + "\"";
    const auto replacement = std::string("\"") + JsonEscape(value) + "\"";
    bool changed = false;
    size_t search = 0;
    while (true) {
        const auto pos = lower.find(needle, search);
        if (pos == std::string::npos) break;
        const auto colon = lower.find(':', pos + needle.size());
        if (colon == std::string::npos) break;
        auto valueStart = colon + 1;
        while (valueStart < lower.size() && std::isspace(static_cast<unsigned char>(lower[valueStart]))) ++valueStart;
        if (valueStart >= lower.size() || lower[valueStart] != '"') {
            search = colon + 1;
            continue;
        }

        auto valueEnd = valueStart + 1;
        auto escaped = false;
        while (valueEnd < lower.size()) {
            const auto ch = lower[valueEnd];
            if (!escaped && ch == '"') break;
            escaped = !escaped && ch == '\\';
            if (ch != '\\') escaped = false;
            ++valueEnd;
        }
        if (valueEnd >= lower.size()) break;

        json.replace(valueStart, valueEnd - valueStart + 1, replacement);
        lower.replace(valueStart, valueEnd - valueStart + 1, ToLowerAscii(replacement));
        changed = true;
        search = valueStart + replacement.size();
    }
    return changed;
}

bool JsonStringEqualsCi(const std::string& json, const std::string& key, const std::string& expected) {
    const auto lower = ToLowerAscii(json);
    const auto needle = std::string("\"") + ToLowerAscii(key) + "\"";
    const auto pos = lower.find(needle);
    if (pos == std::string::npos) return false;
    const auto colon = lower.find(':', pos + needle.size());
    if (colon == std::string::npos) return false;
    auto valueStart = colon + 1;
    while (valueStart < lower.size() && std::isspace(static_cast<unsigned char>(lower[valueStart]))) ++valueStart;
    if (valueStart >= lower.size() || lower[valueStart] != '"') return false;
    ++valueStart;
    const auto valueEnd = lower.find('"', valueStart);
    if (valueEnd == std::string::npos) return false;
    return lower.substr(valueStart, valueEnd - valueStart) == ToLowerAscii(expected);
}

void InsertJsonPropertyBeforeEnd(std::string& json, const std::string& propertyJson) {
    const auto end = json.find_last_of('}');
    if (end == std::string::npos) return;
    auto hasValue = false;
    for (size_t i = 1; i < end; ++i) {
        if (!std::isspace(static_cast<unsigned char>(json[i]))) {
            hasValue = true;
            break;
        }
    }
    json.insert(end, (hasValue ? "," : "") + propertyJson);
}

std::string HardenPluginConfigJson(const std::string& key, std::string json) {
    const auto normalized = ToLowerAscii(key);
    auto ensure = [&](const std::string& propertyName, const std::string& propertyJson) {
        if (!JsonKeyExistsCi(json, propertyName)) {
            InsertJsonPropertyBeforeEnd(json, propertyJson);
        }
    };

    if (normalized == "vehicle-rental") {
        ensure("EnableRuntimeVehicleReturnDestroy", R"JSON("EnableRuntimeVehicleReturnDestroy":true)JSON");
        ensure("EnableRuntimeVehicleExpireDestroy", R"JSON("EnableRuntimeVehicleExpireDestroy":true)JSON");
        ensure("EnableRuntimeVehicleTrackAfterSpawn", R"JSON("EnableRuntimeVehicleTrackAfterSpawn":true)JSON");
        ensure("EnableRuntimeVehicleGenericClassFallback", R"JSON("EnableRuntimeVehicleGenericClassFallback":false)JSON");
        ensure("CaptureVehiclesBeforeSpawn", R"JSON("CaptureVehiclesBeforeSpawn":false)JSON");
        ensure("ExpireDestroyMaxAttempts", R"JSON("ExpireDestroyMaxAttempts":6)JSON");
        ensure("RuntimeDestroyResolveCooldownSeconds", R"JSON("RuntimeDestroyResolveCooldownSeconds":45)JSON");
        ensure("CleanupMaxPerRun", R"JSON("CleanupMaxPerRun":1)JSON");
        ensure("RefreshPlayerBeforeVehicleSpawn", R"JSON("RefreshPlayerBeforeVehicleSpawn":false)JSON");
        ensure("GlobalSpawnCooldownSeconds", R"JSON("GlobalSpawnCooldownSeconds":60)JSON");
        ensure("PlayerSpawnCooldownSeconds", R"JSON("PlayerSpawnCooldownSeconds":600)JSON");
        ensure("VipVehicles", R"JSON("VipVehicles":[])JSON");
    } else if (normalized == "base-loot-collector") {
        // Keep the API fallback capable of creating the same bounded scan
        // configuration as the canonical bridge template.  Without these
        // fields the panel could save a config which Lua correctly rejects
        // fail-closed as an unbounded/unknown overlap route.
        ensure("FlagZoneShape", R"JSON("FlagZoneShape":"square")JSON");
        ensure("FlagZoneHalfExtentCm", R"JSON("FlagZoneHalfExtentCm":5000)JSON");
        ensure("CollectWholeFlagZone", R"JSON("CollectWholeFlagZone":true)JSON");
        ensure("EnableSphereOverlapScan", R"JSON("EnableSphereOverlapScan":true)JSON");
        ensure("SphereOverlapObjectTypes", R"JSON("SphereOverlapObjectTypes":"0;1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30;31")JSON");
        ensure("SphereOverlapMaxActors", R"JSON("SphereOverlapMaxActors":1000)JSON");
        ensure("SphereOverlapWorkBudgetMs", R"JSON("SphereOverlapWorkBudgetMs":120)JSON");
        ensure("AllowRuntimeFlagFallback", R"JSON("AllowRuntimeFlagFallback":false)JSON");
        ensure("AllowGlobalInventoryUserScan", R"JSON("AllowGlobalInventoryUserScan":false)JSON");
        ensure("DatabaseMode", R"JSON("DatabaseMode":"native-dll")JSON");
        ensure("AllowSqliteExeFallback", R"JSON("AllowSqliteExeFallback":false)JSON");
        ensure("RelocateBeforeMove", R"JSON("RelocateBeforeMove":false)JSON");
        ensure("AllowUnsafeDropAroundFallback", R"JSON("AllowUnsafeDropAroundFallback":false)JSON");
        ensure("ControlledRelocate", R"JSON("ControlledRelocate":true)JSON");
        ensure("ControlledRelocateDelayMs", R"JSON("ControlledRelocateDelayMs":1200)JSON");
        ensure("ControlledRelocateMaxPerRun", R"JSON("ControlledRelocateMaxPerRun":25)JSON");
        ensure("RelocateIfFartherThanCm", R"JSON("RelocateIfFartherThanCm":650)JSON");
        ensure("MaxScannedItems", R"JSON("MaxScannedItems":5000)JSON");
        ensure("CombinedItemScanLimit", R"JSON("CombinedItemScanLimit":5000)JSON");
        ensure("CollectAllOnSingleCommand", R"JSON("CollectAllOnSingleCommand":true)JSON");
        ensure("BatchItemsPerTick", R"JSON("BatchItemsPerTick":1)JSON");
        ensure("InventoryLocationMaxAttempts", R"JSON("InventoryLocationMaxAttempts":16)JSON");
        ensure("BatchDelayMs", R"JSON("BatchDelayMs":200)JSON");
        ensure("MoveWorkBudgetMs", R"JSON("MoveWorkBudgetMs":90)JSON");
        ensure("SingleItemMoveBudgetMs", R"JSON("SingleItemMoveBudgetMs":30)JSON");
        ensure("ScanWorkBudgetMs", R"JSON("ScanWorkBudgetMs":0)JSON");
        ensure("ChestScanWorkBudgetMs", R"JSON("ChestScanWorkBudgetMs":0)JSON");
        ensure("InventoryLocationOccupancyBatch", R"JSON("InventoryLocationOccupancyBatch":true)JSON");
        ensure("EnableNativeStoreRpc", R"JSON("EnableNativeStoreRpc":false)JSON");
        ensure("NativeStoreRpcExclusive", R"JSON("NativeStoreRpcExclusive":true)JSON");
        ensure("NativeStoreRpcVerifyDelayMs", R"JSON("NativeStoreRpcVerifyDelayMs":180)JSON");
        ensure("NativeStoreRpcMaxPerRun", R"JSON("NativeStoreRpcMaxPerRun":1)JSON");
    } else if (normalized == "fast-travel") {
        ensure("MinArrivalZ", R"JSON("MinArrivalZ":12000)JSON");
    } else if (normalized == "battlepass") {
        ensure("DelayAfterJoinSeconds", R"JSON("DelayAfterJoinSeconds":60)JSON");
        ensure("RetryWindowSeconds", R"JSON("RetryWindowSeconds":300)JSON");
        ensure("InterItemDelayMs", R"JSON("InterItemDelayMs":1500)JSON");
    } else if (normalized == "item-upgrade") {
        ensure("AllowSameItemId", R"JSON("AllowSameItemId":true)JSON");
    }
    return json;
}

std::string PluginConfigJson(const std::wstring& baseDir, const std::string& key, const std::string& fallbackJson, bool* legacyAliasOnly = nullptr) {
    const auto canonicalKey = CanonicalPluginConfigKey(key);
    const bool legacyOnly = HasLegacyOnlyPluginConfigAlias(baseDir, canonicalKey);
    if (legacyAliasOnly != nullptr) *legacyAliasOnly = legacyOnly;
    for (const auto& path : {
        PluginConfigOverlayPath(baseDir, canonicalKey),
        PluginConfigLegacyOverlayPath(baseDir, canonicalKey),
        PluginConfigPath(baseDir, canonicalKey),
        PluginConfigLegacyPath(baseDir, canonicalKey)
    }) {
        // A prior managed save can leave only the recovery overlay available.
        // Read it as the effective config rather than silently falling through
        // to defaults or another module location.
        std::string text;
        if (TryReadPluginConfigJsonText(path, text)) return HardenPluginConfigJson(canonicalKey, text);
    }
    for (const auto& path : PluginConfigLegacyAliasPaths(baseDir, canonicalKey)) {
        std::string text;
        if (TryReadPluginConfigJsonText(path, text)) return HardenPluginConfigJson(canonicalKey, text);
    }
    return HardenPluginConfigJson(canonicalKey, fallbackJson);
}

bool TopLevelBoolField(const std::string& json, const std::string& key, bool fallback) {
    const auto wanted = ToLowerAscii(key);
    int objectDepth = 0;
    int arrayDepth = 0;

    for (size_t i = 0; i < json.size(); ++i) {
        const char ch = json[i];
        if (ch == '{') {
            ++objectDepth;
            continue;
        }
        if (ch == '}') {
            objectDepth = std::max(0, objectDepth - 1);
            continue;
        }
        if (ch == '[') {
            ++arrayDepth;
            continue;
        }
        if (ch == ']') {
            arrayDepth = std::max(0, arrayDepth - 1);
            continue;
        }
        if (ch != '"') continue;

        std::string token;
        bool esc = false;
        size_t j = i + 1;
        for (; j < json.size(); ++j) {
            const char sj = json[j];
            if (esc) {
                token.push_back(sj);
                esc = false;
                continue;
            }
            if (sj == '\\') {
                esc = true;
                continue;
            }
            if (sj == '"') break;
            token.push_back(sj);
        }
        if (j >= json.size()) return fallback;
        i = j;

        if (objectDepth != 1 || arrayDepth != 0 || ToLowerAscii(token) != wanted) continue;
        size_t pos = i + 1;
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        if (pos >= json.size() || json[pos] != ':') continue;
        ++pos;
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        const auto raw = ToLowerAscii(TrimAscii(json.substr(pos, 5)));
        if (raw.rfind("true", 0) == 0 || raw.rfind("1", 0) == 0) return true;
        if (raw.rfind("false", 0) == 0 || raw.rfind("0", 0) == 0) return false;
        return fallback;
    }

    return fallback;
}

std::string PluginObjectJson(
    const std::wstring& baseDir,
    const std::string& key,
    const std::string& name,
    const std::string& description,
    const std::string& category,
    const std::string& defaultConfig) {
    bool legacyAliasOnly = false;
    const auto config = PluginConfigJson(baseDir, key, defaultConfig, &legacyAliasOnly);
    const bool legacyAliasConflict = !legacyAliasOnly && HasLegacyPluginConfigAlias(baseDir, key);
    const bool enabled = TopLevelBoolField(config, "Enabled", true);
    std::ostringstream ss;
    ss << "{\"key\":\"" << JsonEscape(key)
       << "\",\"name\":\"" << JsonEscape(name)
       << "\",\"description\":\"" << JsonEscape(description)
       << "\",\"category\":\"" << JsonEscape(category)
       << "\",\"config\":" << config
       << ",\"enabled\":" << (enabled ? "true" : "false")
       << ",\"migrationRequired\":" << (legacyAliasOnly ? "true" : "false")
       << ",\"legacyAliasConflict\":" << (legacyAliasConflict ? "true" : "false")
       << ",\"runtimeConfigActive\":" << (legacyAliasOnly ? "false" : "true")
       << "}";
    return ss.str();
}

std::string DefaultFastTravelConfigJson() {
    return R"JSON({"Enabled":true,"TransferCooldownMinutes":60,"RatePerMeter":0.04,"FixedFare":0,"TeleportDelaySeconds":5,"TravelConfirmationSeconds":90,"CancelMoveDistance":150,"SuccessArrivalDistance":1500,"MinArrivalZ":12000,"Outposts":[{"DisplayName":"A0 Trader","CommandAlias":"a0","CenterZone":[-621973.438,-557260.438],"ArrivalPoint":[-621973.438,-557260.438,50],"Price":0},{"DisplayName":"B4 Trader","CommandAlias":"b4","CenterZone":[571278.25,-224427.219],"ArrivalPoint":[571278.25,-224427.219,50],"Price":0},{"DisplayName":"C2 Trader","CommandAlias":"c2","CenterZone":[-153247.156,289822.219],"ArrivalPoint":[-153247.156,289822.219,50],"Price":0},{"DisplayName":"Z3 Trader","CommandAlias":"z3","CenterZone":[24006.01,-676488.25],"ArrivalPoint":[24006.01,-676488.25,50],"Price":0}]})JSON";
}

std::string DefaultVehicleRentalConfigJson() {
    return R"JSON({"Enabled":true,"DefaultRentalMinutes":10,"MinRentalMinutes":10,"MaxRentalMinutes":60,"ChargePenaltyOnMissingVehicle":true,"EnableRuntimeVehicleReturnDestroy":true,"EnableRuntimeVehicleExpireDestroy":true,"EnableRuntimeVehicleTrackAfterSpawn":true,"EnableRuntimeVehicleGenericClassFallback":false,"CaptureVehiclesBeforeSpawn":false,"ExpireDestroyMaxAttempts":6,"RuntimeDestroyResolveCooldownSeconds":45,"CleanupMaxPerRun":1,"RefreshPlayerBeforeVehicleSpawn":false,"GlobalSpawnCooldownSeconds":60,"PlayerSpawnCooldownSeconds":600,"DefaultMissingVehiclePenalty":25000,"WarningMinutesBeforeExpiry":[5,1],"Vehicles":[{"Alias":"rager","DisplayName":"Rager","AssetName":"BPC_Rager","PricePer10Minutes":15000,"InitialCharge":5000,"DefaultMinutes":10,"MaxMinutes":120,"MissingVehiclePenalty":50000},{"Alias":"wolfswaggen","DisplayName":"WolfsWagen","AssetName":"BPC_WolfsWagen","PricePer10Minutes":12000,"InitialCharge":4000,"DefaultMinutes":10,"MaxMinutes":120,"MissingVehiclePenalty":50000},{"Alias":"laika","DisplayName":"Laika","AssetName":"BPC_Laika","PricePer10Minutes":8000,"InitialCharge":4000,"DefaultMinutes":10,"MaxMinutes":120,"MissingVehiclePenalty":50000},{"Alias":"Dirtbike","DisplayName":"Транспорт","AssetName":"BPC_Dirtbike","PricePer10Minutes":2500,"InitialCharge":1000,"DefaultMinutes":10,"MaxMinutes":120,"MissingVehiclePenalty":25000}],"VipVehicles":[]})JSON";
}

std::string DefaultBattlepassConfigJson() {
    auto itemsForDay = [](int day, bool vip) -> const char* {
        if (day == 2) return vip ? "Apple_2|4" : "Apple_2|2";
        if (day == 5) return vip ? "CannedGoulash|2" : "CannedGoulash|1";
        if (day == 10) return vip ? "Emergency_bandage_Big|2" : "Emergency_bandage_Big|1";
        if (day == 20) return vip ? "Apple_2|4;CannedGoulash|2" : "Apple_2|2;CannedGoulash|1";
        if (day == 30) return vip ? "MRE_Cheeseburger|2;Emergency_bandage_Big|2" : "MRE_Cheeseburger|1;Emergency_bandage_Big|1";
        return "";
    };
    auto moneyForDay = [](int day, bool vip) {
        if (vip) {
            if (day == 1) return 5000;
            if (day == 2) return 8000;
            if (day == 3) return 3000;
            return 2500 + day * 400;
        }
        if (day == 1) return 3000;
        if (day == 2) return 5000;
        if (day == 3) return 1000;
        return 1000 + day * 250;
    };

    auto appendRewards = [&](std::ostringstream& ss, bool vip) {
        for (int day = 1; day <= 30; ++day) {
            if (day > 1) ss << ",";
            const int gold = vip ? (day % 5 == 0 ? 2 : 0) : ((day == 7 || day == 14 || day == 21 || day == 30) ? 1 : 0);
            const int fame = vip ? (day % 3 == 0 ? 50 : 0) : (day % 5 == 0 ? 25 : 0);
            ss << "{\"Enabled\":true,\"Day\":" << day
               << ",\"MoneyAmount\":" << moneyForDay(day, vip)
               << ",\"GoldAmount\":" << gold
               << ",\"FameAmount\":" << fame
               << ",\"ItemsText\":\"" << itemsForDay(day, vip)
               << "\",\"Message\":\"" << (vip ? "VIP Battlepass" : "Battlepass")
               << " награда {day}/{maxDays} получена.\"}";
        }
    };

    std::ostringstream ss;
    ss << R"JSON({"Name":"Battlepass","Description":"Автоматическая 30-дневная серия наград за реальные дни входа. Выдача начинается через минуту после входа, чтобы персонаж успел прогрузиться.","Enabled":true,"MaxDays":30,"DelayAfterJoinSeconds":60,"PollIntervalMs":5000,"RetryWindowSeconds":300,"InterItemDelayMs":1500,"RequiredPermission":"","SuccessMessage":"Battlepass награда {day}/{maxDays} выдана.","VipSettings":{"Enabled":true,"RequiredPermission":"battlepass.vip","SuccessMessage":"VIP Battlepass награда {day}/{maxDays} выдана."},"Rewards":[)JSON";
    appendRewards(ss, false);
    ss << R"JSON(],"VipRewards":[)JSON";
    appendRewards(ss, true);
    ss << R"JSON(]})JSON";
    return ss.str();
}

std::string DefaultScheduledEventsConfigJson() {
    return R"JSON({"Enabled":false,"Name":"Планировщик заданий","Description":"Задания сервера по времени или интервалу: cargo drop, world events, admin-команды и наборы предметов в точках карты.","Instructions":["Включите модуль, добавьте или выберите задание, затем нажмите Сохранить изменения.","ScheduleTimes теперь исполняется воркером по локальному времени сервера: 12:00, 18:30. Если ScheduleTimes пустой, работает IntervalMinutes.","Mode=Airdrop вызывает SCUM cargo drop командой ScheduleWorldEvent BP_CargoDropEvent X={X} Y={Y} Z={Z}.","Mode=WorldEvent запускает выбранный класс world event из dump-подтверждённых классов через ScheduleWorldEvent {WorldEventClass} X={X} Y={Y} Z={Z}.","Mode=SpawnItems выдаёт набор в точку карты. Формат ItemsText: ItemId|Количество;ItemId2|Количество.","Mode=Command выполняет одну проверенную admin-команду без символа #."],"WorldEventClasses":["BP_CargoDropEvent","BP_EncounterCargoDropEvent","BP_DeathmatchGameEvent","BP_TeamDeathmatchGameEvent","BP_CTFGameEvent","BP_DropZoneGameEvent"],"MaxRunsPerTick":1,"PollIntervalMs":15000,"Jobs":[{"Enabled":false,"Name":"Cargo drop B2 в 12:00 и 18:00","Mode":"Airdrop","ScheduleTimes":"12:00, 18:00","IntervalMinutes":60,"RunOnStartup":false,"PointGroup":"airdrop","ItemSet":"","WorldEventClass":"BP_CargoDropEvent","CommandTemplate":"ScheduleWorldEvent BP_CargoDropEvent X={X} Y={Y} Z={Z}","Announcement":"[Ивент] Cargo drop вызван в районе {Point}.","MaxItemsPerRun":30,"PointCount":1},{"Enabled":false,"Name":"World event DropZone C2 в 20:00","Mode":"WorldEvent","ScheduleTimes":"20:00","IntervalMinutes":180,"RunOnStartup":false,"PointGroup":"world-event","ItemSet":"","WorldEventClass":"BP_DropZoneGameEvent","CommandTemplate":"ScheduleWorldEvent {WorldEventClass} X={X} Y={Y} Z={Z}","Announcement":"[Ивент] {WorldEventClass} запущен в районе {Point}.","MaxItemsPerRun":1,"PointCount":1},{"Enabled":false,"Name":"Тайник с лутом в 21:00","Mode":"SpawnItems","ScheduleTimes":"21:00","IntervalMinutes":120,"RunOnStartup":false,"PointGroup":"stash","ItemSet":"basic","WorldEventClass":"","CommandTemplate":"","Announcement":"[Ивент] Тайник {ItemSet} появился в районе {Point}.","MaxItemsPerRun":30,"PointCount":1},{"Enabled":false,"Name":"Команда по расписанию","Mode":"Command","ScheduleTimes":"03:55","IntervalMinutes":1440,"RunOnStartup":false,"PointGroup":"","ItemSet":"","WorldEventClass":"","CommandTemplate":"Announce Плановая проверка сервера.","Announcement":"","MaxItemsPerRun":1,"PointCount":1}],"Points":[{"Name":"B2 cargo","Group":"airdrop","X":-107205,"Y":-250108,"Z":27875.398,"Radius":0},{"Name":"C2 world event","Group":"world-event","X":-153247,"Y":289822,"Z":200,"Radius":150},{"Name":"C2 лес","Group":"stash","X":-153247,"Y":289822,"Z":200,"Radius":150}],"ItemSets":[{"Name":"basic","Weight":1,"ItemsText":"Apple_2|2;CannedGoulash|1;Emergency_bandage_Big|1"},{"Name":"cargo","Weight":1,"ItemsText":"MRE_TunaSalad|2;Water_05l|2;Emergency_bandage_Big|2"}]})JSON";
}

std::string PluginsJson(const std::wstring& baseDir) {
    const std::vector<std::string> plugins = {
        PluginObjectJson(baseDir, "welcome-pack", "Стартовый набор", "Стартовый набор с кулдауном и подробной выдачей предметов.", "players",
            R"JSON({"Enabled":true,"RequiredPermission":"","CooldownHours":30000,"SerializeClaimsGlobally":true,"InterItemDelayMs":1500,"SuccessMessage":"Стартовый набор выдан.","Items":[{"ItemId":"Bamboo_Hat_02","Quantity":1},{"ItemId":"Beijing_Shoes_04","Quantity":1},{"ItemId":"Tang_pants_04","Quantity":1},{"ItemId":"Tang_shirt_04","Quantity":1},{"ItemId":"Apple_2","Quantity":1},{"ItemId":"BeefRavioli","Quantity":1},{"ItemId":"CannedGoulash","Quantity":1},{"ItemId":"Emergency_bandage_Big","Quantity":1}]})JSON"),
        PluginObjectJson(baseDir, "character-packs", "Пресеты персонажа", "Панельные пресеты атрибутов, навыков и командных пакетов. Игровые slash-команды скрыты от игроков.", "players",
            R"JSON({"Enabled":true,"RequiredPermission":"","DefaultAttributes":{"Strength":8,"Constitution":8,"Dexterity":8,"Intelligence":8},"SkillPresets":[{"Name":"rifles-core","Skill":"rifles","Level":3,"Experience":0,"Description":"Быстрый пресет стрелкового навыка."},{"Name":"medical-core","Skill":"medical","Level":3,"Experience":0,"Description":"Быстрый пресет медицины."},{"Name":"driving-core","Skill":"driving","Level":3,"Experience":0,"Description":"Быстрый пресет вождения."}],"Packs":[{"Name":"fullstats","Aliases":["full"],"Description":"Быстрый полный пресет базовых атрибутов живому игроку.","Enabled":true,"Permission":"","CooldownHours":0,"SuccessMessage":"Атрибуты персонажа обновлены.","Actions":[{"Type":"PlayerCommand","Value":"SetAttributes 8 8 8 8","StopOnFailure":true}]}]})JSON"),
        PluginObjectJson(baseDir, "daily-pack", "Ежедневный пак", "Ежедневная выдача предметов игроку через безопасную очередь WelcomePack.", "players",
            R"JSON({"Name":"Ежедневный пак","Description":"Ежедневная выдача предметов игроку через безопасную очередь WelcomePack.","Enabled":true,"CooldownHours":24,"RequiredPermission":"","SuccessMessage":"Ежедневный пак выдан.","VipItems":[{"ItemId":"Apple_2","Quantity":1}],"VipSettings":{"Enabled":true,"RequiredPermission":"daily-pack.vip","CooldownHours":12,"MoneyAmount":5000,"GoldAmount":5,"FameAmount":50,"SuccessMessage":"VIP бонус ежедневного пака выдан."},"Items":[{"ItemId":"Apple_2","Quantity":2},{"ItemId":"CannedGoulash","Quantity":1},{"ItemId":"Emergency_bandage_Big","Quantity":1}]})JSON"),
        PluginObjectJson(baseDir, "battlepass", "Battlepass", "Автоматическая 30-дневная серия наград за реальные дни входа.", "players",
            DefaultBattlepassConfigJson()),
        PluginObjectJson(baseDir, "vip-system", "VIP игроки", "VIP-участники по SteamID64, сроки, уровни, права и отдельные лимиты модулей.", "players",
            R"JSON({"Enabled":true,"DefaultTier":"vip","DefaultDurationDays":30,"ExtendExisting":true,"BasePermissions":["vip.active","welcome-pack.vip","sethome.vip","daily-pack.vip","battlepass.vip","sector-scan.vip","vehicle-rental.vip","base-loot.vip"],"TierPermissions":{"premium":["vip.premium"]},"Members":[],"Features":{"WelcomePack":{"Enabled":true,"Items":[]},"DailyPack":{"Enabled":true,"CooldownHours":12,"Items":[]},"Battlepass":{"Enabled":true},"HomeSystem":{"Enabled":true,"MaxHomes":5},"SectorScan":{"Enabled":true,"Free":true,"CooldownSeconds":30},"VehicleRental":{"Enabled":true,"DiscountPercent":25,"DefaultMinutes":30,"MaxMinutes":180,"SpawnCooldownSeconds":60},"BaseLootCollector":{"Enabled":true,"RadiusCm":7000,"CooldownSeconds":60,"MaxItemsPerRun":150}},"Wargm":{"Enabled":true,"DefaultDurationDays":30,"SuccessMessage":"VIP активирован."}})JSON"),
        PluginObjectJson(baseDir, "fast-travel", "Быстрое перемещение", "Платные маршруты, задержка, кулдаун и отмена при движении.", "players",
            DefaultFastTravelConfigJson()),
        PluginObjectJson(baseDir, "home-system", "Домашние точки", "Именные дома, лимиты, кулдауны и возврат к сохранённым точкам.", "players",
            R"JSON({"Enabled":true,"DefaultMaxHomes":2,"VipMaxHomes":5,"VipPermission":"sethome.vip","TeleportDelaySeconds":0,"TeleportCooldownMinutes":0,"CancelMoveDistance":150,"SuccessArrivalDistance":600})JSON"),
        PluginObjectJson(baseDir, "vehicle-rental", "Аренда транспорта", "Аренда транспорта с ценой, сроком, возвратом и состоянием.", "vehicles",
            DefaultVehicleRentalConfigJson()),
        PluginObjectJson(baseDir, "money-transfer", "Переводы денег", "Игроки переводят валюту друг другу командой /sendmoney.", "commerce",
            R"JSON({"Name":"Переводы денег","Description":"Игроки переводят деньги друг другу командой /sendmoney <ник|SteamID> <сумма>.","Enabled":true,"RequiredPermission":"","Currency":"Normal","MinAmount":100,"MaxAmount":100000,"DailyLimit":500000,"CooldownSeconds":30,"FeePercent":0,"FeeFixed":0,"RequireBothOnline":true,"AllowSelfTransfer":false,"SuccessMessage":"Перевод отправлен: ${amount} игроку {target}.","ReceivedMessage":"Получен перевод ${amount} от {sender}.","UsageMessage":"Формат: /sendmoney <ник|SteamID> <сумма>."})JSON"),
        PluginObjectJson(baseDir, "item-upgrade", "Апгрейд предметов", "Платная замена предмета в руках на настроенный улучшенный вариант.", "commerce",
            R"JSON({"Name":"Апгрейд предметов","Description":"Заменяет предмет в руках на настроенный ResultItemId после оплаты. Включайте только для проверенных пар предметов.","Enabled":false,"AllowReplacement":false,"AllowSameItemId":true,"RequiredPermission":"","CooldownSeconds":60,"RequireItemInHands":true,"SpawnResultNearPlayer":true,"SuccessMessage":"Апгрейд выполнен: {source} -> {result}.","Rules":[{"Enabled":false,"Alias":"m82-light","DisplayName":"Лёгкий M82A1","SourceItemId":"Weapon_M82A1","ResultItemId":"Weapon_M82A1","Weight":1,"Health":0,"Uses":0,"Dirtiness":0,"AmmoCount":0,"CashValue":0,"KeyCardSector":"","CostMoney":10000,"CostGold":0,"CostFame":0,"RequiredCostItemId":"","RequiredCostItemQuantity":0,"SuccessMessage":"Предмет улучшен. Заберите новую версию рядом с собой."}]})JSON"),
        PluginObjectJson(baseDir, "wargm-shop", "Магазин Wargm", "Очередь покупок по SteamID с правилами безопасной выдачи.", "commerce",
            R"JSON({"Enabled":false,"ProjectId":0,"ApiKey":"","WargmServerId":0,"PollIntervalSeconds":0,"AutoDeliverPending":true,"AutoDeliveryMaxPerRun":5,"DeliveryDelaySeconds":0,"RetryDelaySeconds":60,"MaxDeliveryAttempts":0,"DeliveryFilter":"","RequireRecipientName":false,"DeliverOnlyToOnlinePlayers":true,"AnnounceBeforeDelivery":true,"AnnouncementTemplate":"[Wargm] {player}, покупка \"{offerTitle}\" готовится к выдаче: {delivery}.","BroadcastDeliveries":false,"AllowUnsafeCommandDelivery":false,"UnsafeCommandInterItemDelayMs":5000,"UnsafeCommandInterOperationCooldownSeconds":90,"UnsafeCommandMaxItemsPerOperation":128,"UnsafeCommandMaxQuantityPerItem":10,"VehicleGlobalSpawnCooldownSeconds":20,"VehiclePlayerSpawnCooldownSeconds":60,"UseClaimInsteadOfSuccess":false,"Rules":[{"Enabled":true,"MatchOfferId":"","MatchItemId":"","MatchTitleContains":"Starter Pack","DeliveryMode":"SpawnItem","DeliveryLabel":"Starter Pack","ItemId":"CannedGoulash","Items":[],"VehicleAsset":"","Quantity":1,"Amount":0,"SkillName":"","SkillLevel":0,"SkillExperience":0,"Strength":0,"Constitution":0,"Dexterity":0,"Intelligence":0,"CommandTemplate":"","SuccessMessage":"Покупка Wargm выдана."}]})JSON"),
        PluginObjectJson(baseDir, "gamestores-shop", "Магазин GameStores", "Интеграция с GameStores: получение корзины, безопасная выдача по /pay и подтверждение покупок.", "commerce",
            R"JSON({"Enabled":false,"ShopId":0,"SecretKey":"","ServerId":0,"PollIntervalSeconds":0,"AutoDeliverPending":true,"RequirePlayerClaim":true,"PlayerClaimCommand":"/pay","ClaimCooldownSeconds":10,"ManualClaimMaxPerRun":5,"AutoDeliveryMaxPerRun":5,"DeliveryDelaySeconds":0,"RetryDelaySeconds":60,"MaxDeliveryAttempts":0,"DeliverOnlyToOnlinePlayers":true,"AllowDirectItemIdDelivery":false,"AllowUnsafeCommandDelivery":false,"HttpTimeoutSeconds":20,"Rules":[{"Enabled":true,"MatchBucketId":"","MatchProductId":"","MatchItemId":"","MatchTitleContains":"Starter Pack","MatchCommandContains":"","DeliveryMode":"SpawnItem","DeliveryLabel":"Starter Pack","ItemId":"CannedGoulash","Items":[],"VehicleAsset":"","Quantity":1,"Amount":0,"SkillName":"","SkillLevel":0,"SkillExperience":0,"DurationDays":0,"Tier":"","Strength":0,"Constitution":0,"Dexterity":0,"Intelligence":0,"CommandTemplate":"","SuccessMessage":"Покупка GameStores выдана."}]})JSON"),
        PluginObjectJson(baseDir, "bounty-hunt", "Охота за головами", "Серии убийств, bounty-цели, топ охотников и награды.", "events",
            R"JSON({"Enabled":true,"BroadcastThreshold":3,"HeroThreshold":5,"MaxBoardEntries":5,"LeaderKillRewardMode":"None","LeaderKillRewardAmount":0,"BroadcastLeaderKillReward":true})JSON"),
        PluginObjectJson(baseDir, "server-kill-feed", "Лента убийств", "Глобальные сообщения об убийствах с оружием и дистанцией.", "events",
            R"JSON({"Enabled":true,"Prefix":"WILD","Source":"SaveFilesLogs","UseRuntimeHooks":false,"AnnounceSuicides":true,"IncludeDistance":true,"IncludeWeapon":true,"FanoutToPlayers":false,"DuplicateWindowSeconds":4})JSON"),
        PluginObjectJson(baseDir, "scheduled-events", "Планировщик заданий", "Задания сервера по времени или интервалу: cargo drop, world events, admin-команды и наборы предметов в точках карты.", "events",
            DefaultScheduledEventsConfigJson()),
        PluginObjectJson(baseDir, "base-loot-collector", "Сбор лута базы", "Команда /loot собирает лежащий лут в сундуки своего флага с проверкой отряда.", "players",
            R"JSON({"Name":"Сбор лута базы","Description":"Команда /loot собирает лежащий рядом лут в сундуки того же флага. Доступ по умолчанию только участникам отряда владельца флага.","Enabled":false,"RequiredPermission":"","CommandAliases":["loot","collectloot","sortloot"],"RequireSquadOwnedFlag":true,"AllowFlagOwnerWithoutSquad":true,"AllowAdminsBypass":false,"RadiusCm":5000,"CooldownSeconds":120,"MaxItemsPerRun":80,"MaxChestsPerRun":40,"MaxScannedItems":5000,"MaxScannedChests":160,"CombinedItemScanLimit":5000,"CollectAllOnSingleCommand":true,"BatchItemsPerTick":1,"BatchDelayMs":200,"MoveWorkBudgetMs":90,"SingleItemMoveBudgetMs":30,"ScanWorkBudgetMs":0,"ChestScanWorkBudgetMs":0,"RequireOnFloorPresence":true,"NameContains":true,"DefaultChestName":"Loot","FallbackToDefaultChest":true,"DatabaseMode":"native-dll","AllowSqliteExeFallback":false,"SqliteExe":"","DatabasePath":"..\\..\\Saved\\SaveFiles\\SCUM.db","DryRun":false,"RelocateBeforeMove":false,"AllowUnsafeDropAroundFallback":false,"ControlledRelocate":true,"ControlledRelocateDelayMs":1200,"ControlledRelocateMaxPerRun":25,"RelocateIfFartherThanCm":650,"RelocateZOffset":25,"InventoryLocationOccupancyBatch":true,"InventoryLocationMaxAttempts":16,"EnableNativeStoreRpc":false,"NativeStoreRpcExclusive":true,"NativeStoreRpcVerifyDelayMs":180,"NativeStoreRpcMaxPerRun":1,"SuccessMessage":"[Лут] Перемещено: {Moved}. Пропущено: {Skipped}.","EmptyMessage":"[Лут] Подходящего лута рядом с флагом не найдено.","NoFlagMessage":"[Лут] Встань внутри зоны своего флага.","AccessDeniedMessage":"[Лут] Сбор доступен только участникам отряда владельца флага.","DbUnavailableMessage":"[Лут] Проверка владельца флага недоступна: native SQLite DLL не прочитала SCUM.db.","VipSettings":{"Enabled":true,"RequiredPermission":"base-loot.vip","RadiusCm":7000,"CooldownSeconds":60,"MaxItemsPerRun":150},"Rules":[{"Enabled":true,"Name":"Оружие","MatchContains":"weapon;rifle;shotgun;pistol;bow;sword;knife;m82;ak47","ChestName":"Weapons"},{"Enabled":true,"Name":"Патроны","MatchContains":"ammo;cal;bullet;magazine;shell;arrow","ChestName":"Ammo"},{"Enabled":true,"Name":"Медицина","MatchContains":"bandage;medical;vitamin;painkiller;antibiotic;firstaid","ChestName":"Medical"},{"Enabled":true,"Name":"Еда и вода","MatchContains":"apple;food;can;water;drink;mre;meat;goulash","ChestName":"Food"},{"Enabled":true,"Name":"Одежда","MatchContains":"jacket;pants;boots;helmet;vest;backpack;gloves;shirt","ChestName":"Gear"}]})JSON"),
        PluginObjectJson(baseDir, "zone-robot-schedule", "Роботы по зонам", "Планировщик включения и отключения зонных команд для роботов/событий.", "events",
            R"JSON({"Name":"Роботы по зонам","Description":"Расписание команд для зон. Используйте только проверенные SCUM admin-команды конкретного сервера.","Enabled":false,"PollIntervalMs":15000,"Rules":[{"Enabled":false,"Name":"B2 ночной режим","Zone":"B2","ScheduleTimesOn":"21:00","ScheduleTimesOff":"06:00","CommandTemplateOn":"Announce Роботы включены в зоне {Zone}.","CommandTemplateOff":"Announce Роботы отключены в зоне {Zone}.","AnnouncementOn":"[Зоны] Роботы включены в {Zone}.","AnnouncementOff":"[Зоны] Роботы отключены в {Zone}."}]})JSON"),
        PluginObjectJson(baseDir, "panel-quests", "Квесты сервера", "Квестовая доска панели с наградами, объявлениями и безопасным claim-режимом.", "events",
            R"JSON({"Name":"Квесты сервера","Description":"Панельная квестовая доска. Безопасный режим выдаёт награду по /quest claim и не удаляет предметы из инвентаря без доказанного парсера.","Enabled":false,"RequiredPermission":"","ClaimCooldownHours":24,"AnnounceOnClaim":true,"ListMessage":"Активные квесты: {quests}","Quests":[{"Enabled":true,"Alias":"north","Title":"Северный заказ","Mode":"Claim","Description":"Тестовый ежедневный квест панели.","TargetItemId":"","TargetCount":0,"RewardMoney":5000,"RewardGold":0,"RewardFame":0,"RewardItemsText":"Apple_2|2","SuccessMessage":"Квест выполнен: {title}.","Announcement":"[Квест] {player} получил награду за {title}."}]})JSON"),
        PluginObjectJson(baseDir, "sector-scan", "Скан сектора", "Платная проверка сектора по live-позициям игроков.", "intel",
            R"JSON({"Enabled":true,"RequiredPermission":"","ScanCost":1000,"CooldownSeconds":120,"IncludeSelf":true,"IncludePlayerNames":true,"MaxListedNames":5,"SuccessMessage":"[Скан] Квадрат {sector}: найдено {count}. {names}","EmptyMessage":"[Скан] Квадрат {sector} пуст.","NotEnoughMoneyMessage":"[Скан] Нужно {cost} денег. Сейчас не хватает.","CooldownMessage":"[Скан] Подожди ещё {seconds} сек."})JSON"),
        PluginObjectJson(baseDir, "private-messages", "Личные сообщения", "Личные сообщения, быстрый ответ и короткая история.", "chat",
            R"JSON({"Enabled":true,"UsePermission":false,"AllowPermission":"pm.use","UseCooldown":true,"CooldownTimeSeconds":3,"EnableLogging":true,"EnableHistory":true})JSON"),
        PluginObjectJson(baseDir, "discord-log", "Логи Discord", "Пересылка чата, входов, выходов, респавнов и убийств в Discord.", "integrations",
            R"JSON({"Enabled":false,"TransportMode":"Webhook","ServerLabel":"","GuildId":"","DefaultChannelId":"","PresenceChannelId":"","ChatChannelId":"","CombatChannelId":"","SystemChannelId":"","DefaultWebhookUrl":"","PresenceWebhookUrl":"","ChatWebhookUrl":"","CombatWebhookUrl":"","SystemWebhookUrl":"","Username":"WILD","AvatarUrl":"","NotifyOnLoad":true,"NotifyPlayerConnected":true,"NotifyPlayerDisconnected":true,"NotifyPlayerRespawned":false,"NotifyPlayerChat":true,"NotifyPlayerKills":true,"NotifyPlayerDeaths":true,"IncludeSteamId":true,"IgnoreSlashCommands":true,"IgnoredChatPrefixes":["/"],"ForwardLocalChat":true,"ForwardGlobalChat":true,"ForwardSquadChat":true,"ForwardAdminChat":false,"MinPostIntervalMs":800,"MaxQueueLength":200,"DuplicateWindowSeconds":15,"HttpTimeoutSeconds":10})JSON")
    };

    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < plugins.size(); ++i) {
        if (i) ss << ",";
        ss << plugins[i];
    }
    ss << "]";
    return ss.str();
}

std::string ItemCatalogJson() {
    return "[]";
}

std::string VehicleCatalogJson() {
    return "[]";
}

std::string SkillCatalogJson() {
    return R"JSON([
{"key":"archery","skill":"Archery","name":"Стрельба из лука","category":"Бой"},
{"key":"aviation","skill":"Aviation","name":"Авиация","category":"Транспорт"},
{"key":"awareness","skill":"Awareness","name":"Внимательность","category":"Поддержка"},
{"key":"brawling","skill":"Brawling","name":"Рукопашный бой","category":"Бой"},
{"key":"camouflage","skill":"Camouflage","name":"Маскировка","category":"Скрытность"},
{"key":"cooking","skill":"Cooking","name":"Готовка","category":"Выживание"},
{"key":"demolition","skill":"Demolition","name":"Подрывное дело","category":"Ремесло"},
{"key":"driving","skill":"Driving","name":"Вождение","category":"Транспорт"},
{"key":"endurance","skill":"Endurance","name":"Выносливость","category":"Физические"},
{"key":"engineering","skill":"Engineering","name":"Инженерия","category":"Ремесло"},
{"key":"farming","skill":"Farming","name":"Фермерство","category":"Выживание"},
{"key":"handgun","skill":"Handgun","name":"Пистолеты","category":"Бой"},
{"key":"medical","skill":"Medical","name":"Медицина","category":"Поддержка"},
{"key":"melee weapons","skill":"Melee Weapons","name":"Холодное оружие","category":"Бой"},
{"key":"motorcycling","skill":"MotorCycling","name":"Мотоциклы","category":"Транспорт"},
{"key":"resistance","skill":"Resistance","name":"Сопротивляемость","category":"Физические"},
{"key":"rifles","skill":"Rifles","name":"Винтовки","category":"Бой"},
{"key":"running","skill":"Running","name":"Бег","category":"Физические"},
{"key":"sniping","skill":"Sniping","name":"Снайперская стрельба","category":"Бой"},
{"key":"stealth","skill":"Stealth","name":"Скрытность","category":"Скрытность"},
{"key":"survival","skill":"Survival","name":"Выживание","category":"Выживание"},
{"key":"tactics","skill":"Tactics","name":"Тактика","category":"Поддержка"},
{"key":"thievery","skill":"Thievery","name":"Воровство","category":"Поддержка"}
])JSON";
}

std::string DownloadsJson() {
    return R"JSON([
{"name":"INSTALL_RU.txt","path":"INSTALL_RU.txt","description":"Короткая инструкция по установке на любой SCUM-хостинг."},
{"name":"INSTALL_CLIENTS_RU.md","path":"INSTALL_CLIENTS_RU.md","description":"Полная инструкция для клиента и диагностика подключения."},
{"name":"PANEL_ACCESS.txt","path":"PANEL_ACCESS.txt","description":"Адрес панели, порт и место API-ключа."},
{"name":"MANIFEST.txt","path":"MANIFEST.txt","description":"Список файлов текущего пакета."},
{"name":"BUILD_INFO.json","path":"BUILD_INFO.json","description":"Информация о сборке и архитектуре."}
])JSON";
}

std::string WebJsonAssetOrFallback(const std::wstring& baseDir, const std::wstring& fileName, const std::string& fallbackJson) {
    auto text = TrimAscii(ReadTextFile(JoinPath(JoinPath(JoinPath(baseDir, L"ScumNeDjin"), L"web"), fileName)));
    if (!text.empty() && (text.front() == '{' || text.front() == '[')) {
        return text;
    }
    return fallbackJson;
}

std::string JsonStringField(const std::string& body, const std::string& key) {
    const auto needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return {};
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = body.find('"', pos);
    if (pos == std::string::npos) return {};
    std::string out;
    bool esc = false;
    for (++pos; pos < body.size(); ++pos) {
        const auto ch = body[pos];
        if (esc) {
            AppendJsonStringEscape(out, ch);
            esc = false;
            continue;
        }
        if (ch == '\\') {
            esc = true;
            continue;
        }
        if (ch == '"') break;
        out.push_back(ch);
    }
    return out;
}

std::string JsonUnquoteRawString(const std::string& rawValue) {
    auto value = TrimAscii(rawValue);
    if (value.size() < 2 || value.front() != '"') return {};
    std::string out;
    bool esc = false;
    for (size_t pos = 1; pos < value.size(); ++pos) {
        const auto ch = value[pos];
        if (esc) {
            AppendJsonStringEscape(out, ch);
            esc = false;
            continue;
        }
        if (ch == '\\') {
            esc = true;
            continue;
        }
        if (ch == '"') break;
        out.push_back(ch);
    }
    return out;
}

std::string JsonRootRawField(const std::string& body, const std::string& key) {
    auto pos = body.find('{');
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < body.size()) {
        while (pos < body.size() && (std::isspace(static_cast<unsigned char>(body[pos])) || body[pos] == ',')) ++pos;
        if (pos >= body.size() || body[pos] == '}') return {};
        if (body[pos] != '"') {
            ++pos;
            continue;
        }

        std::string name;
        bool esc = false;
        ++pos;
        for (; pos < body.size(); ++pos) {
            const char ch = body[pos];
            if (esc) {
                AppendJsonStringEscape(name, ch);
                esc = false;
                continue;
            }
            if (ch == '\\') {
                esc = true;
                continue;
            }
            if (ch == '"') {
                ++pos;
                break;
            }
            name.push_back(ch);
        }

        while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
        if (pos >= body.size() || body[pos] != ':') continue;
        ++pos;
        while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
        if (pos >= body.size()) return {};

        const auto valueStart = pos;
        const char first = body[pos];
        if (first == '"') {
            bool valueEsc = false;
            for (++pos; pos < body.size(); ++pos) {
                const char ch = body[pos];
                if (valueEsc) {
                    valueEsc = false;
                    continue;
                }
                if (ch == '\\') {
                    valueEsc = true;
                    continue;
                }
                if (ch == '"') {
                    ++pos;
                    break;
                }
            }
        } else if (first == '{' || first == '[') {
            const char open = first;
            const char close = first == '{' ? '}' : ']';
            int depth = 0;
            bool inString = false;
            bool valueEsc = false;
            for (; pos < body.size(); ++pos) {
                const char ch = body[pos];
                if (valueEsc) {
                    valueEsc = false;
                    continue;
                }
                if (ch == '\\' && inString) {
                    valueEsc = true;
                    continue;
                }
                if (ch == '"') {
                    inString = !inString;
                    continue;
                }
                if (inString) continue;
                if (ch == open) ++depth;
                if (ch == close) {
                    --depth;
                    if (depth == 0) {
                        ++pos;
                        break;
                    }
                }
            }
        } else {
            while (pos < body.size() && body[pos] != ',' && body[pos] != '}') ++pos;
        }

        auto raw = TrimAscii(body.substr(valueStart, pos - valueStart));
        if (name == key) return raw;
    }
    return {};
}

std::string JsonRootStringField(const std::string& body, const std::string& key) {
    return JsonUnquoteRawString(JsonRootRawField(body, key));
}

int JsonIntField(const std::string& body, const std::string& key, int fallback) {
    const auto needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return fallback;
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return fallback;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    const auto end = body.find_first_not_of("-0123456789", pos);
    const auto raw = body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    return raw.empty() ? fallback : std::atoi(raw.c_str());
}

std::string JsonRawField(const std::string& body, const std::string& key) {
    const auto needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return {};
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) return {};

    const char first = body[pos];
    if (first == '{' || first == '[') {
        const char open = first;
        const char close = first == '{' ? '}' : ']';
        int depth = 0;
        bool inString = false;
        bool esc = false;
        for (size_t i = pos; i < body.size(); ++i) {
            const char ch = body[i];
            if (esc) {
                esc = false;
                continue;
            }
            if (ch == '\\') {
                esc = inString;
                continue;
            }
            if (ch == '"') {
                inString = !inString;
                continue;
            }
            if (inString) continue;
            if (ch == open) ++depth;
            if (ch == close) {
                --depth;
                if (depth == 0) return body.substr(pos, i - pos + 1);
            }
        }
        return {};
    }

    if (first == '"') {
        bool esc = false;
        for (size_t i = pos + 1; i < body.size(); ++i) {
            const char ch = body[i];
            if (esc) {
                esc = false;
                continue;
            }
            if (ch == '\\') {
                esc = true;
                continue;
            }
            if (ch == '"') return body.substr(pos, i - pos + 1);
        }
        return {};
    }

    const auto end = body.find_first_of(",}\r\n", pos);
    return TrimAscii(body.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
}

bool JsonBoolField(const std::string& body, const std::string& key, bool fallback) {
    auto raw = ToLowerAscii(TrimAscii(JsonRawField(body, key)));
    if (raw == "true" || raw == "1") return true;
    if (raw == "false" || raw == "0") return false;
    return fallback;
}

std::string JsonNumberTextField(const std::string& body, const std::string& key, const std::string& fallback) {
    const auto raw = TrimAscii(JsonRawField(body, key));
    if (raw.empty()) return fallback;
    for (const char ch : raw) {
        if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' || ch == '.')) {
            return fallback;
        }
    }
    return raw;
}

struct HttpPostResult {
    bool ok = false;
    int status = 0;
    std::string message;
    std::string body;
};

HttpPostResult HttpPostJson(const std::string& url, const std::string& jsonBody, int timeoutSeconds, const std::wstring& extraHeaders = L"") {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        return { false, 0, "Поддерживаются только http/https webhook URL." };
    }

    const auto wideUrl = Utf8ToWide(url);
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        return { false, 0, "Не удалось разобрать webhook URL." };
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const INTERNET_PORT port = parts.nPort;

    HINTERNET session = WinHttpOpen(
        L"ScumNeDjin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) return { false, 0, "WinHTTP не смог открыть сетевую сессию." };

    const int timeoutMs = std::clamp(timeoutSeconds, 1, 60) * 1000;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return { false, 0, "WinHTTP не смог подключиться к webhook." };
    }

    HINTERNET request = WinHttpOpenRequest(
        connect,
        L"POST",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return { false, 0, "WinHTTP не смог создать webhook-запрос." };
    }

    if (secure) {
        DWORD securityFlags =
            SECURITY_FLAG_IGNORE_UNKNOWN_CA |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
    }

    const std::wstring headers = L"Content-Type: application/json\r\n" + extraHeaders;
    const BOOL sent = WinHttpSendRequest(
        request,
        headers.c_str(),
        static_cast<DWORD>(headers.size()),
        const_cast<char*>(jsonBody.data()),
        static_cast<DWORD>(jsonBody.size()),
        static_cast<DWORD>(jsonBody.size()),
        0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return { false, 0, "Webhook-запрос завершился до получения ответа." };
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);

    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        std::string chunk;
        chunk.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
        chunk.resize(read);
        body += chunk;
        if (body.size() > 512 * 1024) break;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    const bool ok = status >= 200 && status < 300;
    return { ok, static_cast<int>(status), ok ? "HTTP POST выполнен." : "HTTP POST вернул ошибочный статус.", body };
}

bool AppendModuleStateRecord(const std::wstring& baseDir, const std::string& key, const std::string& recordJson);

struct HttpTextResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string message;
};

HttpTextResult HttpGetText(const std::string& url, int timeoutSeconds, const std::wstring& extraHeaders = L"") {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        return { false, 0, {}, "Поддерживаются только http/https URL." };
    }

    const auto wideUrl = Utf8ToWide(url);
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        return { false, 0, {}, "Не удалось разобрать URL." };
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const INTERNET_PORT port = parts.nPort;

    HINTERNET session = WinHttpOpen(
        L"ScumNeDjin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) return { false, 0, {}, "WinHTTP не смог открыть сетевую сессию." };

    const int timeoutMs = std::clamp(timeoutSeconds, 1, 60) * 1000;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return { false, 0, {}, "WinHTTP не смог подключиться." };
    }

    HINTERNET request = WinHttpOpenRequest(
        connect,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return { false, 0, {}, "WinHTTP не смог создать запрос." };
    }

    if (secure) {
        DWORD securityFlags =
            SECURITY_FLAG_IGNORE_UNKNOWN_CA |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
    }

    const BOOL sent = WinHttpSendRequest(
        request,
        extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str(),
        static_cast<DWORD>(extraHeaders.size()),
        nullptr,
        0,
        0,
        0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return { false, 0, {}, "HTTP-запрос завершился до получения ответа." };
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);

    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        std::string chunk;
        chunk.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
        chunk.resize(read);
        body += chunk;
        if (body.size() > 2 * 1024 * 1024) break;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    const bool ok = status >= 200 && status < 300;
    return { ok, static_cast<int>(status), body, ok ? "HTTP-запрос выполнен." : "HTTP вернул ошибочный статус." };
}

struct HttpRawResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string headers;
    std::string message;
};

std::string ReadResponseHeadersUtf8(HINTERNET request) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_RAW_HEADERS_CRLF,
        WINHTTP_HEADER_NAME_BY_INDEX,
        nullptr,
        &bytes,
        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return {};
    std::wstring raw;
    raw.resize(bytes / sizeof(wchar_t));
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_RAW_HEADERS_CRLF,
            WINHTTP_HEADER_NAME_BY_INDEX,
            raw.data(),
            &bytes,
            WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    raw.resize(wcsnlen_s(raw.c_str(), raw.size()));
    return WideToUtf8(raw);
}

HttpRawResult HttpRequestText(
    const std::string& url,
    const std::wstring& method,
    const std::string& body,
    const std::wstring& contentType,
    const std::wstring& extraHeaders,
    int timeoutSeconds) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        return { false, 0, {}, {}, "Поддерживаются только http/https URL." };
    }

    const auto wideUrl = Utf8ToWide(url);
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        return { false, 0, {}, {}, "Не удалось разобрать URL." };
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET session = WinHttpOpen(
        L"ScumNeDjin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) return { false, 0, {}, {}, "WinHTTP не смог открыть сетевую сессию." };

    const int timeoutMs = std::clamp(timeoutSeconds, 1, 60) * 1000;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return { false, 0, {}, {}, "WinHTTP не смог подключиться." };
    }

    HINTERNET request = WinHttpOpenRequest(
        connect,
        method.c_str(),
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return { false, 0, {}, {}, "WinHTTP не смог создать запрос." };
    }

    if (secure) {
        DWORD securityFlags =
            SECURITY_FLAG_IGNORE_UNKNOWN_CA |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
    }

    std::wstring headers = extraHeaders;
    if (!contentType.empty()) headers += L"Content-Type: " + contentType + L"\r\n";
    const BOOL sent = WinHttpSendRequest(
        request,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        static_cast<DWORD>(headers.size()),
        body.empty() ? nullptr : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return { false, 0, {}, {}, "HTTP-запрос завершился до получения ответа." };
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);

    const auto rawHeaders = ReadResponseHeadersUtf8(request);

    std::string responseBody;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        std::string chunk;
        chunk.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
        chunk.resize(read);
        responseBody += chunk;
        if (responseBody.size() > 2 * 1024 * 1024) break;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    const bool ok = status >= 200 && status < 300;
    return { ok, static_cast<int>(status), responseBody, rawHeaders, ok ? "HTTP-запрос выполнен." : "HTTP вернул ошибочный статус." };
}

std::map<std::string, std::string> ParseCookieHeader(const std::string& cookies) {
    std::map<std::string, std::string> jar;
    size_t pos = 0;
    while (pos < cookies.size()) {
        const auto semi = cookies.find(';', pos);
        auto part = TrimAscii(cookies.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos));
        const auto eq = part.find('=');
        if (eq != std::string::npos) {
            const auto name = TrimAscii(part.substr(0, eq));
            const auto value = TrimAscii(part.substr(eq + 1));
            if (!name.empty()) jar[name] = value;
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return jar;
}

std::string CookieHeaderFromJar(const std::map<std::string, std::string>& jar) {
    std::ostringstream ss;
    bool first = true;
    for (const auto& [name, value] : jar) {
        if (name.empty()) continue;
        if (!first) ss << "; ";
        first = false;
        ss << name << "=" << value;
    }
    return ss.str();
}

void MergeSetCookies(std::string& cookies, const std::string& headers) {
    auto jar = ParseCookieHeader(cookies);
    std::istringstream lines(headers);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto lower = ToLowerAscii(line);
        if (lower.rfind("set-cookie:", 0) != 0) continue;
        auto cookie = TrimAscii(line.substr(std::string("set-cookie:").size()));
        const auto semi = cookie.find(';');
        if (semi != std::string::npos) cookie = cookie.substr(0, semi);
        const auto eq = cookie.find('=');
        if (eq == std::string::npos) continue;
        const auto name = TrimAscii(cookie.substr(0, eq));
        const auto value = TrimAscii(cookie.substr(eq + 1));
        if (!name.empty()) jar[name] = value;
    }
    cookies = CookieHeaderFromJar(jar);
}

std::wstring ArkPanelHeaders(const ArkPanelSession& session) {
    std::wstring headers;
    if (!session.cookies.empty()) headers += L"Cookie: " + Utf8ToWide(session.cookies) + L"\r\n";
    if (!session.csrf.empty()) headers += L"X-CSRF-TOKEN: " + Utf8ToWide(session.csrf) + L"\r\n";
    return headers;
}

std::string ExtractCsrfToken(const std::string& html) {
    static const std::regex csrfRe(R"(name\s*=\s*["']CSRF_TOKEN["']\s+content\s*=\s*["']([^"']+)["'])", std::regex::icase);
    std::smatch match;
    if (std::regex_search(html, match, csrfRe)) return match[1].str();
    return {};
}

bool ArkPanelEnabled(const std::wstring& baseDir) {
    const bool hasLogin = !ConfigTextValue(baseDir, "ark_panel_email").empty() &&
        !ConfigTextValue(baseDir, "ark_panel_password").empty();
    const bool hasCookie = !ConfigTextValue(baseDir, "ark_panel_cookie").empty();
    return ConfigBoolValue(baseDir, "ark_panel_enabled", false) &&
        !ConfigTextValue(baseDir, "ark_panel_url").empty() &&
        (hasLogin || hasCookie) &&
        !ConfigTextValue(baseDir, "ark_panel_server_id").empty();
}

std::string ArkPanelBaseUrl(const std::wstring& baseDir) {
    auto url = ConfigTextValue(baseDir, "ark_panel_url", "");
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

bool ArkPanelEnsureSessionLocked(const std::wstring& baseDir, std::string& errorOut) {
    if (!ArkPanelEnabled(baseDir)) {
        errorOut = "Командная консоль хостинга не настроена.";
        return false;
    }

    const auto nowTick = GetTickCount64();
    const auto configuredCookie = ConfigTextValue(baseDir, "ark_panel_cookie", "");
    const bool cookieMode = !configuredCookie.empty();
    if (!g_arkPanelSession.cookies.empty() && (cookieMode || !g_arkPanelSession.csrf.empty()) &&
        nowTick - g_arkPanelSession.refreshedTick < 20ULL * 60ULL * 1000ULL) {
        return true;
    }

    ArkPanelSession session;
    const auto baseUrl = ArkPanelBaseUrl(baseDir);
    if (baseUrl.empty()) {
        errorOut = "Не указан URL панели хостинга для командной консоли.";
        return false;
    }
    if (cookieMode) {
        session.cookies = configuredCookie;
        session.refreshedTick = nowTick;
        g_arkPanelSession = session;
        return true;
    }
    const auto loginPage = HttpRequestText(baseUrl + "/account/login", L"GET", "", L"", L"", 15);
    MergeSetCookies(session.cookies, loginPage.headers);
    session.csrf = ExtractCsrfToken(loginPage.body);
    if (!loginPage.ok || session.csrf.empty()) {
        errorOut = "Не удалось открыть страницу авторизации панели хостинга.";
        return false;
    }

    const auto email = ConfigTextValue(baseDir, "ark_panel_email");
    const auto password = ConfigTextValue(baseDir, "ark_panel_password");
    const auto form = std::string("email=") + UrlEncode(email) +
        "&password=" + UrlEncode(password) +
        "&google_code_auth=&remember_auth_user=remember-me";
    auto loginHeaders = ArkPanelHeaders(session);
    const auto login = HttpRequestText(baseUrl + "/account/login/ajax", L"POST", form,
        L"application/x-www-form-urlencoded", loginHeaders, 20);
    MergeSetCookies(session.cookies, login.headers);
    if (!login.ok || login.body.find("\"status\":\"success\"") == std::string::npos) {
        errorOut = std::string("Ark Hoster login failed: ") + (login.body.empty() ? login.message : login.body.substr(0, 500));
        return false;
    }

    const auto serverId = ConfigTextValue(baseDir, "ark_panel_server_id");
    auto consoleHeaders = ArkPanelHeaders(session);
    const auto console = HttpRequestText(baseUrl + "/servers/control/console/" + UrlEncode(serverId) + "/", L"GET", "",
        L"", consoleHeaders, 20);
    MergeSetCookies(session.cookies, console.headers);
    const auto csrf = ExtractCsrfToken(console.body);
    if (!csrf.empty()) session.csrf = csrf;
    if (!console.ok || session.csrf.empty()) {
        errorOut = "Ark Hoster login ok, but console page/CSRF is unavailable.";
        return false;
    }

    session.refreshedTick = nowTick;
    g_arkPanelSession = session;
    return true;
}

struct ArkCommandResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string message;
};

struct RconPacket {
    int id = 0;
    int type = 0;
    std::string body;
};

std::string LocalRconHost(const std::wstring& baseDir) {
    return ConfigTextValue(baseDir, "server_command_host",
        ConfigTextValue(baseDir, "local_rcon_host", "127.0.0.1"));
}

int LocalRconPort(const std::wstring& baseDir) {
    return ConfigIntValue(baseDir, "server_command_port",
        ConfigIntValue(baseDir, "local_rcon_port", 28015, 1, 65535), 1, 65535);
}

std::string LocalRconPassword(const std::wstring& baseDir) {
    return ConfigTextValue(baseDir, "server_command_password",
        ConfigTextValue(baseDir, "local_rcon_password", ""));
}

bool LocalRconEnabled(const std::wstring& baseDir) {
    return ConfigBoolValue(baseDir, "server_command_enabled",
            ConfigBoolValue(baseDir, "local_rcon_enabled", false)) &&
        !LocalRconHost(baseDir).empty() &&
        !LocalRconPassword(baseDir).empty();
}

int LocalRconTimeoutMs(const std::wstring& baseDir) {
    return ConfigIntValue(baseDir, "server_command_timeout_ms",
        ConfigIntValue(baseDir, "local_rcon_timeout_ms", 8000, 1000, 30000), 1000, 30000);
}

int LocalRconCommandDelayMs(const std::wstring& baseDir) {
    return ConfigIntValue(baseDir, "server_command_delay_ms",
        ConfigIntValue(baseDir, "local_rcon_command_delay_ms", 1500, 0, 10000), 0, 10000);
}

bool AllowUnverifiedServerCommandDelivery(const std::wstring& baseDir) {
    return ConfigBoolValue(baseDir, "allow_unverified_server_command_delivery",
        ConfigBoolValue(baseDir, "allow_unverified_rcon_delivery", false));
}

std::string LocalRconCommandText(std::string command) {
    command = TrimAscii(command);
    while (!command.empty() && command.front() == '#') {
        command.erase(command.begin());
        command = TrimAscii(command);
    }
    return command;
}

void AppendRconI32(std::string& bytes, int value) {
    char raw[4];
    std::memcpy(raw, &value, sizeof(raw));
    bytes.append(raw, sizeof(raw));
}

int ReadRconI32(const char* bytes) {
    int value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool RecvExact(SOCKET sock, char* buffer, int size, std::string& errorOut) {
    int offset = 0;
    while (offset < size) {
        const int got = recv(sock, buffer + offset, size - offset, 0);
        if (got <= 0) {
            const int err = WSAGetLastError();
            errorOut = err == WSAETIMEDOUT ? "Command channel receive timed out" : ("Command channel receive failed: " + std::to_string(err));
            return false;
        }
        offset += got;
    }
    return true;
}

bool SendRconPacket(SOCKET sock, int id, int type, const std::string& body, std::string& errorOut) {
    std::string packet;
    const int size = static_cast<int>(4 + 4 + body.size() + 2);
    AppendRconI32(packet, size);
    AppendRconI32(packet, id);
    AppendRconI32(packet, type);
    packet.append(body);
    packet.push_back('\0');
    packet.push_back('\0');

    const char* ptr = packet.data();
    int remaining = static_cast<int>(packet.size());
    while (remaining > 0) {
        const int sent = send(sock, ptr, remaining, 0);
        if (sent <= 0) {
            errorOut = "Command channel send failed: " + std::to_string(WSAGetLastError());
            return false;
        }
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

bool ReceiveRconPacket(SOCKET sock, RconPacket& packet, std::string& errorOut) {
    char sizeBytes[4];
    if (!RecvExact(sock, sizeBytes, 4, errorOut)) return false;
    const int size = ReadRconI32(sizeBytes);
    if (size < 10 || size > 1024 * 1024) {
        errorOut = "Command packet size is invalid: " + std::to_string(size);
        return false;
    }
    std::string payload(static_cast<size_t>(size), '\0');
    if (!RecvExact(sock, payload.data(), size, errorOut)) return false;
    packet.id = ReadRconI32(payload.data());
    packet.type = ReadRconI32(payload.data() + 4);
    packet.body.assign(payload.data() + 8, static_cast<size_t>(size - 10));
    return true;
}

SOCKET ConnectLocalRconSocket(const std::string& host, int port, int timeoutMs, std::string& errorOut) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const auto portText = std::to_string(port);
    const int gai = getaddrinfo(host.c_str(), portText.c_str(), &hints, &result);
    if (gai != 0 || result == nullptr) {
        errorOut = "Command channel host resolve failed.";
        return INVALID_SOCKET;
    }

    SOCKET sock = INVALID_SOCKET;
    for (auto ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        const DWORD timeout = static_cast<DWORD>(std::clamp(timeoutMs, 1000, 30000));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        if (connect(sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == 0) {
            freeaddrinfo(result);
            return sock;
        }
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(result);
    errorOut = "Command channel connect failed: " + std::to_string(WSAGetLastError());
    return INVALID_SOCKET;
}

ArkCommandResult LocalRconSendConsoleCommandLocked(const std::wstring& baseDir, const std::string& rawCommand) {
    if (!LocalRconEnabled(baseDir)) {
        return { false, 0, {}, "Командный канал сервера не настроен." };
    }
    const auto command = LocalRconCommandText(rawCommand);
    if (command.empty()) return { false, 0, {}, "Команда не задана." };
    if (command.find('\r') != std::string::npos || command.find('\n') != std::string::npos) {
        return { false, 0, {}, "Команда должна быть одной строкой." };
    }

    const auto host = LocalRconHost(baseDir);
    const int port = LocalRconPort(baseDir);
    const auto password = LocalRconPassword(baseDir);
    const int timeoutMs = LocalRconTimeoutMs(baseDir);

    std::string error;
    SOCKET sock = ConnectLocalRconSocket(host, port, timeoutMs, error);
    if (sock == INVALID_SOCKET) return { false, 0, {}, error };

    auto closeSocket = [&]() {
        if (sock != INVALID_SOCKET) {
            shutdown(sock, SD_BOTH);
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
    };

    const int authId = g_localRconRequestId.fetch_add(2);
    if (!SendRconPacket(sock, authId, 3, password, error)) {
        closeSocket();
        return { false, 0, {}, error };
    }

    bool authed = false;
    const auto authDeadline = GetTickCount64() + static_cast<uint64_t>(timeoutMs);
    while (GetTickCount64() < authDeadline) {
        RconPacket packet;
        if (!ReceiveRconPacket(sock, packet, error)) break;
        if (packet.id == -1) {
            closeSocket();
            return { false, 0, packet.body, "Командный канал сервера отклонил пароль." };
        }
        if (packet.id == authId && packet.type == 2) {
            authed = true;
            break;
        }
    }
    if (!authed) {
        closeSocket();
        return { false, 0, {}, error.empty() ? "Командный канал сервера не завершил авторизацию." : error };
    }

    const int execId = g_localRconRequestId.fetch_add(2);
    const int markerId = g_localRconRequestId.fetch_add(2);
    if (!SendRconPacket(sock, execId, 2, command, error) ||
        !SendRconPacket(sock, markerId, 2, "", error)) {
        closeSocket();
        return { false, 0, {}, error };
    }

    bool markerSeen = false;
    std::string body;
    const auto execDeadline = GetTickCount64() + static_cast<uint64_t>(timeoutMs);
    while (GetTickCount64() < execDeadline) {
        RconPacket packet;
        if (!ReceiveRconPacket(sock, packet, error)) break;
        if (packet.id == execId) {
            body += packet.body;
        } else if (packet.id == markerId) {
            markerSeen = true;
            break;
        }
    }
    closeSocket();

    if (!markerSeen && body.empty()) {
        return { false, 0, {}, error.empty() ? "Командный канал сервера не вернул ответ вовремя." : error };
    }
    return { true, 0, body, markerSeen ? "Команда отправлена через командный канал сервера." : "Команда отправлена; маркер ответа не пришёл вовремя после частичного вывода." };
}

ArkCommandResult LocalRconSendConsoleCommand(const std::wstring& baseDir, const std::string& command) {
    std::lock_guard<std::mutex> lock(g_localRconMutex);
    return LocalRconSendConsoleCommandLocked(baseDir, command);
}

std::string LocalRconStatusJson(const std::wstring& baseDir) {
    const bool enabled = ConfigBoolValue(baseDir, "server_command_enabled",
        ConfigBoolValue(baseDir, "local_rcon_enabled", false));
    const bool configured = LocalRconEnabled(baseDir);
    std::ostringstream ss;
    ss << "{\"enabled\":" << (enabled ? "true" : "false")
       << ",\"configured\":" << (configured ? "true" : "false")
       << ",\"host\":\"" << JsonEscape(LocalRconHost(baseDir)) << "\""
       << ",\"port\":" << LocalRconPort(baseDir)
       << ",\"message\":\"" << JsonEscape(configured
            ? "Командный канал сервера настроен."
            : "Командный канал сервера отключён или пароль не задан.")
       << "\"}";
    return ss.str();
}

ArkCommandResult ArkPanelSendConsoleCommandLocked(const std::wstring& baseDir, const std::string& command) {
    std::string error;
    if (!ArkPanelEnsureSessionLocked(baseDir, error)) return { false, 0, {}, error };

    const auto baseUrl = ArkPanelBaseUrl(baseDir);
    const auto serverId = ConfigTextValue(baseDir, "ark_panel_server_id");
    const auto form = std::string("cmd=") + UrlEncode(command);
    auto headers = ArkPanelHeaders(g_arkPanelSession);
    auto response = HttpRequestText(baseUrl + "/servers/control/sendconsole/" + UrlEncode(serverId),
        L"POST", form, L"application/x-www-form-urlencoded", headers, 20);
    MergeSetCookies(g_arkPanelSession.cookies, response.headers);

    if (response.body.find("HTTP_X_CSRF_TOKEN NOT VALID") != std::string::npos ||
        response.body.find("CSRF_TOKEN NOT VALID") != std::string::npos) {
        g_arkPanelSession = ArkPanelSession{};
        if (!ArkPanelEnsureSessionLocked(baseDir, error)) return { false, response.status, response.body, error };
        headers = ArkPanelHeaders(g_arkPanelSession);
        response = HttpRequestText(baseUrl + "/servers/control/sendconsole/" + UrlEncode(serverId),
            L"POST", form, L"application/x-www-form-urlencoded", headers, 20);
        MergeSetCookies(g_arkPanelSession.cookies, response.headers);
    }

    const bool panelSuccess = response.body.find("\"status\":\"success\"") != std::string::npos;
    if (!response.ok || !panelSuccess) {
        return { false, response.status, response.body, response.body.empty() ? response.message : response.body };
    }
    return { true, response.status, response.body, "Команда отправлена через консоль хостинга." };
}

ArkCommandResult ArkPanelSendConsoleCommand(const std::wstring& baseDir, const std::string& command) {
    std::lock_guard<std::mutex> lock(g_arkPanelMutex);
    return ArkPanelSendConsoleCommandLocked(baseDir, command);
}

ArkCommandResult ArkPanelControlActionLocked(const std::wstring& baseDir, const std::string& action) {
    const auto clean = ToLowerAscii(TrimAscii(action));
    if (clean != "start" && clean != "stop" && clean != "restart") {
        return { false, 0, {}, "Разрешены только start, stop или restart." };
    }
    std::string error;
    if (!ArkPanelEnsureSessionLocked(baseDir, error)) return { false, 0, {}, error };

    const auto baseUrl = ArkPanelBaseUrl(baseDir);
    const auto serverId = ConfigTextValue(baseDir, "ark_panel_server_id");
    auto headers = ArkPanelHeaders(g_arkPanelSession);
    headers += L"X-Requested-With: XMLHttpRequest\r\n";
    auto response = HttpRequestText(baseUrl + "/servers/control/action/" + UrlEncode(serverId) + "/" + clean,
        L"GET", "", L"", headers, 75);
    MergeSetCookies(g_arkPanelSession.cookies, response.headers);

    if (response.body.find("HTTP_X_CSRF_TOKEN NOT VALID") != std::string::npos ||
        response.body.find("CSRF_TOKEN NOT VALID") != std::string::npos) {
        g_arkPanelSession = ArkPanelSession{};
        if (!ArkPanelEnsureSessionLocked(baseDir, error)) return { false, response.status, response.body, error };
        headers = ArkPanelHeaders(g_arkPanelSession);
        headers += L"X-Requested-With: XMLHttpRequest\r\n";
        response = HttpRequestText(baseUrl + "/servers/control/action/" + UrlEncode(serverId) + "/" + clean,
            L"GET", "", L"", headers, 75);
        MergeSetCookies(g_arkPanelSession.cookies, response.headers);
    }

    if (!response.ok) {
        return { false, response.status, response.body, response.body.empty() ? response.message : response.body };
    }
    return { true, response.status, response.body, "Команда управления отправлена через панель хостинга." };
}

ArkCommandResult ArkPanelControlAction(const std::wstring& baseDir, const std::string& action) {
    std::lock_guard<std::mutex> lock(g_arkPanelMutex);
    return ArkPanelControlActionLocked(baseDir, action);
}

std::string ServerControlEnvelope(const std::wstring& baseDir, const Request& req) {
    auto action = JsonRootStringField(req.body, "action");
    if (action.empty()) action = QueryValue(req.path, "action");
    if (action.empty()) action = FormValue(req.body, "action");
    if (action.empty()) return Envelope(false, "Действие не задано.");
    const auto clean = ToLowerAscii(TrimAscii(action));
    const auto result = ArkPanelControlAction(baseDir, clean);
    if (!result.ok) {
        auto message = result.message;
        if (message.empty() && result.status > 0) {
            message = "Панель хостинга вернула HTTP " + std::to_string(result.status) + ".";
        }
        return Envelope(false, message.empty() ? "Панель хостинга не приняла действие." : message);
    }
    std::string message = "Команда управления отправлена в панель хостинга.";
    if (clean == "stop") {
        message = "Команда остановки отправлена. Если панель работает внутри SCUMServer.exe, она станет недоступна после остановки.";
    } else if (clean == "restart") {
        message = "Команда перезапуска отправлена в панель хостинга.";
    } else if (clean == "start") {
        message = "Команда запуска отправлена в панель хостинга.";
    }
    return Envelope(true, std::string("{\"action\":\"") + JsonEscape(clean) +
        "\",\"serverId\":\"" + JsonEscape(ConfigTextValue(baseDir, "ark_panel_server_id")) +
        "\",\"statusCode\":" + std::to_string(result.status) +
        ",\"message\":\"" + JsonEscape(message) + "\"}");
}

int DetectGamePortFromLog(const std::wstring& baseDir) {
    const auto log = ReadTextFile(SavedLogPath(baseDir));
    if (log.empty()) return 0;

    static const std::regex portRegex(
        R"((?:listening\s+on\s+port|Created\s+socket\s+for\s+bind\s+address:.*?\s+port)\s+(\d+))",
        std::regex_constants::icase);
    int port = 0;
    for (std::sregex_iterator it(log.begin(), log.end(), portRegex), end; it != end; ++it) {
        try {
            port = std::stoi((*it)[1].str());
        } catch (...) {
        }
    }
    return port;
}

struct GameEndpoint {
    std::string bindAddress;
    int port = 0;
};

GameEndpoint DetectGameEndpointFromLog(const std::wstring& baseDir) {
    const auto log = ReadTextFile(SavedLogPath(baseDir));
    if (log.empty()) return {};

    static const std::regex bindRegex(
        R"(Created\s+socket\s+for\s+bind\s+address:\s*([^\s]+)\s+on\s+port\s+(\d+))",
        std::regex_constants::icase);
    GameEndpoint endpoint;
    for (std::sregex_iterator it(log.begin(), log.end(), bindRegex), end; it != end; ++it) {
        endpoint.bindAddress = TrimAscii((*it)[1].str());
        try {
            endpoint.port = std::stoi((*it)[2].str());
        } catch (...) {
            endpoint.port = 0;
        }
    }
    if (endpoint.port <= 0) endpoint.port = DetectGamePortFromLog(baseDir);
    return endpoint;
}

bool IsSteam64Text(const std::string& value) {
    const auto text = TrimAscii(value);
    return text.size() >= 15 && text.size() <= 20 &&
        std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool IsSafeScumAdminToken(const std::string& value) {
    const auto text = TrimAscii(value);
    if (text.empty()) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
    });
}

bool IsPlausibleScumItemToken(const std::string& value) {
    const auto text = TrimAscii(value);
    if (!IsSafeScumAdminToken(text)) return false;
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isalpha(ch) != 0;
    });
}

std::string ScumAdminTargetRef(const std::string& steamId, const std::string& name) {
    const auto steam = TrimAscii(steamId);
    if (IsSteam64Text(steam)) return steam;

    const auto playerName = TrimAscii(name);
    if (playerName.empty()) return {};
    if (playerName.find('\r') != std::string::npos ||
        playerName.find('\n') != std::string::npos ||
        playerName.find('"') != std::string::npos ||
        playerName.find('\\') != std::string::npos) {
        return {};
    }
    return std::string("\"") + playerName + "\"";
}

std::string RequestCommandText(const Request& req) {
    auto command = JsonRootStringField(req.body, "command");
    if (command.empty()) command = JsonRootStringField(req.body, "cmd");
    if (command.empty()) command = JsonRootStringField(req.body, "commandText");
    if (command.empty()) command = JsonRootStringField(req.body, "text");
    const auto wrapper = ToLowerAscii(TrimAscii(command));
    if (wrapper == "admin_exec" || wrapper == "admin-exec") {
        const auto args = JsonRootRawField(req.body, "args");
        if (!args.empty()) {
            auto nested = JsonRootStringField(args, "commandText");
            if (nested.empty()) nested = JsonRootStringField(args, "adminCommand");
            if (nested.empty()) nested = JsonRootStringField(args, "command");
            if (nested.empty()) nested = JsonRootStringField(args, "cmd");
            if (nested.empty()) nested = JsonRootStringField(args, "text");
            nested = TrimAscii(nested);
            if (!nested.empty() && ToLowerAscii(nested) != "admin_exec" && ToLowerAscii(nested) != "admin-exec") {
                command = nested;
            }
        }
    }
    if (command.empty()) command = FormValue(req.body, "command");
    if (command.empty()) command = FormValue(req.body, "cmd");
    if (command.empty()) command = FormValue(req.body, "commandText");
    if (command.empty()) command = FormValue(req.body, "text");
    command = TrimAscii(command);
    if (!command.empty() && command.front() != '#') command = "#" + command;
    return command;
}

std::string ArkPanelStatusJson(const std::wstring& baseDir) {
    const bool enabled = ConfigBoolValue(baseDir, "ark_panel_enabled", false);
    const bool configured = ArkPanelEnabled(baseDir);
    const bool cookieMode = !ConfigTextValue(baseDir, "ark_panel_cookie").empty();
    bool cachedSession = false;
    uint64_t ageMs = 0;
    {
        std::lock_guard<std::mutex> lock(g_arkPanelMutex);
        cachedSession = !g_arkPanelSession.cookies.empty() && (cookieMode || !g_arkPanelSession.csrf.empty());
        if (cachedSession) ageMs = GetTickCount64() - g_arkPanelSession.refreshedTick;
    }

    std::ostringstream ss;
    ss << "{\"enabled\":" << (enabled ? "true" : "false")
       << ",\"configured\":" << (configured ? "true" : "false")
       << ",\"cachedSession\":" << (cachedSession ? "true" : "false")
       << ",\"sessionAgeMs\":" << ageMs
       << ",\"baseUrl\":\"" << JsonEscape(ArkPanelBaseUrl(baseDir)) << "\""
       << ",\"serverId\":\"" << JsonEscape(ConfigTextValue(baseDir, "ark_panel_server_id")) << "\""
       << ",\"authMode\":\"" << (cookieMode ? "cookie" : "login") << "\""
       << ",\"message\":\""
       << JsonEscape(configured
            ? "Командный канал хостинга настроен; команды отправляются через консоль хостинга."
            : "Командный канал хостинга настроен не полностью.")
       << "\"}";
    return ss.str();
}

std::string ArkPanelAdminEnvelope(const std::wstring& baseDir, const Request& req) {
    auto command = RequestCommandText(req);
    if (command.empty()) return Envelope(false, "Команда не задана.");
    if (command.find('\r') != std::string::npos || command.find('\n') != std::string::npos) {
        return Envelope(false, "Команда должна быть одной строкой.");
    }

    const auto result = ArkPanelSendConsoleCommand(baseDir, command);
    if (!result.ok) {
        auto message = result.message.empty() ? result.body : result.message;
        if (message.size() > 700) message = message.substr(0, 700);
        return Envelope(false, std::string("Консоль хостинга не приняла команду: ") + message);
    }

    std::ostringstream data;
    data << "{\"transport\":\"hosting-panel-command\""
         << ",\"command\":\"" << JsonEscape(command) << "\""
         << ",\"status\":" << result.status
         << ",\"panelBody\":" << JsonValueOrString(result.body)
         << "}";
    return Envelope(true, data.str());
}

struct GrantItem {
    std::string itemId;
    int quantity = 1;
};

std::string ModuleStateJson(const std::wstring& baseDir, const std::string& key, const std::string& fallbackJson = "[]") {
    const auto text = TrimAscii(ReadTextFile(ModuleReadableStatePath(baseDir, key)));
    if (text.empty()) return fallbackJson;
    if (text.front() == '[' || text.front() == '{') return text;
    return fallbackJson;
}

std::string QueueStatusPassiveJson(const std::wstring& baseDir) {
    const auto fallback = "{\"available\":false,\"depth\":0,\"pending\":0,\"busy\":false,\"max\":0,\"spacingMs\":0,\"nextDelayMs\":0,\"source\":\"passive-state\",\"message\":\"Очередь ещё не опубликовала состояние; live bridge не опрашивался.\"}";
    auto state = TrimAscii(ModuleStateJson(baseDir, "queue-status", fallback));
    if (state.empty() || state == "[]") state = fallback;
    if (state.front() != '{') state = fallback;
    return state;
}

std::string QueueStatusJson(const std::wstring& baseDir) {
    const auto live = BridgeExecute(baseDir, "queue_status", "{}", 2500);
    if (live.ok && !live.body.empty()) {
        auto data = TrimAscii(JsonRootRawField(live.body, "data"));
        if (!data.empty() && data.front() == '{') {
            return data;
        }
    }
    return QueueStatusPassiveJson(baseDir);
}

std::string PluginManifestJson() {
    return R"JSON({"pluginCount":20,"plugins":[
{"Key":"BountyHunt","File":"BountyHunt.dll","Name":"Охота за головами","Author":"ScumNeDjin","Version":"1.1.1","Description":"Серии убийств, bounty-цели, топ охотников и награды.","ConfigKey":"bounty-hunt","SourceFile":"native/lua module"},
{"Key":"CharacterCommandPacks","File":"CharacterCommandPacks.dll","Name":"Пресеты персонажа","Author":"ScumNeDjin","Version":"1.3.1","Description":"Панельные пресеты атрибутов, навыков и командных пакетов. Игровые slash-команды намеренно скрыты.","ConfigKey":"character-packs","SourceFile":"native/lua module"},
{"Key":"DailyPack","File":"DailyPack.dll","Name":"Ежедневный набор","Author":"ScumNeDjin","Version":"1.1.0","Description":"Ежедневная выдача предметов с отдельными VIP-бонусами.","ConfigKey":"daily-pack","SourceFile":"native/lua module"},
{"Key":"Battlepass","File":"Battlepass.dll","Name":"Battlepass","Author":"ScumNeDjin","Version":"0.1.0","Description":"Автоматическая 30-дневная серия наград за реальные дни входа.","ConfigKey":"battlepass","SourceFile":"native/lua module"},
{"Key":"DiscordLogBridge","File":"DiscordLogBridge.dll","Name":"Логи Discord","Author":"ScumNeDjin","Version":"1.2.2","Description":"Пересылка чата, присутствия, боя и системных событий в Discord.","ConfigKey":"discord-log","SourceFile":"native/http module"},
{"Key":"fastTravel","File":"fastTravel.dll","Name":"Быстрое перемещение","Author":"ScumNeDjin","Version":"1.4.5","Description":"Маршруты, цена, задержка, кулдаун и отмена при движении.","ConfigKey":"fast-travel","SourceFile":"native/lua module"},
{"Key":"PrivateMessages","File":"PrivateMessages.dll","Name":"Личные сообщения","Author":"ScumNeDjin","Version":"1.0.0","Description":"Личные сообщения, быстрый ответ и короткая история.","ConfigKey":"private-messages","SourceFile":"native/lua module"},
{"Key":"SectorScan","File":"SectorScan.dll","Name":"Скан сектора","Author":"ScumNeDjin","Version":"1.0.1","Description":"Платная проверка сектора по live-позициям игроков.","ConfigKey":"sector-scan","SourceFile":"native/lua module"},
{"Key":"ServerKillFeed","File":"ServerKillFeed.dll","Name":"Лента убийств","Author":"ScumNeDjin","Version":"1.0.2","Description":"Глобальные сообщения об убийствах и защита от дублей.","ConfigKey":"server-kill-feed","SourceFile":"native/log module"},
{"Key":"ScheduledEvents","File":"ScheduledEvents.dll","Name":"Планировщик событий","Author":"ScumNeDjin","Version":"1.0.0","Description":"Cargo drop, admin-команды и наборы предметов по расписанию.","ConfigKey":"scheduled-events","SourceFile":"native/scheduler module"},
{"Key":"BaseLootCollector","File":"BaseLootCollector.dll","Name":"Сбор лута базы","Author":"ScumNeDjin","Version":"0.1.0","Description":"Команда /loot собирает лежащий лут в сундуки своего флага с проверкой отряда.","ConfigKey":"base-loot-collector","SourceFile":"native/lua module"},
{"Key":"ItemUpgrade","File":"ItemUpgrade.dll","Name":"Апгрейд предметов","Author":"ScumNeDjin","Version":"0.1.0","Description":"Платная замена предмета в руках на настроенный ResultItemId.","ConfigKey":"item-upgrade","SourceFile":"native/lua module"},
{"Key":"MoneyTransfer","File":"MoneyTransfer.dll","Name":"Переводы денег","Author":"ScumNeDjin","Version":"0.1.0","Description":"Игроки переводят валюту друг другу через /sendmoney.","ConfigKey":"money-transfer","SourceFile":"native/lua module"},
{"Key":"PanelQuests","File":"PanelQuests.dll","Name":"Квесты сервера","Author":"ScumNeDjin","Version":"0.1.0","Description":"Панельная квестовая доска с наградами и объявлениями.","ConfigKey":"panel-quests","SourceFile":"native/lua module"},
{"Key":"ZoneRobotSchedule","File":"ZoneRobotSchedule.dll","Name":"Роботы по зонам","Author":"ScumNeDjin","Version":"0.1.0","Description":"Расписание зонных команд для роботов/событий.","ConfigKey":"zone-robot-schedule","SourceFile":"native/lua module"},
{"Key":"sethome","File":"sethome.dll","Name":"Домашние точки","Author":"ScumNeDjin","Version":"1.4.3","Description":"Именные дома, лимиты, кулдауны и отмена при движении.","ConfigKey":"home-system","SourceFile":"native/lua module"},
{"Key":"VehicleRental","File":"VehicleRental.dll","Name":"Аренда транспорта","Author":"ScumNeDjin","Version":"1.2.5","Description":"Аренда транспорта с продлением, возвратом и истечением срока.","ConfigKey":"vehicle-rental","SourceFile":"native/lua module"},
{"Key":"VipSystem","File":"VipSystem.dll","Name":"VIP игроки","Author":"ScumNeDjin","Version":"1.1.0","Description":"VIP-участники по SteamID64, сроки, уровни, права и отдельные лимиты модулей.","ConfigKey":"vip-system","SourceFile":"native/lua module"},
{"Key":"WargmShopBridge","File":"WargmShopBridge.dll","Name":"Магазин Wargm","Author":"ScumNeDjin","Version":"1.3.10","Description":"Выдача ожидающих покупок по SteamID с безопасными режимами.","ConfigKey":"wargm-shop","SourceFile":"native/http module"},
{"Key":"GameStoresShopBridge","File":"GameStoresShopBridge.dll","Name":"Магазин GameStores","Author":"ScumNeDjin","Version":"0.1.0","Description":"Получение оплаченных позиций GameStores, безопасная выдача и подтверждение после успеха.","ConfigKey":"gamestores-shop","SourceFile":"native/http module"},
{"Key":"WelcomePack","File":"WelcomePack.dll","Name":"Стартовый набор","Author":"ScumNeDjin","Version":"1.4.22","Description":"Настраиваемый стартовый набор через прямую выдачу предметов.","ConfigKey":"welcome-pack","SourceFile":"native/lua module"}
]})JSON";
}

std::string DiscordWebhookForRoute(const std::string& config, const std::string& requestedRoute) {
    auto route = ToLowerAscii(TrimAscii(requestedRoute));
    if (route.empty()) route = "system";
    std::string webhook;
    if (route == "presence") webhook = JsonStringField(config, "PresenceWebhookUrl");
    else if (route == "chat") webhook = JsonStringField(config, "ChatWebhookUrl");
    else if (route == "combat") webhook = JsonStringField(config, "CombatWebhookUrl");
    else webhook = JsonStringField(config, "SystemWebhookUrl");
    if (webhook.empty()) webhook = JsonStringField(config, "DefaultWebhookUrl");
    return webhook;
}

std::string DiscordBridgeStatusJson(const std::wstring& baseDir) {
    const auto config = PluginConfigJson(baseDir, "discord-log",
        R"JSON({"Enabled":false,"TransportMode":"Webhook","ServerLabel":"SCUM","DefaultWebhookUrl":"","PresenceWebhookUrl":"","ChatWebhookUrl":"","CombatWebhookUrl":"","SystemWebhookUrl":"","Username":"WILD","NotifyPlayerChat":true,"NotifyPlayerKills":true,"NotifyPlayerDeaths":true})JSON");
    const bool enabled = JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false));
    const auto mode = JsonStringField(config, "TransportMode").empty() ? "Webhook" : JsonStringField(config, "TransportMode");
    const auto systemHook = DiscordWebhookForRoute(config, "system");
    const auto presenceHook = DiscordWebhookForRoute(config, "presence");
    const auto chatHook = DiscordWebhookForRoute(config, "chat");
    const auto combatHook = DiscordWebhookForRoute(config, "combat");
    const auto state = ModuleStateJson(baseDir, "discord-log", "[]");
    std::ostringstream ss;
    ss << "{\"enabled\":" << (enabled ? "true" : "false")
       << ",\"transportMode\":\"" << JsonEscape(mode) << "\""
       << ",\"senderRunning\":true"
       << ",\"senderFaulted\":false"
       << ",\"queueLength\":0"
       << ",\"lastError\":\"\""
       << ",\"counts\":{\"enqueued\":0,\"sent\":0,\"failed\":0,\"skipped\":0}"
       << ",\"routes\":["
       << "{\"route\":\"system\",\"configured\":" << (!systemHook.empty() ? "true" : "false") << "},"
       << "{\"route\":\"presence\",\"configured\":" << (!presenceHook.empty() ? "true" : "false") << "},"
       << "{\"route\":\"chat\",\"configured\":" << (!chatHook.empty() ? "true" : "false") << "},"
       << "{\"route\":\"combat\",\"configured\":" << (!combatHook.empty() ? "true" : "false") << "}]"
       << ",\"recent\":" << state
       << "}";
    return ss.str();
}

std::string DiscordBridgeChannelsJson(const std::wstring& baseDir) {
    const auto config = PluginConfigJson(baseDir, "discord-log", "{}");
    std::ostringstream ss;
    ss << "{\"source\":\"config\",\"channels\":["
       << "{\"route\":\"default\",\"channelId\":\"" << JsonEscape(JsonStringField(config, "DefaultChannelId")) << "\"},"
       << "{\"route\":\"presence\",\"channelId\":\"" << JsonEscape(JsonStringField(config, "PresenceChannelId")) << "\"},"
       << "{\"route\":\"chat\",\"channelId\":\"" << JsonEscape(JsonStringField(config, "ChatChannelId")) << "\"},"
       << "{\"route\":\"combat\",\"channelId\":\"" << JsonEscape(JsonStringField(config, "CombatChannelId")) << "\"},"
       << "{\"route\":\"system\",\"channelId\":\"" << JsonEscape(JsonStringField(config, "SystemChannelId")) << "\"}"
       << "]}";
    return ss.str();
}

std::wstring DiscordSecretPath(const std::wstring& baseDir) {
    return JoinPath(JoinPath(ProjectRuntimeRoot(baseDir), L"configs"), L"discord-log.secret.json");
}

std::wstring DiscordSecretLegacyPath(const std::wstring& baseDir) {
    return JoinPath(JoinPath(ProjectLegacyRoot(baseDir), L"configs"), L"discord-log.secret.json");
}

std::wstring DiscordSecretOverlayPath(const std::wstring& baseDir) {
    return JoinPath(JoinPath(JoinPath(ProjectRuntimeRoot(baseDir), L"state"), L"configs"), L"discord-log.secret.json");
}

std::wstring DiscordSecretLegacyOverlayPath(const std::wstring& baseDir) {
    return JoinPath(JoinPath(JoinPath(ProjectLegacyRoot(baseDir), L"state"), L"configs"), L"discord-log.secret.json");
}

std::string DiscordSecretStatusJson(const std::wstring& baseDir) {
    const auto livePath = DiscordSecretOverlayPath(baseDir);
    const auto basePath = DiscordSecretPath(baseDir);
    const auto legacyLivePath = DiscordSecretLegacyOverlayPath(baseDir);
    const auto legacyBasePath = DiscordSecretLegacyPath(baseDir);
    std::ostringstream ss;
    ss << "{\"exists\":" << ((FileExists(livePath) || FileExists(basePath) || FileExists(legacyLivePath) || FileExists(legacyBasePath)) ? "true" : "false")
       << ",\"path\":\"Saved/ScumNeDjin/state/configs/discord-log.secret.json\""
       << ",\"message\":\"Токен бота хранится отдельно и никогда не возвращается через API.\"}";
    return ss.str();
}

bool AppendModuleStateRecord(const std::wstring& baseDir, const std::string& key, const std::string& recordJson);

std::string DiscordBridgeTestResponse(const std::wstring& baseDir, const Request& req) {
    const auto config = PluginConfigJson(baseDir, "discord-log", "{}");
    if (!JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false))) {
        return Envelope(false, "Журнал Discord сейчас отключён.");
    }
    auto route = JsonStringField(req.body, "route");
    if (route.empty()) route = "system";
    auto message = JsonStringField(req.body, "message");
    if (message.empty()) message = "Тестовое сообщение ScumNeDjin для Discord.";
    const auto webhook = DiscordWebhookForRoute(config, route);
    if (webhook.empty()) {
        return Envelope(false, "Для этой ленты Discord не настроен webhook.");
    }
    auto username = JsonStringField(config, "Username");
    if (username.empty()) username = "ScumNeDjin";
    const auto payload = std::string("{\"username\":\"") + JsonEscape(username) +
        "\",\"embeds\":[{\"title\":\"ScumNeDjin: проверка " + JsonEscape(route) +
        "\",\"description\":\"" + JsonEscape(message) +
        "\",\"color\":3447003,\"timestamp\":\"" + UtcIsoNow() + "\"}]}";
    const auto post = HttpPostJson(webhook, payload, JsonIntField(config, "HttpTimeoutSeconds", 10));
    AppendModuleStateRecord(baseDir, "discord-log",
        std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"route\":\"" + JsonEscape(route) +
        "\",\"message\":\"" + JsonEscape(message) +
        "\",\"ok\":" + (post.ok ? "true" : "false") +
        ",\"status\":" + std::to_string(post.status) +
        ",\"result\":\"" + JsonEscape(post.message) + "\"}");
    if (!post.ok) return Envelope(false, post.message);
    return Envelope(true, std::string("{\"queued\":false,\"sent\":true,\"route\":\"") + JsonEscape(route) + "\",\"status\":" + std::to_string(post.status) + "}");
}

std::vector<std::string> ExtractArrayObjects(const std::string& json);

size_t ModuleStateMaxBytes(const std::string& key) {
    const auto clean = StateStorageKey(key);
    if (clean == "action-log") return 1024 * 1024;
    if (clean == "chat" || clean == "economy" || clean == "discord-log") return 768 * 1024;
    if (clean == "server-kill-feed" || clean == "kill-feed" || clean == "wargm-shop" || clean == "gamestores-shop") return 1024 * 1024;
    return 2 * 1024 * 1024;
}

int ModuleStateMaxRows(const std::string& key) {
    const auto clean = StateStorageKey(key);
    if (clean == "action-log") return 1200;
    if (clean == "chat" || clean == "economy" || clean == "discord-log") return 1200;
    if (clean == "server-kill-feed" || clean == "kill-feed" || clean == "wargm-shop" || clean == "gamestores-shop") return 1500;
    return 2500;
}

std::string JoinJsonObjectArray(const std::vector<std::string>& objects) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < objects.size(); ++i) {
        if (i) out << ",";
        out << objects[i];
    }
    out << "]";
    return out.str();
}

bool AppendModuleStateRecord(const std::wstring& baseDir, const std::string& key, const std::string& recordJson) {
    const auto path = ModuleStatePath(baseDir, key);
    auto current = TrimAscii(ReadTextFile(path));
    if (current.empty() || current.front() != '[' || current.back() != ']') current = "[]";
    current.pop_back();
    if (TrimAscii(current) != "[") current += ",";
    current += recordJson;
    current += "]";
    const auto maxBytes = ModuleStateMaxBytes(key);
    const auto maxRows = ModuleStateMaxRows(key);
    if (current.size() > maxBytes) {
        auto objects = ExtractArrayObjects(current);
        if (objects.empty()) {
            current = "[" + recordJson + "]";
        } else {
            if (static_cast<int>(objects.size()) > maxRows) {
                objects.erase(objects.begin(), objects.end() - maxRows);
            }
            current = JoinJsonObjectArray(objects);
            while (current.size() > maxBytes && objects.size() > 1) {
                objects.erase(objects.begin());
                current = JoinJsonObjectArray(objects);
            }
        }
    }
    return WriteTextFile(path, current);
}

bool AppendActionRecord(
    const std::wstring& baseDir,
    const std::string& action,
    bool ok,
    const std::string& steamId,
    const std::string& name,
    const std::string& message,
    std::string detailsJson = "{}") {
    detailsJson = TrimAscii(detailsJson);
    if (detailsJson.empty() || (detailsJson.front() != '{' && detailsJson.front() != '[')) {
        detailsJson = std::string("\"") + JsonEscape(detailsJson) + "\"";
    }
    return AppendModuleStateRecord(baseDir, "action-log",
        std::string("{\"type\":\"action\",\"timestampUtc\":\"") + UtcIsoNow() +
        "\",\"action\":\"" + JsonEscape(action) +
        "\",\"ok\":" + (ok ? "true" : "false") +
        ",\"steamId\":\"" + JsonEscape(steamId) +
        "\",\"name\":\"" + JsonEscape(name) +
        "\",\"message\":\"" + JsonEscape(message) +
        "\",\"details\":" + detailsJson + "}");
}

bool JsonArrayHasItems(const std::string& json) {
    const auto text = TrimAscii(json);
    return text.size() > 2 && text.front() == '[' && text.back() == ']';
}

std::string DbRowsOrErrorArray(const std::wstring& baseDir, const std::string& sql, int maxRows = 1000) {
    std::string rows;
    std::string error;
    if (SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), sql, maxRows, rows, error)) {
        return rows;
    }
    return std::string("[{\"source\":\"sqlite\",\"error\":\"") + JsonEscape(error) + "\"}]";
}

std::string BuildWorldPersistenceStatusJson(const std::wstring& baseDir) {
    const std::string countSql =
        "SELECT "
        "(SELECT COUNT(*) FROM base) AS baseRows, "
        "(SELECT COUNT(*) FROM base_element) AS baseElementRows, "
        "(SELECT COUNT(*) FROM placeable) AS placeableRows, "
        "(SELECT COUNT(*) FROM placeable_basebuilding) AS placeableBasebuildingRows, "
        "(SELECT COUNT(*) FROM base WHERE id >= 900000000 OR name LIKE 'Nedjin_%') AS nedjinDbOnlyBaseRows, "
        "(SELECT COUNT(*) FROM base_element WHERE element_id >= 900000000 OR base_id >= 900000000) AS nedjinDbOnlyBaseElementRows, "
        "(SELECT COUNT(*) FROM base b WHERE NOT EXISTS (SELECT 1 FROM base_element e WHERE e.base_id = b.id)) AS baseRowsWithoutElements, "
        "(SELECT COUNT(*) FROM base_element e WHERE NOT EXISTS (SELECT 1 FROM base b WHERE b.id = e.base_id)) AS elementRowsWithoutBase";

    std::string rows;
    std::string error;
    if (!SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), countSql, 1, rows, error)) {
        return std::string("{\"online\":false,\"source\":\"SCUM.db\",\"message\":\"Не удалось проверить persistence-таблицы SCUM.db.\",\"error\":\"")
            + JsonEscape(error) + "\"}";
    }

    const auto objects = ExtractArrayObjects(rows);
    const auto counts = objects.empty() ? "{}" : objects.front();
    const auto suspiciousBases = DbRowsOrErrorArray(baseDir,
        "SELECT id, name, location_x AS x, location_y AS y, map_id AS mapId, user_profile_id AS userProfileId "
        "FROM base "
        "WHERE id >= 900000000 OR name LIKE 'Nedjin_%' "
        "ORDER BY id LIMIT 25", 25);
    const auto suspiciousElements = DbRowsOrErrorArray(baseDir,
        "SELECT element_id AS elementId, base_id AS baseId, asset, location_x AS x, location_y AS y, location_z AS z, owner_profile_id AS ownerProfileId "
        "FROM base_element "
        "WHERE element_id >= 900000000 OR base_id >= 900000000 "
        "ORDER BY element_id LIMIT 25", 25);

    std::ostringstream ss;
    ss << "{\"online\":true"
       << ",\"source\":\"SCUM.db read-only\""
       << ",\"message\":\"DB-only строки не считаются рабочими зданиями без подтверждения AConZBaseManager и клиента.\""
       << ",\"requiresRuntimeConfirmation\":true"
       << ",\"counts\":" << counts
       << ",\"suspiciousBases\":" << suspiciousBases
       << ",\"suspiciousBaseElements\":" << suspiciousElements
       << "}";
    return ss.str();
}

std::string WorldPersistenceStatusJson(const std::wstring& baseDir) {
    // /api/status is refreshed by the web UI. Keep this read-only diagnostic
    // bounded so concurrent panel tabs do not fan out identical SQL queries.
    const auto now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_worldPersistenceCacheMutex);
    if (!g_worldPersistenceCache.body.empty() && now - g_worldPersistenceCache.tick < 5000) {
        return g_worldPersistenceCache.body;
    }
    g_worldPersistenceCache.body = BuildWorldPersistenceStatusJson(baseDir);
    g_worldPersistenceCache.tick = GetTickCount64();
    return g_worldPersistenceCache.body;
}

std::string HttpServerStatusJson() {
    std::ostringstream ss;
    ss << "{\"activeClients\":" << g_httpActiveClients.load(std::memory_order_relaxed)
       << ",\"clientLimit\":" << g_httpClientLimit.load(std::memory_order_relaxed)
       << ",\"rejectedClients\":" << g_httpRejectedClients.load(std::memory_order_relaxed)
       << ",\"threadStartFailures\":" << g_httpThreadStartFailures.load(std::memory_order_relaxed)
       << "}";
    return ss.str();
}

std::string StateEventsOrLogJson(
    const std::wstring& baseDir,
    const std::string& stateKey,
    const std::string& type,
    const std::vector<std::string>& logPatterns) {
    const auto state = ModuleStateJson(baseDir, stateKey);
    if (JsonArrayHasItems(state)) return state;
    return EventsFromLogJson(baseDir, type, logPatterns);
}

std::string SquadsDbJson(const std::wstring& baseDir) {
    return DbRowsOrErrorArray(baseDir,
        "SELECT "
        "s.id AS squadId, s.name AS squadName, s.message AS message, s.information AS information, "
        "s.score AS score, s.member_limit AS memberLimit, "
        "s.last_member_login_time AS lastMemberLoginUtc, s.last_member_logout_time AS lastMemberLogoutUtc, "
        "m.id AS memberId, m.rank AS memberRank, m.user_profile_id AS memberProfileId, up.user_id AS memberSteamId, "
        "up.name AS memberName, up.fame_points AS famePoints, up.money_balance AS moneyBalance "
        "FROM squad s "
        "LEFT JOIN squad_member m ON m.squad_id = s.id "
        "LEFT JOIN user_profile up ON up.id = m.user_profile_id "
        "ORDER BY s.score DESC, s.id, m.rank DESC");
}

std::string FlagsDbJson(const std::wstring& baseDir) {
    return DbRowsOrErrorArray(baseDir,
        "SELECT "
        "f.element_id AS elementId, b.id AS baseId, b.name AS baseName, "
        "COALESCE(e.location_x, b.location_x) AS x, "
        "COALESCE(e.location_y, b.location_y) AS y, "
        "COALESCE(e.location_z, 0) AS z, "
        "up.name AS ownerName, up.user_id AS ownerSteamId, "
        "f.overtake_end_time AS overtakeEndTime, f.overtaker_user_profile_id AS overtakerProfileId, "
        "f.expanded_elements AS expandedElements "
        "FROM base_element_flag f "
        "LEFT JOIN base_element e ON e.element_id = f.element_id "
        "LEFT JOIN base b ON b.id = e.base_id "
        "LEFT JOIN user_profile up ON up.id = COALESCE(NULLIF(b.owner_user_profile_id, -1), NULLIF(b.user_profile_id, -1), NULLIF(e.owner_profile_id, -1), "
        "(SELECT e2.owner_profile_id FROM base_element e2 WHERE e2.base_id=b.id AND e2.owner_profile_id IS NOT NULL AND e2.owner_profile_id > 0 ORDER BY e2.element_id LIMIT 1)) "
        "ORDER BY f.element_id DESC");
}

std::string VehiclesDbJson(const std::wstring& baseDir) {
    return DbRowsOrErrorArray(baseDir,
        "SELECT "
        "vehicle_entity_id AS entityId, "
        "replace(vehicle_asset_id, 'Vehicle:', '') AS asset, "
        "vehicle_asset_id AS assetId, vehicle_alias AS alias, "
        "datetime(vehicle_last_access_time, 'unixepoch') AS lastAccessUtc, "
        "is_vehicle_automatically_created AS automatic, "
        "is_vehicle_functional AS functional, "
        "time_spent_in_forbidden_zone AS forbiddenZoneSeconds "
        "FROM vehicle_spawner "
        "ORDER BY vehicle_entity_id DESC");
}

std::string VehicleAdminToken(std::string vehicleId) {
    vehicleId = TrimAscii(vehicleId);
    if (vehicleId.rfind("Vehicle:", 0) == 0) vehicleId = vehicleId.substr(8);

    const auto normalized = ToLowerAscii(vehicleId);
    if (normalized == "bp_kinglet_mariner" || normalized == "kinglet_mariner" || normalized == "kingletmariner") {
        return "BPC_Kinglet_Mariner";
    }
    if (normalized == "bp_kinglet_duster" || normalized == "kinglet_duster" || normalized == "kingletduster") {
        return "BPC_Kinglet_Duster";
    }
    if (normalized == "bp_kinglet_scout" || normalized == "kinglet_scout" || normalized == "kingletscout") {
        return "BPC_Kinglet_Scout";
    }

    return vehicleId;
}

long long ExtractVehicleEntityIdFromText(const std::string& text) {
    static const std::regex patterns[] = {
        std::regex(R"rx(\bID\s+([0-9]+))rx", std::regex_constants::icase),
        std::regex(R"rx(\bVehicleId[\s:=]+([0-9]+))rx", std::regex_constants::icase),
        std::regex(R"rx(\bEntityId[\s:=]+([0-9]+))rx", std::regex_constants::icase),
        std::regex(R"rx("runtimeRef"\s*:\s*"([0-9]+)")rx", std::regex_constants::icase),
        std::regex(R"rx("entityId"\s*:\s*"?([0-9]+)"?)rx", std::regex_constants::icase)
    };
    for (const auto& pattern : patterns) {
        std::smatch match;
        if (!std::regex_search(text, match, pattern)) continue;
        try {
            const auto value = std::stoll(match[1].str());
            if (value > 0) return value;
        } catch (...) {
        }
    }
    return 0;
}

long long LatestVehicleEntityId(const std::wstring& baseDir, const std::string& vehicleId) {
    const auto token = VehicleAdminToken(vehicleId);
    if (token.empty()) return 0;

    const auto sql = std::string("SELECT vehicle_entity_id AS entityId FROM vehicle_spawner WHERE vehicle_asset_id = ") +
        SqlTextLiteral("Vehicle:" + token) +
        " AND COALESCE(is_vehicle_functional, 0) <> 0 ORDER BY vehicle_entity_id DESC LIMIT 1";
    std::string rows;
    std::string error;
    if (!SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), sql, 1, rows, error)) return 0;

    const std::regex idRe(R"rx("entityId"\s*:\s*([0-9]+))rx");
    std::smatch match;
    if (!std::regex_search(rows, match, idRe)) return 0;
    try {
        return std::stoll(match[1].str());
    } catch (...) {
        return 0;
    }
}

bool WaitForVehicleEntity(
    const std::wstring& baseDir,
    const std::string& vehicleId,
    long long beforeEntityId,
    long long& entityIdOut) {
    const int delaysMs[] = { 250, 500, 750, 1000, 1500, 2000, 2500, 3000, 3500, 4000 };
    for (const auto delayMs : delaysMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        const auto current = LatestVehicleEntityId(baseDir, vehicleId);
        if (current > beforeEntityId) {
            entityIdOut = current;
            return true;
        }
    }
    entityIdOut = LatestVehicleEntityId(baseDir, vehicleId);
    return entityIdOut > beforeEntityId;
}

std::vector<std::string> VehicleSpawnCommands(const std::string& vehicleId, const std::string& steamId, const std::string& name) {
    const auto token = VehicleAdminToken(vehicleId);
    std::vector<std::string> commands;
    commands.push_back("SpawnVehicle " + token + " 1 Modifier Full");
    commands.push_back("SpawnVehicle " + token + " Modifier Full");
    commands.push_back("SpawnVehicle " + token + " 1");
    commands.push_back("SpawnVehicle " + token);
    return commands;
}

std::string VehicleRconLocationRef(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey) {
    const auto args = std::string("{\"steamId\":\"") + JsonEscape(steamId) +
        "\",\"name\":\"" + JsonEscape(name) +
        "\",\"runtimeKey\":\"" + JsonEscape(runtimeKey) + "\"}";
    const auto br = BridgeExecute(baseDir, "player_details", args, 3000);
    if (br.ok && !br.body.empty()) {
        auto player = JsonRawField(br.body, "player");
        if (player.empty()) player = JsonRawField(br.body, "data");
        if (player.empty()) player = br.body;
        const auto x = JsonNumberTextField(player, "x", JsonNumberTextField(player, "X", ""));
        const auto y = JsonNumberTextField(player, "y", JsonNumberTextField(player, "Y", ""));
        const auto z = JsonNumberTextField(player, "z", JsonNumberTextField(player, "Z", ""));
        if (!x.empty() && !y.empty() && !z.empty()) return x + " " + y + " " + z;
    }
    return ScumAdminTargetRef(steamId, name);
}

std::string ScumLocationArgument(std::string locationRef) {
    locationRef = TrimAscii(std::move(locationRef));
    if (locationRef.empty()) return locationRef;
    if (locationRef.size() >= 2 && locationRef.front() == '"' && locationRef.back() == '"') return locationRef;

    const bool looksLikeCoordinate =
        locationRef.find(' ') != std::string::npos ||
        locationRef.find('{') != std::string::npos ||
        locationRef.find('=') != std::string::npos ||
        locationRef.find('|') != std::string::npos;
    if (!looksLikeCoordinate) return locationRef;

    std::string quoted = "\"";
    for (const char ch : locationRef) {
        if (ch == '"') continue;
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

bool Utf8ContinuationByte(unsigned char ch) {
    return (ch & 0xC0) == 0x80;
}

size_t Utf8SequenceLengthAt(const std::string& value, size_t index) {
    if (index >= value.size()) return 0;
    const auto b0 = static_cast<unsigned char>(value[index]);
    if (b0 < 0x80) return 1;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        return (index + 1 < value.size() && Utf8ContinuationByte(static_cast<unsigned char>(value[index + 1]))) ? 2 : 0;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (index + 2 >= value.size()) return 0;
        const auto b1 = static_cast<unsigned char>(value[index + 1]);
        const auto b2 = static_cast<unsigned char>(value[index + 2]);
        if (!Utf8ContinuationByte(b1) || !Utf8ContinuationByte(b2)) return 0;
        if (b0 == 0xE0 && b1 < 0xA0) return 0;
        if (b0 == 0xED && b1 >= 0xA0) return 0;
        return 3;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (index + 3 >= value.size()) return 0;
        const auto b1 = static_cast<unsigned char>(value[index + 1]);
        const auto b2 = static_cast<unsigned char>(value[index + 2]);
        const auto b3 = static_cast<unsigned char>(value[index + 3]);
        if (!Utf8ContinuationByte(b1) || !Utf8ContinuationByte(b2) || !Utf8ContinuationByte(b3)) return 0;
        if (b0 == 0xF0 && b1 < 0x90) return 0;
        if (b0 == 0xF4 && b1 >= 0x90) return 0;
        return 4;
    }
    return 0;
}

std::string Utf8CleanAndClamp(const std::string& value, size_t maxLen) {
    std::string out;
    out.reserve(std::min(value.size(), maxLen));
    for (size_t i = 0; i < value.size();) {
        const auto len = Utf8SequenceLengthAt(value, i);
        if (len == 0) {
            ++i;
            continue;
        }
        if (out.size() + len > maxLen) break;
        out.append(value, i, len);
        i += len;
    }
    return out;
}

std::string ScumSingleLineMessage(std::string message, size_t maxLen = 220) {
    message = TrimAscii(std::move(message));
    for (char& ch : message) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    message = TrimAscii(message);
    return Utf8CleanAndClamp(message, maxLen);
}

struct PanelLiveTarget {
    bool ok = false;
    std::string message;
    std::string body;
    std::string steamId;
    std::string name;
    std::string runtimeKey;
    std::string x;
    std::string y;
    std::string z;
};

PanelLiveTarget ValidatePanelLiveTargetForCommandChannel(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    const std::string& reason,
    bool requireLocation,
    bool requireSteam,
    int timeoutMs = 6000) {
    PanelLiveTarget target;
    auto args = std::string("{\"steamId\":\"") + JsonEscape(steamId) +
        "\",\"name\":\"" + JsonEscape(name) +
        "\",\"reason\":\"" + JsonEscape(reason) +
        "\",\"requireLocation\":" + (requireLocation ? "true" : "false") +
        ",\"requireSteam\":" + (requireSteam ? "true" : "false");
    if (!runtimeKey.empty()) args += ",\"runtimeKey\":\"" + JsonEscape(runtimeKey) + "\"";
    args += "}";

    const auto resolved = BridgeExecute(baseDir, "resolve_player_target", args, timeoutMs);
    target.body = resolved.body;
    if (!resolved.ok) {
        target.message = resolved.message.empty()
            ? "Игрок не подтверждён в live runtime; команда панели отменена."
            : resolved.message;
        AppendActionRecord(baseDir, "panel_player_resolve_native", false, steamId, name, target.message,
            std::string("{\"reason\":\"") + JsonEscape(reason) +
            "\",\"requireLocation\":" + (requireLocation ? "true" : "false") +
            ",\"requireSteam\":" + (requireSteam ? "true" : "false") +
            ",\"transport\":\"ue4ss-bridge-live\"}");
        return target;
    }

    auto data = JsonRawField(resolved.body, "data");
    if (data.empty()) data = resolved.body;
    target.steamId = JsonStringField(data, "steamId");
    target.name = JsonStringField(data, "name");
    target.runtimeKey = JsonStringField(data, "runtimeKey");
    target.x = JsonNumberTextField(data, "x", "");
    target.y = JsonNumberTextField(data, "y", "");
    target.z = JsonNumberTextField(data, "z", "");
    if (target.steamId.empty()) target.steamId = steamId;
    if (target.name.empty()) target.name = name;
    if (target.runtimeKey.empty()) target.runtimeKey = runtimeKey;

    if (requireSteam && target.steamId.empty()) {
        target.message = "Игрок подтверждён, но SteamID64 не получен; команда панели отменена.";
        AppendActionRecord(baseDir, "panel_player_resolve_native", false, steamId, name, target.message,
            std::string("{\"reason\":\"") + JsonEscape(reason) + "\",\"code\":\"steam\"}");
        return target;
    }
    if (requireLocation && (target.x.empty() || target.y.empty() || target.z.empty())) {
        target.message = "Игрок подтверждён, но live-координаты не получены; команда панели отменена.";
        AppendActionRecord(baseDir, "panel_player_resolve_native", false, target.steamId, target.name, target.message,
            std::string("{\"reason\":\"") + JsonEscape(reason) + "\",\"code\":\"location\"}");
        return target;
    }

    target.ok = true;
    target.message = resolved.message.empty() ? "Игрок подтверждён в live runtime." : resolved.message;
    return target;
}

struct VehicleSpawnOutcome {
    bool ok{};
    std::string message;
    std::string bridgeBody;
    std::string commandText;
    std::string runtimeRef;
    long long entityId{};
    std::string attemptsJson = "[]";
};

VehicleSpawnOutcome SpawnVehicleVerified(
    const std::wstring& baseDir,
    const std::string& vehicleId,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey = {}) {
    VehicleSpawnOutcome outcome;
    const auto token = VehicleAdminToken(vehicleId);
    if (token.empty()) {
        outcome.message = "ID транспорта не указан.";
        return outcome;
    }

    const auto liveTarget = ValidatePanelLiveTargetForCommandChannel(
        baseDir, steamId, name, runtimeKey, "spawn-vehicle-live-route", true, true);
    if (!liveTarget.ok) {
        outcome.message = liveTarget.message;
        outcome.bridgeBody = liveTarget.body;
        return outcome;
    }
    const auto liveSteamId = liveTarget.steamId.empty() ? steamId : liveTarget.steamId;
    const auto liveName = liveTarget.name.empty() ? name : liveTarget.name;
    const auto liveRuntimeKey = liveTarget.runtimeKey.empty() ? runtimeKey : liveTarget.runtimeKey;

    const auto beforeEntityId = LatestVehicleEntityId(baseDir, token);
    auto targetArgs = std::string("\"steamId\":\"") + JsonEscape(liveSteamId) +
        "\",\"name\":\"" + JsonEscape(liveName) + "\"";
    if (!liveRuntimeKey.empty()) targetArgs += ",\"runtimeKey\":\"" + JsonEscape(liveRuntimeKey) + "\"";
    auto br = BridgeExecute(baseDir, "spawn_vehicle",
        std::string("{") + targetArgs +
        ",\"vehicleId\":\"" + JsonEscape(token) + "\"}",
        45000);

    {
        std::ostringstream attempts;
        attempts << "[{\"transport\":\"ue4ss-bridge-live\""
                 << ",\"command\":\"spawn_vehicle\""
                 << ",\"vehicleId\":\"" << JsonEscape(token) << "\""
                 << ",\"ok\":" << (br.ok ? "true" : "false")
                 << ",\"message\":\"" << JsonEscape(br.message) << "\"";
        if (!br.body.empty()) attempts << ",\"bridge\":" << br.body;
        attempts << "}]";
        outcome.attemptsJson = attempts.str();
    }
    outcome.commandText = "spawn_vehicle " + token;
    if (br.ok) {
        const auto runtimeRef = JsonStringField(br.body, "runtimeRef");
        long long entityId = ExtractVehicleEntityIdFromText(runtimeRef);
        if (entityId <= 0) entityId = ExtractVehicleEntityIdFromText(br.body);
        if (entityId <= 0) entityId = ExtractVehicleEntityIdFromText(br.message);
        outcome.ok = true;
        outcome.message = entityId > 0 ? "vehicle bridge-live returned entity id" : "vehicle bridge-live accepted";
        outcome.bridgeBody = br.body;
        outcome.runtimeRef = runtimeRef.empty() && entityId > 0 ? std::to_string(entityId) : runtimeRef;
        outcome.entityId = entityId;
        if (entityId <= 0 && WaitForVehicleEntity(baseDir, token, beforeEntityId, entityId)) {
            outcome.message = "vehicle bridge-live db-verified";
            outcome.entityId = entityId;
        }
        return outcome;
    }

    if (LocalRconEnabled(baseDir)) {
        const auto rconLocationRef = VehicleRconLocationRef(baseDir, liveSteamId, liveName, liveRuntimeKey);
        if (rconLocationRef.empty()) {
            outcome.message = "Для спавна транспорта через командный канал нужен SteamID64 или live-координаты.";
            outcome.commandText = "SpawnVehicle " + token;
            return outcome;
        }

        const auto commandText = std::string("SpawnVehicle ") + token + " 1 Location " + ScumLocationArgument(rconLocationRef);
        const auto rcon = LocalRconSendConsoleCommand(baseDir, commandText);
        std::ostringstream attempts;
        attempts << "[{\"transport\":\"ue4ss-bridge-live\""
                 << ",\"command\":\"spawn_vehicle\""
                 << ",\"vehicleId\":\"" << JsonEscape(token) << "\""
                 << ",\"ok\":false"
                 << ",\"message\":\"" << JsonEscape(br.message) << "\"";
        if (!br.body.empty()) attempts << ",\"bridge\":" << br.body;
        attempts << "},{\"transport\":\"server-command\""
                 << ",\"command\":\"" << JsonEscape(LocalRconCommandText(commandText)) << "\""
                 << ",\"vehicleId\":\"" << JsonEscape(token) << "\""
                 << ",\"ok\":" << (rcon.ok ? "true" : "false")
                 << ",\"message\":\"" << JsonEscape(rcon.message) << "\"";
        if (!rcon.body.empty()) attempts << ",\"commandBody\":" << JsonValueOrString(rcon.body);
        attempts << "}]";
        outcome.attemptsJson = attempts.str();
        outcome.commandText = commandText;
        outcome.bridgeBody = rcon.body.empty() ? br.body : rcon.body;

        if (!rcon.ok) {
            outcome.message = rcon.message.empty() ? "Командный канал сервера не принял команду SpawnVehicle." : rcon.message;
            return outcome;
        }

        long long entityId = ExtractVehicleEntityIdFromText(rcon.body);
        if (entityId <= 0) entityId = ExtractVehicleEntityIdFromText(rcon.message);
        if (entityId > 0) {
            outcome.ok = true;
            outcome.message = "vehicle server-command returned entity id";
            outcome.entityId = entityId;
            outcome.runtimeRef = std::to_string(entityId);
            return outcome;
        }
        if (WaitForVehicleEntity(baseDir, token, beforeEntityId, entityId)) {
            outcome.ok = true;
            outcome.message = "vehicle server-command db-verified";
            outcome.entityId = entityId;
            outcome.runtimeRef = std::to_string(entityId);
            return outcome;
        }

        outcome.message = "Командный канал принял SpawnVehicle, но SCUM.db не показала новый функциональный транспорт.";
        return outcome;
    }

    if (!AllowUnverifiedServerCommandDelivery(baseDir)) {
        outcome.message = br.message.empty() ? "UE4SS bridge не подтвердил выдачу транспорта." : br.message;
        outcome.bridgeBody = br.body;
        return outcome;
    }

    if (ArkPanelEnabled(baseDir)) {
        const auto rconLocationRef = VehicleRconLocationRef(baseDir, liveSteamId, liveName, liveRuntimeKey);
        if (rconLocationRef.empty()) {
            outcome.message = "Для спавна транспорта через консоль хостинга нужен SteamID64 или live-координаты.";
            outcome.commandText = "SpawnVehicle " + token;
            return outcome;
        }

        const auto commandText = std::string("#SpawnVehicle ") + token + " 1 Location " + ScumLocationArgument(rconLocationRef);
        const auto rcon = ArkPanelSendConsoleCommand(baseDir, commandText);
        std::ostringstream attempts;
        attempts << "[{\"transport\":\"hosting-panel-command\""
                 << ",\"command\":\"" << JsonEscape(commandText) << "\""
                 << ",\"vehicleId\":\"" << JsonEscape(token) << "\""
                 << ",\"ok\":" << (rcon.ok ? "true" : "false")
                 << ",\"status\":" << rcon.status
                 << ",\"message\":\"" << JsonEscape(rcon.message) << "\"";
        if (!rcon.body.empty()) attempts << ",\"panelBody\":" << JsonValueOrString(rcon.body);
        attempts << "}]";
        outcome.attemptsJson = attempts.str();
        outcome.commandText = commandText;

        if (!rcon.ok) {
            outcome.message = rcon.message.empty() ? "Консоль хостинга не приняла команду SpawnVehicle." : rcon.message;
            outcome.bridgeBody = rcon.body;
            return outcome;
        }

        long long entityId = ExtractVehicleEntityIdFromText(rcon.body);
        if (entityId <= 0) entityId = ExtractVehicleEntityIdFromText(rcon.message);
        if (entityId > 0) {
            outcome.ok = true;
            outcome.message = "vehicle hosting-panel command returned entity id";
            outcome.bridgeBody = rcon.body;
            outcome.entityId = entityId;
            outcome.runtimeRef = std::to_string(entityId);
            return outcome;
        }
        if (WaitForVehicleEntity(baseDir, token, beforeEntityId, entityId)) {
            outcome.ok = true;
            outcome.message = "vehicle hosting-panel command db-verified";
            outcome.bridgeBody = rcon.body;
            outcome.entityId = entityId;
            return outcome;
        }

        outcome.bridgeBody = rcon.body;
        outcome.message = "Консоль хостинга приняла SpawnVehicle, но SCUM.db не показала новый функциональный транспорт.";
        return outcome;
    }

    br = BridgeExecute(baseDir, "spawn_vehicle",
        std::string("{") + targetArgs +
        ",\"vehicleId\":\"" + JsonEscape(token) + "\"}",
        18000);

    std::ostringstream attempts;
    attempts << "[{\"command\":\"spawn_vehicle\""
             << ",\"vehicleId\":\"" << JsonEscape(token) << "\""
             << ",\"ok\":" << (br.ok ? "true" : "false")
             << ",\"message\":\"" << JsonEscape(br.message) << "\"";
    if (!br.body.empty()) attempts << ",\"bridge\":" << br.body;
    attempts << "}]";
    outcome.attemptsJson = attempts.str();

    if (!br.ok) {
        outcome.message = br.message;
        outcome.bridgeBody = br.body;
        outcome.commandText = "spawn_vehicle " + token;
        return outcome;
    }

    const auto runtimeRef = JsonStringField(br.body, "runtimeRef");
    long long entityId = ExtractVehicleEntityIdFromText(runtimeRef);
    if (entityId <= 0) entityId = ExtractVehicleEntityIdFromText(br.body);
    if (entityId <= 0) entityId = ExtractVehicleEntityIdFromText(br.message);
    if (entityId > 0 || WaitForVehicleEntity(baseDir, token, beforeEntityId, entityId)) {
        outcome.ok = true;
        outcome.message = entityId > 0 ? "vehicle command returned entity id" : "vehicle db-verified";
        outcome.bridgeBody = br.body;
        outcome.commandText = "spawn_vehicle " + token;
        outcome.runtimeRef = runtimeRef.empty() && entityId > 0 ? std::to_string(entityId) : runtimeRef;
        outcome.entityId = entityId;
        return outcome;
    }

    outcome.bridgeBody = br.body;
    outcome.commandText = "spawn_vehicle " + token;
    outcome.message = "vehicle command returned but SCUM.db did not show a new functional vehicle";
    return outcome;
}

std::string PlayerInventoryDbJson(const std::wstring& baseDir, const std::string& steamId, const std::string& name) {
    const auto filter = PlayerSqlFilter(steamId, name);
    const std::string itemIdExpr =
        "CASE "
        "WHEN instr(COALESCE(e.class, ''), '.') > 0 THEN replace(substr(COALESCE(e.class, ''), instr(COALESCE(e.class, ''), '.') + 1), '_C', '') "
        "WHEN instr(COALESCE(e.class, ''), ':') > 0 THEN replace(substr(COALESCE(e.class, ''), instr(COALESCE(e.class, ''), ':') + 1), '_C', '') "
        "ELSE replace(COALESCE(e.class, ''), '_C', '') END";
    const std::string quickItemIdExpr =
        "CASE "
        "WHEN instr(COALESCE(e.class, q.item_entity_setup, ''), '.') > 0 THEN replace(substr(COALESCE(e.class, q.item_entity_setup, ''), instr(COALESCE(e.class, q.item_entity_setup, ''), '.') + 1), '_C', '') "
        "WHEN instr(COALESCE(e.class, q.item_entity_setup, ''), ':') > 0 THEN replace(substr(COALESCE(e.class, q.item_entity_setup, ''), instr(COALESCE(e.class, q.item_entity_setup, ''), ':') + 1), '_C', '') "
        "ELSE replace(COALESCE(e.class, q.item_entity_setup, ''), '_C', '') END";

    const std::string inventorySql =
        "WITH RECURSIVE latest_prisoner_entity AS ("
        "  SELECT prisoner_id, MAX(entity_id) AS entity_id FROM prisoner_entity GROUP BY prisoner_id"
        "), "
        "player_roots AS ("
        "  SELECT up.user_id AS steam_id, up.name AS player_name, pe.entity_id AS player_entity_id, ec.id AS inventory_component_id "
        "  FROM user_profile up "
        "  JOIN latest_prisoner_entity lpe ON lpe.prisoner_id = up.prisoner_id "
        "  JOIN prisoner_entity pe ON pe.prisoner_id = lpe.prisoner_id AND pe.entity_id = lpe.entity_id "
        "  JOIN entity_component ec ON ec.entity_id = pe.entity_id "
        "  WHERE up.user_id IS NOT NULL AND up.user_id <> '' " + filter + " AND ec.name = 'Inventory'"
        "), "
        "inventory_tree AS ("
        "  SELECT pr.steam_id, pr.player_name, pr.player_entity_id, pr.player_entity_id AS container_entity_id, "
        "         eice.entity_id, 0 AS depth, eice.data AS slot, "
        "         printf('%08d:%08d', eice.data, eice.entity_id) AS path "
        "  FROM player_roots pr "
        "  JOIN entity_inventory_component_entry eice ON eice.entity_component_id = pr.inventory_component_id "
        "  UNION ALL "
        "  SELECT it.steam_id, it.player_name, it.player_entity_id, it.entity_id AS container_entity_id, "
        "         child.entity_id, it.depth + 1, child.data AS slot, "
        "         it.path || '>' || printf('%08d:%08d', child.data, child.entity_id) AS path "
        "  FROM inventory_tree it "
        "  JOIN entity_component ec ON ec.entity_id = it.entity_id AND ec.name = 'Inventory' "
        "  JOIN entity_inventory_component_entry child ON child.entity_component_id = ec.id "
        "  WHERE it.depth < 6"
        ") "
        "SELECT it.steam_id AS steamId, it.player_name AS playerName, it.player_entity_id AS playerEntityId, "
        "       it.container_entity_id AS containerEntityId, it.entity_id AS entityId, it.depth AS depth, it.slot AS slot, it.path AS path, "
        "       COALESCE(e.class, '') AS itemClass, " + itemIdExpr + " AS itemId, "
        "       CASE WHEN it.depth = 0 AND it.slot = 2 THEN 'hands' "
        "            WHEN it.depth = 0 AND it.slot = 5 THEN 'equipped' "
        "            WHEN it.depth = 0 THEN 'root' "
        "            ELSE 'container' END AS slotKind, "
        "       CASE WHEN EXISTS (SELECT 1 FROM entity_component c WHERE c.entity_id = it.entity_id AND c.name = 'Inventory') THEN 1 ELSE 0 END AS hasInventory "
        "FROM inventory_tree it "
        "JOIN entity e ON e.id = it.entity_id "
        "ORDER BY it.steam_id, it.path";

    const std::string quickSql =
        "WITH latest_prisoner_entity AS ("
        "  SELECT prisoner_id, MAX(entity_id) AS entity_id FROM prisoner_entity GROUP BY prisoner_id"
        ") "
        "SELECT up.user_id AS steamId, up.name AS playerName, pe.entity_id AS playerEntityId, "
        "       q.slot_index AS slotIndex, q.item_entity_id AS entityId, q.item_entity_setup AS itemEntitySetup, "
        "       COALESCE(e.class, '') AS itemClass, " + quickItemIdExpr + " AS itemId, "
        "       q.is_in_throwing_mode AS throwingMode "
        "FROM user_profile up "
        "JOIN latest_prisoner_entity lpe ON lpe.prisoner_id = up.prisoner_id "
        "JOIN prisoner_entity pe ON pe.prisoner_id = lpe.prisoner_id AND pe.entity_id = lpe.entity_id "
        "JOIN prisoner_inventory_quick_access_slot q ON q.prisoner_entity_id = pe.entity_id "
        "LEFT JOIN entity e ON e.id = q.item_entity_id "
        "WHERE up.user_id IS NOT NULL AND up.user_id <> '' " + filter +
        "ORDER BY q.slot_index";

    std::string rows = "[]";
    std::string quickSlots = "[]";
    std::string rowsError;
    std::string quickError;
    const bool rowsOk = SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), inventorySql, 2500, rows, rowsError);
    const bool quickOk = SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), quickSql, 30, quickSlots, quickError);
    if (!rowsOk) rows = "[]";
    if (!quickOk) quickSlots = "[]";

    std::ostringstream ss;
    ss << "{\"source\":\"scum-db-recursive\""
       << ",\"steamId\":\"" << JsonEscape(steamId) << "\""
       << ",\"name\":\"" << JsonEscape(name) << "\""
       << ",\"rows\":" << rows
       << ",\"quickSlots\":" << quickSlots
       << ",\"errors\":[";
    bool first = true;
    if (!rowsError.empty()) {
        ss << "{\"area\":\"inventory\",\"message\":\"" << JsonEscape(rowsError) << "\"}";
        first = false;
    }
    if (!quickError.empty()) {
        if (!first) ss << ",";
        ss << "{\"area\":\"quickSlots\",\"message\":\"" << JsonEscape(quickError) << "\"}";
    }
    ss << "]}";
    return ss.str();
}

std::string EconomyDbJson(const std::wstring& baseDir) {
    std::string wallets;
    std::string walletError;
    if (!SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir),
        "SELECT id AS profileId, name, user_id AS steamId, fame_points AS famePoints, "
        "money_balance AS walletBalance, last_login_time AS lastLoginUtc, last_logout_time AS lastLogoutUtc "
        "FROM user_profile ORDER BY money_balance DESC, fame_points DESC LIMIT 200",
        200, wallets, walletError)) {
        wallets = "[]";
    }

    std::string bankAccounts;
    std::string bankError;
    if (!SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir),
        "SELECT r.id AS accountId, r.user_profile_id AS profileId, up.name AS ownerName, up.user_id AS ownerSteamId, "
        "r.bank_account_number AS accountNumber, r.save_timestamp AS saveTimestamp, "
        "c.currency_type AS currencyType, c.account_balance AS accountBalance "
        "FROM bank_account_registry r "
        "LEFT JOIN bank_account_registry_currencies c ON c.bank_account_id = r.id "
        "LEFT JOIN user_profile up ON up.id = r.user_profile_id "
        "ORDER BY c.account_balance DESC, r.id LIMIT 300",
        300, bankAccounts, bankError)) {
        bankAccounts = "[]";
    }

    const auto recent = StateEventsOrLogJson(baseDir, "economy", "economy",
        { "Economy", "ATM", "Currency", "Transaction", "Trader:", "Bank:" });

    std::ostringstream ss;
    ss << "{\"source\":\"sqlite+nedjin-state\""
       << ",\"wallets\":" << wallets
       << ",\"bankAccounts\":" << bankAccounts
       << ",\"recent\":" << recent
       << ",\"errors\":[";
    bool first = true;
    if (!walletError.empty()) {
        ss << "{\"area\":\"wallets\",\"message\":\"" << JsonEscape(walletError) << "\"}";
        first = false;
    }
    if (!bankError.empty()) {
        if (!first) ss << ",";
        ss << "{\"area\":\"bankAccounts\",\"message\":\"" << JsonEscape(bankError) << "\"}";
    }
    ss << "]}";
    return ss.str();
}

std::vector<GrantItem> ParseGrantItems(const std::string& configJson) {
    std::vector<GrantItem> items;
    auto itemsSpec = JsonStringField(configJson, "itemsSpec");
    if (itemsSpec.empty()) itemsSpec = JsonStringField(configJson, "ItemsSpec");
    if (!TrimAscii(itemsSpec).empty()) {
        std::replace(itemsSpec.begin(), itemsSpec.end(), '\r', ';');
        std::replace(itemsSpec.begin(), itemsSpec.end(), '\n', ';');
        std::stringstream ss(itemsSpec);
        std::string part;
        while (std::getline(ss, part, ';')) {
            part = TrimAscii(part);
            if (part.empty()) continue;
            auto sep = part.find('|');
            if (sep == std::string::npos) sep = part.find(':');
            auto itemId = sep == std::string::npos ? part : part.substr(0, sep);
            auto qtyText = sep == std::string::npos ? std::string("1") : part.substr(sep + 1);
            itemId = TrimAscii(itemId);
            if (!IsPlausibleScumItemToken(itemId)) continue;
            const int quantity = std::clamp(std::atoi(TrimAscii(qtyText).c_str()), 1, 100);
            items.push_back(GrantItem{ itemId, quantity });
        }
        if (!items.empty()) return items;
    }
    auto rawItems = JsonRawField(configJson, "Items");
    if (rawItems.empty()) rawItems = JsonRawField(configJson, "items");
    const auto haystack = rawItems.empty() ? configJson : rawItems;

    std::vector<std::string> objects;
    const auto arrayStart = haystack.find('[');
    if (arrayStart != std::string::npos) {
        bool inString = false;
        bool escaped = false;
        int depth = 0;
        size_t objectStart = std::string::npos;
        for (size_t i = arrayStart + 1; i < haystack.size(); ++i) {
            const char ch = haystack[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\' && inString) {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                inString = !inString;
                continue;
            }
            if (inString) continue;
            if (ch == '{') {
                if (depth == 0) objectStart = i;
                ++depth;
                continue;
            }
            if (ch == '}') {
                --depth;
                if (depth == 0 && objectStart != std::string::npos) {
                    objects.push_back(haystack.substr(objectStart, i - objectStart + 1));
                    objectStart = std::string::npos;
                }
                continue;
            }
            if (ch == ']' && depth == 0) break;
        }
    }
    if (objects.empty() && haystack.find('{') != std::string::npos) {
        objects.push_back(haystack);
    }

    for (const auto& object : objects) {
        GrantItem item;
        item.itemId = JsonStringField(object, "ItemId");
        if (item.itemId.empty()) item.itemId = JsonStringField(object, "itemId");
        item.itemId = TrimAscii(item.itemId);
        item.quantity = JsonIntField(object, "Quantity", JsonIntField(object, "quantity", 1));
        item.quantity = std::clamp(item.quantity, 1, 100);
        if (IsPlausibleScumItemToken(item.itemId)) items.push_back(item);
    }
    return items;
}

bool IdentityTextLooksUnsafe(const std::string& value);

std::string PlayerTargetArgs(const std::string& steamId, const std::string& name, const std::string& runtimeKey = {}) {
    const auto safeSteamId = IdentityTextLooksUnsafe(steamId) ? std::string{} : steamId;
    const auto safeName = IdentityTextLooksUnsafe(name) ? std::string{} : name;
    auto args = std::string("\"steamId\":\"") + JsonEscape(safeSteamId) + "\",\"name\":\"" + JsonEscape(safeName) + "\"";
    if (!runtimeKey.empty()) args += ",\"runtimeKey\":\"" + JsonEscape(runtimeKey) + "\"";
    return args;
}

std::string GrantItemsSpec(const std::vector<GrantItem>& items) {
    std::ostringstream ss;
    bool first = true;
    for (const auto& item : items) {
        const auto itemId = TrimAscii(item.itemId);
        if (!IsPlausibleScumItemToken(itemId)) continue;
        if (!first) ss << ";";
        first = false;
        ss << itemId << "|" << std::clamp(item.quantity, 1, 100);
    }
    return ss.str();
}

std::string PrewarmItemsSpecFromRequest(const std::string& body) {
    auto spec = JsonStringField(body, "itemsSpec");
    if (spec.empty()) spec = JsonStringField(body, "ItemsSpec");
    if (!TrimAscii(spec).empty()) return spec;

    auto items = ParseGrantItems(body);
    if (!items.empty()) return GrantItemsSpec(items);

    auto itemId = JsonStringField(body, "itemId");
    if (itemId.empty()) itemId = JsonStringField(body, "ItemId");
    if (itemId.empty()) itemId = JsonStringField(body, "itemClass");
    if (itemId.empty()) itemId = JsonStringField(body, "class");
    if (itemId.empty()) itemId = JsonStringField(body, "name");
    itemId = TrimAscii(itemId);
    if (!IsPlausibleScumItemToken(itemId)) return {};

    const int quantity = std::clamp(JsonIntField(body, "quantity", JsonIntField(body, "Quantity", 1)), 1, 100);
    return itemId + "|" + std::to_string(quantity);
}

struct ResolvedPlayerIdentity {
    std::string steamId;
    std::string name;
    std::string runtimeKey;
    std::string profileId;
    std::string joinedUtc;
    std::string lastSeenUtc;
    bool fromLog = false;
};

bool IdentityTextLooksUnsafe(const std::string& value) {
    const auto trimmed = TrimAscii(value);
    if (trimmed.empty()) return false;
    const auto lower = ToLowerAscii(trimmed);
    const bool allDigits = std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
    if (allDigits && (trimmed.size() < 15 || trimmed.size() > 20)) return true;
    return lower == "unknown" ||
        lower.find("fstring:") != std::string::npos ||
        lower.find("uscriptstruct:") != std::string::npos ||
        lower.find("uobject:") != std::string::npos ||
        lower.find("userdata:") != std::string::npos ||
        lower.find("fname:") != std::string::npos ||
        lower.find("function:") != std::string::npos ||
        lower == "name" ||
        lower == "steamid" ||
        lower == "steam_id" ||
        lower == "targetname" ||
        lower == "targetsteamid" ||
        lower == "player";
}

ResolvedPlayerIdentity ResolvePlayerIdentity(
    const std::wstring& baseDir,
    const std::string& requestedSteamId,
    const std::string& requestedName,
    const std::string& requestedRuntimeKey = {}) {
    ResolvedPlayerIdentity identity;
    identity.steamId = IdentityTextLooksUnsafe(requestedSteamId) ? std::string{} : requestedSteamId;
    identity.name = IdentityTextLooksUnsafe(requestedName) ? std::string{} : requestedName;
    identity.runtimeKey = requestedRuntimeKey;

    const auto sessions = OnlineSessions(InferPlayerSessionsFromLog(baseDir));
    const auto wantedSteam = ToLowerAscii(TrimAscii(requestedSteamId));
    const auto wantedName = ToLowerAscii(TrimAscii(requestedName));
    const auto wantedRuntime = TrimAscii(requestedRuntimeKey);
    const bool hasSafeWantedSteam = !wantedSteam.empty() && !IdentityTextLooksUnsafe(requestedSteamId);
    const bool hasSafeWantedName = !wantedName.empty() && !IdentityTextLooksUnsafe(requestedName);
    const bool hasSafeWantedRuntime = !wantedRuntime.empty();
    const bool hasExplicitTarget = hasSafeWantedSteam || hasSafeWantedName || hasSafeWantedRuntime;

    const LogPlayerSession* matched = nullptr;
    for (const auto& session : sessions) {
        const auto sessionSteam = ToLowerAscii(session.steamId);
        const auto sessionName = ToLowerAscii(session.name);
        if (hasSafeWantedSteam && !sessionSteam.empty() &&
            (sessionSteam.find(wantedSteam) != std::string::npos || wantedSteam.find(sessionSteam) != std::string::npos)) {
            matched = &session;
            break;
        }
        if (hasSafeWantedName && !sessionName.empty() &&
            (sessionName.find(wantedName) != std::string::npos || wantedName.find(sessionName) != std::string::npos)) {
            matched = &session;
            break;
        }
    }

    if (matched == nullptr && sessions.size() == 1 && !hasExplicitTarget) {
        matched = &sessions.front();
    }

    if (matched != nullptr) {
        if (!matched->steamId.empty()) identity.steamId = matched->steamId;
        if (!matched->name.empty()) identity.name = matched->name;
        if (!matched->profileId.empty()) identity.profileId = matched->profileId;
        identity.joinedUtc = matched->joinedUtc;
        identity.lastSeenUtc = matched->lastSeenUtc;
        identity.fromLog = true;
    }

    return identity;
}

ResolvedPlayerIdentity ResolveLivePlayerIdentity(
    const std::wstring& baseDir,
    const std::string& requestedSteamId,
    const std::string& requestedName,
    const std::string& requestedRuntimeKey = {}) {
    auto identity = ResolvePlayerIdentity(baseDir, requestedSteamId, requestedName, requestedRuntimeKey);

    const auto wantedSteam = ToLowerAscii(TrimAscii(requestedSteamId));
    const auto wantedName = ToLowerAscii(TrimAscii(requestedName));
    const auto wantedRuntime = TrimAscii(requestedRuntimeKey);
    const bool hasSafeWantedSteam = !wantedSteam.empty() && !IdentityTextLooksUnsafe(requestedSteamId);
    const bool hasSafeWantedName = !wantedName.empty() && !IdentityTextLooksUnsafe(requestedName);
    const bool hasWantedRuntime = !wantedRuntime.empty();

    if (!hasSafeWantedSteam && !hasSafeWantedName && !hasWantedRuntime) {
        return identity;
    }
    const bool alreadyHasSteam = !identity.steamId.empty() && !IdentityTextLooksUnsafe(identity.steamId);
    const bool alreadyHasRuntime = !identity.runtimeKey.empty();
    if (alreadyHasSteam && (alreadyHasRuntime || !hasWantedRuntime)) {
        return identity;
    }

    const auto liveBody = LivePlayersJson(baseDir, 3000);
    for (const auto& object : ExtractPlayerObjects(liveBody)) {
        const auto liveSteam = FlatJsonStringValue(object, "steamId");
        const auto liveName = FlatJsonStringValue(object, "name");
        auto liveRuntime = FlatJsonStringValue(object, "runtimeKey");
        if (liveRuntime.empty()) liveRuntime = FlatJsonStringValue(object, "targetRuntimeKey");
        const auto liveProfileId = FlatJsonStringValue(object, "profileId");
        const auto liveSteamLower = ToLowerAscii(liveSteam);
        const auto liveNameLower = ToLowerAscii(liveName);

        bool matched = false;
        if (hasWantedRuntime && !liveRuntime.empty() && liveRuntime == wantedRuntime) {
            matched = true;
        } else if (hasSafeWantedSteam && !liveSteamLower.empty() &&
            (liveSteamLower == wantedSteam || liveSteamLower.find(wantedSteam) != std::string::npos || wantedSteam.find(liveSteamLower) != std::string::npos)) {
            matched = true;
        } else if (hasSafeWantedName && !liveNameLower.empty() &&
            (liveNameLower == wantedName || liveNameLower.find(wantedName) != std::string::npos || wantedName.find(liveNameLower) != std::string::npos)) {
            matched = true;
        }
        if (!matched) continue;

        if (!liveSteam.empty() && !IdentityTextLooksUnsafe(liveSteam)) identity.steamId = liveSteam;
        if (!liveName.empty() && !IdentityTextLooksUnsafe(liveName)) identity.name = liveName;
        if (!liveRuntime.empty()) identity.runtimeKey = liveRuntime;
        if (!liveProfileId.empty()) identity.profileId = liveProfileId;
        identity.lastSeenUtc = UtcIsoNow();
        identity.fromLog = false;
        break;
    }

    return identity;
}

std::vector<std::string> ExtractArrayObjects(const std::string& json) {
    std::vector<std::string> objects;
    const auto arrayStart = json.find('[');
    if (arrayStart == std::string::npos) return objects;
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    size_t objectStart = std::string::npos;
    for (size_t i = arrayStart + 1; i < json.size(); ++i) {
        const char ch = json[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\' && inString) {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            inString = !inString;
            continue;
        }
        if (inString) continue;
        if (ch == '{') {
            if (depth == 0) objectStart = i;
            ++depth;
            continue;
        }
        if (ch == '}') {
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(json.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
            continue;
        }
        if (ch == ']' && depth == 0) break;
    }
    return objects;
}

std::string ModuleStateTailJson(const std::wstring& baseDir, const std::string& key, int limit) {
    const auto path = ModuleReadableStatePath(baseDir, key);
    bool truncated = false;
    auto state = TrimAscii(ReadTailBytes(path, std::max<size_t>(ModuleStateMaxBytes(key), 512 * 1024), &truncated));
    if (state.empty()) return "[]";
    if (!truncated && state.front() == '{') return state;
    if (!truncated && limit <= 0) return state;
    auto parseText = truncated ? ("[" + state) : state;
    auto objects = ExtractArrayObjects(parseText);
    if (truncated && !objects.empty()) {
        objects.erase(objects.begin());
    }
    if (objects.empty()) return "[]";
    if (limit <= 0 || static_cast<int>(objects.size()) <= limit) {
        return truncated ? JoinJsonObjectArray(objects) : state;
    }
    std::vector<std::string> out;
    const auto start = objects.size() - static_cast<size_t>(limit);
    for (size_t i = start; i < objects.size(); ++i) {
        out.push_back(objects[i]);
    }
    return JoinJsonObjectArray(out);
}

void PushUniqueString(std::vector<std::string>& values, const std::string& value) {
    if (value.empty()) return;
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

bool AddLogClearTarget(std::vector<std::string>& keys, std::string target) {
    target = ToLowerAscii(TrimAscii(target));
    target = StateStorageKey(target);
    if (target.empty()) return false;

    if (target == "all" || target == "logs" || target == "project" || target == "project-logs") {
        PushUniqueString(keys, "chat");
        PushUniqueString(keys, "kill-feed");
        PushUniqueString(keys, "server-kill-feed");
        PushUniqueString(keys, "action-log");
        PushUniqueString(keys, "discord-log");
        PushUniqueString(keys, "wargm-shop");
        PushUniqueString(keys, "gamestores-shop");
        return true;
    }
    if (target == "chat" || target == "global" || target == "local" || target == "squad" ||
        target == "admin" || target == "command-chat" || target == "commands-chat") {
        PushUniqueString(keys, "chat");
        return true;
    }
    if (target == "kills" || target == "kill" || target == "killfeed" || target == "kill-feed" ||
        target == "server-kill-feed" || target == "combat") {
        PushUniqueString(keys, "kill-feed");
        PushUniqueString(keys, "server-kill-feed");
        return true;
    }
    if (target == "action" || target == "actions" || target == "action-log" ||
        target == "nedjin" || target == "commands" || target == "command-trace") {
        PushUniqueString(keys, "action-log");
        return true;
    }
    if (target == "discord" || target == "discord-log") {
        PushUniqueString(keys, "discord-log");
        return true;
    }
    if (target == "wargm" || target == "wargm-shop") {
        PushUniqueString(keys, "wargm-shop");
        return true;
    }
    if (target == "gamestores" || target == "gamestores-shop" || target == "game-stores") {
        PushUniqueString(keys, "gamestores-shop");
        return true;
    }

    return false;
}

std::vector<std::string> LogClearTargets(const Request& req) {
    std::vector<std::string> keys;
    bool hadExplicitTarget = false;
    hadExplicitTarget = AddLogClearTarget(keys, QueryValue(req.path, "target")) || hadExplicitTarget;
    hadExplicitTarget = AddLogClearTarget(keys, QueryValue(req.path, "type")) || hadExplicitTarget;
    hadExplicitTarget = AddLogClearTarget(keys, JsonRootStringField(req.body, "target")) || hadExplicitTarget;
    hadExplicitTarget = AddLogClearTarget(keys, JsonRootStringField(req.body, "type")) || hadExplicitTarget;

    const auto targetsRaw = JsonRootRawField(req.body, "targets");
    if (!targetsRaw.empty()) {
        size_t pos = 0;
        while (pos < targetsRaw.size()) {
            const auto quote = targetsRaw.find('"', pos);
            if (quote == std::string::npos) break;
            size_t end = quote + 1;
            bool escaped = false;
            for (; end < targetsRaw.size(); ++end) {
                const char ch = targetsRaw[end];
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (ch == '\\') {
                    escaped = true;
                    continue;
                }
                if (ch == '"') break;
            }
            if (end >= targetsRaw.size()) break;
            hadExplicitTarget = AddLogClearTarget(keys, JsonUnquoteRawString(targetsRaw.substr(quote, end - quote + 1))) || hadExplicitTarget;
            pos = end + 1;
        }
    }

    if (keys.empty() && !hadExplicitTarget) PushUniqueString(keys, "chat");
    return keys;
}

std::string ClearProjectLogsJson(const std::wstring& baseDir, const std::vector<std::string>& keys) {
    std::ostringstream files;
    files << "[";
    bool first = true;
    bool allOk = true;
    long long totalBefore = 0;
    long long totalAfter = 0;
    for (const auto& key : keys) {
        const auto beforeText = ReadTextFile(ModuleReadableStatePath(baseDir, key));
        const auto beforeBytes = static_cast<long long>(beforeText.size());
        const auto ok = WriteTextFile(ModuleStatePath(baseDir, key), "[]");
        const auto afterText = ReadTextFile(ModuleStatePath(baseDir, key));
        const auto afterBytes = static_cast<long long>(afterText.size());
        allOk = allOk && ok;
        totalBefore += beforeBytes;
        totalAfter += afterBytes;
        if (!first) files << ",";
        first = false;
        files << "{\"key\":\"" << JsonEscape(key) << "\""
              << ",\"ok\":" << (ok ? "true" : "false")
              << ",\"bytesBefore\":" << beforeBytes
              << ",\"bytesAfter\":" << afterBytes
              << "}";
    }
    files << "]";

    std::ostringstream out;
    out << "{\"cleared\":" << (allOk ? "true" : "false")
        << ",\"message\":\"Журналы проекта очищены. SCUM.log не изменялся.\""
        << ",\"serverLogTouched\":false"
        << ",\"bytesBefore\":" << totalBefore
        << ",\"bytesAfter\":" << totalAfter
        << ",\"files\":" << files.str()
        << "}";
    return out.str();
}

std::string JsonScalarField(const std::string& body, const std::string& key) {
    auto value = JsonStringField(body, key);
    if (!value.empty()) return value;
    auto raw = TrimAscii(JsonRawField(body, key));
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        return JsonStringField(std::string("{\"") + key + "\":" + raw + "}", key);
    }
    if (raw.empty() || raw.front() == '{' || raw.front() == '[') return {};
    return raw;
}

std::string JsonScalarAny(const std::string& body, std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        auto value = JsonScalarField(body, key);
        if (!TrimAscii(value).empty()) return TrimAscii(value);
    }
    return {};
}

std::string JsonRawFieldAny(const std::string& body, std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        auto value = TrimAscii(JsonRawField(body, key));
        if (!value.empty()) return value;
    }
    return {};
}

std::string FirstNonEmpty(std::initializer_list<std::string> values) {
    for (auto value : values) {
        value = TrimAscii(value);
        if (!value.empty()) return value;
    }
    return {};
}

std::string JsonDirectRawValueAt(const std::string& body, size_t pos) {
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) return {};

    const char first = body[pos];
    if (first == '{' || first == '[') {
        const char open = first;
        const char close = first == '{' ? '}' : ']';
        int depth = 0;
        bool inString = false;
        bool esc = false;
        for (size_t i = pos; i < body.size(); ++i) {
            const char ch = body[i];
            if (esc) {
                esc = false;
                continue;
            }
            if (ch == '\\') {
                esc = inString;
                continue;
            }
            if (ch == '"') {
                inString = !inString;
                continue;
            }
            if (inString) continue;
            if (ch == open) ++depth;
            if (ch == close) {
                --depth;
                if (depth == 0) return body.substr(pos, i - pos + 1);
            }
        }
        return {};
    }

    if (first == '"') {
        bool esc = false;
        for (size_t i = pos + 1; i < body.size(); ++i) {
            const char ch = body[i];
            if (esc) {
                esc = false;
                continue;
            }
            if (ch == '\\') {
                esc = true;
                continue;
            }
            if (ch == '"') return body.substr(pos, i - pos + 1);
        }
        return {};
    }

    const auto end = body.find_first_of(",}\r\n", pos);
    return TrimAscii(body.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
}

std::string JsonDirectRawField(const std::string& body, const std::string& key) {
    size_t pos = body.find('{');
    if (pos == std::string::npos) return {};
    bool inString = false;
    bool esc = false;
    int depth = 0;
    for (size_t i = pos; i < body.size(); ++i) {
        const char ch = body[i];
        if (esc) {
            esc = false;
            continue;
        }
        if (ch == '\\' && inString) {
            esc = true;
            continue;
        }
        if (ch == '"') {
            if (!inString && depth == 1) {
                size_t j = i + 1;
                std::string prop;
                bool propEsc = false;
                for (; j < body.size(); ++j) {
                    const char pj = body[j];
                    if (propEsc) {
                        prop.push_back(pj);
                        propEsc = false;
                        continue;
                    }
                    if (pj == '\\') {
                        propEsc = true;
                        continue;
                    }
                    if (pj == '"') break;
                    prop.push_back(pj);
                }
                if (j >= body.size()) return {};
                size_t colon = j + 1;
                while (colon < body.size() && std::isspace(static_cast<unsigned char>(body[colon]))) ++colon;
                if (colon < body.size() && body[colon] == ':' && prop == key) {
                    return JsonDirectRawValueAt(body, colon + 1);
                }
                i = j;
                continue;
            }
            inString = !inString;
            continue;
        }
        if (inString) continue;
        if (ch == '{' || ch == '[') ++depth;
        if (ch == '}' || ch == ']') {
            --depth;
            if (depth <= 0 && ch == '}') break;
        }
    }
    return {};
}

std::string JsonDirectScalarField(const std::string& body, const std::string& key) {
    const auto raw = TrimAscii(JsonDirectRawField(body, key));
    if (raw.empty() || raw.front() == '{' || raw.front() == '[') return {};
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        return JsonStringField(std::string("{\"") + key + "\":" + raw + "}", key);
    }
    return raw;
}

std::string JsonDirectScalarAny(const std::string& body, std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        auto value = JsonDirectScalarField(body, key);
        if (!TrimAscii(value).empty()) return TrimAscii(value);
    }
    return {};
}

std::string JsonDirectNestedScalarAny(const std::string& body, const std::string& parentKey, std::initializer_list<const char*> keys) {
    const auto nested = TrimAscii(JsonDirectRawField(body, parentKey));
    if (nested.empty() || nested.front() != '{') return {};
    return JsonDirectScalarAny(nested, keys);
}

int JsonDirectIntField(const std::string& body, const std::string& key, int fallback) {
    const auto raw = TrimAscii(JsonDirectRawField(body, key));
    if (raw.empty()) return fallback;
    return std::atoi(raw.c_str());
}

std::vector<std::string> ExtractJsonObjectsDeep(const std::string& json) {
    std::vector<std::string> objects;
    std::vector<size_t> starts;
    bool inString = false;
    bool escaped = false;
    for (size_t i = 0; i < json.size(); ++i) {
        const char ch = json[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\' && inString) {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            inString = !inString;
            continue;
        }
        if (inString) continue;
        if (ch == '{') {
            starts.push_back(i);
            continue;
        }
        if (ch == '}' && !starts.empty()) {
            const auto start = starts.back();
            starts.pop_back();
            objects.push_back(json.substr(start, i - start + 1));
        }
    }
    return objects;
}

std::string FindSteamIdInText(const std::string& text) {
    static const std::regex re(R"(\b\d{17}\b)");
    std::smatch match;
    if (std::regex_search(text, match, re)) return match.str(0);
    return {};
}

std::string NormalizeDeliveryMode(std::string mode) {
    mode = ToLowerAscii(TrimAscii(mode));
    if (mode == "money" || mode == "currency" || mode == "normal") return "money";
    if (mode == "gold") return "gold";
    if (mode == "vehicle" || mode == "spawnvehicle") return "vehicle";
    if (mode == "skill" || mode == "setskill") return "skill";
    if (mode == "allskills" || mode == "skills") return "allskills";
    if (mode == "fame" || mode == "changefame" || mode == "famepoints") return "fame";
    if (mode == "setfame" || mode == "setfamepoints") return "setfame";
    if (mode == "vip") return "vip";
    if (mode == "cargodrop" || mode == "cargo" || mode == "airdrop") return "cargodrop";
    if (mode == "attributes" || mode == "attribute" || mode == "stats") return "attributes";
    if (mode == "playercommand" || mode == "servercommand" || mode == "command" || mode == "admincommand") return "command";
    return "item";
}

struct WargmOperationNative {
    std::string operationId;
    std::string offerId;
    std::string title;
    std::string itemId;
    std::string steamId;
    std::string recipientName;
    std::string delivery;
    int quantity = 1;
};

WargmOperationNative ParseWargmOperation(const std::string& object) {
    const auto offer = JsonDirectRawField(object, "offer");
    const auto user = JsonDirectRawField(object, "user");
    WargmOperationNative op;
    op.operationId = JsonDirectScalarAny(object, { "operation_id", "operationId", "id", "Id" });
    op.offerId = FirstNonEmpty({
        JsonDirectScalarAny(object, { "offer_id", "goods_id", "product_id", "offerId", "productId" }),
        JsonDirectScalarAny(offer, { "id", "offer_id", "offerId" })
    });
    op.title = FirstNonEmpty({
        JsonDirectScalarAny(object, { "title", "name", "offer_title", "product_name", "goods_name", "offerTitle", "productName" }),
        JsonDirectScalarAny(offer, { "title", "name" })
    });
    op.itemId = FirstNonEmpty({
        JsonDirectScalarAny(object, { "item", "item_id", "itemId", "asset", "asset_id", "assetId" }),
        JsonDirectScalarAny(offer, { "item", "item_id", "itemId", "asset", "asset_id", "assetId" })
    });
    op.steamId = FirstNonEmpty({
        JsonDirectScalarAny(object, { "steam_id", "user_steam_id", "steamId", "userSteamId", "steam64", "steam_id64", "steamId64" }),
        JsonDirectScalarAny(user, { "steam_id", "steamId", "steam64", "steam_id64", "steamId64" }),
        FindSteamIdInText(object)
    });
    op.recipientName = FirstNonEmpty({
        JsonDirectScalarAny(object, { "recipient", "recipientName", "target", "player", "nickname", "player_name", "playerName", "username", "userName" }),
        JsonDirectScalarAny(user, { "name", "nickname", "username", "display_name", "displayName" })
    });
    op.delivery = JsonDirectScalarField(object, "delivery");
    op.quantity = std::max(1, JsonDirectIntField(object, "quantity",
        JsonDirectIntField(object, "qty",
            JsonDirectIntField(object, "count",
                JsonDirectIntField(object, "buy_count",
                    JsonDirectIntField(object, "set_count", 1))))));
    return op;
}

std::vector<std::string> ExtractWargmOperationObjects(const std::string& json) {
    auto objects = ExtractArrayObjects(json);
    if (objects.empty()) {
        objects = ExtractJsonObjectsDeep(json);
    }

    std::vector<std::string> operations;
    std::vector<std::string> seenIds;
    for (const auto& object : objects) {
        const auto op = ParseWargmOperation(object);
        if (op.operationId.empty()) continue;
        if (op.offerId.empty() && op.title.empty() && op.itemId.empty() && op.steamId.empty()) continue;
        if (std::find(seenIds.begin(), seenIds.end(), op.operationId) != seenIds.end()) continue;
        seenIds.push_back(op.operationId);
        operations.push_back(object);
    }
    return operations;
}

bool WargmRuleMatches(const std::string& rule, const WargmOperationNative& op) {
    if (!JsonBoolField(rule, "Enabled", JsonBoolField(rule, "enabled", true))) return false;
    const auto offer = JsonScalarAny(rule, { "MatchOfferId", "matchOfferId", "MatchProductId", "matchProductId", "MatchGoodsId", "matchGoodsId" });
    const auto item = JsonScalarAny(rule, { "MatchItemId", "matchItemId", "MatchObjectId", "matchObjectId", "MatchAssetId", "matchAssetId" });
    const auto title = ToLowerAscii(JsonScalarAny(rule, { "MatchTitleContains", "matchTitleContains", "MatchNameContains", "matchNameContains" }));
    if (!offer.empty() && ToLowerAscii(offer) == ToLowerAscii(op.offerId)) return true;
    if (!item.empty() && ToLowerAscii(item) == ToLowerAscii(op.itemId)) return true;
    if (!title.empty() && ToLowerAscii(op.title).find(title) != std::string::npos) return true;
    return false;
}

std::string ResolveWargmItemsSpec(const std::string& rule, const WargmOperationNative& op) {
    auto items = ParseGrantItems(rule);
    if (!items.empty()) return GrantItemsSpec(items);

    auto itemId = JsonScalarAny(rule, { "ItemId", "itemId" });
    if (TrimAscii(itemId).empty()) itemId = op.itemId;
    itemId = TrimAscii(itemId);
    const int quantity = std::clamp(JsonIntField(rule, "Quantity", JsonIntField(rule, "quantity", op.quantity)), 1, 1000);
    if (!IsPlausibleScumItemToken(itemId)) return {};
    return itemId + "|" + std::to_string(quantity);
}

std::string BuildWargmPendingRow(const WargmOperationNative& op, const std::string& rule) {
    const auto mode = NormalizeDeliveryMode(JsonScalarAny(rule, { "DeliveryMode", "deliveryMode", "mode" }));
    std::ostringstream row;
    row << "{\"operationId\":\"" << JsonEscape(op.operationId) << "\""
        << ",\"offerId\":\"" << JsonEscape(op.offerId) << "\""
        << ",\"title\":\"" << JsonEscape(op.title) << "\""
        << ",\"steamId\":\"" << JsonEscape(op.steamId) << "\""
        << ",\"recipientName\":\"" << JsonEscape(op.recipientName) << "\""
        << ",\"delivery\":\"" << JsonEscape(op.delivery) << "\""
        << ",\"createdAtUtc\":\"" << UtcIsoNow() << "\""
        << ",\"delivered\":false";

    if (mode == "money" || mode == "gold") {
        const int amount = JsonIntField(rule, "Amount", JsonIntField(rule, "amount", op.quantity));
        if (amount == 0) return {};
        row << ",\"mode\":\"" << mode << "\",\"amount\":" << amount;
    } else if (mode == "vehicle") {
        const auto vehicleId = JsonScalarAny(rule, { "VehicleAsset", "vehicleAsset", "vehicleId" });
        if (vehicleId.empty()) return {};
        row << ",\"mode\":\"vehicle\",\"vehicleId\":\"" << JsonEscape(VehicleAdminToken(vehicleId)) << "\"";
    } else if (mode == "skill") {
        const auto skill = JsonScalarAny(rule, { "SkillName", "skillName", "skill" });
        if (skill.empty()) return {};
        const int level = JsonIntField(rule, "SkillLevel", JsonIntField(rule, "level", 0));
        const int experience = JsonIntField(rule, "SkillExperience", JsonIntField(rule, "experience", 0));
        row << ",\"mode\":\"skill\",\"skill\":\"" << JsonEscape(skill) << "\",\"level\":" << level << ",\"experience\":" << experience;
    } else if (mode == "allskills") {
        const int level = JsonIntField(rule, "SkillLevel", JsonIntField(rule, "level", 3));
        const int experience = JsonIntField(rule, "SkillExperience", JsonIntField(rule, "experience", 0));
        row << ",\"mode\":\"allskills\",\"skill\":\"allskills\",\"level\":" << level << ",\"experience\":" << experience;
    } else if (mode == "fame" || mode == "setfame") {
        const int amount = JsonIntField(rule, "Amount", JsonIntField(rule, "amount", op.quantity));
        if (amount == 0) return {};
        row << ",\"mode\":\"" << mode << "\",\"amount\":" << amount;
    } else if (mode == "attributes") {
        row << ",\"mode\":\"attributes\""
            << ",\"strength\":" << JsonNumberTextField(rule, "Strength", JsonNumberTextField(rule, "strength", "0"))
            << ",\"constitution\":" << JsonNumberTextField(rule, "Constitution", JsonNumberTextField(rule, "constitution", "0"))
            << ",\"dexterity\":" << JsonNumberTextField(rule, "Dexterity", JsonNumberTextField(rule, "dexterity", "0"))
            << ",\"intelligence\":" << JsonNumberTextField(rule, "Intelligence", JsonNumberTextField(rule, "intelligence", "0"));
    } else if (mode == "vip" || mode == "cargodrop") {
        const auto command = JsonScalarAny(rule, { "CommandTemplate", "commandTemplate", "command" });
        if (command.empty()) return {};
        row << ",\"mode\":\"command\",\"command\":\"" << JsonEscape(command) << "\"";
    } else if (mode == "command") {
        const auto command = JsonScalarAny(rule, { "CommandTemplate", "commandTemplate", "command" });
        if (command.empty()) return {};
        row << ",\"mode\":\"command\",\"command\":\"" << JsonEscape(command) << "\"";
    } else {
        const auto spec = ResolveWargmItemsSpec(rule, op);
        if (spec.empty()) return {};
        auto firstItem = spec;
        const auto semi = firstItem.find(';');
        if (semi != std::string::npos) firstItem = firstItem.substr(0, semi);
        auto itemId = firstItem;
        int quantity = 1;
        const auto pipe = firstItem.find('|');
        if (pipe != std::string::npos) {
            itemId = firstItem.substr(0, pipe);
            quantity = std::max(1, std::atoi(firstItem.substr(pipe + 1).c_str()));
        }
        row << ",\"mode\":\"item\",\"itemsSpec\":\"" << JsonEscape(spec) << "\""
            << ",\"itemId\":\"" << JsonEscape(itemId) << "\""
            << ",\"quantity\":" << quantity;
    }
    row << "}";
    return row.str();
}

bool WargmJsonLooksLikeError(const std::string& json) {
    const auto lower = ToLowerAscii(json);
    return lower.find("\"success\":false") != std::string::npos ||
        lower.find("\"success\":0") != std::string::npos ||
        lower.find("\"status\":\"error\"") != std::string::npos ||
        lower.find("\"state\":\"error\"") != std::string::npos ||
        lower.find("\"error\":\"") != std::string::npos ||
        lower.find("\"errors\":[") != std::string::npos ||
        lower.find("\"errors\":{") != std::string::npos;
}

std::string WargmApiUrl(const std::string& endpoint, const std::vector<std::pair<std::string, std::string>>& query) {
    std::ostringstream url;
    url << "https://api.wargm.ru/v1/" << endpoint << "?";
    bool first = true;
    for (const auto& pair : query) {
        if (TrimAscii(pair.second).empty()) continue;
        if (!first) url << "&";
        first = false;
        url << UrlEncode(pair.first) << "=" << UrlEncode(pair.second);
    }
    return url.str();
}

std::vector<std::string> WargmSettledKeys(const std::wstring& baseDir);
bool WargmOperationSettled(const std::vector<std::string>& keys, const WargmOperationNative& op);

std::string WargmSyncJson(const std::wstring& baseDir) {
    bool expected = false;
    if (!g_wargmSyncBusy.compare_exchange_strong(expected, true)) {
        return Envelope(false, "Проверка Wargm уже выполняется.");
    }
    struct BusyGuard { ~BusyGuard() { g_wargmSyncBusy = false; } } guard;

    const auto config = PluginConfigJson(baseDir, "wargm-shop", "{}");
    if (!JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false))) {
        return Envelope(false, "Магазин Wargm выключен в настройках.");
    }
    const int projectId = JsonIntField(config, "ProjectId", JsonIntField(config, "projectId", 0));
    const auto apiKey = JsonScalarAny(config, { "ApiKey", "apiKey" });
    if (projectId <= 0 || TrimAscii(apiKey).empty()) {
        return Envelope(false, "Не настроены ProjectId или API key Wargm.");
    }

    std::vector<std::pair<std::string, std::string>> query = {
        { "client", std::to_string(projectId) + ":" + TrimAscii(apiKey) },
        { "numeric_string", "true" },
        { "status", "pending" },
        { "claimed", "0" },
        { "type", "shop" }
    };
    const auto filter = JsonScalarAny(config, { "DeliveryFilter", "deliveryFilter" });
    if (!filter.empty()) query.push_back({ "delivery", filter });
    const int serverId = JsonIntField(config, "WargmServerId", JsonIntField(config, "wargmServerId", 0));
    if (serverId > 0) query.push_back({ "server_id", std::to_string(serverId) });

    const int timeoutSeconds = std::clamp(JsonIntField(config, "HttpTimeoutSeconds", 20), 3, 60);
    const auto http = HttpGetText(WargmApiUrl("shop/operations", query), timeoutSeconds);
    if (!http.ok) {
        AppendModuleStateRecord(baseDir, "wargm-shop",
            std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"api-error\",\"httpStatus\":" + std::to_string(http.status) + ",\"message\":\"" + JsonEscape(http.message) + "\"}");
        return Envelope(false, http.message);
    }
    if (WargmJsonLooksLikeError(http.body)) {
        AppendModuleStateRecord(baseDir, "wargm-shop",
            std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"api-error\",\"httpStatus\":" + std::to_string(http.status) + ",\"message\":\"Wargm API вернул ошибку.\"}");
        return Envelope(false, "Wargm API вернул ошибку.");
    }

    auto operationObjects = ExtractWargmOperationObjects(http.body);
    const auto settledKeys = WargmSettledKeys(baseDir);
    const auto rules = ExtractArrayObjects(JsonRawFieldAny(config, { "Rules", "rules" }));
    std::vector<std::string> rows;
    int skipped = 0;
    int settled = 0;
    int matched = 0;
    int ruleBuildFailed = 0;
    int noRule = 0;
    for (const auto& object : operationObjects) {
        const auto op = ParseWargmOperation(object);
        if (op.operationId.empty()) {
            ++skipped;
            continue;
        }
        if (WargmOperationSettled(settledKeys, op)) {
            ++settled;
            continue;
        }
        std::string matchedRule;
        for (const auto& rule : rules) {
            if (WargmRuleMatches(rule, op)) {
                matchedRule = rule;
                break;
            }
        }
        if (matchedRule.empty()) {
            ++skipped;
            ++noRule;
            continue;
        }
        ++matched;
        auto row = BuildWargmPendingRow(op, matchedRule);
        if (row.empty()) {
            ++skipped;
            ++ruleBuildFailed;
            continue;
        }
        rows.push_back(row);
    }

    std::ostringstream pending;
    pending << "[";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i) pending << ",";
        pending << rows[i];
    }
    pending << "]";
    WriteTextFile(ModuleStatePath(baseDir, "wargm-pending"), pending.str());
    AppendModuleStateRecord(baseDir, "wargm-shop",
        std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"synced\",\"fetched\":" + std::to_string(operationObjects.size()) +
        ",\"rules\":" + std::to_string(rules.size()) +
        ",\"matched\":" + std::to_string(matched) +
        ",\"pending\":" + std::to_string(rows.size()) + ",\"skipped\":" + std::to_string(skipped) +
        ",\"settled\":" + std::to_string(settled) +
        ",\"noRule\":" + std::to_string(noRule) +
        ",\"ruleBuildFailed\":" + std::to_string(ruleBuildFailed) + "}");

    return Envelope(true, std::string("{\"synced\":true,\"fetched\":") + std::to_string(operationObjects.size()) +
        ",\"rules\":" + std::to_string(rules.size()) +
        ",\"matched\":" + std::to_string(matched) +
        ",\"pending\":" + std::to_string(rows.size()) + ",\"skipped\":" + std::to_string(skipped) +
        ",\"settled\":" + std::to_string(settled) +
        ",\"noRule\":" + std::to_string(noRule) +
        ",\"ruleBuildFailed\":" + std::to_string(ruleBuildFailed) + "}");
}

std::string OperationIdFromWargmKey(const std::string& key) {
    for (const std::string prefix : { "operationId:", "OperationId:", "id:", "Id:" }) {
        if (ToLowerAscii(key).rfind(ToLowerAscii(prefix), 0) == 0) return TrimAscii(key.substr(prefix.size()));
    }
    return {};
}

void AddWargmSettledKey(std::vector<std::string>& keys, const std::string& value) {
    const auto trimmed = TrimAscii(value);
    if (trimmed.empty()) return;
    if (std::find(keys.begin(), keys.end(), trimmed) == keys.end()) keys.push_back(trimmed);
}

std::vector<std::string> WargmSettledKeys(const std::wstring& baseDir) {
    std::vector<std::string> keys;
    for (const auto& stateName : { "wargm-delivered", "wargm-confirmed" }) {
        for (const auto& object : ExtractArrayObjects(ModuleStateJson(baseDir, stateName))) {
            const auto key = FlatJsonStringValue(object, "key");
            AddWargmSettledKey(keys, key);
            const auto operationId = FirstNonEmpty({
                FlatJsonStringValue(object, "operationId"),
                FlatJsonStringValue(object, "OperationId"),
                FlatJsonStringValue(object, "id"),
                FlatJsonStringValue(object, "Id"),
                OperationIdFromWargmKey(key)
            });
            if (!operationId.empty()) AddWargmSettledKey(keys, "operationId:" + operationId);
        }
    }
    return keys;
}

bool WargmOperationSettled(const std::vector<std::string>& keys, const WargmOperationNative& op) {
    if (op.operationId.empty()) return false;
    const auto key = "operationId:" + op.operationId;
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

std::string WargmConfirmDeliveredJson(const std::wstring& baseDir) {
    const auto config = PluginConfigJson(baseDir, "wargm-shop", "{}");
    if (!JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false))) {
        return Envelope(false, "Магазин Wargm выключен в настройках.");
    }
    const int projectId = JsonIntField(config, "ProjectId", JsonIntField(config, "projectId", 0));
    const auto apiKey = JsonScalarAny(config, { "ApiKey", "apiKey" });
    if (projectId <= 0 || TrimAscii(apiKey).empty()) {
        return Envelope(false, "Не настроены ProjectId или API key Wargm.");
    }

    const auto delivered = ExtractArrayObjects(ModuleStateJson(baseDir, "wargm-delivered"));
    const auto confirmedRows = ExtractArrayObjects(ModuleStateJson(baseDir, "wargm-confirmed"));
    std::vector<std::string> confirmedKeys;
    for (const auto& row : confirmedRows) {
        const auto key = JsonScalarField(row, "key");
        if (!key.empty()) confirmedKeys.push_back(ToLowerAscii(key));
        const auto opId = JsonScalarAny(row, { "operationId", "OperationId", "id", "Id" });
        if (!opId.empty()) confirmedKeys.push_back("operationid:" + ToLowerAscii(opId));
    }

    const auto endpoint = JsonBoolField(config, "UseClaimInsteadOfSuccess", JsonBoolField(config, "useClaimInsteadOfSuccess", false))
        ? std::string("shop/operation_claim")
        : std::string("shop/operation_success");
    int scanned = 0;
    int acked = 0;
    int failed = 0;
    const int serverId = JsonIntField(config, "WargmServerId", JsonIntField(config, "wargmServerId", 0));
    for (const auto& row : delivered) {
        ++scanned;
        const auto key = JsonScalarField(row, "key");
        auto opId = JsonScalarAny(row, { "operationId", "OperationId", "id", "Id" });
        if (opId.empty()) opId = OperationIdFromWargmKey(key);
        if (opId.empty()) continue;
        const auto confirmKey = !key.empty() ? key : ("operationId:" + opId);
        const auto loweredKey = ToLowerAscii(confirmKey);
        const auto loweredOp = "operationid:" + ToLowerAscii(opId);
        if (std::find(confirmedKeys.begin(), confirmedKeys.end(), loweredKey) != confirmedKeys.end() ||
            std::find(confirmedKeys.begin(), confirmedKeys.end(), loweredOp) != confirmedKeys.end()) {
            continue;
        }

        std::vector<std::pair<std::string, std::string>> query = {
            { "client", std::to_string(projectId) + ":" + TrimAscii(apiKey) },
            { "operation_id", opId }
        };
        if (serverId > 0) query.push_back({ "server_id", std::to_string(serverId) });
        const auto http = HttpGetText(WargmApiUrl(endpoint, query), std::clamp(JsonIntField(config, "HttpTimeoutSeconds", 20), 3, 60));
        if (http.ok && !WargmJsonLooksLikeError(http.body)) {
            ++acked;
            confirmedKeys.push_back(loweredKey);
            confirmedKeys.push_back(loweredOp);
            AppendModuleStateRecord(baseDir, "wargm-confirmed",
                std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"confirmed\",\"key\":\"" + JsonEscape(confirmKey) +
                "\",\"operationId\":\"" + JsonEscape(opId) + "\",\"endpoint\":\"" + JsonEscape(endpoint) + "\"}");
            AppendModuleStateRecord(baseDir, "wargm-shop",
                std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"confirmed\",\"key\":\"" + JsonEscape(confirmKey) +
                "\",\"operationId\":\"" + JsonEscape(opId) + "\",\"endpoint\":\"" + JsonEscape(endpoint) + "\"}");
        } else {
            ++failed;
            AppendModuleStateRecord(baseDir, "wargm-shop",
                std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"confirm-failed\",\"operationId\":\"" + JsonEscape(opId) +
                "\",\"endpoint\":\"" + JsonEscape(endpoint) + "\",\"httpStatus\":" + std::to_string(http.status) + "}");
        }
    }
    return Envelope(true, std::string("{\"confirmed\":true,\"endpoint\":\"") + JsonEscape(endpoint) + "\",\"scanned\":" +
        std::to_string(scanned) + ",\"acked\":" + std::to_string(acked) + ",\"failed\":" + std::to_string(failed) + "}");
}

struct GameStoresItemNative {
    std::string id;
    std::string name;
    std::string amount;
    std::string type;
    std::string command;
    std::string itemId;
    std::string steamId;
    std::string recipientName;
    std::string runtimeKey;
    int quantity = 1;
};

GameStoresItemNative ParseGameStoresItem(const std::string& object, const BridgeRuntimePlayer& player) {
    GameStoresItemNative item;
    item.id = JsonDirectScalarAny(object, { "id", "Id" });
    item.name = JsonDirectScalarAny(object, { "name", "Name", "title", "Title" });
    item.amount = JsonDirectScalarAny(object, { "amount", "Amount", "quantity", "Quantity", "count", "Count" });
    item.type = JsonDirectScalarAny(object, { "type", "Type" });
    item.command = FirstNonEmpty({
        JsonDirectScalarAny(object, { "command", "Command", "commands", "Commands" }),
        JsonDirectNestedScalarAny(object, "data", { "command", "Command", "commands", "Commands" })
    });
    item.itemId = JsonDirectScalarAny(object, { "item_id", "itemId", "ItemId", "product_id", "productId", "ProductId" });
    item.steamId = player.steamId;
    item.recipientName = player.name;
    item.runtimeKey = player.runtimeKey;
    item.quantity = std::max(1, std::atoi(item.amount.c_str()));
    return item;
}

bool GameStoresJsonIsEmptyBucket(const std::string& json) {
    return JsonIntField(json, "code", 0) == 104 || json.find("\"code\":104") != std::string::npos;
}

bool GameStoresJsonLooksLikeError(const std::string& json) {
    if (GameStoresJsonIsEmptyBucket(json)) return false;
    const int code = JsonIntField(json, "code", 0);
    if (code != 0 && code != 100 && code != 107) return true;
    const auto lower = ToLowerAscii(json);
    return lower.find("\"success\":false") != std::string::npos ||
        lower.find("\"success\":0") != std::string::npos ||
        lower.find("\"result\":\"error\"") != std::string::npos ||
        lower.find("\"status\":\"error\"") != std::string::npos ||
        lower.find("\"state\":\"error\"") != std::string::npos ||
        lower.find("\"error\":\"") != std::string::npos ||
        lower.find("\"errors\":[") != std::string::npos ||
        lower.find("\"errors\":{") != std::string::npos;
}

std::vector<std::string> ExtractGameStoresItemObjects(const std::string& json, const BridgeRuntimePlayer& player) {
    std::vector<std::string> source = ExtractArrayObjects(json);
    if (source.empty()) source = ExtractJsonObjectsDeep(json);
    std::vector<std::string> items;
    std::vector<std::string> seen;
    for (const auto& object : source) {
        const auto item = ParseGameStoresItem(object, player);
        if (item.id.empty()) continue;
        if (item.itemId.empty() && item.name.empty() && item.command.empty()) continue;
        if (std::find(seen.begin(), seen.end(), item.id) != seen.end()) continue;
        seen.push_back(item.id);
        items.push_back(object);
    }
    return items;
}

std::string GameStoresApiUrl(const std::vector<std::pair<std::string, std::string>>& query) {
    std::ostringstream url;
    url << "https://gamestores.app/api/?";
    bool first = true;
    for (const auto& pair : query) {
        if (TrimAscii(pair.second).empty()) continue;
        if (!first) url << "&";
        first = false;
        url << UrlEncode(pair.first) << "=" << UrlEncode(pair.second);
    }
    return url.str();
}

bool GameStoresRuleMatches(const std::string& rule, const GameStoresItemNative& item) {
    if (!JsonBoolField(rule, "Enabled", JsonBoolField(rule, "enabled", true))) return false;
    const auto bucket = JsonScalarAny(rule, { "MatchBucketId", "matchBucketId", "MatchOrderId", "matchOrderId" });
    const auto product = JsonScalarAny(rule, { "MatchProductId", "matchProductId", "MatchItemId", "matchItemId", "MatchObjectId", "matchObjectId" });
    const auto title = ToLowerAscii(JsonScalarAny(rule, { "MatchTitleContains", "matchTitleContains", "MatchNameContains", "matchNameContains" }));
    const auto command = ToLowerAscii(JsonScalarAny(rule, { "MatchCommandContains", "matchCommandContains" }));
    if (!bucket.empty() && ToLowerAscii(bucket) == ToLowerAscii(item.id)) return true;
    if (!product.empty()) {
        const auto needle = ToLowerAscii(product);
        if (needle == ToLowerAscii(item.itemId) || needle == ToLowerAscii(item.id)) return true;
    }
    if (!title.empty() && ToLowerAscii(item.name).find(title) != std::string::npos) return true;
    if (!command.empty() && ToLowerAscii(item.command).find(command) != std::string::npos) return true;
    return false;
}

std::string ResolveGameStoresItemsSpec(const std::string& rule, const GameStoresItemNative& item, bool allowDirectItemIdDelivery) {
    auto items = ParseGrantItems(rule);
    if (!items.empty()) return GrantItemsSpec(items);

    auto itemId = JsonScalarAny(rule, { "ItemId", "itemId" });
    const bool allowDirect = allowDirectItemIdDelivery ||
        JsonBoolField(rule, "AllowDirectItemIdDelivery", JsonBoolField(rule, "allowDirectItemIdDelivery", false));
    if (TrimAscii(itemId).empty() && allowDirect) {
        itemId = item.itemId;
    }
    itemId = TrimAscii(itemId);
    const int quantity = std::clamp(JsonIntField(rule, "Quantity", JsonIntField(rule, "quantity", item.quantity)), 1, 1000);
    if (!IsPlausibleScumItemToken(itemId)) return {};
    return itemId + "|" + std::to_string(quantity);
}

std::string BuildGameStoresOrderKey(const std::string& shopId, const std::string& serverId, const std::string& bucketId) {
    return std::string("gamestores:") + shopId + ":" + serverId + ":id:" + bucketId;
}

bool GameStoresKeyExists(const std::vector<std::string>& keys, const std::string& value) {
    const auto trimmed = TrimAscii(value);
    if (trimmed.empty()) return false;
    const auto lowered = ToLowerAscii(trimmed);
    for (const auto& existing : keys) {
        if (ToLowerAscii(existing) == lowered) return true;
    }
    return false;
}

void AddGameStoresKey(std::vector<std::string>& keys, const std::string& value) {
    if (!GameStoresKeyExists(keys, value)) AddWargmSettledKey(keys, value);
}

void AddGameStoresRowKeys(std::vector<std::string>& keys, const std::string& row, const std::string& shopId, const std::string& serverId) {
    const auto key = JsonScalarAny(row, { "key", "Key" });
    AddGameStoresKey(keys, key);
    const auto bucketId = JsonScalarAny(row, { "bucketId", "BucketId", "id", "Id" });
    if (!bucketId.empty()) {
        AddGameStoresKey(keys, BuildGameStoresOrderKey(shopId, serverId, bucketId));
        AddGameStoresKey(keys, "bucketId:" + bucketId);
        AddGameStoresKey(keys, "id:" + bucketId);
    }
}

bool GameStoresRowSettled(const std::vector<std::string>& settledKeys, const std::string& row, const std::string& shopId, const std::string& serverId) {
    std::vector<std::string> rowKeys;
    AddGameStoresRowKeys(rowKeys, row, shopId, serverId);
    for (const auto& key : rowKeys) {
        if (GameStoresKeyExists(settledKeys, key)) return true;
    }
    return false;
}

std::string BuildGameStoresPendingRow(const GameStoresItemNative& item, const std::string& rule, const std::string& shopId, const std::string& serverId, bool allowDirectItemIdDelivery) {
    const auto mode = NormalizeDeliveryMode(JsonScalarAny(rule, { "DeliveryMode", "deliveryMode", "mode" }));
    const auto key = BuildGameStoresOrderKey(shopId, serverId, item.id);
    std::ostringstream row;
    row << "{\"key\":\"" << JsonEscape(key) << "\""
        << ",\"bucketId\":\"" << JsonEscape(item.id) << "\""
        << ",\"productId\":\"" << JsonEscape(item.itemId) << "\""
        << ",\"title\":\"" << JsonEscape(item.name) << "\""
        << ",\"type\":\"" << JsonEscape(item.type) << "\""
        << ",\"steamId\":\"" << JsonEscape(item.steamId) << "\""
        << ",\"recipientName\":\"" << JsonEscape(item.recipientName) << "\""
        << ",\"runtimeKey\":\"" << JsonEscape(item.runtimeKey) << "\""
        << ",\"source\":\"gamestores-auto\""
        << ",\"createdAtUtc\":\"" << UtcIsoNow() << "\""
        << ",\"delivered\":false";

    if (mode == "money" || mode == "gold") {
        const int amount = JsonIntField(rule, "Amount", JsonIntField(rule, "amount", item.quantity));
        if (amount == 0) return {};
        row << ",\"mode\":\"" << mode << "\",\"amount\":" << amount;
    } else if (mode == "vehicle") {
        const auto vehicleId = JsonScalarAny(rule, { "VehicleAsset", "vehicleAsset", "vehicleId" });
        if (vehicleId.empty()) return {};
        row << ",\"mode\":\"vehicle\",\"vehicleId\":\"" << JsonEscape(VehicleAdminToken(vehicleId)) << "\"";
    } else if (mode == "skill") {
        const auto skill = JsonScalarAny(rule, { "SkillName", "skillName", "skill" });
        if (skill.empty()) return {};
        row << ",\"mode\":\"skill\",\"skill\":\"" << JsonEscape(skill) << "\",\"level\":"
            << JsonIntField(rule, "SkillLevel", JsonIntField(rule, "level", 0))
            << ",\"experience\":" << JsonIntField(rule, "SkillExperience", JsonIntField(rule, "experience", 0));
    } else if (mode == "allskills") {
        row << ",\"mode\":\"allskills\",\"skill\":\"allskills\",\"level\":"
            << JsonIntField(rule, "SkillLevel", JsonIntField(rule, "level", 3))
            << ",\"experience\":" << JsonIntField(rule, "SkillExperience", JsonIntField(rule, "experience", 0));
    } else if (mode == "fame" || mode == "setfame") {
        const int amount = JsonIntField(rule, "Amount", JsonIntField(rule, "amount", item.quantity));
        if (amount == 0) return {};
        row << ",\"mode\":\"" << mode << "\",\"amount\":" << amount;
    } else if (mode == "attributes") {
        row << ",\"mode\":\"attributes\""
            << ",\"strength\":" << JsonNumberTextField(rule, "Strength", JsonNumberTextField(rule, "strength", "0"))
            << ",\"constitution\":" << JsonNumberTextField(rule, "Constitution", JsonNumberTextField(rule, "constitution", "0"))
            << ",\"dexterity\":" << JsonNumberTextField(rule, "Dexterity", JsonNumberTextField(rule, "dexterity", "0"))
            << ",\"intelligence\":" << JsonNumberTextField(rule, "Intelligence", JsonNumberTextField(rule, "intelligence", "0"));
    } else if (mode == "command") {
        const auto command = JsonScalarAny(rule, { "CommandTemplate", "commandTemplate", "command" });
        if (command.empty()) return {};
        row << ",\"mode\":\"command\",\"command\":\"" << JsonEscape(command) << "\"";
    } else {
        const auto spec = ResolveGameStoresItemsSpec(rule, item, allowDirectItemIdDelivery);
        if (spec.empty()) return {};
        auto firstItem = spec;
        const auto semi = firstItem.find(';');
        if (semi != std::string::npos) firstItem = firstItem.substr(0, semi);
        auto itemId = firstItem;
        int quantity = 1;
        const auto pipe = firstItem.find('|');
        if (pipe != std::string::npos) {
            itemId = firstItem.substr(0, pipe);
            quantity = std::max(1, std::atoi(firstItem.substr(pipe + 1).c_str()));
        }
        row << ",\"mode\":\"item\",\"itemsSpec\":\"" << JsonEscape(spec) << "\""
            << ",\"itemId\":\"" << JsonEscape(itemId) << "\""
            << ",\"quantity\":" << quantity;
    }
    row << "}";
    return row.str();
}

constexpr const char* kGameStoresJournalSchema = "scum-nedjin-gamestores-delivery-journal-v1";
constexpr const char* kGameStoresJournalDirectoryName = "gamestores-delivery-journal";
constexpr const char* kGameStoresJournalOperationLockName = "operation.lock";

enum class GameStoresJournalState {
    None,
    Reserved,
    Dispatching,
    Delivered,
    Uncertain,
    Corrupt
};

struct GameStoresJournalEntry {
    std::string key;
    std::string hash;
    std::string bucketId;
    std::string steamId;
    std::string name;
    std::string source;
    std::string owner;
    std::string utc;
};

struct GameStoresJournalSnapshot {
    GameStoresJournalState state = GameStoresJournalState::None;
    GameStoresJournalEntry entry;
};

bool GameStoresJournalAsciiWhitespace(unsigned char ch) {
    return ch == ' ' || (ch >= '\t' && ch <= '\r');
}

std::string CanonicalGameStoresJournalKey(std::string value) {
    size_t first = 0;
    size_t last = value.size();
    while (first < last && GameStoresJournalAsciiWhitespace(static_cast<unsigned char>(value[first]))) ++first;
    while (last > first && GameStoresJournalAsciiWhitespace(static_cast<unsigned char>(value[last - 1]))) --last;

    std::string canonical;
    canonical.reserve(last - first);
    for (size_t index = first; index < last; ++index) {
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        canonical.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch));
    }
    return canonical;
}

std::wstring GameStoresJournalDirectory(const std::wstring& baseDir) {
    return JoinPath(JoinPath(ProjectRuntimeRoot(baseDir), L"state"), Utf8ToWide(kGameStoresJournalDirectoryName));
}

std::string GameStoresJournalStateName(GameStoresJournalState state) {
    switch (state) {
    case GameStoresJournalState::Reserved: return "reserved";
    case GameStoresJournalState::Dispatching: return "dispatching";
    case GameStoresJournalState::Delivered: return "delivered";
    case GameStoresJournalState::Uncertain: return "uncertain";
    default: return {};
    }
}

std::string GameStoresJournalMarkerSuffix(GameStoresJournalState state) {
    switch (state) {
    case GameStoresJournalState::Reserved: return ".reserved.json";
    case GameStoresJournalState::Dispatching: return ".dispatching.json";
    case GameStoresJournalState::Delivered: return ".delivered.json";
    case GameStoresJournalState::Uncertain: return ".uncertain.json";
    default: return {};
    }
}

std::wstring GameStoresJournalMarkerPath(const std::wstring& baseDir, const std::string& hash, GameStoresJournalState state) {
    return JoinPath(GameStoresJournalDirectory(baseDir), Utf8ToWide(hash + GameStoresJournalMarkerSuffix(state)));
}

GameStoresJournalEntry CreateGameStoresJournalEntry(
    const std::string& key,
    const std::string& bucketId,
    const std::string& steamId,
    const std::string& name,
    const std::string& source,
    const std::string& owner) {
    GameStoresJournalEntry entry;
    entry.key = CanonicalGameStoresJournalKey(key);
    entry.hash = Sha256Hex(entry.key);
    entry.bucketId = TrimAscii(bucketId);
    entry.steamId = TrimAscii(steamId);
    entry.name = TrimAscii(name);
    entry.source = TrimAscii(source);
    entry.owner = TrimAscii(owner);
    entry.utc = UtcIsoNow();
    return entry;
}

std::string GameStoresJournalRecordJson(const GameStoresJournalEntry& entry, GameStoresJournalState state, const std::string& reason = {}) {
    return std::string("{\"schema\":\"") + kGameStoresJournalSchema +
        "\",\"state\":\"" + GameStoresJournalStateName(state) +
        "\",\"key\":\"" + JsonEscape(entry.key) +
        "\",\"hash\":\"" + JsonEscape(entry.hash) +
        "\",\"bucketId\":\"" + JsonEscape(entry.bucketId) +
        "\",\"steamId\":\"" + JsonEscape(entry.steamId) +
        "\",\"name\":\"" + JsonEscape(entry.name) +
        "\",\"source\":\"" + JsonEscape(entry.source) +
        "\",\"owner\":\"" + JsonEscape(entry.owner) +
        "\",\"utc\":\"" + JsonEscape(entry.utc) +
        "\",\"reason\":\"" + JsonEscape(reason) + "\"}";
}

bool GameStoresJournalWriteMarker(
    const std::wstring& baseDir,
    const GameStoresJournalEntry& entry,
    GameStoresJournalState state,
    const std::string& reason = {}) {
    if (entry.key.empty() || entry.hash.empty() || GameStoresJournalStateName(state).empty()) return false;
    // WriteTextFileAtomicallyExact creates its temporary payload through CREATE_NEW,
    // flushes it, and never replaces an existing terminal marker or uses .live.
    return WriteTextFileAtomicallyExact(
        GameStoresJournalMarkerPath(baseDir, entry.hash, state),
        GameStoresJournalRecordJson(entry, state, reason));
}

bool GameStoresJournalReadMarker(
    const std::wstring& baseDir,
    const std::string& hash,
    GameStoresJournalState state,
    const std::string& expectedKey,
    GameStoresJournalEntry& entry) {
    std::string text;
    if (!ReadTextFileExact(GameStoresJournalMarkerPath(baseDir, hash, state), text)) return false;

    const auto key = CanonicalGameStoresJournalKey(JsonScalarAny(text, { "key" }));
    const auto recordHash = JsonScalarAny(text, { "hash" });
    if (JsonScalarAny(text, { "schema" }) != kGameStoresJournalSchema ||
        JsonScalarAny(text, { "state" }) != GameStoresJournalStateName(state) ||
        key.empty() || recordHash != hash || Sha256Hex(key) != hash ||
        (!expectedKey.empty() && key != expectedKey) ||
        JsonScalarAny(text, { "utc" }).empty()) {
        return false;
    }

    entry.key = key;
    entry.hash = hash;
    entry.bucketId = JsonScalarAny(text, { "bucketId" });
    entry.steamId = JsonScalarAny(text, { "steamId" });
    entry.name = JsonScalarAny(text, { "name" });
    entry.source = JsonScalarAny(text, { "source" });
    entry.owner = JsonScalarAny(text, { "owner" });
    entry.utc = JsonScalarAny(text, { "utc" });
    return true;
}

GameStoresJournalSnapshot InspectGameStoresJournal(const std::wstring& baseDir, const std::string& key) {
    GameStoresJournalSnapshot snapshot;
    const auto canonicalKey = CanonicalGameStoresJournalKey(key);
    if (canonicalKey.empty()) {
        snapshot.state = GameStoresJournalState::Corrupt;
        return snapshot;
    }

    const auto hash = Sha256Hex(canonicalKey);
    if (hash.empty()) {
        snapshot.state = GameStoresJournalState::Corrupt;
        return snapshot;
    }

    bool corrupt = false;
    std::map<GameStoresJournalState, GameStoresJournalEntry> found;
    for (const auto state : { GameStoresJournalState::Reserved, GameStoresJournalState::Dispatching,
        GameStoresJournalState::Delivered, GameStoresJournalState::Uncertain }) {
        const auto path = GameStoresJournalMarkerPath(baseDir, hash, state);
        if (!FileExists(path)) continue;
        GameStoresJournalEntry entry;
        if (!GameStoresJournalReadMarker(baseDir, hash, state, canonicalKey, entry)) {
            corrupt = true;
            continue;
        }
        found[state] = entry;
    }

    const auto delivered = found.find(GameStoresJournalState::Delivered);
    if (delivered != found.end()) {
        snapshot.state = GameStoresJournalState::Delivered;
        snapshot.entry = delivered->second;
        return snapshot;
    }
    if (corrupt) {
        snapshot.state = GameStoresJournalState::Corrupt;
        return snapshot;
    }
    for (const auto state : { GameStoresJournalState::Uncertain, GameStoresJournalState::Dispatching, GameStoresJournalState::Reserved }) {
        const auto it = found.find(state);
        if (it != found.end()) {
            snapshot.state = state;
            snapshot.entry = it->second;
            return snapshot;
        }
    }
    return snapshot;
}

std::vector<GameStoresJournalEntry> GameStoresJournalDeliveredEntries(const std::wstring& baseDir) {
    std::vector<GameStoresJournalEntry> entries;
    const auto directory = GameStoresJournalDirectory(baseDir);
    try {
        if (!fs::exists(directory)) return entries;
        const std::string suffix = ".delivered.json";
        for (const auto& item : fs::directory_iterator(directory)) {
            if (!item.is_regular_file()) continue;
            const auto name = WideToUtf8(item.path().filename().wstring());
            if (name.size() <= suffix.size() || name.rfind(suffix) != name.size() - suffix.size()) continue;
            const auto hash = name.substr(0, name.size() - suffix.size());
            GameStoresJournalEntry entry;
            if (GameStoresJournalReadMarker(baseDir, hash, GameStoresJournalState::Delivered, {}, entry)) {
                entries.push_back(entry);
            }
        }
    } catch (...) {
    }
    return entries;
}

thread_local HANDLE g_gameStoresJournalLeaseHandle = INVALID_HANDLE_VALUE;
thread_local unsigned int g_gameStoresJournalLeaseDepth = 0;

class GameStoresOperationLease {
public:
    explicit GameStoresOperationLease(const std::wstring& baseDir) {
        if (g_gameStoresJournalLeaseDepth > 0) {
            ++g_gameStoresJournalLeaseDepth;
            acquired_ = true;
            return;
        }

        const auto directory = GameStoresJournalDirectory(baseDir);
        EnsureDirectory(directory);
        const auto path = JoinPath(directory, Utf8ToWide(kGameStoresJournalOperationLockName));
        const auto deadline = GetTickCount64() + 30000ULL;
        do {
            const auto handle = CreateFileW(
                path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                g_gameStoresJournalLeaseHandle = handle;
                g_gameStoresJournalLeaseDepth = 1;
                acquired_ = true;
                return;
            }

            const auto error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) {
                error_ = "Не удалось получить межпроцессную блокировку GameStores; выдача не начата.";
                return;
            }
            Sleep(50);
        } while (GetTickCount64() < deadline);

        error_ = "Ожидание межпроцессной блокировки GameStores истекло; выдача не начата.";
    }

    ~GameStoresOperationLease() {
        if (!acquired_ || g_gameStoresJournalLeaseDepth == 0) return;
        --g_gameStoresJournalLeaseDepth;
        if (g_gameStoresJournalLeaseDepth == 0 && g_gameStoresJournalLeaseHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(g_gameStoresJournalLeaseHandle);
            g_gameStoresJournalLeaseHandle = INVALID_HANDLE_VALUE;
        }
    }

    bool acquired() const { return acquired_; }
    const std::string& error() const { return error_; }

private:
    bool acquired_ = false;
    std::string error_ = "GameStores operation lease is unavailable; delivery was not started.";
};

std::string GameStoresJournalDeliveredRecordJson(const GameStoresJournalEntry& entry) {
    return std::string("{\"utc\":\"") + JsonEscape(entry.utc) +
        "\",\"status\":\"delivered\",\"source\":\"" + JsonEscape(entry.source) +
        "\",\"key\":\"" + JsonEscape(entry.key) +
        "\",\"bucketId\":\"" + JsonEscape(entry.bucketId) +
        "\",\"steamId\":\"" + JsonEscape(entry.steamId) +
        "\",\"name\":\"" + JsonEscape(entry.name) +
        "\",\"journalRecovered\":true,\"message\":\"Recovered from immutable GameStores delivery journal.\"}";
}

void ReconcileGameStoresDeliveredJournal(const std::wstring& baseDir) {
    std::vector<std::string> known;
    for (const auto& row : ExtractArrayObjects(ModuleStateJson(baseDir, "gamestores-delivered"))) {
        AddGameStoresKey(known, JsonScalarAny(row, { "key", "Key" }));
        const auto bucketId = JsonScalarAny(row, { "bucketId", "BucketId", "id", "Id" });
        if (!bucketId.empty()) AddGameStoresKey(known, "id:" + bucketId);
    }

    for (const auto& entry : GameStoresJournalDeliveredEntries(baseDir)) {
        if (GameStoresKeyExists(known, entry.key) ||
            (!entry.bucketId.empty() && GameStoresKeyExists(known, "id:" + entry.bucketId))) {
            continue;
        }
        if (AppendModuleStateRecord(baseDir, "gamestores-delivered", GameStoresJournalDeliveredRecordJson(entry))) {
            AddGameStoresKey(known, entry.key);
            if (!entry.bucketId.empty()) AddGameStoresKey(known, "id:" + entry.bucketId);
        }
    }
}

std::vector<std::string> GameStoresSettledKeys(const std::wstring& baseDir) {
    std::vector<std::string> keys;
    for (const auto& stateName : { "gamestores-delivered", "gamestores-confirmed" }) {
        for (const auto& object : ExtractArrayObjects(ModuleStateJson(baseDir, stateName))) {
            AddWargmSettledKey(keys, JsonScalarAny(object, { "key", "Key" }));
            const auto bucketId = JsonScalarAny(object, { "bucketId", "BucketId", "id", "Id" });
            if (!bucketId.empty()) AddWargmSettledKey(keys, "bucketId:" + bucketId);
            if (!bucketId.empty()) AddWargmSettledKey(keys, "id:" + bucketId);
        }
    }
    for (const auto& entry : GameStoresJournalDeliveredEntries(baseDir)) {
        AddGameStoresKey(keys, entry.key);
        if (!entry.bucketId.empty()) {
            AddGameStoresKey(keys, "bucketId:" + entry.bucketId);
            AddGameStoresKey(keys, "id:" + entry.bucketId);
        }
    }
    return keys;
}

bool GameStoresItemSettled(const std::vector<std::string>& keys, const GameStoresItemNative& item, const std::string& shopId, const std::string& serverId) {
    const auto key = BuildGameStoresOrderKey(shopId, serverId, item.id);
    return GameStoresKeyExists(keys, key) ||
        GameStoresKeyExists(keys, "bucketId:" + item.id) ||
        GameStoresKeyExists(keys, "id:" + item.id);
}

bool TryParseUtcIsoSeconds(const std::string& value, std::time_t& out) {
    const auto text = TrimAscii(value);
    if (text.empty()) return false;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    const auto epoch = _mkgmtime(&tm);
    if (epoch <= 0) return false;
    out = epoch;
    return true;
}

struct GameStoresAttemptInfo {
    int count = 0;
    std::time_t lastAttemptUtc = 0;
    bool hasUnresolvedDispatch = false;
};

std::string GameStoresPendingKey(const std::string& row, const std::string& shopId, const std::string& serverId) {
    const auto explicitKey = JsonScalarAny(row, { "key", "Key" });
    if (!explicitKey.empty()) return explicitKey;
    const auto bucketId = JsonScalarAny(row, { "bucketId", "BucketId", "id", "Id" });
    if (!bucketId.empty()) return BuildGameStoresOrderKey(shopId, serverId, bucketId);
    return {};
}

std::map<std::string, GameStoresAttemptInfo> GameStoresAttemptIndex(const std::wstring& baseDir) {
    std::map<std::string, GameStoresAttemptInfo> result;
    const auto rows = ExtractArrayObjects(ModuleStateJson(baseDir, "gamestores-attempts"));
    const size_t start = rows.size() > 1000 ? rows.size() - 1000 : 0;
    for (size_t i = start; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const auto key = JsonScalarAny(row, { "key", "Key" });
        if (key.empty()) continue;
        auto& info = result[ToLowerAscii(key)];
        const auto status = ToLowerAscii(JsonScalarAny(row, { "status", "Status" }));
        if (status.empty() || status == "delivering") {
            ++info.count;
        }
        if (status == "delivering" || status == "claim-delivering") {
            info.hasUnresolvedDispatch = true;
        }
        if (status.empty() || status == "delivering" || status == "retry" || status == "failed") {
            std::time_t utc = 0;
            if (TryParseUtcIsoSeconds(JsonScalarAny(row, { "utc", "Utc", "timestampUtc", "TimestampUtc" }), utc) &&
                utc > info.lastAttemptUtc) {
                info.lastAttemptUtc = utc;
            }
        }
    }
    return result;
}

std::string GameStoresSyncJson(const std::wstring& baseDir) {
    std::lock_guard<std::recursive_mutex> operationGuard(g_gameStoresOperationMutex);
    GameStoresOperationLease lease(baseDir);
    if (!lease.acquired()) return Envelope(false, lease.error());
    bool expected = false;
    if (!g_gameStoresSyncBusy.compare_exchange_strong(expected, true)) {
        return Envelope(false, "Проверка GameStores уже выполняется.");
    }
    struct BusyGuard { ~BusyGuard() { g_gameStoresSyncBusy = false; } } guard;

    const auto config = PluginConfigJson(baseDir, "gamestores-shop", "{}");
    if (!JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false))) {
        return Envelope(false, "Магазин GameStores выключен в настройках.");
    }
    const auto shopId = FirstNonEmpty({ JsonScalarAny(config, { "ShopId", "shopId", "StoreId", "storeId" }), "0" });
    const auto secret = JsonScalarAny(config, { "SecretKey", "secretKey", "ApiKey", "apiKey" });
    const auto serverId = FirstNonEmpty({ JsonScalarAny(config, { "ServerId", "serverId", "GameStoresServerId", "gameStoresServerId" }), "0" });
    if (shopId == "0" || TrimAscii(secret).empty()) {
        return Envelope(false, "Не настроены ShopId или SecretKey GameStores.");
    }

    const auto playersBody = LivePlayersJson(baseDir, 2500);
    auto players = BridgeRuntimePlayers(playersBody);
    players.erase(std::remove_if(players.begin(), players.end(), [](const BridgeRuntimePlayer& player) {
        return TrimAscii(player.steamId).empty() || IdentityTextLooksUnsafe(player.steamId);
    }), players.end());

    ReconcileGameStoresDeliveredJournal(baseDir);
    const auto rules = ExtractArrayObjects(JsonRawFieldAny(config, { "Rules", "rules" }));
    const auto settledKeys = GameStoresSettledKeys(baseDir);
    const bool allowDirectItemIdDelivery = JsonBoolField(config, "AllowDirectItemIdDelivery",
        JsonBoolField(config, "allowDirectItemIdDelivery", false));
    std::vector<std::string> rows;
    std::vector<std::string> pendingKeys;
    int retainedPending = 0;
    for (const auto& row : ExtractArrayObjects(ModuleStateJson(baseDir, "gamestores-pending"))) {
        if (GameStoresRowSettled(settledKeys, row, shopId, serverId)) continue;
        rows.push_back(row);
        AddGameStoresRowKeys(pendingKeys, row, shopId, serverId);
        ++retainedPending;
    }

    int fetched = 0;
    int matched = 0;
    int noRule = 0;
    int settled = 0;
    int alreadyPending = 0;
    int apiFailures = 0;
    int ruleBuildFailed = 0;

    const int timeoutSeconds = std::clamp(JsonIntField(config, "HttpTimeoutSeconds", 20), 3, 60);
    for (const auto& player : players) {
        std::vector<std::pair<std::string, std::string>> query = {
            { "shop_id", shopId },
            { "secret", TrimAscii(secret) },
            { "server", serverId },
            { "items", "true" },
            { "steam_id", TrimAscii(player.steamId) }
        };
        const auto http = HttpGetText(GameStoresApiUrl(query), timeoutSeconds);
        if (!http.ok) {
            ++apiFailures;
            AppendModuleStateRecord(baseDir, "gamestores-shop",
                std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"api-error\",\"steamId\":\"" +
                JsonEscape(player.steamId) + "\",\"httpStatus\":" + std::to_string(http.status) +
                ",\"message\":\"" + JsonEscape(http.message) + "\"}");
            continue;
        }
        if (GameStoresJsonIsEmptyBucket(http.body)) continue;
        if (GameStoresJsonLooksLikeError(http.body)) {
            ++apiFailures;
            AppendModuleStateRecord(baseDir, "gamestores-shop",
                std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"api-error\",\"steamId\":\"" +
                JsonEscape(player.steamId) + "\",\"httpStatus\":" + std::to_string(http.status) +
                ",\"message\":\"GameStores API вернул ошибку.\"}");
            continue;
        }

        for (const auto& object : ExtractGameStoresItemObjects(http.body, player)) {
            auto item = ParseGameStoresItem(object, player);
            ++fetched;
            const auto orderKey = BuildGameStoresOrderKey(shopId, serverId, item.id);
            const auto journal = InspectGameStoresJournal(baseDir, orderKey);
            if (GameStoresItemSettled(settledKeys, item, shopId, serverId) || journal.state != GameStoresJournalState::None) {
                ++settled;
                continue;
            }
            if (GameStoresKeyExists(pendingKeys, orderKey) ||
                GameStoresKeyExists(pendingKeys, "bucketId:" + item.id) ||
                GameStoresKeyExists(pendingKeys, "id:" + item.id)) {
                ++alreadyPending;
                continue;
            }

            std::string matchedRule;
            for (const auto& rule : rules) {
                if (GameStoresRuleMatches(rule, item)) {
                    matchedRule = rule;
                    break;
                }
            }
            if (matchedRule.empty() && !allowDirectItemIdDelivery) {
                ++noRule;
                AppendModuleStateRecord(baseDir, "gamestores-shop",
                    std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"no-rule\",\"bucketId\":\"" +
                    JsonEscape(item.id) + "\",\"productId\":\"" + JsonEscape(item.itemId) +
                    "\",\"title\":\"" + JsonEscape(item.name) + "\",\"steamId\":\"" + JsonEscape(item.steamId) + "\"}");
                continue;
            }
            auto row = BuildGameStoresPendingRow(item, matchedRule, shopId, serverId, allowDirectItemIdDelivery);
            if (row.empty()) {
                ++ruleBuildFailed;
                continue;
            }
            ++matched;
            rows.push_back(row);
            AddGameStoresRowKeys(pendingKeys, row, shopId, serverId);
        }
    }

    std::ostringstream pending;
    pending << "[";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i) pending << ",";
        pending << rows[i];
    }
    pending << "]";
    WriteTextFile(ModuleStatePath(baseDir, "gamestores-pending"), pending.str());
    AppendModuleStateRecord(baseDir, "gamestores-shop",
        std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"synced\",\"players\":" + std::to_string(players.size()) +
        ",\"fetched\":" + std::to_string(fetched) +
        ",\"rules\":" + std::to_string(rules.size()) +
        ",\"matched\":" + std::to_string(matched) +
        ",\"pending\":" + std::to_string(rows.size()) +
        ",\"retainedPending\":" + std::to_string(retainedPending) +
        ",\"settled\":" + std::to_string(settled) +
        ",\"alreadyPending\":" + std::to_string(alreadyPending) +
        ",\"noRule\":" + std::to_string(noRule) +
        ",\"apiFailures\":" + std::to_string(apiFailures) +
        ",\"ruleBuildFailed\":" + std::to_string(ruleBuildFailed) + "}");

    return Envelope(true, std::string("{\"synced\":true,\"players\":") + std::to_string(players.size()) +
        ",\"fetched\":" + std::to_string(fetched) +
        ",\"rules\":" + std::to_string(rules.size()) +
        ",\"matched\":" + std::to_string(matched) +
        ",\"pending\":" + std::to_string(rows.size()) +
        ",\"retainedPending\":" + std::to_string(retainedPending) +
        ",\"settled\":" + std::to_string(settled) +
        ",\"alreadyPending\":" + std::to_string(alreadyPending) +
        ",\"noRule\":" + std::to_string(noRule) +
        ",\"apiFailures\":" + std::to_string(apiFailures) +
        ",\"ruleBuildFailed\":" + std::to_string(ruleBuildFailed) + "}");
}

struct PrisonerAttributes {
    bool ok = false;
    double strength = 0.0;
    double constitution = 0.0;
    double dexterity = 0.0;
    double intelligence = 0.0;
    std::string steamId;
    std::string name;
    std::string error;
};

std::string SqlQuote(const std::string& value) {
    std::string out = "'";
    for (const char ch : value) {
        if (ch == '\'') out += "''";
        else out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

std::string JsonDoubleValue(double value) {
    if (!std::isfinite(value)) return "0";
    return std::to_string(static_cast<long long>(std::llround(value)));
}

std::string JsonPreciseDoubleValue(double value) {
    std::ostringstream ss;
    ss << value;
    return ss.str();
}

int HexNibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::vector<unsigned char> HexToBytes(const std::string& hex) {
    std::vector<unsigned char> bytes;
    if (hex.size() % 2 != 0) return bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = HexNibble(hex[i]);
        const int lo = HexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            bytes.clear();
            return bytes;
        }
        bytes.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return bytes;
}

std::string BytesToHex(const std::vector<unsigned char>& bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

bool ReadI32Le(const std::vector<unsigned char>& bytes, size_t& offset, int& value) {
    if (offset + 4 > bytes.size()) return false;
    uint32_t raw = static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    value = static_cast<int>(raw);
    offset += 4;
    return true;
}

bool ReadTaggedFString(const std::vector<unsigned char>& bytes, size_t& offset, std::string& value) {
    int length = 0;
    if (!ReadI32Le(bytes, offset, length)) return false;
    if (length == 0) {
        value.clear();
        return true;
    }
    if (length > 0) {
        if (length > 512 || offset + static_cast<size_t>(length) > bytes.size()) return false;
        value.assign(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<size_t>(length));
        offset += static_cast<size_t>(length);
        if (!value.empty() && value.back() == '\0') value.pop_back();
        return true;
    }

    const int wcharCount = -length;
    if (wcharCount <= 0 || wcharCount > 512 || offset + static_cast<size_t>(wcharCount) * 2 > bytes.size()) return false;
    value.clear();
    value.reserve(static_cast<size_t>(wcharCount));
    for (int i = 0; i < wcharCount; ++i) {
        const unsigned char lo = bytes[offset + static_cast<size_t>(i) * 2];
        const unsigned char hi = bytes[offset + static_cast<size_t>(i) * 2 + 1];
        if (lo == 0 && hi == 0) break;
        value.push_back(hi == 0 ? static_cast<char>(lo) : '?');
    }
    offset += static_cast<size_t>(wcharCount) * 2;
    return true;
}

bool TryLocateTaggedDoubleAt(const std::vector<unsigned char>& bytes, size_t start, const std::string& propertyName, double& value, size_t* valueOffset) {
    size_t offset = start;
    std::string name;
    std::string type;
    if (!ReadTaggedFString(bytes, offset, name) || name != propertyName) return false;
    if (!ReadTaggedFString(bytes, offset, type) || type != "DoubleProperty") return false;
    int size = 0;
    int arrayIndex = 0;
    if (!ReadI32Le(bytes, offset, size) || !ReadI32Le(bytes, offset, arrayIndex)) return false;
    if (size != 8 || arrayIndex != 0 || offset >= bytes.size()) return false;
    const unsigned char hasGuid = bytes[offset++];
    if (hasGuid != 0) offset += 16;
    if (offset + 8 > bytes.size()) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(double));
    if (valueOffset) *valueOffset = offset;
    return std::isfinite(value);
}

bool TryReadTaggedDoubleAt(const std::vector<unsigned char>& bytes, size_t start, const std::string& propertyName, double& value) {
    return TryLocateTaggedDoubleAt(bytes, start, propertyName, value, nullptr);
}

bool FindTaggedDouble(const std::vector<unsigned char>& bytes, const std::string& propertyName, double& value) {
    for (size_t i = 0; i + 32 < bytes.size(); ++i) {
        if (TryReadTaggedDoubleAt(bytes, i, propertyName, value)) return true;
    }
    return false;
}

bool FindTaggedDoubleOffset(const std::vector<unsigned char>& bytes, const std::string& propertyName, double& value, size_t& valueOffset) {
    for (size_t i = 0; i + 32 < bytes.size(); ++i) {
        if (TryLocateTaggedDoubleAt(bytes, i, propertyName, value, &valueOffset)) return true;
    }
    return false;
}

bool WriteTaggedDouble(std::vector<unsigned char>& bytes, const std::string& propertyName, double value, std::string& error) {
    double previous = 0.0;
    size_t offset = 0;
    if (!FindTaggedDoubleOffset(bytes, propertyName, previous, offset)) {
        error = "attribute tag not found: " + propertyName;
        return false;
    }
    if (offset + sizeof(double) > bytes.size()) {
        error = "attribute tag offset is outside body_simulation: " + propertyName;
        return false;
    }
    std::memcpy(bytes.data() + offset, &value, sizeof(double));
    return true;
}

std::string AttributesJson(const PrisonerAttributes& attrs) {
    std::ostringstream ss;
    ss << "{\"steamId\":\"" << JsonEscape(attrs.steamId)
       << "\",\"name\":\"" << JsonEscape(attrs.name)
       << "\",\"strength\":" << JsonDoubleValue(attrs.strength)
       << ",\"constitution\":" << JsonDoubleValue(attrs.constitution)
       << ",\"dexterity\":" << JsonDoubleValue(attrs.dexterity)
       << ",\"intelligence\":" << JsonDoubleValue(attrs.intelligence) << "}";
    return ss.str();
}

bool ReadPrisonerAttributesFromDb(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    PrisonerAttributes& attrs) {
    attrs = PrisonerAttributes{};
    const auto cleanSteam = TrimAscii(steamId);
    const auto cleanName = TrimAscii(name);
    if (cleanSteam.empty() && cleanName.empty()) {
        attrs.error = "player identity is missing";
        return false;
    }

    std::string where;
    if (!cleanSteam.empty()) where = "up.user_id = " + SqlQuote(cleanSteam);
    if (!cleanName.empty()) {
        const auto nameClause = "LOWER(up.name) = LOWER(" + SqlQuote(cleanName) + ")";
        where = where.empty() ? nameClause : ("(" + where + " OR " + nameClause + ")");
    }

    const std::string sql =
        "SELECT up.user_id AS steamId, up.name AS playerName, hex(p.body_simulation) AS bodyHex "
        "FROM user_profile up "
        "JOIN prisoner p ON p.id = up.prisoner_id "
        "WHERE " + where + " LIMIT 1";

    std::string rows;
    std::string error;
    if (!SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), sql, 1, rows, error)) {
        attrs.error = error.empty() ? "SCUM.db attribute query failed" : error;
        return false;
    }

    const auto objects = ExtractArrayObjects(rows);
    if (objects.empty()) {
        attrs.error = "player body_simulation row not found";
        return false;
    }

    attrs.steamId = FlatJsonStringValue(objects.front(), "steamId");
    attrs.name = FlatJsonStringValue(objects.front(), "playerName");
    const auto bodyHex = FlatJsonStringValue(objects.front(), "bodyHex");
    const auto bytes = HexToBytes(bodyHex);
    if (bytes.empty()) {
        attrs.error = "body_simulation blob is empty or unreadable";
        return false;
    }

    const bool ok =
        FindTaggedDouble(bytes, "BaseStrength", attrs.strength) &&
        FindTaggedDouble(bytes, "BaseConstitution", attrs.constitution) &&
        FindTaggedDouble(bytes, "BaseDexterity", attrs.dexterity) &&
        FindTaggedDouble(bytes, "BaseIntelligence", attrs.intelligence);
    attrs.ok = ok;
    if (!ok) attrs.error = "attribute tags were not found in body_simulation";
    return ok;
}

std::string UtcIsoFromEpoch(std::time_t value) {
    std::tm tm{};
    gmtime_s(&tm, &value);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

long long JsonLongLongField(const std::string& body, const std::string& key, long long fallback) {
    const auto raw = TrimAscii(JsonRawField(body, key));
    if (raw.empty()) return fallback;
    for (const char ch : raw) {
        if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '-') return fallback;
    }
    return _strtoi64(raw.c_str(), nullptr, 10);
}

struct PlayerWallet {
    bool ok = false;
    long long profileId = 0;
    std::string steamId;
    std::string name;
    long long walletBalance = 0;
    long long normalBalance = 0;
    long long goldBalance = 0;
    double famePoints = 0.0;
    std::string error;
};

long long JsonInt64Value(const std::string& object, const std::string& key, long long fallback = 0) {
    const auto raw = FlatJsonNumberValue(object, key);
    if (raw.empty()) return fallback;
    return _strtoi64(raw.c_str(), nullptr, 10);
}

double JsonDoubleField(const std::string& object, const std::string& key, double fallback = 0.0) {
    const auto raw = FlatJsonNumberValue(object, key);
    if (raw.empty()) return fallback;
    return std::atof(raw.c_str());
}

bool FillWalletFromObject(const std::string& object, PlayerWallet& wallet) {
    wallet.profileId = JsonInt64Value(object, "profileId", 0);
    wallet.steamId = FlatJsonStringValue(object, "steamId");
    wallet.name = FlatJsonStringValue(object, "name");
    wallet.walletBalance = JsonInt64Value(object, "walletBalance", 0);
    wallet.normalBalance = JsonInt64Value(object, "normalBalance", wallet.walletBalance);
    wallet.goldBalance = JsonInt64Value(object, "goldBalance", 0);
    wallet.famePoints = JsonDoubleField(object, "famePoints", 0.0);
    wallet.ok = wallet.profileId > 0 || !wallet.steamId.empty() || !wallet.name.empty();
    if (!wallet.ok) wallet.error = "player wallet row not found";
    return wallet.ok;
}

bool ReadPlayerWalletFromDb(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    PlayerWallet& wallet) {
    wallet = PlayerWallet{};
    const auto identity = ResolvePlayerIdentity(baseDir, steamId, name);
    const auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
    const auto targetName = identity.name.empty() ? name : identity.name;
    const auto filter = PlayerSqlFilter(targetSteamId, targetName);
    if (filter.empty()) {
        wallet.error = "player identity is missing";
        return false;
    }

    std::string rows;
    std::string error;
    const auto complexSql =
        std::string("SELECT up.id AS profileId, up.user_id AS steamId, up.name AS name, ")
        + "up.money_balance AS walletBalance, up.fame_points AS famePoints, "
        + "COALESCE((SELECT MAX(c.account_balance) FROM bank_account_registry r "
        + "JOIN bank_account_registry_currencies c ON c.bank_account_id = r.id "
        + "WHERE (r.user_profile_id = up.id OR r.account_owner_user_profile_id = up.id) AND c.currency_type = 1), up.money_balance, 0) AS normalBalance, "
        + "COALESCE((SELECT MAX(c.account_balance) FROM bank_account_registry r "
        + "JOIN bank_account_registry_currencies c ON c.bank_account_id = r.id "
        + "WHERE (r.user_profile_id = up.id OR r.account_owner_user_profile_id = up.id) AND c.currency_type = 2), 0) AS goldBalance "
        + "FROM user_profile up WHERE 1=1 " + filter
        + "ORDER BY up.last_login_time DESC LIMIT 1";

    if (!SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), complexSql, 1, rows, error)) {
        const auto fallbackSql =
            std::string("SELECT up.id AS profileId, up.user_id AS steamId, up.name AS name, ")
            + "up.money_balance AS walletBalance, up.money_balance AS normalBalance, "
            + "0 AS goldBalance, up.fame_points AS famePoints "
            + "FROM user_profile up WHERE 1=1 " + filter
            + "ORDER BY up.last_login_time DESC LIMIT 1";
        if (!SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), fallbackSql, 1, rows, error)) {
            wallet.error = error.empty() ? "SCUM.db wallet query failed" : error;
            return false;
        }
    }

    const auto objects = ExtractArrayObjects(rows);
    if (objects.empty()) {
        wallet.error = "player wallet row not found";
        return false;
    }
    return FillWalletFromObject(objects.front(), wallet);
}

std::string WalletJson(const PlayerWallet& wallet) {
    std::ostringstream ss;
    ss << "{\"steamId\":\"" << JsonEscape(wallet.steamId)
       << "\",\"name\":\"" << JsonEscape(wallet.name)
       << "\",\"profileId\":" << wallet.profileId
       << ",\"moneyBalance\":" << wallet.walletBalance
       << ",\"normalBalance\":" << wallet.normalBalance
       << ",\"goldBalance\":" << wallet.goldBalance
       << ",\"famePoints\":" << JsonDoubleValue(wallet.famePoints) << "}";
    return ss.str();
}

struct PlayerSkillsSnapshot {
    bool ok = false;
    std::string steamId;
    std::string name;
    std::string skillsJson = "[]";
    std::string error;
};

bool ReadPlayerSkillsFromDb(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    PlayerSkillsSnapshot& snapshot) {
    snapshot = PlayerSkillsSnapshot{};
    const auto requestedSteamId = TrimAscii(steamId);
    const auto requestedName = TrimAscii(name);
    const bool hasSteamId = IsDigitsText(requestedSteamId) &&
        requestedSteamId.size() >= 16 && requestedSteamId.size() <= 20;
    const auto filter = PlayerSqlFilter(requestedSteamId, requestedName);
    if (filter.empty()) {
        snapshot.error = "player identity is missing";
        return false;
    }

    const auto dbPath = SavedDbPath(baseDir);
    if (!FileExists(dbPath)) {
        snapshot.error = "SCUM.db is unavailable";
        return false;
    }

    const std::string sql =
        "SELECT up.user_id AS steamId, up.name AS playerName, "
        "COALESCE(ps.name, '') AS name, COALESCE(ps.name, '') AS skillName, "
        "COALESCE(ps.level, 0) AS level, COALESCE(ps.experience, 0) AS experience "
        "FROM user_profile up "
        "JOIN prisoner_skill ps ON ps.prisoner_id = up.prisoner_id "
        "WHERE up.user_id IS NOT NULL AND up.user_id <> '' " + filter +
        "ORDER BY lower(COALESCE(ps.name, '')), ps.name LIMIT 128";

    if (!SqliteQueryRowsJson(baseDir, dbPath, sql, 128, snapshot.skillsJson, snapshot.error)) {
        snapshot.error = "SCUM.db prisoner_skill query failed";
        snapshot.skillsJson = "[]";
        return false;
    }

    snapshot.steamId = hasSteamId ? requestedSteamId : "";
    snapshot.name = requestedName;
    const auto rows = ExtractArrayObjects(snapshot.skillsJson);
    if (!rows.empty()) {
        const auto resolvedSteamId = FlatJsonStringValue(rows.front(), "steamId");
        const auto resolvedName = FlatJsonStringValue(rows.front(), "playerName");
        if (!resolvedSteamId.empty()) snapshot.steamId = resolvedSteamId;
        if (!resolvedName.empty()) snapshot.name = resolvedName;
    }
    snapshot.ok = true;
    return true;
}

std::string PlayerSkillsJson(const PlayerSkillsSnapshot& snapshot) {
    std::ostringstream ss;
    ss << "{\"source\":\"scum-db-prisoner-skill\""
       << ",\"readOnly\":true"
       << ",\"steamId\":\"" << JsonEscape(snapshot.steamId) << "\""
       << ",\"name\":\"" << JsonEscape(snapshot.name) << "\""
       << ",\"skills\":" << (snapshot.skillsJson.empty() ? "[]" : snapshot.skillsJson)
       << "}";
    return ss.str();
}

long long WalletCurrencyBalance(const PlayerWallet& wallet, const std::string& currency) {
    return ToLowerAscii(currency) == "gold" ? wallet.goldBalance : wallet.normalBalance;
}

struct MoneyChangeOutcome {
    bool ok = false;
    bool bridgeOk = false;
    bool dbPatched = false;
    bool verified = false;
    bool commandAccepted = false;
    bool uncertain = false;
    std::string operationId;
    std::string transport;
    std::string message;
    std::string bridgeBody;
    PlayerWallet before;
    PlayerWallet after;
};

MoneyChangeOutcome ChangeMoneyVerified(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    int amount,
    std::string currency,
    bool allowBridgeFallback = true,
    bool singleDirectTransport = false) {
    MoneyChangeOutcome outcome;
    outcome.operationId = "money-" + std::to_string(GetTickCount64()) + "-" +
        std::to_string(g_verifiedMoneyOperationSequence.fetch_add(1) + 1);
    if (currency.empty()) currency = "Normal";
    if (ToLowerAscii(currency) != "gold") currency = "Normal";
    const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
    auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
    auto targetName = identity.name.empty() ? name : identity.name;
    auto targetRuntimeKey = identity.runtimeKey.empty() ? runtimeKey : identity.runtimeKey;

    if (targetSteamId.empty() && targetName.empty() && targetRuntimeKey.empty()) {
        outcome.message = "Для live-команды изменения баланса нужен SteamID64, runtimeKey или безопасное имя игрока.";
        AppendActionRecord(baseDir, "change_money", false, targetSteamId, targetName, outcome.message,
            std::string("{\"amount\":") + std::to_string(amount) + ",\"currency\":\"" + JsonEscape(currency) +
            "\",\"transport\":\"live-target-preflight\"}");
        return outcome;
    }

    auto routeTarget = ValidatePanelLiveTargetForCommandChannel(
        baseDir, targetSteamId, targetName, targetRuntimeKey, "change-money-live-route", false, true);
    if (!routeTarget.ok) {
        outcome.message = routeTarget.message;
        outcome.bridgeBody = routeTarget.body.empty() ? routeTarget.message : routeTarget.body;
        AppendActionRecord(baseDir, "change_money", false, targetSteamId, targetName, outcome.message,
            std::string("{\"amount\":") + std::to_string(amount) + ",\"currency\":\"" + JsonEscape(currency) +
            "\",\"transport\":\"live-target-preflight\"}");
        return outcome;
    }
    if (!routeTarget.steamId.empty()) targetSteamId = routeTarget.steamId;
    if (!routeTarget.name.empty()) targetName = routeTarget.name;
    if (!routeTarget.runtimeKey.empty()) targetRuntimeKey = routeTarget.runtimeKey;

    std::unique_lock<std::mutex> operationLock(g_verifiedMoneyOperationMutex);
    if (!ReadPlayerWalletFromDb(baseDir, targetSteamId, targetName, outcome.before)) {
        outcome.message = outcome.before.error.empty() ? "Не удалось прочитать кошелёк до live-команды." : outcome.before.error;
        AppendActionRecord(baseDir, "change_money", false, targetSteamId, targetName, outcome.message,
            std::string("{\"amount\":") + std::to_string(amount) + ",\"currency\":\"" + JsonEscape(currency) + "\"}");
        return outcome;
    }

    const bool goldCurrency = ToLowerAscii(currency) == "gold";
    const auto beforeBalance = WalletCurrencyBalance(outcome.before, currency);
    const auto beforeWalletBalance = outcome.before.walletBalance;
    const auto expected = beforeBalance + static_cast<long long>(amount);
    const auto expectedWallet = beforeWalletBalance + static_cast<long long>(amount);
    if ((goldCurrency && expected < 0) || (!goldCurrency && expected < 0 && expectedWallet < 0)) {
        outcome.message = "Недостаточно средств: " + currency;
        outcome.after = outcome.before;
        AppendActionRecord(baseDir, "change_money", false, outcome.before.steamId, outcome.before.name, outcome.message,
            std::string("{\"amount\":") + std::to_string(amount) +
            ",\"currency\":\"" + JsonEscape(currency) +
            "\",\"before\":" + std::to_string(beforeBalance) +
            ",\"walletBefore\":" + std::to_string(beforeWalletBalance) + "}");
        return outcome;
    }

    bool commandAccepted = false;
    bool directTransportAttempted = false;
    std::string commandTransport;
    std::string commandMessage;
    std::string commandBody;
    const auto targetRef = ScumAdminTargetRef(targetSteamId, targetName);
    if (!targetRef.empty() || !targetRuntimeKey.empty() || !targetSteamId.empty() || !targetName.empty()) {
        PanelLiveTarget commandTarget = routeTarget;
        bool commandTargetChecked = false;
        const auto ensureCommandTarget = [&]() -> bool {
            if (!commandTargetChecked) {
                commandTarget = ValidatePanelLiveTargetForCommandChannel(
                    baseDir, targetSteamId, targetName, targetRuntimeKey, "change-money-command-channel", false, true);
                commandTargetChecked = true;
            }
            if (!commandTarget.ok) {
                commandTransport = "live-target-preflight";
                commandMessage = commandTarget.message;
                commandBody = commandTarget.body.empty() ? commandTarget.message : commandTarget.body;
                return false;
            }
            return true;
        };
        const bool localRconPreferred = LocalRconEnabled(baseDir);
        if (localRconPreferred && ensureCommandTarget()) {
            directTransportAttempted = true;
            const auto liveTargetRef = ScumAdminTargetRef(commandTarget.steamId, commandTarget.name);
            if (!liveTargetRef.empty()) {
                const auto rconCommand = std::string("ChangeCurrencyBalance ") + currency + " " + std::to_string(amount) + " " + liveTargetRef;
                const auto rcon = LocalRconSendConsoleCommand(baseDir, rconCommand);
                commandAccepted = rcon.ok;
                commandTransport = "server-command";
                commandMessage = rcon.message;
                commandBody = std::string("{\"command\":\"") + JsonEscape(LocalRconCommandText(rconCommand)) +
                    "\",\"commandBody\":" + JsonValueOrString(rcon.body) + "}";
            } else {
                commandTransport = "live-target-preflight";
                commandMessage = "Игрок подтверждён, но безопасный targetRef не получен; команда панели отменена.";
            }
        }
        // A strict payment must never retry a possibly-sent command through a
        // second transport.  If Local RCON was selected, a non-success result
        // is treated as uncertain and the caller receives no gameplay effect.
        if (!commandAccepted && ArkPanelEnabled(baseDir) && (!singleDirectTransport || !localRconPreferred)) {
            commandTargetChecked = false;
            if (ensureCommandTarget()) {
                directTransportAttempted = true;
                const auto liveTargetRef = ScumAdminTargetRef(commandTarget.steamId, commandTarget.name);
                if (!liveTargetRef.empty()) {
                    const auto arkCommand = std::string("#ChangeCurrencyBalance ") + currency + " " + std::to_string(amount) + " " + liveTargetRef;
                    const auto rcon = ArkPanelSendConsoleCommand(baseDir, arkCommand);
                    commandAccepted = rcon.ok;
                    commandTransport = "hosting-panel-command";
                    commandMessage = rcon.message;
                    commandBody = std::string("{\"command\":\"") + JsonEscape(arkCommand) +
                        "\",\"status\":" + std::to_string(rcon.status) +
                        ",\"panelBody\":" + JsonValueOrString(rcon.body) + "}";
                } else {
                    commandTransport = "live-target-preflight";
                    commandMessage = "Игрок подтверждён, но безопасный targetRef не получен; команда панели отменена.";
                }
            }
        }
        if (allowBridgeFallback && !commandAccepted) {
            commandTargetChecked = false;
            if (ensureCommandTarget()) {
                auto br = BridgeExecute(baseDir, "change_money",
                    std::string("{") + PlayerTargetArgs(commandTarget.steamId, commandTarget.name, commandTarget.runtimeKey) +
                    ",\"amount\":" + std::to_string(amount) +
                    ",\"currency\":\"" + JsonEscape(currency) + "\"}", 20000);
                if (br.ok) {
                    commandAccepted = true;
                    commandTransport = "ue4ss-bridge-immediate";
                    commandMessage = br.message;
                    commandBody = br.body;
                } else {
                    commandTransport = "ue4ss-bridge-immediate";
                    commandMessage = br.message;
                    commandBody = br.body.empty() ? br.message : br.body;
                }
            }
        }
    } else {
        commandTransport = "none";
        commandMessage = "Для live-команды изменения баланса нужен SteamID64 или безопасное имя игрока.";
    }

    outcome.bridgeOk = commandAccepted;
    outcome.commandAccepted = commandAccepted;
    outcome.transport = commandTransport;
    outcome.bridgeBody = commandBody;

    if (commandAccepted) {
        for (int i = 0; i < 30; ++i) {
            if (i > 0) std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (ReadPlayerWalletFromDb(baseDir, targetSteamId, targetName, outcome.after)) {
                const auto afterBalance = WalletCurrencyBalance(outcome.after, currency);
                const auto afterWalletBalance = outcome.after.walletBalance;
                if ((goldCurrency && afterBalance == expected) ||
                    (!goldCurrency && (afterBalance == expected || afterWalletBalance == expectedWallet))) {
                    outcome.verified = true;
                    break;
                }
            }
        }
    }

    outcome.ok = commandAccepted && outcome.verified;
    outcome.uncertain = !outcome.verified && (commandAccepted || (singleDirectTransport && directTransportAttempted));
    if (outcome.ok) {
        outcome.message = "Баланс изменён live-командой и подтверждён через SCUM.db.";
    } else {
        outcome.after = outcome.after.ok ? outcome.after : outcome.before;
        const auto afterBalance = outcome.after.ok ? WalletCurrencyBalance(outcome.after, currency) : beforeBalance;
        const auto afterWalletBalance = outcome.after.ok ? outcome.after.walletBalance : beforeWalletBalance;
        outcome.message = std::string("money verification failed: before=") + std::to_string(beforeBalance) +
            " expected=" + std::to_string(expected) +
            " after=" + std::to_string(afterBalance) +
            " walletBefore=" + std::to_string(beforeWalletBalance) +
            " walletExpected=" + std::to_string(expectedWallet) +
            " walletAfter=" + std::to_string(afterWalletBalance) +
            " command=" + (commandAccepted ? "ok" : commandMessage) +
            " dbWrite=false dbFallback=disabled";
    }

    AppendModuleStateRecord(baseDir, "economy",
        std::string("{\"type\":\"economy\",\"timestampUtc\":\"") + UtcIsoNow() +
        "\",\"message\":\"balance change verified\",\"steamId\":\"" + JsonEscape(outcome.before.steamId.empty() ? targetSteamId : outcome.before.steamId) +
        "\",\"name\":\"" + JsonEscape(outcome.before.name.empty() ? targetName : outcome.before.name) +
        "\",\"currency\":\"" + JsonEscape(currency) +
        "\",\"amount\":" + std::to_string(amount) +
        ",\"before\":" + std::to_string(beforeBalance) +
        ",\"walletBefore\":" + std::to_string(beforeWalletBalance) +
        ",\"after\":" + std::to_string(outcome.after.ok ? WalletCurrencyBalance(outcome.after, currency) : beforeBalance) +
        ",\"walletAfter\":" + std::to_string(outcome.after.ok ? outcome.after.walletBalance : beforeWalletBalance) +
        ",\"bridgeOk\":" + (commandAccepted ? "true" : "false") +
        ",\"operationId\":\"" + JsonEscape(outcome.operationId) + "\"" +
        ",\"commandAccepted\":" + (outcome.commandAccepted ? "true" : "false") +
        ",\"uncertain\":" + (outcome.uncertain ? "true" : "false") +
        ",\"transport\":\"" + JsonEscape(commandTransport) + "\"" +
        ",\"dbWrite\":" + (outcome.dbPatched ? "true" : "false") +
        ",\"liveApply\":\"" + std::string(commandAccepted ? "admin-command" : "none") + "\"" +
        ",\"ok\":" + (outcome.ok ? "true" : "false") +
        ",\"result\":\"" + JsonEscape(outcome.message) + "\"}");

    AppendActionRecord(baseDir, "change_money", outcome.ok, outcome.before.steamId.empty() ? targetSteamId : outcome.before.steamId,
        outcome.before.name.empty() ? targetName : outcome.before.name, outcome.message,
        std::string("{\"amount\":") + std::to_string(amount) +
        ",\"currency\":\"" + JsonEscape(currency) +
        "\",\"before\":" + std::to_string(beforeBalance) +
        ",\"walletBefore\":" + std::to_string(beforeWalletBalance) +
        ",\"expected\":" + std::to_string(expected) +
        ",\"walletExpected\":" + std::to_string(expectedWallet) +
        ",\"after\":" + std::to_string(outcome.after.ok ? WalletCurrencyBalance(outcome.after, currency) : beforeBalance) +
        ",\"walletAfter\":" + std::to_string(outcome.after.ok ? outcome.after.walletBalance : beforeWalletBalance) +
        ",\"bridgeOk\":" + (commandAccepted ? "true" : "false") +
        ",\"operationId\":\"" + JsonEscape(outcome.operationId) + "\"" +
        ",\"commandAccepted\":" + (outcome.commandAccepted ? "true" : "false") +
        ",\"uncertain\":" + (outcome.uncertain ? "true" : "false") +
        ",\"transport\":\"" + JsonEscape(commandTransport) + "\"" +
        ",\"dbWrite\":" + (outcome.dbPatched ? "true" : "false") +
        ",\"liveApply\":\"" + std::string(commandAccepted ? "admin-command" : "none") + "\"}");

    return outcome;
}

struct FameSetOutcome {
    bool ok = false;
    bool bridgeOk = false;
    bool dbPatched = false;
    bool verified = false;
    std::string message;
    std::string bridgeBody;
    PlayerWallet before;
    PlayerWallet after;
};

FameSetOutcome SetFameVerified(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    int amount) {
    FameSetOutcome outcome;
    const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
    auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
    auto targetName = identity.name.empty() ? name : identity.name;
    auto targetRuntimeKey = identity.runtimeKey.empty() ? runtimeKey : identity.runtimeKey;

    if (targetSteamId.empty() && targetName.empty() && targetRuntimeKey.empty()) {
        outcome.message = "Для изменения славы нужен SteamID64, runtimeKey или безопасное имя игрока.";
        AppendActionRecord(baseDir, "set_fame", false, targetSteamId, targetName, outcome.message,
            std::string("{\"amount\":") + std::to_string(amount) +
            ",\"transport\":\"live-target-preflight\"}");
        return outcome;
    }

    auto routeTarget = ValidatePanelLiveTargetForCommandChannel(
        baseDir, targetSteamId, targetName, targetRuntimeKey, "set-fame-live-route", false, true);
    if (!routeTarget.ok) {
        outcome.message = routeTarget.message;
        outcome.bridgeBody = routeTarget.body.empty() ? routeTarget.message : routeTarget.body;
        AppendActionRecord(baseDir, "set_fame", false, targetSteamId, targetName, outcome.message,
            std::string("{\"amount\":") + std::to_string(amount) +
            ",\"transport\":\"live-target-preflight\"}");
        return outcome;
    }
    if (!routeTarget.steamId.empty()) targetSteamId = routeTarget.steamId;
    if (!routeTarget.name.empty()) targetName = routeTarget.name;
    if (!routeTarget.runtimeKey.empty()) targetRuntimeKey = routeTarget.runtimeKey;
    ReadPlayerWalletFromDb(baseDir, targetSteamId, targetName, outcome.before);

    const auto targetRef = ScumAdminTargetRef(targetSteamId, targetName);
    bool commandAccepted = false;
    std::string commandMessage;
    std::string commandBody;
    std::string commandTransport;
    if (!targetRef.empty() || !targetRuntimeKey.empty() || !targetSteamId.empty() || !targetName.empty()) {
        PanelLiveTarget commandTarget = routeTarget;
        bool commandTargetChecked = true;
        const auto ensureCommandTarget = [&]() -> bool {
            if (!commandTargetChecked) {
                commandTarget = ValidatePanelLiveTargetForCommandChannel(
                    baseDir, targetSteamId, targetName, targetRuntimeKey, "set-fame-command-channel", false, true);
                commandTargetChecked = true;
            }
            if (!commandTarget.ok) {
                commandTransport = "live-target-preflight";
                commandMessage = commandTarget.message;
                commandBody = commandTarget.body.empty() ? commandTarget.message : commandTarget.body;
                return false;
            }
            return true;
        };
        if (ArkPanelEnabled(baseDir) && ensureCommandTarget()) {
            const auto liveTargetRef = ScumAdminTargetRef(commandTarget.steamId, commandTarget.name);
            if (!liveTargetRef.empty()) {
                const auto arkCommand = std::string("#SetFamePoints ") + std::to_string(amount) + " " + liveTargetRef;
                const auto rcon = ArkPanelSendConsoleCommand(baseDir, arkCommand);
                commandAccepted = rcon.ok;
                commandTransport = "hosting-panel-command";
                commandMessage = rcon.message;
                commandBody = std::string("{\"command\":\"") + JsonEscape(arkCommand) +
                    "\",\"status\":" + std::to_string(rcon.status) +
                    ",\"panelBody\":" + JsonValueOrString(rcon.body) + "}";
            } else {
                commandTransport = "live-target-preflight";
                commandMessage = "Игрок подтверждён, но безопасный targetRef не получен; команда панели отменена.";
            }
        }
        if (!commandAccepted && ensureCommandTarget()) {
            auto br = BridgeExecute(baseDir, "set_fame",
                std::string("{") + PlayerTargetArgs(commandTarget.steamId, commandTarget.name, commandTarget.runtimeKey) +
                ",\"amount\":" + std::to_string(amount) + "}", 20000);
            if (br.ok) {
                commandAccepted = true;
                commandTransport = "ue4ss-bridge-immediate";
                commandMessage = br.message;
                commandBody = br.body;
            } else {
                commandTransport = "ue4ss-bridge-immediate";
                commandMessage = br.message;
                commandBody = br.body.empty() ? br.message : br.body;
            }
        }
    } else {
        commandMessage = "Для изменения славы нужен SteamID64 или безопасное имя игрока.";
    }
    outcome.bridgeOk = commandAccepted;
    outcome.bridgeBody = commandBody;

    if (commandAccepted) {
        for (int i = 0; i < 30; ++i) {
            if (i > 0) std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!ReadPlayerWalletFromDb(baseDir, targetSteamId, targetName, outcome.after)) continue;
            if (std::fabs(outcome.after.famePoints - static_cast<double>(amount)) <= 0.5) {
                outcome.verified = true;
                break;
            }
        }
    }

    outcome.ok = outcome.verified;
    if (outcome.ok) {
        outcome.message = "Слава изменена live-командой и подтверждена через SCUM.db.";
    } else {
        outcome.after = outcome.after.ok ? outcome.after : outcome.before;
        outcome.message = std::string("Проверка славы не подтвердила изменение: requested=") + std::to_string(amount) +
            " actual=" + (outcome.after.ok ? JsonDoubleValue(outcome.after.famePoints) : "unreadable") +
            " command=" + (commandAccepted ? "ok" : commandMessage) +
            " dbWrite=false dbFallback=disabled";
    }
    AppendActionRecord(baseDir, "set_fame", outcome.ok, outcome.after.steamId.empty() ? targetSteamId : outcome.after.steamId,
        outcome.after.name.empty() ? targetName : outcome.after.name, outcome.message,
        std::string("{\"amount\":") + std::to_string(amount) +
        ",\"actual\":" + (outcome.after.ok ? JsonDoubleValue(outcome.after.famePoints) : "null") +
        ",\"bridgeOk\":" + (commandAccepted ? "true" : "false") +
        ",\"transport\":\"" + JsonEscape(commandTransport) + "\"" +
        ",\"dbWrite\":" + (outcome.dbPatched ? "true" : "false") +
        ",\"liveApply\":\"" + std::string(commandAccepted ? "admin-command" : "none") + "\"}");
    return outcome;
}

FameSetOutcome ChangeFameVerified(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    int amount) {
    FameSetOutcome outcome;
    const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
    auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
    auto targetName = identity.name.empty() ? name : identity.name;
    auto targetRuntimeKey = identity.runtimeKey.empty() ? runtimeKey : identity.runtimeKey;

    if (targetSteamId.empty() && targetName.empty() && targetRuntimeKey.empty()) {
        outcome.message = "Для начисления славы нужен SteamID64, runtimeKey или безопасное имя игрока.";
        AppendActionRecord(baseDir, "change_fame", false, targetSteamId, targetName, outcome.message,
            std::string("{\"amount\":") + std::to_string(amount) +
            ",\"transport\":\"live-target-preflight\"}");
        return outcome;
    }

    auto routeTarget = ValidatePanelLiveTargetForCommandChannel(
        baseDir, targetSteamId, targetName, targetRuntimeKey, "change-fame-live-route", false, true);
    if (!routeTarget.ok) {
        outcome.message = routeTarget.message;
        outcome.bridgeBody = routeTarget.body.empty() ? routeTarget.message : routeTarget.body;
        AppendActionRecord(baseDir, "change_fame", false, targetSteamId, targetName, outcome.message,
            std::string("{\"amount\":") + std::to_string(amount) +
            ",\"transport\":\"live-target-preflight\"}");
        return outcome;
    }
    if (!routeTarget.steamId.empty()) targetSteamId = routeTarget.steamId;
    if (!routeTarget.name.empty()) targetName = routeTarget.name;
    if (!routeTarget.runtimeKey.empty()) targetRuntimeKey = routeTarget.runtimeKey;

    if (!ReadPlayerWalletFromDb(baseDir, targetSteamId, targetName, outcome.before)) {
        outcome.message = outcome.before.error.empty() ? "Не удалось прочитать славу до live-команды." : outcome.before.error;
        AppendActionRecord(baseDir, "change_fame", false, targetSteamId, targetName, outcome.message,
            std::string("{\"amount\":") + std::to_string(amount) + "}");
        return outcome;
    }

    const double beforeFame = outcome.before.famePoints;
    const double expected = beforeFame + static_cast<double>(amount);
    const auto targetRef = ScumAdminTargetRef(targetSteamId, targetName);
    bool commandAccepted = false;
    std::string commandMessage;
    std::string commandBody;
    std::string commandTransport;

    if (!targetRef.empty() || !targetRuntimeKey.empty() || !targetSteamId.empty() || !targetName.empty()) {
        PanelLiveTarget commandTarget = routeTarget;
        bool commandTargetChecked = true;
        const auto ensureCommandTarget = [&]() -> bool {
            if (!commandTargetChecked) {
                commandTarget = ValidatePanelLiveTargetForCommandChannel(
                    baseDir, targetSteamId, targetName, targetRuntimeKey, "change-fame-command-channel", false, true);
                commandTargetChecked = true;
            }
            if (!commandTarget.ok) {
                commandTransport = "live-target-preflight";
                commandMessage = commandTarget.message;
                commandBody = commandTarget.body.empty() ? commandTarget.message : commandTarget.body;
                return false;
            }
            return true;
        };
        if (LocalRconEnabled(baseDir) && ensureCommandTarget()) {
            const auto liveTargetRef = ScumAdminTargetRef(commandTarget.steamId, commandTarget.name);
            if (!liveTargetRef.empty()) {
                const auto rconCommand = std::string("ChangeFamePoints ") + std::to_string(amount) + " " + liveTargetRef;
                const auto rcon = LocalRconSendConsoleCommand(baseDir, rconCommand);
                commandAccepted = rcon.ok;
                commandTransport = "server-command";
                commandMessage = rcon.message;
                commandBody = std::string("{\"command\":\"") + JsonEscape(LocalRconCommandText(rconCommand)) +
                    "\",\"commandBody\":" + JsonValueOrString(rcon.body) + "}";
            } else {
                commandTransport = "live-target-preflight";
                commandMessage = "Игрок подтверждён, но безопасный targetRef не получен; команда панели отменена.";
            }
        }
        if (!commandAccepted && ArkPanelEnabled(baseDir) && ensureCommandTarget()) {
            const auto liveTargetRef = ScumAdminTargetRef(commandTarget.steamId, commandTarget.name);
            if (!liveTargetRef.empty()) {
                const auto arkCommand = std::string("#ChangeFamePoints ") + std::to_string(amount) + " " + liveTargetRef;
                const auto rcon = ArkPanelSendConsoleCommand(baseDir, arkCommand);
                commandAccepted = rcon.ok;
                commandTransport = "hosting-panel-command";
                commandMessage = rcon.message;
                commandBody = std::string("{\"command\":\"") + JsonEscape(arkCommand) +
                    "\",\"status\":" + std::to_string(rcon.status) +
                    ",\"panelBody\":" + JsonValueOrString(rcon.body) + "}";
            } else {
                commandTransport = "live-target-preflight";
                commandMessage = "Игрок подтверждён, но безопасный targetRef не получен; команда панели отменена.";
            }
        }
        if (!commandAccepted && ensureCommandTarget()) {
            auto br = BridgeExecute(baseDir, "change_fame",
                std::string("{") + PlayerTargetArgs(commandTarget.steamId, commandTarget.name, commandTarget.runtimeKey) +
                ",\"amount\":" + std::to_string(amount) + "}", 20000);
            if (br.ok) {
                commandAccepted = true;
                commandTransport = "ue4ss-bridge-immediate";
                commandMessage = br.message;
                commandBody = br.body;
            } else {
                commandTransport = "ue4ss-bridge-immediate";
                commandMessage = br.message;
                commandBody = br.body.empty() ? br.message : br.body;
            }
        }
    } else {
        commandMessage = "Для начисления славы нужен SteamID64 или безопасное имя игрока.";
    }

    outcome.bridgeOk = commandAccepted;
    outcome.bridgeBody = commandBody;
    if (commandAccepted) {
        for (int i = 0; i < 30; ++i) {
            if (i > 0) std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!ReadPlayerWalletFromDb(baseDir, targetSteamId, targetName, outcome.after)) continue;
            if (std::fabs(outcome.after.famePoints - expected) <= 0.5) {
                outcome.verified = true;
                break;
            }
        }
    }

    outcome.ok = outcome.verified;
    if (outcome.ok) {
        outcome.message = "Слава начислена live-командой и подтверждена через SCUM.db.";
    } else {
        outcome.after = outcome.after.ok ? outcome.after : outcome.before;
        outcome.message = std::string("Проверка начисления славы не подтвердила изменение: before=") + JsonDoubleValue(beforeFame) +
            " amount=" + std::to_string(amount) +
            " expected=" + JsonDoubleValue(expected) +
            " actual=" + (outcome.after.ok ? JsonDoubleValue(outcome.after.famePoints) : "unreadable") +
            " command=" + (commandAccepted ? "ok" : commandMessage) +
            " dbWrite=false dbFallback=disabled";
    }

    AppendActionRecord(baseDir, "change_fame", outcome.ok, outcome.after.steamId.empty() ? targetSteamId : outcome.after.steamId,
        outcome.after.name.empty() ? targetName : outcome.after.name, outcome.message,
        std::string("{\"amount\":") + std::to_string(amount) +
        ",\"before\":" + JsonDoubleValue(beforeFame) +
        ",\"expected\":" + JsonDoubleValue(expected) +
        ",\"actual\":" + (outcome.after.ok ? JsonDoubleValue(outcome.after.famePoints) : "null") +
        ",\"bridgeOk\":" + (commandAccepted ? "true" : "false") +
        ",\"transport\":\"" + JsonEscape(commandTransport) + "\"" +
        ",\"dbWrite\":" + (outcome.dbPatched ? "true" : "false") +
        ",\"liveApply\":\"" + std::string(commandAccepted ? "admin-command" : "none") + "\"}");
    return outcome;
}

bool TryReadVehicleEntityExists(
    const std::wstring& baseDir,
    long long entityId,
    bool& existsOut,
    std::string& errorOut) {
    existsOut = false;
    errorOut.clear();
    if (entityId <= 0) return true;
    std::string rows;
    const auto sql = std::string("SELECT vehicle_entity_id AS entityId FROM vehicle_spawner WHERE vehicle_entity_id = ")
        + std::to_string(entityId) + " AND COALESCE(is_vehicle_functional, 0) <> 0 LIMIT 1";
    if (!SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), sql, 1, rows, errorOut)) {
        if (errorOut.empty()) errorOut = "vehicle_spawner query failed";
        return false;
    }
    existsOut = !ExtractArrayObjects(rows).empty();
    return true;
}

bool WaitForVehicleEntityGone(const std::wstring& baseDir, long long entityId, std::string& errorOut) {
    const int delaysMs[] = { 250, 500, 750, 1000, 1500, 2000, 2500, 3000 };
    for (const auto delayMs : delaysMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        bool exists = true;
        std::string error;
        if (TryReadVehicleEntityExists(baseDir, entityId, exists, error)) {
            if (!exists) return true;
        } else {
            errorOut = error;
        }
    }
    return false;
}

struct VehicleDestroyOutcome {
    bool ok = false;
    bool bridgeOk = false;
    bool existedBefore = false;
    bool existsAfter = true;
    long long entityId = 0;
    std::string message;
    std::string bridgeBody;
    std::string commandText;
};

VehicleDestroyOutcome DestroyVehicleVerified(
    const std::wstring& baseDir,
    const std::string& vehicleRef,
    const std::string& steamId,
    const std::string& name,
    const std::string& reason) {
    VehicleDestroyOutcome outcome;
    const auto ref = TrimAscii(vehicleRef);
    if (ref.rfind("runtime:", 0) == 0) {
        auto br = BridgeExecute(baseDir, "destroy_vehicle_ref",
            std::string("{\"vehicleRef\":\"") + JsonEscape(ref) +
            "\",\"steamId\":\"" + JsonEscape(steamId) +
            "\",\"name\":\"" + JsonEscape(name) +
            "\",\"reason\":\"" + JsonEscape(reason) + "\"}",
            6000);
        outcome.ok = br.ok;
        outcome.bridgeOk = br.ok;
        outcome.bridgeBody = br.body;
        outcome.message = br.ok ? "vehicle destroy runtime-verified" : br.message;
        outcome.commandText = "destroy_vehicle_ref";
        outcome.existsAfter = !br.ok;
        AppendActionRecord(baseDir, "vehicle_destroy", outcome.ok, steamId, name, outcome.message,
            std::string("{\"vehicleRef\":\"") + JsonEscape(ref) +
            "\",\"reason\":\"" + JsonEscape(reason) +
            "\",\"route\":\"runtime\",\"bridgeOk\":" + (br.ok ? "true" : "false") + "}");
        return outcome;
    }
    if (ref.empty() || !IsDigitsText(ref)) {
        outcome.message = "vehicle destroy requires numeric entityId";
        AppendActionRecord(baseDir, "vehicle_destroy", false, steamId, name, outcome.message,
            std::string("{\"vehicleRef\":\"") + JsonEscape(ref) + "\",\"reason\":\"" + JsonEscape(reason) + "\"}");
        return outcome;
    }
    outcome.entityId = _strtoi64(ref.c_str(), nullptr, 10);
    outcome.commandText = "DestroyVehicle " + std::to_string(outcome.entityId);

    std::string existsError;
    if (!TryReadVehicleEntityExists(baseDir, outcome.entityId, outcome.existedBefore, existsError)) {
        outcome.message = existsError.empty() ? "vehicle readback failed before destroy" : existsError;
        AppendActionRecord(baseDir, "vehicle_destroy", false, steamId, name, outcome.message,
            std::string("{\"vehicleRef\":\"") + JsonEscape(ref) + "\",\"reason\":\"" + JsonEscape(reason) + "\"}");
        return outcome;
    }
    if (!outcome.existedBefore) {
        outcome.ok = true;
        outcome.existsAfter = false;
        outcome.message = "vehicle already absent from db";
        AppendActionRecord(baseDir, "vehicle_destroy", true, steamId, name, outcome.message,
            std::string("{\"vehicleRef\":\"") + JsonEscape(ref) + "\",\"reason\":\"" + JsonEscape(reason) + "\",\"alreadyAbsent\":true}");
        return outcome;
    }

    bool commandAccepted = false;
    std::string commandTransport;
    std::string commandMessage;
    std::string commandBody;
    if (LocalRconEnabled(baseDir)) {
        const auto rcon = LocalRconSendConsoleCommand(baseDir, outcome.commandText);
        commandAccepted = rcon.ok;
        commandTransport = "server-command";
        commandMessage = rcon.message;
        commandBody = std::string("{\"command\":\"") + JsonEscape(LocalRconCommandText(outcome.commandText)) +
            "\",\"commandBody\":" + JsonValueOrString(rcon.body) + "}";
    }
    if (!commandAccepted && ArkPanelEnabled(baseDir)) {
        const auto arkCommand = std::string("#") + outcome.commandText;
        const auto rcon = ArkPanelSendConsoleCommand(baseDir, arkCommand);
        commandAccepted = rcon.ok;
        commandTransport = "hosting-panel-command";
        commandMessage = rcon.message;
        commandBody = std::string("{\"command\":\"") + JsonEscape(arkCommand) +
            "\",\"status\":" + std::to_string(rcon.status) +
            ",\"panelBody\":" + JsonValueOrString(rcon.body) + "}";
    }
    if (!commandAccepted) {
        auto br = BridgeExecute(baseDir, "admin_exec",
            std::string("{\"commandText\":\"") + JsonEscape(outcome.commandText) +
            "\",\"steamId\":\"" + JsonEscape(steamId) +
            "\",\"name\":\"" + JsonEscape(name) + "\"}",
            12000);
        commandAccepted = br.ok;
        commandTransport = "ue4ss-bridge";
        commandMessage = br.message;
        commandBody = br.body;
    }
    outcome.bridgeOk = commandAccepted;
    outcome.bridgeBody = commandBody;

    std::string waitError;
    if (WaitForVehicleEntityGone(baseDir, outcome.entityId, waitError)) {
        outcome.ok = true;
        outcome.existsAfter = false;
        outcome.message = "vehicle destroy db-verified";
    } else {
        bool exists = true;
        std::string finalError;
        TryReadVehicleEntityExists(baseDir, outcome.entityId, exists, finalError);
        outcome.existsAfter = exists;
        outcome.message = "vehicle destroy could not be verified";
        if (!waitError.empty()) outcome.message += ": " + waitError;
        else if (!finalError.empty()) outcome.message += ": " + finalError;
        else if (!commandAccepted) outcome.message += ": " + commandMessage;
    }

    AppendActionRecord(baseDir, "vehicle_destroy", outcome.ok, steamId, name, outcome.message,
        std::string("{\"vehicleRef\":\"") + JsonEscape(ref) +
        "\",\"reason\":\"" + JsonEscape(reason) +
        "\",\"command\":\"" + JsonEscape(outcome.commandText) +
        "\",\"transport\":\"" + JsonEscape(commandTransport) +
        "\",\"bridgeOk\":" + (commandAccepted ? "true" : "false") +
        ",\"existedBefore\":" + (outcome.existedBefore ? "true" : "false") +
        ",\"existsAfter\":" + (outcome.existsAfter ? "true" : "false") + "}");
    return outcome;
}

std::string RentalRecordKey(const std::string& object) {
    const auto steam = FlatJsonStringValue(object, "steamId");
    if (!steam.empty()) return "steam:" + steam;
    const auto name = ToLowerAscii(TrimAscii(FlatJsonStringValue(object, "name")));
    if (!name.empty()) return "name:" + name;
    return {};
}

std::string RentalStatus(const std::string& object) {
    auto status = ToLowerAscii(TrimAscii(FlatJsonStringValue(object, "status")));
    if (!status.empty()) return status;
    if (ToLowerAscii(FlatJsonStringValue(object, "returned")) == "true") return "returned";
    return "active";
}

bool RentalIsClosed(const std::string& object) {
    const auto status = RentalStatus(object);
    return status == "returned" || status == "expired" || status == "expired_untracked" ||
        status == "sold_or_missing" || status == "missing_vehicle" ||
        status == "cancelled" || status == "failed";
}

std::string RentalDestroyRef(const std::string& object) {
    const auto numeric = JsonLongLongField(object, "entityId", JsonLongLongField(object, "vehicleEntityId", 0));
    auto ref = numeric > 0 ? std::to_string(numeric) : std::string{};
    if (ref.empty()) ref = FlatJsonStringValue(object, "destroyRef");
    if (ref.empty()) {
        const auto destroyNumeric = JsonLongLongField(object, "destroyRef", 0);
        if (destroyNumeric > 0) ref = std::to_string(destroyNumeric);
    }
    ref = TrimAscii(ref);
    return ref == "0" ? "" : ref;
}

long long VehicleRentalDestroyRetryDelaySeconds(int attempts) {
    const int n = std::max(1, attempts);
    long long delay = 60;
    for (int i = 2; i <= std::min(n, 6); ++i) delay *= 2;
    return std::min<long long>(delay, 3600);
}

std::string VehicleRentalCleanupJson(const std::wstring& baseDir) {
    const auto objects = ExtractArrayObjects(ModuleStateJson(baseDir, "vehicle-rentals"));
    std::map<std::string, std::string> latest;
    for (const auto& object : objects) {
        const auto key = RentalRecordKey(object);
        if (!key.empty()) latest[key] = object;
    }

    const auto now = static_cast<long long>(std::time(nullptr));
    const auto config = PluginConfigJson(baseDir, "vehicle-rental", DefaultVehicleRentalConfigJson());
    const int maxDestroyAttempts = std::clamp(
        JsonIntField(config, "ExpireDestroyMaxAttempts",
            JsonIntField(config, "expireDestroyMaxAttempts",
                JsonIntField(config, "DestroyMaxAttempts",
                    JsonIntField(config, "destroyMaxAttempts", 6)))),
        1, 20);
    std::ostringstream report;
    report << "{\"checked\":" << latest.size() << ",\"expired\":";
    int expired = 0;
    std::ostringstream records;
    records << "[";
    bool first = true;
    for (const auto& pair : latest) {
        const auto& object = pair.second;
        if (RentalIsClosed(object)) continue;
        const auto expiresAt = JsonLongLongField(object, "expiresAt", 0);
        if (expiresAt <= 0 || expiresAt > now) continue;
        const auto existingNextRetryAt = JsonLongLongField(object, "nextRetryAt", 0);
        if (existingNextRetryAt > now) continue;
        ++expired;
        const auto steamId = FlatJsonStringValue(object, "steamId");
        const auto name = FlatJsonStringValue(object, "name");
        const auto alias = FlatJsonStringValue(object, "alias");
        const auto vehicleId = FlatJsonStringValue(object, "vehicleId");
        const auto displayName = FlatJsonStringValue(object, "displayName").empty()
            ? vehicleId
            : FlatJsonStringValue(object, "displayName");
        const auto destroyRef = RentalDestroyRef(object);
        bool destroyAttempted = !destroyRef.empty();
        bool destroyOk = false;
        std::string destroyMessage = destroyAttempted ? "" : "missing vehicle entity id";
        int destroyAttempts = std::max(0, JsonIntField(object, "destroyAttempts", 0));
        if (destroyAttempted) {
            ++destroyAttempts;
            auto destroy = DestroyVehicleVerified(baseDir, destroyRef, steamId, name, "rental-cleanup");
            destroyOk = destroy.ok;
            destroyMessage = destroy.message;
        } else {
            ++destroyAttempts;
            AppendActionRecord(baseDir, "vehicle_destroy", false, steamId, name, destroyMessage,
                std::string("{\"reason\":\"rental-cleanup\",\"displayName\":\"") + JsonEscape(displayName) + "\"}");
        }
        const bool giveUpUntracked = !destroyOk && destroyAttempts >= maxDestroyAttempts;
        const std::string status = destroyOk ? "expired" : (giveUpUntracked ? "expired_untracked" : "expire_pending");
        const long long nextRetryAt = status == "expire_pending"
            ? now + VehicleRentalDestroyRetryDelaySeconds(destroyAttempts)
            : 0;

        AppendModuleStateRecord(baseDir, "vehicle-rentals",
            std::string("{\"utc\":\"") + UtcIsoNow() +
            "\",\"steamId\":\"" + JsonEscape(steamId) +
            "\",\"name\":\"" + JsonEscape(name) +
            "\",\"alias\":\"" + JsonEscape(alias) +
            "\",\"status\":\"" + status + "\",\"expiresAt\":" + std::to_string(expiresAt) +
            ",\"displayName\":\"" + JsonEscape(displayName) +
            "\",\"vehicleId\":\"" + JsonEscape(vehicleId) +
            "\",\"minutes\":" + std::to_string(JsonLongLongField(object, "minutes", 0)) +
            ",\"startedAt\":" + std::to_string(JsonLongLongField(object, "startedAt", 0)) +
            ",\"initialCharge\":" + std::to_string(JsonLongLongField(object, "initialCharge", 0)) +
            ",\"pricePer10Minutes\":" + std::to_string(JsonLongLongField(object, "pricePer10Minutes", 0)) +
            ",\"totalCharge\":" + std::to_string(JsonLongLongField(object, "totalCharge", 0)) +
            ",\"destroyRef\":\"" + JsonEscape(destroyRef) +
            "\",\"destroyAttempted\":" + (destroyAttempted ? "true" : "false") +
            ",\"destroyOk\":" + (destroyOk ? "true" : "false") +
            ",\"destroyAttempts\":" + std::to_string(destroyAttempts) +
            ",\"nextRetryAt\":" + std::to_string(nextRetryAt) +
            ",\"destroyMessage\":\"" + JsonEscape(destroyMessage) + "\"}");

        if (!first) records << ",";
        first = false;
        records << "{\"steamId\":\"" << JsonEscape(steamId)
                << "\",\"name\":\"" << JsonEscape(name)
                << "\",\"displayName\":\"" << JsonEscape(displayName)
                << "\",\"status\":\"" << JsonEscape(status)
                << "\",\"expiresAt\":" << expiresAt
                << ",\"destroyRef\":\"" << JsonEscape(destroyRef)
                << "\",\"destroyAttempted\":" << (destroyAttempted ? "true" : "false")
                << ",\"destroyOk\":" << (destroyOk ? "true" : "false")
                << ",\"destroyAttempts\":" << destroyAttempts
                << ",\"nextRetryAt\":" << nextRetryAt
                << ",\"message\":\"" << JsonEscape(destroyMessage) << "\"}";
    }
    records << "]";
    report << expired << ",\"records\":" << records.str() << "}";
    return report.str();
}

bool StateRecordMatchesIdentity(const std::string& object, const std::string& steamId, const std::string& name) {
    const auto recordSteam = FlatJsonStringValue(object, "steamId");
    const auto recordName = FlatJsonStringValue(object, "name");
    const auto cleanSteam = TrimAscii(steamId);
    const auto cleanName = ToLowerAscii(TrimAscii(name));
    if (!cleanSteam.empty() && !recordSteam.empty() && recordSteam == cleanSteam) return true;
    if (!cleanName.empty() && !recordName.empty() && ToLowerAscii(recordName) == cleanName) return true;
    return false;
}

long long LatestStateEpochForIdentity(
    const std::wstring& baseDir,
    const std::string& key,
    const std::string& steamId,
    const std::string& name) {
    long long latest = 0;
    for (const auto& object : ExtractArrayObjects(ModuleStateJson(baseDir, key))) {
        if (!StateRecordMatchesIdentity(object, steamId, name)) continue;
        latest = std::max(latest, JsonLongLongField(object, "at", 0));
    }
    return latest;
}

int CooldownRemainingSeconds(
    const std::wstring& baseDir,
    const std::string& key,
    const std::string& steamId,
    const std::string& name,
    int cooldownSeconds) {
    if (cooldownSeconds <= 0) return 0;
    const auto last = LatestStateEpochForIdentity(baseDir, key, steamId, name);
    if (last <= 0) return 0;
    const auto now = static_cast<long long>(std::time(nullptr));
    const auto remaining = (last + cooldownSeconds) - now;
    return remaining > 0 ? static_cast<int>(std::min<long long>(remaining, INT_MAX)) : 0;
}

bool RemoveStateRecordsForIdentity(
    const std::wstring& baseDir,
    const std::string& key,
    const std::string& steamId,
    const std::string& name) {
    if (TrimAscii(steamId).empty() && TrimAscii(name).empty()) {
        return WriteTextFile(ModuleStatePath(baseDir, key), "[]");
    }
    const auto objects = ExtractArrayObjects(ModuleStateJson(baseDir, key));
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto& object : objects) {
        if (StateRecordMatchesIdentity(object, steamId, name)) continue;
        if (!first) out << ",";
        first = false;
        out << object;
    }
    out << "]";
    return WriteTextFile(ModuleStatePath(baseDir, key), out.str());
}

std::string ChatStateWithLogIdentityJson(const std::wstring& baseDir, int limit) {
    const int safeLimit = std::max(1, limit);
    const auto state = ModuleStateJson(baseDir, "chat");
    if (!JsonArrayHasItems(state)) {
        return EventsFromLogJson(baseDir, "chat", { "LogChat", "Chat:", "GlobalChat", "LocalChat", "SquadChat", "AdminChat", " sent message " });
    }

    const auto objects = ExtractArrayObjects(state);
    if (objects.empty()) return state;

    std::string uniqueTargetSteamId;
    std::string uniqueTargetName;
    bool hasMultipleTargets = false;
    for (const auto& object : objects) {
        const auto targetSteamId = FlatJsonStringValue(object, "targetSteamId");
        const auto targetName = FlatJsonStringValue(object, "targetName");
        if (!targetSteamId.empty()) {
            if (uniqueTargetSteamId.empty()) uniqueTargetSteamId = targetSteamId;
            else if (uniqueTargetSteamId != targetSteamId) hasMultipleTargets = true;
        }
        if (!targetName.empty()) {
            if (uniqueTargetName.empty()) uniqueTargetName = targetName;
            else if (uniqueTargetName != targetName) hasMultipleTargets = true;
        }
    }
    if (hasMultipleTargets) {
        uniqueTargetSteamId.clear();
        uniqueTargetName.clear();
    }

    std::ostringstream ss;
    ss << "[";
    const size_t first = objects.size() > static_cast<size_t>(safeLimit)
        ? objects.size() - static_cast<size_t>(safeLimit)
        : 0;
    for (size_t i = first; i < objects.size(); ++i) {
        if (i > first) ss << ",";
        const auto& object = objects[i];
        auto name = FlatJsonStringValue(object, "name");
        auto steamId = FlatJsonStringValue(object, "steamId");
        const auto source = FlatJsonStringValue(object, "source");
        const auto sourceLower = ToLowerAscii(source);
        const bool fromPanel = sourceLower == "panel";
        bool identityFromLog = false;
        bool identityFromStateContext = false;

        if (fromPanel) {
            if (name.empty() || IdentityTextLooksUnsafe(name)) name = "Panel";
            if (IdentityTextLooksUnsafe(steamId)) steamId.clear();
        } else {
            const auto identity = ResolvePlayerIdentity(baseDir, steamId, name);
            const auto nameLower = ToLowerAscii(TrimAscii(name));
            const bool commandReplyToUnknown =
                sourceLower == "command-reply" &&
                (nameLower == "nedjin" ||
                 nameLower == "nedjin -> unknown" ||
                 nameLower.find("nedjin -> uscriptstruct:") != std::string::npos ||
                 nameLower.find("nedjin -> function:") != std::string::npos);
            if (commandReplyToUnknown && !identity.name.empty()) {
                name = std::string("ScumNeDjin -> ") + identity.name;
                identityFromLog = identity.fromLog;
            }
            if ((name.empty() || IdentityTextLooksUnsafe(name)) && !identity.name.empty()) {
                name = identity.name;
                identityFromLog = identity.fromLog;
            }
            if ((steamId.empty() || IdentityTextLooksUnsafe(steamId)) && !identity.steamId.empty()) {
                steamId = identity.steamId;
                identityFromLog = identity.fromLog;
            }
            if ((name.empty() || IdentityTextLooksUnsafe(name)) && !uniqueTargetName.empty()) {
                name = uniqueTargetName;
                identityFromStateContext = true;
            }
            if ((steamId.empty() || IdentityTextLooksUnsafe(steamId)) && !uniqueTargetSteamId.empty()) {
                steamId = uniqueTargetSteamId;
                identityFromStateContext = true;
            }
        }

        const auto type = FlatJsonStringValue(object, "type").empty() ? "chat" : FlatJsonStringValue(object, "type");
        ss << "{\"type\":\"" << JsonEscape(type) << "\""
           << ",\"timestampUtc\":\"" << JsonEscape(FlatJsonStringValue(object, "timestampUtc")) << "\""
           << ",\"message\":\"" << JsonEscape(FlatJsonStringValue(object, "message")) << "\""
           << ",\"name\":\"" << JsonEscape(name) << "\""
           << ",\"steamId\":\"" << JsonEscape(steamId) << "\""
           << ",\"channel\":\"" << JsonEscape(FlatJsonStringValue(object, "channel")) << "\""
           << ",\"source\":\"" << JsonEscape(source) << "\"";
        const auto targetSteamId = FlatJsonStringValue(object, "targetSteamId");
        const auto targetName = FlatJsonStringValue(object, "targetName");
        if (!targetSteamId.empty()) ss << ",\"targetSteamId\":\"" << JsonEscape(targetSteamId) << "\"";
        if (!targetName.empty()) ss << ",\"targetName\":\"" << JsonEscape(targetName) << "\"";
        if (identityFromLog) ss << ",\"identitySource\":\"server-log\"";
        else if (identityFromStateContext) ss << ",\"identitySource\":\"state-context\"";
        if (object.find("\"ok\":true") != std::string::npos) ss << ",\"ok\":true";
        else if (object.find("\"ok\":false") != std::string::npos) ss << ",\"ok\":false";
        ss << "}";
    }
    ss << "]";
    return ss.str();
}

std::string BridgePlayerDetailsWithLogIdentityJson(
    const std::wstring& baseDir,
    const std::string& bridgeBody,
    const std::string& requestedSteamId,
    const std::string& requestedName) {
    auto playerObject = JsonRawField(bridgeBody, "player");
    if (playerObject.empty()) playerObject = bridgeBody;

    const auto rawName = FlatJsonStringValue(playerObject, "name");
    const auto rawSteamId = FlatJsonStringValue(playerObject, "steamId");
    const auto identity = ResolvePlayerIdentity(baseDir, requestedSteamId, requestedName);
    const bool rawUnsafe = IdentityTextLooksUnsafe(rawName) || IdentityTextLooksUnsafe(rawSteamId);
    const bool hasOverlay = identity.fromLog || !identity.name.empty() || !identity.steamId.empty();
    if (!rawUnsafe && !identity.fromLog) return {};
    if (!hasOverlay && playerObject.empty()) return {};

    const auto resolvedName = !identity.name.empty() ? identity.name : rawName;
    const auto resolvedSteamId = !identity.steamId.empty() ? identity.steamId : rawSteamId;
    std::ostringstream ss;
    ss << "{\"ok\":true,\"source\":\"ue4ss+server-log\",\"command\":\"player_details\""
       << ",\"message\":\"player resolved with server-log identity overlay\""
       << ",\"data\":{\"player\":{\"name\":\"" << JsonEscape(resolvedName)
       << "\",\"steamId\":\"" << JsonEscape(resolvedSteamId)
       << "\",\"online\":true,\"status\":\"online\""
       << ",\"source\":\"ue4ss+server-log\""
       << ",\"identitySource\":\"" << (identity.fromLog ? "server-log" : "request") << "\""
       << ",\"positionSource\":\"ue4ss\""
       << ",\"x\":" << FlatJsonNumberValue(playerObject, "x")
       << ",\"y\":" << FlatJsonNumberValue(playerObject, "y")
       << ",\"z\":" << FlatJsonNumberValue(playerObject, "z");
    const auto runtimeKey = FlatJsonStringValue(playerObject, "runtimeKey");
    if (!runtimeKey.empty()) ss << ",\"runtimeKey\":\"" << JsonEscape(runtimeKey) << "\"";
    if (!rawName.empty() && rawName != resolvedName) ss << ",\"runtimeName\":\"" << JsonEscape(rawName) << "\"";
    if (!rawSteamId.empty() && rawSteamId != resolvedSteamId) ss << ",\"runtimeSteamId\":\"" << JsonEscape(rawSteamId) << "\"";
    if (!identity.joinedUtc.empty()) ss << ",\"joinedUtc\":\"" << JsonEscape(identity.joinedUtc) << "\"";
    if (!identity.lastSeenUtc.empty()) ss << ",\"lastSeenUtc\":\"" << JsonEscape(identity.lastSeenUtc) << "\"";
    ss << "}}}";
    return ss.str();
}

std::string BridgePlayerCommand(
    const std::wstring& baseDir,
    const std::string& command,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    const std::string& extraArgs,
    int timeoutMs = 5000,
    bool requireLocation = false,
    bool requireSteam = false) {
    const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
    auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
    auto targetName = identity.name.empty() ? name : identity.name;
    auto targetRuntimeKey = identity.runtimeKey.empty() ? runtimeKey : identity.runtimeKey;
    const auto liveTarget = ValidatePanelLiveTargetForCommandChannel(
        baseDir, targetSteamId, targetName, targetRuntimeKey, command + "-native-route", requireLocation, requireSteam, requireLocation ? 8000 : 6000);
    if (!liveTarget.ok) {
        return Envelope(false, liveTarget.message);
    }
    if (!liveTarget.steamId.empty()) targetSteamId = liveTarget.steamId;
    if (!liveTarget.name.empty()) targetName = liveTarget.name;
    if (!liveTarget.runtimeKey.empty()) targetRuntimeKey = liveTarget.runtimeKey;
    auto args = std::string("{") + PlayerTargetArgs(targetSteamId, targetName, targetRuntimeKey);
    if (!extraArgs.empty()) args += "," + extraArgs;
    args += "}";
    auto br = BridgeExecute(baseDir, command, args, timeoutMs);
    return br.ok ? Envelope(true, br.body) : Envelope(false, br.message);
}

std::string TargetChatEnvelope(
    const std::wstring& baseDir,
    const std::string& moduleKey,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    const std::string& message,
    const std::string& channel,
int timeoutMs = 5000) {
    const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
    const auto fallbackSteamId = IdentityTextLooksUnsafe(steamId) ? std::string{} : steamId;
    const auto fallbackName = IdentityTextLooksUnsafe(name) ? std::string{} : name;
    auto targetSteamId = identity.steamId.empty() ? fallbackSteamId : identity.steamId;
    auto targetName = identity.name.empty() ? fallbackName : identity.name;
    auto targetRuntimeKey = identity.runtimeKey.empty() ? runtimeKey : identity.runtimeKey;

    if (targetSteamId.empty() && targetName.empty() && targetRuntimeKey.empty()) {
        return Envelope(false, "Укажи игрока для адресного сообщения.");
    }

    const auto liveTarget = ValidatePanelLiveTargetForCommandChannel(
        baseDir, targetSteamId, targetName, targetRuntimeKey, "chat-target-native-route", false, false, 6000);
    if (!liveTarget.ok) {
        AppendModuleStateRecord(baseDir, moduleKey,
            std::string("{\"type\":\"chat\",\"timestampUtc\":\"") + UtcIsoNow() +
            "\",\"message\":\"" + JsonEscape(message) +
            "\",\"channel\":\"" + JsonEscape(channel.empty() ? "server" : channel) +
            "\",\"source\":\"panel\",\"targetSteamId\":\"" + JsonEscape(targetSteamId) +
            "\",\"targetName\":\"" + JsonEscape(targetName) +
            "\",\"targetRuntimeKey\":\"" + JsonEscape(targetRuntimeKey) +
            "\",\"transport\":\"ue4ss-chat-target\",\"ok\":false,\"preflight\":false,\"error\":\"" +
            JsonEscape(liveTarget.message) + "\"}");
        return Envelope(false, liveTarget.message);
    }
    if (!liveTarget.steamId.empty()) targetSteamId = liveTarget.steamId;
    if (!liveTarget.name.empty()) targetName = liveTarget.name;
    if (!liveTarget.runtimeKey.empty()) targetRuntimeKey = liveTarget.runtimeKey;

    auto args = std::string("{\"targetSteamId\":\"") + JsonEscape(targetSteamId) +
        "\",\"targetName\":\"" + JsonEscape(targetName) +
        "\",\"message\":\"" + JsonEscape(message) + "\"";
    if (!targetRuntimeKey.empty()) {
        args += ",\"targetRuntimeKey\":\"" + JsonEscape(targetRuntimeKey) +
            "\",\"runtimeKey\":\"" + JsonEscape(targetRuntimeKey) + "\"";
    }
    args += "}";

    const auto br = BridgeExecute(baseDir, "chat_target", args, timeoutMs);
    AppendModuleStateRecord(baseDir, moduleKey,
        std::string("{\"type\":\"chat\",\"timestampUtc\":\"") + UtcIsoNow() +
        "\",\"message\":\"" + JsonEscape(message) +
        "\",\"channel\":\"" + JsonEscape(channel.empty() ? "server" : channel) +
        "\",\"source\":\"panel\",\"targetSteamId\":\"" + JsonEscape(targetSteamId) +
        "\",\"targetName\":\"" + JsonEscape(targetName) +
        "\",\"targetRuntimeKey\":\"" + JsonEscape(targetRuntimeKey) +
        "\",\"transport\":\"ue4ss-chat-target\"" +
        ",\"ok\":" + (br.ok ? "true" : "false") + "}");

    return br.ok
        ? Envelope(true, std::string("{\"transport\":\"ue4ss-chat-target\",\"targetSteamId\":\"") +
            JsonEscape(targetSteamId) + "\",\"targetName\":\"" + JsonEscape(targetName) +
            "\",\"targetRuntimeKey\":\"" + JsonEscape(targetRuntimeKey) +
            "\",\"bridge\":" + JsonValueOrString(br.body) + "}")
        : Envelope(false, br.message.empty() ? "UE4SS-мост не смог отправить адресное сообщение игроку." : br.message);
}

std::string BridgeBodyOrError(
    const std::wstring& baseDir,
    const std::string& command,
    const std::string& args,
    int timeoutMs = 5000) {
    auto br = BridgeExecute(baseDir, command, args, timeoutMs);
    return br.ok ? Envelope(true, br.body) : Envelope(false, br.message);
}

std::string CleanEntityIdsText(std::string text) {
    std::string out;
    bool lastWasSpace = true;
    for (const auto ch : text) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isdigit(uch)) {
            out.push_back(ch);
            lastWasSpace = false;
        } else if (!lastWasSpace) {
            out.push_back(' ');
            lastWasSpace = true;
        }
    }
    return TrimAscii(out);
}

std::string DestroyInventoryEntitiesEnvelope(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    const std::string& entitiesText) {
    const auto cleanIds = CleanEntityIdsText(entitiesText);
    if (cleanIds.empty()) return Envelope(false, "EntityID list is empty.");
    return BridgePlayerCommand(baseDir, "admin_exec", steamId, name, runtimeKey,
        std::string("\"commandText\":\"ScumNeDjinDestroyEntities ") + JsonEscape(cleanIds) + "\"", 25000);
}

std::string DeliverItemsSequentialJson(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    const std::vector<GrantItem>& items,
    const std::string& source) {
    if (items.empty()) return Envelope(false, "Для выдачи не настроены предметы.");
    const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
    const auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
    const auto targetName = identity.name.empty() ? name : identity.name;
    const auto targetRuntimeKey = identity.runtimeKey.empty() ? runtimeKey : identity.runtimeKey;

    std::ostringstream results;
    results << "[";
    int okCount = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        auto args = std::string("{") + PlayerTargetArgs(targetSteamId, targetName, targetRuntimeKey) +
            ",\"itemId\":\"" + JsonEscape(item.itemId) +
            "\",\"quantity\":" + std::to_string(item.quantity) + "}";
        auto br = BridgeExecute(baseDir, "deliver_items", args, 45000);
        if (i) results << ",";
        results << "{\"itemId\":\"" << JsonEscape(item.itemId)
                << "\",\"quantity\":" << item.quantity
                << ",\"ok\":" << (br.ok ? "true" : "false")
                << ",\"message\":\"" << JsonEscape(br.message) << "\"";
        if (!br.body.empty()) results << ",\"bridge\":" << br.body;
        results << "}";
        if (br.ok) ++okCount;
    }
    results << "]";

    const bool allOk = okCount == static_cast<int>(items.size());
    std::ostringstream payload;
    payload << "{\"source\":\"" << JsonEscape(source)
            << "\",\"target\":{\"steamId\":\"" << JsonEscape(targetSteamId)
            << "\",\"name\":\"" << JsonEscape(targetName)
            << "\",\"runtimeKey\":\"" << JsonEscape(targetRuntimeKey)
            << "\"},\"requested\":" << items.size()
            << ",\"delivered\":" << okCount
            << ",\"allOk\":" << (allOk ? "true" : "false")
            << ",\"results\":" << results.str() << "}";
    return Envelope(true, payload.str());
}

struct LivePlayerPosition {
    bool ok = false;
    std::string x;
    std::string y;
    std::string z;
};

LivePlayerPosition ReadLivePlayerPositionForCommand(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey) {
    LivePlayerPosition pos;
    auto args = std::string("{") + PlayerTargetArgs(steamId, name, runtimeKey) + "}";
    const auto br = BridgeExecute(baseDir, "player_details", args, 3000);
    if (!br.ok || br.body.empty()) return pos;

    auto player = JsonRawField(br.body, "player");
    if (player.empty()) player = JsonRawField(br.body, "data");
    if (player.empty()) player = br.body;

    pos.x = JsonNumberTextField(player, "x", JsonNumberTextField(player, "X", ""));
    pos.y = JsonNumberTextField(player, "y", JsonNumberTextField(player, "Y", ""));
    pos.z = JsonNumberTextField(player, "z", JsonNumberTextField(player, "Z", ""));
    pos.ok = !pos.x.empty() && !pos.y.empty() && !pos.z.empty();
    return pos;
}

std::string ScumRconLocationRef(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey) {
    const auto pos = ReadLivePlayerPositionForCommand(baseDir, steamId, name, runtimeKey);
    if (pos.ok) return pos.x + " " + pos.y + " " + pos.z;
    const auto steam = TrimAscii(steamId);
    return IsSteam64Text(steam) ? steam : std::string{};
}

struct NativeBaseElementCatalogEntry {
    bool ok = false;
    std::string actorClass;
    std::string path;
    std::string packagePath;
    std::string parent;
    std::string category;
    std::string route;
    std::string note;
    std::string error;
};

bool NativeBaseElementParentAllowed(const std::string& parent) {
    const auto clean = ToLowerAscii(parent);
    return clean == "basebuildingcomponent" || clean == "modularbasebuildingcomponent";
}

std::string NormalizeBaseElementLookup(std::string value) {
    value = TrimAscii(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool BaseElementCatalogObjectMatches(const std::string& object, const std::string& lookup) {
    const auto actorClass = ToLowerAscii(JsonStringField(object, "actorClass"));
    const auto name = ToLowerAscii(JsonStringField(object, "name"));
    const auto path = ToLowerAscii(JsonStringField(object, "path"));
    const auto packagePath = ToLowerAscii(JsonStringField(object, "packagePath"));
    const auto raw = ToLowerAscii(NormalizeBaseElementLookup(lookup));
    if (raw.empty()) return false;

    std::vector<std::string> candidates{ raw };
    if (raw.find('/') == std::string::npos && raw.size() > 2 && raw.substr(raw.size() - 2) != "_c") {
        candidates.push_back(raw + "_c");
    }

    for (const auto& candidate : candidates) {
        if (candidate == actorClass || candidate == name || candidate == path || candidate == packagePath) return true;
        if (!path.empty() && path.size() > candidate.size() && path.rfind("." + candidate) == path.size() - candidate.size() - 1) return true;
    }
    return false;
}

NativeBaseElementCatalogEntry ResolveNativeBaseElementCatalogEntry(const std::wstring& baseDir, const std::string& spec) {
    NativeBaseElementCatalogEntry result;
    const auto lookup = NormalizeBaseElementLookup(spec.empty() ? "BP_Base_Cabin_C" : spec);
    const auto catalogPath = JoinPath(JoinPath(JoinPath(baseDir, L"ScumNeDjin"), L"web"), L"actor-catalog.json");
    const auto catalog = TrimAscii(ReadTextFile(catalogPath));
    if (catalog.empty()) {
        result.error = "actor-catalog.json не найден; нельзя безопасно валидировать base-element.";
        return result;
    }

    const auto actorsJson = JsonRawField(catalog, "actors");
    const auto objects = ExtractArrayObjects(actorsJson.empty() ? catalog : actorsJson);
    for (const auto& object : objects) {
        if (!BaseElementCatalogObjectMatches(object, lookup)) continue;

        result.actorClass = JsonStringField(object, "actorClass");
        result.path = JsonStringField(object, "path");
        result.packagePath = JsonStringField(object, "packagePath");
        result.parent = JsonStringField(object, "nativeParentName");
        result.category = JsonStringField(object, "category");
        result.route = JsonStringField(object, "summonRoute");
        result.note = JsonStringField(object, "summonNote");

        const auto category = ToLowerAscii(result.category);
        if (category != "basebuilding" || !NativeBaseElementParentAllowed(result.parent)) {
            result.error = "Каталог нашёл объект, но это не готовый base-building element: category=" +
                result.category + ", parent=" + result.parent + ".";
            return result;
        }
        if (result.path.empty()) {
            result.error = "У base-building element нет полного asset path в actor-catalog.";
            return result;
        }

        result.ok = true;
        return result;
    }

    result.error = "Base-building element не найден в actor-catalog: " + lookup;
    return result;
}

long long ParseInt64Text(const std::string& value, long long fallback = 0) {
    const auto clean = TrimAscii(value);
    if (clean.empty()) return fallback;
    char* end{};
    const auto parsed = _strtoi64(clean.c_str(), &end, 10);
    return end && *end == '\0' ? parsed : fallback;
}

double ParseDoubleText(const std::string& value, double fallback = 0.0) {
    const auto clean = TrimAscii(value);
    if (clean.empty()) return fallback;
    char* end{};
    const auto parsed = std::strtod(clean.c_str(), &end);
    return end && *end == '\0' && std::isfinite(parsed) ? parsed : fallback;
}

std::string SqlNumber(double value) {
    if (!std::isfinite(value)) value = 0.0;
    std::ostringstream ss;
    ss.precision(17);
    ss << value;
    return ss.str();
}

std::string SqlIntegerOrNull(long long value) {
    return value > 0 ? std::to_string(value) : "NULL";
}

long long ReadDbInt64Value(const std::wstring& baseDir, const std::string& sql, const std::string& key, long long fallback) {
    std::string rows;
    std::string error;
    if (!SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), sql, 1, rows, error)) return fallback;
    const auto objects = ExtractArrayObjects(rows);
    if (objects.empty()) return fallback;
    return ParseInt64Text(FlatJsonNumberValue(objects.front(), key), fallback);
}

std::string FlatJsonStringOrNumber(const std::string& object, const std::string& key) {
    auto value = FlatJsonStringValue(object, key);
    if (value.empty()) value = FlatJsonNumberValue(object, key);
    return value;
}

std::string BaseLootAccessDataJson(
    long long baseId,
    long long profileId,
    const std::string& steamId,
    long long ownerProfileId,
    const std::string& ownerSquadId,
    const std::string& playerSquadId,
    bool allowed,
    bool dbAvailable,
    const std::string& reason,
    const std::string& message) {
    std::ostringstream data;
    data << "{\"source\":\"native-dll-sqlite\""
         << ",\"dbAvailable\":" << (dbAvailable ? "true" : "false")
         << ",\"baseId\":" << baseId
         << ",\"profileId\":" << profileId
         << ",\"steamId\":\"" << JsonEscape(steamId) << "\""
         << ",\"ownerProfileId\":" << ownerProfileId
         << ",\"ownerSquadId\":\"" << JsonEscape(ownerSquadId) << "\""
         << ",\"playerSquadId\":\"" << JsonEscape(playerSquadId) << "\""
         << ",\"allowed\":" << (allowed ? "true" : "false")
         << ",\"reason\":\"" << JsonEscape(reason) << "\""
         << ",\"message\":\"" << JsonEscape(message) << "\""
         << "}";
    return data.str();
}

std::string BaseLootAccessJson(
    const std::wstring& baseDir,
    const std::string& body,
    bool& commandOk,
    std::string& commandMessage) {
    commandOk = false;
    commandMessage.clear();

    const long long baseId = JsonLongLongField(body, "baseId", JsonLongLongField(body, "BaseId", 0));
    long long profileId = JsonLongLongField(body, "profileId",
        JsonLongLongField(body, "serverUserProfileId",
            JsonLongLongField(body, "userProfileId", JsonLongLongField(body, "ProfileId", 0))));
    auto steamId = JsonStringField(body, "steamId");
    if (steamId.empty()) steamId = JsonStringField(body, "steam");
    const bool allowOwnerWithoutSquad = JsonBoolField(body, "allowFlagOwnerWithoutSquad",
        JsonBoolField(body, "AllowFlagOwnerWithoutSquad", true));

    if (baseId <= 0) {
        commandMessage = "baseId is missing";
        return BaseLootAccessDataJson(baseId, profileId, steamId, 0, "", "", false, true, "base_id_missing", commandMessage);
    }

    if (profileId <= 0 && IsSteam64Text(steamId)) {
        const auto profileSql =
            std::string("SELECT id AS profileId FROM user_profile WHERE user_id=") +
            SqlTextLiteral(steamId) + " ORDER BY id DESC LIMIT 1";
        std::string rows;
        std::string error;
        if (!SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), profileSql, 1, rows, error)) {
            commandMessage = error.empty() ? "SCUM.db profile lookup failed" : error;
            return BaseLootAccessDataJson(baseId, profileId, steamId, 0, "", "", false, false, "db_error", commandMessage);
        }
        const auto objects = ExtractArrayObjects(rows);
        if (!objects.empty()) {
            profileId = ParseInt64Text(FlatJsonStringOrNumber(objects.front(), "profileId"), 0);
        }
    }

    if (profileId <= 0) {
        commandOk = true;
        commandMessage = "player profile id was not resolved";
        return BaseLootAccessDataJson(baseId, profileId, steamId, 0, "", "", false, true, "profile_not_found", commandMessage);
    }

    const auto ownerProfileExpr =
        std::string("COALESCE(")
        + "NULLIF(b.owner_user_profile_id, -1),"
        + "NULLIF(b.user_profile_id, -1),"
        + "(SELECT e.owner_profile_id FROM base_element_flag f "
        + "JOIN base_element e ON e.element_id=f.element_id "
        + "WHERE e.base_id=b.id AND e.owner_profile_id IS NOT NULL AND e.owner_profile_id > 0 LIMIT 1),"
        + "(SELECT e2.owner_profile_id FROM base_element e2 "
        + "WHERE e2.base_id=b.id AND e2.owner_profile_id IS NOT NULL AND e2.owner_profile_id > 0 "
        + "ORDER BY e2.element_id LIMIT 1))";
    const auto ownerSql =
        std::string("SELECT ") + ownerProfileExpr + " AS ownerProfileId "
        + "FROM base b WHERE b.id=" + std::to_string(baseId) + " LIMIT 1";
    const long long ownerProfileId = ReadDbInt64Value(baseDir, ownerSql, "ownerProfileId", 0);
    if (ownerProfileId <= 0) {
        commandOk = true;
        commandMessage = "base owner row was not found";
        return BaseLootAccessDataJson(baseId, profileId, steamId, 0, "", "", false, true, "base_not_found", commandMessage);
    }

    const long long ownerSquadNumeric = ReadDbInt64Value(baseDir,
        "SELECT squad_id AS ownerSquadId FROM squad_member WHERE user_profile_id=" + std::to_string(ownerProfileId) + " LIMIT 1",
        "ownerSquadId",
        0);
    const long long playerSquadNumeric = ReadDbInt64Value(baseDir,
        "SELECT squad_id AS playerSquadId FROM squad_member WHERE user_profile_id=" + std::to_string(profileId) + " LIMIT 1",
        "playerSquadId",
        0);
    const auto ownerSquadId = ownerSquadNumeric > 0 ? std::to_string(ownerSquadNumeric) : std::string();
    const auto playerSquadId = playerSquadNumeric > 0 ? std::to_string(playerSquadNumeric) : std::string();
    bool allowed = false;
    std::string reason = "not_squad_member";
    if (ownerProfileId > 0 && ownerProfileId == profileId && allowOwnerWithoutSquad) {
        allowed = true;
        reason = "owner";
    } else if (!ownerSquadId.empty() && !playerSquadId.empty() && ownerSquadId == playerSquadId) {
        allowed = true;
        reason = "squad";
    }

    commandOk = true;
    commandMessage = allowed ? "base loot access allowed" : "base loot access denied";
    return BaseLootAccessDataJson(baseId, profileId, steamId, ownerProfileId, ownerSquadId, playerSquadId, allowed, true, reason, commandMessage);
}

std::string BaseLootNearestFlagDataJson(
    double queryX,
    double queryY,
    int radiusCm,
    bool dbAvailable,
    bool found,
    bool withinRadius,
    long long baseId,
    long long elementId,
    long long ownerProfileId,
    double flagX,
    double flagY,
    double flagZ,
    double distanceCm,
    const std::string& reason,
    const std::string& message) {
    std::ostringstream data;
    data << "{\"source\":\"native-dll-sqlite\""
         << ",\"dbAvailable\":" << (dbAvailable ? "true" : "false")
         << ",\"queryX\":" << JsonPreciseDoubleValue(queryX)
         << ",\"queryY\":" << JsonPreciseDoubleValue(queryY)
         << ",\"radiusCm\":" << radiusCm
         << ",\"found\":" << (found ? "true" : "false")
         << ",\"withinRadius\":" << (withinRadius ? "true" : "false")
         << ",\"baseId\":" << baseId
         << ",\"elementId\":" << elementId
         << ",\"ownerProfileId\":" << ownerProfileId
         << ",\"x\":" << JsonPreciseDoubleValue(flagX)
         << ",\"y\":" << JsonPreciseDoubleValue(flagY)
         << ",\"z\":" << JsonPreciseDoubleValue(flagZ)
         << ",\"distanceCm\":" << JsonPreciseDoubleValue(distanceCm)
         << ",\"reason\":\"" << JsonEscape(reason) << "\""
         << ",\"message\":\"" << JsonEscape(message) << "\""
         << "}";
    return data.str();
}

std::string BaseLootNearestFlagJson(
    const std::wstring& baseDir,
    const std::string& body,
    bool& commandOk,
    std::string& commandMessage) {
    commandOk = false;
    commandMessage.clear();

    auto xText = JsonNumberTextField(body, "x", JsonNumberTextField(body, "X", ""));
    auto yText = JsonNumberTextField(body, "y", JsonNumberTextField(body, "Y", ""));
    if (xText.empty() || yText.empty()) {
        commandOk = true;
        commandMessage = "player coordinates are missing";
        return BaseLootNearestFlagDataJson(0, 0, 0, true, false, false, 0, 0, 0, 0, 0, 0, 0, "coords_missing", commandMessage);
    }

    double queryX = std::atof(xText.c_str());
    double queryY = std::atof(yText.c_str());
    if (!std::isfinite(queryX) || !std::isfinite(queryY) || (queryX == 0.0 && queryY == 0.0)) {
        commandOk = true;
        commandMessage = "player coordinates are invalid";
        return BaseLootNearestFlagDataJson(queryX, queryY, 0, true, false, false, 0, 0, 0, 0, 0, 0, 0, "coords_invalid", commandMessage);
    }

    const int radiusCm = std::clamp(JsonIntField(body, "radiusCm", JsonIntField(body, "radius", 5000)), 100, 100000);
    const auto ownerProfileExpr =
        std::string("COALESCE(")
        + "NULLIF(b.owner_user_profile_id, -1),"
        + "NULLIF(b.user_profile_id, -1),"
        + "NULLIF(e.owner_profile_id, -1),"
        + "(SELECT e2.owner_profile_id FROM base_element e2 "
        + "WHERE e2.base_id=b.id AND e2.owner_profile_id IS NOT NULL AND e2.owner_profile_id > 0 "
        + "ORDER BY e2.element_id LIMIT 1))";
    const auto flagXExpr = std::string("COALESCE(e.location_x, b.location_x)");
    const auto flagYExpr = std::string("COALESCE(e.location_y, b.location_y)");
    const auto sql =
        std::string("SELECT ")
        + "f.element_id AS elementId, b.id AS baseId, b.name AS baseName, "
        + flagXExpr + " AS x, " + flagYExpr + " AS y, COALESCE(e.location_z, 0) AS z, "
        + ownerProfileExpr + " AS ownerProfileId, "
        + "(((" + flagXExpr + " - (" + xText + ")) * (" + flagXExpr + " - (" + xText + "))) + "
        + "((" + flagYExpr + " - (" + yText + ")) * (" + flagYExpr + " - (" + yText + ")))) AS distanceSq "
        + "FROM base_element_flag f "
        + "JOIN base_element e ON e.element_id = f.element_id "
        + "JOIN base b ON b.id = e.base_id "
        + "WHERE " + flagXExpr + " IS NOT NULL AND " + flagYExpr + " IS NOT NULL "
        + "ORDER BY distanceSq ASC LIMIT 1";

    std::string rows;
    std::string error;
    if (!SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), sql, 1, rows, error)) {
        commandMessage = error.empty() ? "SCUM.db nearest flag lookup failed" : error;
        return BaseLootNearestFlagDataJson(queryX, queryY, radiusCm, false, false, false, 0, 0, 0, 0, 0, 0, 0, "db_error", commandMessage);
    }

    const auto objects = ExtractArrayObjects(rows);
    if (objects.empty()) {
        commandOk = true;
        commandMessage = "base flag row was not found";
        return BaseLootNearestFlagDataJson(queryX, queryY, radiusCm, true, false, false, 0, 0, 0, 0, 0, 0, 0, "flag_not_found", commandMessage);
    }

    const auto row = objects.front();
    const long long baseId = ParseInt64Text(FlatJsonNumberValue(row, "baseId"), 0);
    const long long elementId = ParseInt64Text(FlatJsonNumberValue(row, "elementId"), 0);
    const long long ownerProfileId = ParseInt64Text(FlatJsonNumberValue(row, "ownerProfileId"), 0);
    const double flagX = ParseDoubleText(FlatJsonNumberValue(row, "x"), 0.0);
    const double flagY = ParseDoubleText(FlatJsonNumberValue(row, "y"), 0.0);
    const double flagZ = ParseDoubleText(FlatJsonNumberValue(row, "z"), 0.0);
    const double dx = flagX - queryX;
    const double dy = flagY - queryY;
    const double distanceCm = std::sqrt((dx * dx) + (dy * dy));
    const bool withinRadius = std::isfinite(distanceCm) && distanceCm <= static_cast<double>(radiusCm);

    commandOk = true;
    commandMessage = withinRadius ? "nearest base flag resolved" : "nearest base flag is outside radius";
    return BaseLootNearestFlagDataJson(
        queryX,
        queryY,
        radiusCm,
        true,
        true,
        withinRadius,
        baseId,
        elementId,
        ownerProfileId,
        flagX,
        flagY,
        flagZ,
        distanceCm,
        withinRadius ? "ok" : "outside_radius",
        commandMessage);
}

std::string BaseLootAccessEnvelope(const std::wstring& baseDir, const Request& req) {
    bool commandOk = false;
    std::string message;
    const auto data = BaseLootAccessJson(baseDir, req.body, commandOk, message);
    return commandOk ? Envelope(true, data) : EnvelopeFailureWithJsonData(message.empty() ? "Base loot access check failed." : message, data);
}

std::string BaseLootNearestFlagEnvelope(const std::wstring& baseDir, const Request& req) {
    bool commandOk = false;
    std::string message;
    const auto data = BaseLootNearestFlagJson(baseDir, req.body, commandOk, message);
    return commandOk ? Envelope(true, data) : EnvelopeFailureWithJsonData(message.empty() ? "Base loot nearest flag lookup failed." : message, data);
}

struct NativeBaseElementOwner {
    long long profileId = 0;
    long long prisonerId = 0;
    std::string steamId;
    std::string name;
};

NativeBaseElementOwner ResolveNativeBaseElementOwner(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    long long explicitProfileId,
    long long explicitPrisonerId) {
    NativeBaseElementOwner owner;
    owner.profileId = explicitProfileId;
    owner.prisonerId = explicitPrisonerId;

    const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
    const auto targetSteam = identity.steamId.empty() ? steamId : identity.steamId;
    const auto targetName = identity.name.empty() ? name : identity.name;

    std::string where;
    if (owner.profileId > 0) {
        where = "up.id = " + std::to_string(owner.profileId);
    } else {
        auto filter = PlayerSqlFilter(targetSteam, targetName);
        if (!filter.empty()) {
            where = "1=1 " + filter;
        }
    }
    if (where.empty()) where = "1=1";

    std::string rows;
    std::string error;
    const auto sql =
        std::string("SELECT up.id AS profileId, up.user_id AS steamId, up.name AS playerName, ")
        + "COALESCE(up.prisoner_id, p.id, 0) AS prisonerId "
        + "FROM user_profile up LEFT JOIN prisoner p ON p.user_profile_id = up.id "
        + "WHERE " + where + " ORDER BY up.last_login_time DESC, up.id DESC LIMIT 1";
    if (SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), sql, 1, rows, error)) {
        const auto objects = ExtractArrayObjects(rows);
        if (!objects.empty()) {
            const auto& object = objects.front();
            if (owner.profileId <= 0) owner.profileId = ParseInt64Text(FlatJsonNumberValue(object, "profileId"), 0);
            if (owner.prisonerId <= 0) owner.prisonerId = ParseInt64Text(FlatJsonNumberValue(object, "prisonerId"), 0);
            owner.steamId = FlatJsonStringValue(object, "steamId");
            owner.name = FlatJsonStringValue(object, "playerName");
        }
    }

    return owner;
}

struct NativeBaseElementLocation {
    bool ok = false;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double yaw = 0.0;
    int mapId = 1;
    std::string source;
    std::string error;
};

std::string JsonNumberTextAny(const std::string& body, std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        auto value = JsonNumberTextField(body, key, "");
        if (!value.empty()) return value;
    }
    return "";
}

bool AssignExplicitLocation(const std::string& body, NativeBaseElementLocation& location) {
    auto source = body;
    auto nested = JsonRawField(body, "location");
    if (!nested.empty()) source = nested;

    const auto x = JsonNumberTextAny(source, { "x", "X", "locationX", "LocationX" });
    const auto y = JsonNumberTextAny(source, { "y", "Y", "locationY", "LocationY" });
    const auto z = JsonNumberTextAny(source, { "z", "Z", "locationZ", "LocationZ" });
    if (x.empty() || y.empty() || z.empty()) return false;

    location.x = ParseDoubleText(x);
    location.y = ParseDoubleText(y);
    location.z = ParseDoubleText(z);
    location.ok = true;
    location.source = "request";
    return true;
}

NativeBaseElementLocation ResolveNativeBaseElementLocation(
    const std::wstring& baseDir,
    const std::string& body,
    const NativeBaseElementOwner& owner,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey) {
    NativeBaseElementLocation location;
    if (AssignExplicitLocation(body, location)) {
        location.yaw = ParseDoubleText(JsonNumberTextAny(body, { "yaw", "Yaw", "rotationYaw", "RotationYaw" }), 0.0);
    } else if (!steamId.empty() || !name.empty() || !runtimeKey.empty()) {
        const auto pos = ReadLivePlayerPositionForCommand(baseDir, steamId, name, runtimeKey);
        if (pos.ok) {
            location.x = ParseDoubleText(pos.x);
            location.y = ParseDoubleText(pos.y);
            location.z = ParseDoubleText(pos.z);
            location.ok = true;
            location.source = "live-player";
        }
    }

    if (!location.ok && owner.prisonerId > 0) {
        std::string rows;
        std::string error;
        const auto sql =
            std::string("SELECT map_id AS mapId, location_x AS x, location_y AS y, location_z AS z, rotation_yaw AS yaw ")
            + "FROM prisoner_spawn_location WHERE prisoner_id = " + std::to_string(owner.prisonerId)
            + " ORDER BY map_id DESC LIMIT 1";
        if (SqliteQueryRowsJson(baseDir, SavedDbPath(baseDir), sql, 1, rows, error)) {
            const auto objects = ExtractArrayObjects(rows);
            if (!objects.empty()) {
                const auto& object = objects.front();
                location.x = ParseDoubleText(FlatJsonNumberValue(object, "x"));
                location.y = ParseDoubleText(FlatJsonNumberValue(object, "y"));
                location.z = ParseDoubleText(FlatJsonNumberValue(object, "z"));
                location.yaw = ParseDoubleText(FlatJsonNumberValue(object, "yaw"));
                location.mapId = static_cast<int>(ParseInt64Text(FlatJsonNumberValue(object, "mapId"), 1));
                location.ok = true;
                location.source = "prisoner_spawn_location";
            }
        }
    }

    if (!location.ok) {
        location.error = "Нужны live-координаты игрока или явные x/y/z; безопасно угадать место нельзя.";
        return location;
    }

    const auto yawOverride = JsonNumberTextAny(body, { "yaw", "Yaw", "rotationYaw", "RotationYaw" });
    if (!yawOverride.empty()) location.yaw = ParseDoubleText(yawOverride, location.yaw);
    const auto mapId = ParseInt64Text(JsonNumberTextAny(body, { "mapId", "MapId" }), location.mapId);
    location.mapId = static_cast<int>(mapId <= 0 ? 1 : mapId);

    const double forwardDistance = ParseDoubleText(JsonNumberTextAny(body, { "forwardDistance", "distance", "ForwardDistance", "Distance" }), 0.0);
    if (std::fabs(forwardDistance) > 0.1) {
        const double radians = location.yaw * 3.14159265358979323846 / 180.0;
        location.x += std::cos(radians) * forwardDistance;
        location.y += std::sin(radians) * forwardDistance;
    }
    location.x += ParseDoubleText(JsonNumberTextAny(body, { "offsetX", "OffsetX" }), 0.0);
    location.y += ParseDoubleText(JsonNumberTextAny(body, { "offsetY", "OffsetY" }), 0.0);
    location.z += ParseDoubleText(JsonNumberTextAny(body, { "offsetZ", "OffsetZ", "liftZ", "LiftZ", "zOffset", "ZOffset" }), 0.0);

    if (std::fabs(location.x) > 10000000.0 || std::fabs(location.y) > 10000000.0 || std::fabs(location.z) > 2000000.0) {
        location.ok = false;
        location.error = "Координаты выглядят опасно/вне мира; DB spawn отменён.";
    }
    return location;
}

std::string FileStampUtc() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &time);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &tm);
    return buffer;
}

std::string NativeBaseElementDbSpawnEnvelope(const std::wstring& baseDir, const std::string& body) {
    auto elementSpec = JsonStringField(body, "element");
    if (elementSpec.empty()) elementSpec = JsonStringField(body, "elementClass");
    if (elementSpec.empty()) elementSpec = JsonStringField(body, "baseElement");
    if (elementSpec.empty()) elementSpec = JsonStringField(body, "baseElementClass");
    if (elementSpec.empty()) elementSpec = JsonStringField(body, "actorClass");
    if (elementSpec.empty()) elementSpec = JsonStringField(body, "ActorClass");
    if (elementSpec.empty()) elementSpec = "BP_Base_Cabin_C";

    const auto message = std::string("DB-only base/base_element spawn removed: SCUM.db is read-only for ScumNeDjin, ") +
        "and prior DB-only rows did not register in AConZBaseManager as visible persistent buildings.";
    const auto detailJson = std::string("{\"source\":\"native-db-route\",\"command\":\"native_base_element_db_spawn\"") +
        ",\"blocked\":true" +
        ",\"removed\":true" +
        ",\"dbWrite\":false" +
        ",\"requires\":\"native ConZBaseManager runtime/import wrapper\"" +
        ",\"element\":" + JsonValueOrString(elementSpec) +
        ",\"message\":\"" + JsonEscape(message) + "\"}";
    AppendActionRecord(baseDir, "native_base_element_db_spawn_blocked", false, "", "", message, detailJson);
    return Envelope(false, detailJson);
}
std::string DeliverItemsBatchArkPanelJson(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    const std::vector<GrantItem>& items,
    const std::string& source) {
    if (items.empty()) return Envelope(false, "Для выдачи не настроены предметы.");

    const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
    auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
    auto targetName = identity.name.empty() ? name : identity.name;
    auto targetRuntimeKey = identity.runtimeKey.empty() ? runtimeKey : identity.runtimeKey;
    const auto liveTarget = ValidatePanelLiveTargetForCommandChannel(
        baseDir, targetSteamId, targetName, targetRuntimeKey, "deliver-items-ark-panel", true, true);
    if (!liveTarget.ok) {
        return Envelope(false, liveTarget.message);
    }
    const auto targetRef = ScumRconLocationRef(baseDir,
        liveTarget.steamId.empty() ? targetSteamId : liveTarget.steamId,
        liveTarget.name.empty() ? targetName : liveTarget.name,
        liveTarget.runtimeKey.empty() ? targetRuntimeKey : liveTarget.runtimeKey);
    if (targetRef.empty()) {
        return Envelope(false, "Для выдачи через консоль хостинга нужен SteamID64 или live-координаты.");
    }

    const int delayMs = ConfigIntValue(baseDir, "ark_panel_command_delay_ms", 0, 0, 5000);
    std::ostringstream results;
    results << "[";
    int okCount = 0;
    int requested = 0;

    for (size_t i = 0; i < items.size(); ++i) {
        const auto itemId = TrimAscii(items[i].itemId);
        const int quantity = std::clamp(items[i].quantity, 1, 100);
        if (itemId.empty()) continue;
        ++requested;

        ArkCommandResult result;
        std::string commandText;
        std::string validationMessage;
        if (!IsSafeScumAdminToken(itemId)) {
            validationMessage = "ID предмета содержит небезопасные символы для серверной команды.";
        } else {
            commandText = std::string("#SpawnItem ") + itemId + " " + std::to_string(quantity) + " Location " + ScumLocationArgument(targetRef);
            result = ArkPanelSendConsoleCommand(baseDir, commandText);
            if (i + 1 < items.size()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }
        }

        if (requested > 1) results << ",";
        results << "{\"itemId\":\"" << JsonEscape(itemId)
                << "\",\"quantity\":" << quantity
                << ",\"transport\":\"hosting-panel-command\""
                << ",\"command\":\"" << JsonEscape(commandText) << "\""
                << ",\"ok\":" << (result.ok ? "true" : "false")
                << ",\"status\":" << result.status
                << ",\"message\":\"" << JsonEscape(validationMessage.empty() ? result.message : validationMessage) << "\"";
        if (!result.body.empty()) results << ",\"panelBody\":" << JsonValueOrString(result.body);
        results << "}";
        if (result.ok) ++okCount;
    }
    results << "]";

    if (requested == 0) return Envelope(false, "В выдаче нет корректных предметов.");
    const bool allOk = okCount == requested;
    const auto spec = GrantItemsSpec(items);
    AppendActionRecord(baseDir, "deliver_items_ark_panel", allOk, targetSteamId, targetName,
        allOk ? "Команды выдачи предметов отправлены через консоль хостинга." : "Пакет предметов отправлен через консоль хостинга не полностью.",
        std::string("{\"source\":\"") + JsonEscape(source) +
        "\",\"itemsSpec\":\"" + JsonEscape(spec) +
        "\",\"requested\":" + std::to_string(requested) +
        ",\"delivered\":" + std::to_string(okCount) +
        ",\"transport\":\"hosting-panel-command\"}");

    std::ostringstream payload;
    payload << "{\"source\":\"" << JsonEscape(source)
            << "\",\"transport\":\"hosting-panel-command\""
            << ",\"target\":{\"steamId\":\"" << JsonEscape(targetSteamId)
            << "\",\"name\":\"" << JsonEscape(targetName)
            << "\",\"runtimeKey\":\"" << JsonEscape(targetRuntimeKey)
            << "\",\"commandRef\":\"" << JsonEscape(targetRef)
            << "\"},\"requested\":" << requested
            << ",\"delivered\":" << okCount
            << ",\"allOk\":" << (allOk ? "true" : "false")
            << ",\"message\":\"" << JsonEscape(allOk
                ? "Команды выдачи отправлены. Фактический спавн проверяется по игроку/серверному логу."
                : "Пакет предметов выдан не полностью через консоль хостинга.")
            << "\",\"results\":" << results.str() << "}";
    return allOk ? Envelope(true, payload.str()) : Envelope(false, payload.str());
}

std::string DeliverItemsBatchJson(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    const std::vector<GrantItem>& items,
    const std::string& source) {
    if (items.empty()) return Envelope(false, "Для выдачи не настроены предметы.");
    const auto spec = GrantItemsSpec(items);
    if (spec.empty()) return Envelope(false, "В выдаче нет корректных предметов.");

    const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
    auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
    auto targetName = identity.name.empty() ? name : identity.name;
    auto targetRuntimeKey = identity.runtimeKey.empty() ? runtimeKey : identity.runtimeKey;
    const auto confirmedTarget = ValidatePanelLiveTargetForCommandChannel(
        baseDir, targetSteamId, targetName, targetRuntimeKey, "deliver-items-live-route", true, true);
    if (!confirmedTarget.ok) {
        AppendActionRecord(baseDir, "deliver_items_live_preflight", false, targetSteamId, targetName,
            confirmedTarget.message, std::string("{\"source\":\"") + JsonEscape(source) +
            "\",\"itemsSpec\":\"" + JsonEscape(spec) + "\"}");
        return Envelope(false, confirmedTarget.message);
    }
    if (!confirmedTarget.steamId.empty()) targetSteamId = confirmedTarget.steamId;
    if (!confirmedTarget.name.empty()) targetName = confirmedTarget.name;
    if (!confirmedTarget.runtimeKey.empty()) targetRuntimeKey = confirmedTarget.runtimeKey;
    auto deliveryTransport = ToLowerAscii(TrimAscii(ConfigTextValue(baseDir, "delivery_transport", "auto")));
    if (deliveryTransport.empty()) deliveryTransport = "auto";
    const bool preferLocalRcon = deliveryTransport == "auto" || deliveryTransport == "local-rcon" ||
        deliveryTransport == "local_rcon" || deliveryTransport == "rcon" ||
        deliveryTransport == "server-command" || deliveryTransport == "server_command" ||
        deliveryTransport == "command-channel" || deliveryTransport == "command_channel";
    const bool localRconOnly = deliveryTransport == "local-rcon" || deliveryTransport == "local_rcon" ||
        deliveryTransport == "rcon" || deliveryTransport == "server-command" ||
        deliveryTransport == "server_command" || deliveryTransport == "command-channel" ||
        deliveryTransport == "command_channel";

    if (preferLocalRcon && LocalRconEnabled(baseDir)) {
        const auto liveTarget = ValidatePanelLiveTargetForCommandChannel(
            baseDir, targetSteamId, targetName, targetRuntimeKey, "deliver-items-server-command", true, true);
        if (!liveTarget.ok) {
            if (localRconOnly) return Envelope(false, liveTarget.message);
            AppendActionRecord(baseDir, "deliver_items_server_command_preflight", false, targetSteamId, targetName,
                liveTarget.message, std::string("{\"source\":\"") + JsonEscape(source) +
                "\",\"itemsSpec\":\"" + JsonEscape(spec) + "\"}");
        }
        const auto targetRef = liveTarget.ok
            ? ScumRconLocationRef(baseDir,
                liveTarget.steamId.empty() ? targetSteamId : liveTarget.steamId,
                liveTarget.name.empty() ? targetName : liveTarget.name,
                liveTarget.runtimeKey.empty() ? targetRuntimeKey : liveTarget.runtimeKey)
            : std::string{};
        if (!targetRef.empty()) {
            const int delayMs = LocalRconCommandDelayMs(baseDir);
            std::ostringstream results;
            results << "[";
            int requested = 0;
            int okCount = 0;
            for (size_t i = 0; i < items.size(); ++i) {
                const auto itemId = TrimAscii(items[i].itemId);
                const int quantity = std::clamp(items[i].quantity, 1, 100);
                if (itemId.empty()) continue;
                ++requested;
                ArkCommandResult result;
                std::string commandText;
                std::string validationMessage;
                if (!IsSafeScumAdminToken(itemId)) {
                    validationMessage = "ID предмета содержит небезопасные символы для серверной команды.";
                } else {
                    commandText = std::string("SpawnItem ") + itemId + " " + std::to_string(quantity) + " Location " + ScumLocationArgument(targetRef);
                    result = LocalRconSendConsoleCommand(baseDir, commandText);
                    if (i + 1 < items.size()) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                }
                if (requested > 1) results << ",";
                results << "{\"itemId\":\"" << JsonEscape(itemId)
                        << "\",\"quantity\":" << quantity
                        << ",\"transport\":\"server-command\""
                        << ",\"command\":\"" << JsonEscape(commandText) << "\""
                        << ",\"ok\":" << (result.ok ? "true" : "false")
                        << ",\"status\":" << result.status
                        << ",\"message\":\"" << JsonEscape(validationMessage.empty() ? result.message : validationMessage) << "\"";
                if (!result.body.empty()) results << ",\"commandBody\":" << JsonValueOrString(result.body);
                results << "}";
                if (result.ok) ++okCount;
            }
            results << "]";
            const bool allOk = requested > 0 && okCount == requested;
            AppendActionRecord(baseDir, "deliver_items_server_command", allOk, targetSteamId, targetName,
                allOk ? "Команды выдачи предметов отправлены через командный канал сервера." : "Пакет предметов отправлен через командный канал сервера не полностью.",
                std::string("{\"source\":\"") + JsonEscape(source) +
                "\",\"itemsSpec\":\"" + JsonEscape(spec) +
                "\",\"requested\":" + std::to_string(requested) +
                ",\"delivered\":" + std::to_string(okCount) +
                ",\"transport\":\"server-command\"}");
            std::ostringstream payload;
            payload << "{\"source\":\"" << JsonEscape(source)
                    << "\",\"transport\":\"server-command\""
                    << ",\"target\":{\"steamId\":\"" << JsonEscape(targetSteamId)
                    << "\",\"name\":\"" << JsonEscape(targetName)
                    << "\",\"runtimeKey\":\"" << JsonEscape(targetRuntimeKey)
                    << "\",\"commandRef\":\"" << JsonEscape(targetRef)
                    << "\"},\"requested\":" << requested
                    << ",\"delivered\":" << okCount
                    << ",\"allOk\":" << (allOk ? "true" : "false")
                    << ",\"message\":\"" << JsonEscape(allOk
                        ? "Команды выдачи отправлены через командный канал сервера."
                        : "Пакет предметов выдан не полностью через командный канал сервера.")
                    << "\",\"results\":" << results.str() << "}";
            if (allOk || localRconOnly) return allOk ? Envelope(true, payload.str()) : Envelope(false, payload.str());
            AppendActionRecord(baseDir, "deliver_items_server_command_fallback", false, targetSteamId, targetName,
                "Командный канал сервера не выполнил выдачу; пробую UE4SS bridge.", payload.str());
        } else if (localRconOnly) {
            return Envelope(false, "Для выдачи через командный канал нужен SteamID64 или live-координаты.");
        }
    }
    if (localRconOnly) {
        return Envelope(false, "Delivery transport is server-command only, but server_command_enabled/server_command_password is not configured.");
    }

    auto bridge = BridgeExecute(baseDir, "deliver_items_batch",
        std::string("{") + PlayerTargetArgs(targetSteamId, targetName, targetRuntimeKey) +
        ",\"itemsSpec\":\"" + JsonEscape(spec) + "\"}", 60000);
    AppendActionRecord(baseDir, "deliver_items_bridge_live", bridge.ok, targetSteamId, targetName,
        bridge.ok ? bridge.body : bridge.message,
        std::string("{\"source\":\"") + JsonEscape(source) +
        "\",\"itemsSpec\":\"" + JsonEscape(spec) +
        "\",\"transport\":\"ue4ss-bridge-live\"}");
    if (bridge.ok) {
        return Envelope(true, bridge.body);
    }

    if (!AllowUnverifiedServerCommandDelivery(baseDir)) {
        return Envelope(false, std::string("Live-выдача через UE4SS bridge не подтвердилась: ") + bridge.message);
    }

    if (ArkPanelEnabled(baseDir)) {
        auto ark = DeliverItemsBatchArkPanelJson(baseDir, steamId, name, runtimeKey, items, source);
        if (ark.find("\"ok\":true") != std::string::npos) return ark;
        return Envelope(false, std::string("Live-выдача через UE4SS bridge не подтвердилась, резервная выдача через консоль хостинга тоже не прошла: ") + ark);
    }

    return Envelope(false, std::string("Live-выдача через UE4SS bridge не подтвердилась: ") + bridge.message);
}

std::string LatestPlayerDetailsBody(const std::wstring& baseDir, const std::string& steamId, const std::string& name, const std::string& runtimeKey, bool* okOut = nullptr) {
    auto br = BridgeExecute(baseDir, "player_details", std::string("{") + PlayerTargetArgs(steamId, name, runtimeKey) + "}", 5000);
    if (okOut) *okOut = br.ok;
    if (!br.ok || br.body.empty()) return br.body;
    const auto overlaid = BridgePlayerDetailsWithLogIdentityJson(baseDir, br.body, steamId, name);
    return overlaid.empty() ? br.body : overlaid;
}

std::string SaveHomeFromLivePlayer(const std::wstring& baseDir, const std::string& steamId, const std::string& name, const std::string& runtimeKey, const std::string& label) {
    bool okDetails = false;
    const auto body = LatestPlayerDetailsBody(baseDir, steamId, name, runtimeKey, &okDetails);
    if (!okDetails || body.empty()) return Envelope(false, "Не удалось получить текущую позицию живого игрока для дома.");
    const auto playerObjects = ExtractPlayerObjects(body);
    auto object = playerObjects.empty() ? JsonRawField(body, "player") : playerObjects.front();
    if (object.empty()) object = body;
    const auto x = FlatJsonNumberValue(object, "x");
    const auto y = FlatJsonNumberValue(object, "y");
    const auto z = FlatJsonNumberValue(object, "z");
    const auto resolvedName = FlatJsonStringValue(object, "name");
    const auto resolvedSteam = FlatJsonStringValue(object, "steamId");
    std::ostringstream record;
    record << "{\"utc\":\"" << UtcIsoNow()
           << "\",\"label\":\"" << JsonEscape(label.empty() ? "home" : label)
           << "\",\"steamId\":\"" << JsonEscape(resolvedSteam.empty() ? steamId : resolvedSteam)
           << "\",\"name\":\"" << JsonEscape(resolvedName.empty() ? name : resolvedName)
           << "\",\"x\":" << x << ",\"y\":" << y << ",\"z\":" << z << "}";
    const auto saved = AppendModuleStateRecord(baseDir, "homes", record.str());
    return saved ? Envelope(true, record.str()) : Envelope(false, "Не удалось сохранить домашнюю точку.");
}

bool TryResolveFastTravelAlias(const std::string& configJson, const std::string& alias, std::string& x, std::string& y, std::string& z, std::string& displayName) {
    const auto wanted = ToLowerAscii(TrimAscii(alias));
    if (wanted.empty()) return false;
    const std::regex routeRe(R"rx(\{[^\{\}]*"DisplayName"\s*:\s*"([^"]*)"[^\{\}]*"CommandAlias"\s*:\s*"([^"]+)"[^\{\}]*"ArrivalPoint"\s*:\s*\[\s*([-0-9.]+)\s*,\s*([-0-9.]+)\s*,\s*([-0-9.]+))rx", std::regex::icase);
    for (auto it = std::sregex_iterator(configJson.begin(), configJson.end(), routeRe); it != std::sregex_iterator(); ++it) {
        const auto foundAlias = ToLowerAscii((*it)[2].str());
        if (foundAlias == wanted) {
            displayName = (*it)[1].str();
            x = (*it)[3].str();
            y = (*it)[4].str();
            z = (*it)[5].str();
            return true;
        }
    }
    return false;
}

bool TryResolveVehicleAlias(
    const std::string& configJson,
    const std::string& alias,
    std::string& resolvedAlias,
    std::string& assetName,
    int& initialCharge,
    int& pricePer10Minutes,
    std::string& displayName,
    int& defaultMinutes,
    int& minMinutes,
    int& maxMinutes) {
    const auto wanted = ToLowerAscii(TrimAscii(alias));
    if (wanted.empty()) return false;
    const std::regex vehicleRe(R"rx(\{[^\{\}]*"(?:Alias|alias)"\s*:\s*"([^"]+)"[^\{\}]*\})rx", std::regex::icase);
    for (auto it = std::sregex_iterator(configJson.begin(), configJson.end(), vehicleRe); it != std::sregex_iterator(); ++it) {
        const auto object = (*it)[0].str();
        const auto objectAliasRaw = FirstNonEmpty({ JsonStringField(object, "Alias"), JsonStringField(object, "alias") });
        const auto objectAlias = ToLowerAscii(objectAliasRaw);
        const auto objectAsset = ToLowerAscii(FirstNonEmpty({ JsonStringField(object, "AssetName"), JsonStringField(object, "assetName") }));
        const auto objectDisplay = ToLowerAscii(FirstNonEmpty({ JsonStringField(object, "DisplayName"), JsonStringField(object, "displayName") }));
        if (objectAlias == wanted || objectAsset == wanted || objectDisplay == wanted) {
            resolvedAlias = TrimAscii(objectAliasRaw);
            displayName = FirstNonEmpty({ JsonStringField(object, "DisplayName"), JsonStringField(object, "displayName") });
            assetName = FirstNonEmpty({ JsonStringField(object, "AssetName"), JsonStringField(object, "assetName") });
            initialCharge = JsonIntField(object, "InitialCharge", JsonIntField(object, "initialCharge", initialCharge));
            pricePer10Minutes = JsonIntField(object, "PricePer10Minutes", JsonIntField(object, "pricePer10Minutes", pricePer10Minutes));
            minMinutes = std::max(1, JsonIntField(object, "MinMinutes", JsonIntField(object, "minMinutes", minMinutes)));
            maxMinutes = std::max(minMinutes, JsonIntField(object, "MaxMinutes", JsonIntField(object, "maxMinutes", maxMinutes)));
            defaultMinutes = std::clamp(
                JsonIntField(object, "DefaultMinutes", JsonIntField(object, "defaultMinutes", defaultMinutes)),
                minMinutes,
                maxMinutes);
            return true;
        }
    }
    return false;
}

std::string SectorFromCoordinates(double x, double y) {
    const double minX = -905369.6875;
    const double maxX = 619646.5625;
    const double minY = -904357.625;
    const double maxY = 619659.75;
    static constexpr const char* rows[] = { "D", "C", "B", "A", "Z" };
    static constexpr const char* cols[] = { "4", "3", "2", "1", "0" };
    const int col = std::clamp(static_cast<int>(((maxX - x) / (maxX - minX)) * 5.0), 0, 4);
    const int row = std::clamp(static_cast<int>(((maxY - y) / (maxY - minY)) * 5.0), 0, 4);
    return std::string(rows[row]) + cols[col];
}

std::string SectorScanJson(const std::wstring& baseDir, const std::string& requestedSector) {
    auto br = BridgeExecute(baseDir, "list_players", "{}", 5000);
    if (!br.ok || br.body.empty()) return Envelope(false, br.message);
    auto body = BridgePlayersWithLogIdentityJson(baseDir, br.body);
    if (body.empty()) body = br.body;

    const auto target = ToLowerAscii(TrimAscii(requestedSector));
    std::ostringstream players;
    players << "[";
    int total = 0;
    int count = 0;
    for (const auto& object : ExtractPlayerObjects(body)) {
        const auto xText = FlatJsonNumberValue(object, "x");
        const auto yText = FlatJsonNumberValue(object, "y");
        const auto x = std::atof(xText.c_str());
        const auto y = std::atof(yText.c_str());
        const auto sector = SectorFromCoordinates(x, y);
        ++total;
        if (!target.empty() && ToLowerAscii(sector) != target) continue;
        if (count) players << ",";
        players << "{\"name\":\"" << JsonEscape(FlatJsonStringValue(object, "name"))
                << "\",\"steamId\":\"" << JsonEscape(FlatJsonStringValue(object, "steamId"))
                << "\",\"sector\":\"" << sector
                << "\",\"x\":" << xText
                << ",\"y\":" << yText
                << ",\"z\":" << FlatJsonNumberValue(object, "z") << "}";
        ++count;
    }
    players << "]";

    std::ostringstream payload;
    payload << "{\"sector\":\"" << JsonEscape(requestedSector)
            << "\",\"count\":" << count
            << ",\"onlinePlayers\":" << total
            << ",\"players\":" << players.str()
            << ",\"source\":\"ue4ss\"}";
    return Envelope(true, payload.str());
}

std::string ShopManualDeliver(
    const std::wstring& baseDir,
    const Request& req,
    const std::string& stateKey,
    const std::string& sourceDefault,
    const std::string& itemDeliverySource,
    const std::string& actionPrefix,
    const std::string& displayName) {
    auto steamId = JsonStringField(req.body, "steamId");
    auto name = JsonStringField(req.body, "name");
    auto runtimeKey = JsonStringField(req.body, "runtimeKey");
    const auto identity = ResolveLivePlayerIdentity(baseDir, steamId, name, runtimeKey);
    if (!identity.steamId.empty()) steamId = identity.steamId;
    if (!identity.name.empty()) name = identity.name;
    if (!identity.runtimeKey.empty()) runtimeKey = identity.runtimeKey;
    auto mode = ToLowerAscii(JsonStringField(req.body, "mode"));
    if (mode.empty()) mode = ToLowerAscii(JsonStringField(req.body, "deliveryMode"));
    mode = NormalizeDeliveryMode(mode);
    auto source = JsonStringField(req.body, "source");
    if (source.empty()) source = sourceDefault;
    const auto finish = [&](const std::string& response, const std::string& label) {
        AppendModuleStateRecord(baseDir, stateKey,
            std::string("{\"utc\":\"") + UtcIsoNow() +
            "\",\"source\":\"" + JsonEscape(source) + "\",\"steamId\":\"" + JsonEscape(steamId) +
            "\",\"name\":\"" + JsonEscape(name) +
            "\",\"mode\":\"" + JsonEscape(label) +
            "\",\"response\":" + response + "}");
        return response;
    };
    if (mode == "item" || mode == "spawnitem") {
        auto items = ParseGrantItems(req.body);
        if (items.empty()) {
            items.push_back(GrantItem{ JsonStringField(req.body, "itemId"), JsonIntField(req.body, "quantity", 1) });
        }
        return finish(DeliverItemsBatchJson(baseDir, steamId, name, runtimeKey, items, itemDeliverySource), "item");
    }
    if (mode == "vehicle" || mode == "spawnvehicle") {
        const auto vehicleId = JsonStringField(req.body, "vehicleId");
        auto spawn = SpawnVehicleVerified(baseDir, vehicleId, steamId, name, runtimeKey);
        if (!spawn.ok) {
            return finish(Envelope(false, spawn.message + " попытки=" + spawn.attemptsJson), "vehicle");
        }
        return finish(Envelope(true, std::string("{\"ok\":true,\"source\":\"nedjin-verified\",\"command\":\"spawn_vehicle\",\"message\":\"") +
            JsonEscape(spawn.message) +
            "\",\"data\":{\"vehicleId\":\"" + JsonEscape(VehicleAdminToken(vehicleId)) +
            "\",\"entityId\":" + std::to_string(spawn.entityId) +
            ",\"runtimeRef\":\"" + JsonEscape(spawn.runtimeRef) +
            "\",\"destroyRef\":\"" + JsonEscape(spawn.entityId > 0 ? std::to_string(spawn.entityId) : spawn.runtimeRef) +
            "\",\"command\":\"" + JsonEscape(spawn.commandText) +
            "\",\"attempts\":" + spawn.attemptsJson +
            "},\"bridge\":" + (spawn.bridgeBody.empty() ? "{}" : spawn.bridgeBody) + "}"), "vehicle");
    }
    if (mode == "money" || mode == "currency" || mode == "gold") {
        auto currency = JsonStringField(req.body, "currency");
        if (currency.empty() && mode == "gold") currency = "Gold";
        if (currency.empty()) currency = "Normal";
        const auto amount = JsonIntField(req.body, "amount", 0);
        const auto change = ChangeMoneyVerified(baseDir, steamId, name, runtimeKey, amount, currency);
        if (!change.ok) {
            return finish(Envelope(false, change.message), mode == "gold" ? "gold" : "money");
        }
        std::ostringstream data;
        data << "{\"verified\":true"
             << ",\"bridgeOk\":" << (change.bridgeOk ? "true" : "false")
             << ",\"dbWrite\":" << (change.dbPatched ? "true" : "false")
             << ",\"liveApply\":\"" << (change.bridgeOk ? "admin-command" : "none") << "\""
             << ",\"message\":\"" << JsonEscape(change.message) << "\""
             << ",\"before\":" << WalletJson(change.before)
             << ",\"after\":" << WalletJson(change.after) << "}";
        return finish(Envelope(true, data.str()), mode == "gold" ? "gold" : "money");
    }
    if (mode == "fame" || mode == "changefame" || mode == "setfame") {
        const auto amount = JsonIntField(req.body, "amount", 0);
        const auto fame = mode == "setfame"
            ? SetFameVerified(baseDir, steamId, name, runtimeKey, amount)
            : ChangeFameVerified(baseDir, steamId, name, runtimeKey, amount);
        if (!fame.ok) {
            return finish(Envelope(false, fame.message), mode == "setfame" ? "setfame" : "fame");
        }
        std::ostringstream data;
        data << "{\"verified\":true"
             << ",\"bridgeOk\":" << (fame.bridgeOk ? "true" : "false")
             << ",\"dbWrite\":" << (fame.dbPatched ? "true" : "false")
             << ",\"liveApply\":\"" << (fame.bridgeOk ? "admin-command" : "none") << "\""
             << ",\"message\":\"" << JsonEscape(fame.message) << "\""
             << ",\"before\":" << WalletJson(fame.before)
             << ",\"after\":" << WalletJson(fame.after) << "}";
        return finish(Envelope(true, data.str()), mode == "setfame" ? "setfame" : "fame");
    }
    if (mode == "skill" || mode == "setskill" || mode == "allskills" || mode == "skills") {
        auto skill = JsonStringField(req.body, "skill");
        if (skill.empty()) skill = JsonStringField(req.body, "skillName");
        if ((mode == "allskills" || mode == "skills") && skill.empty()) skill = "allskills";
        const auto level = JsonIntField(req.body, "level", JsonIntField(req.body, "skillLevel", 0));
        const auto experience = JsonIntField(req.body, "experience", JsonIntField(req.body, "skillExperience", 0));
        if (skill.empty()) return finish(Envelope(false, std::string("Не указан навык для выдачи ") + displayName + "."), "skill");
        const auto brJson = BridgePlayerCommand(baseDir, "set_skill", steamId, name, runtimeKey,
            std::string("\"skill\":\"") + JsonEscape(skill) +
            "\",\"level\":" + std::to_string(std::clamp(level, 0, 4)) +
            ",\"experience\":" + std::to_string(std::max(0, experience)), 20000);
        const bool ok = brJson.find("\"ok\":true") != std::string::npos;
        AppendActionRecord(baseDir, actionPrefix + "_set_skill_live", ok, steamId, name, brJson,
            std::string("{\"skill\":\"") + JsonEscape(skill) +
            "\",\"level\":" + std::to_string(std::clamp(level, 0, 4)) +
            ",\"experience\":" + std::to_string(std::max(0, experience)) +
            ",\"liveApply\":\"bridge-admin-command\",\"preflight\":true}");
        if (ok) return finish(brJson, "skill");
        return finish(Envelope(false, "Live-команда изменения навыка не выполнена или игрок не подтверждён в live runtime."), "skill");
    }
    if (mode == "attributes" || mode == "attribute" || mode == "stats") {
        const auto strength = JsonNumberTextField(req.body, "strength", JsonNumberTextField(req.body, "Strength", "0"));
        const auto constitution = JsonNumberTextField(req.body, "constitution", JsonNumberTextField(req.body, "Constitution", "0"));
        const auto dexterity = JsonNumberTextField(req.body, "dexterity", JsonNumberTextField(req.body, "Dexterity", "0"));
        const auto intelligence = JsonNumberTextField(req.body, "intelligence", JsonNumberTextField(req.body, "Intelligence", "0"));
        const auto brJson = BridgePlayerCommand(baseDir, "set_attributes", steamId, name, runtimeKey,
            std::string("\"strength\":") + strength +
            ",\"constitution\":" + constitution +
            ",\"dexterity\":" + dexterity +
            ",\"intelligence\":" + intelligence, 20000);
        const bool ok = brJson.find("\"ok\":true") != std::string::npos;
        AppendActionRecord(baseDir, actionPrefix + "_set_attributes_live", ok, steamId, name, brJson,
            std::string("{\"strength\":") + strength + ",\"constitution\":" + constitution +
            ",\"dexterity\":" + dexterity + ",\"intelligence\":" + intelligence +
            ",\"liveApply\":\"bridge-admin-command\",\"preflight\":true}");
        if (ok) return finish(brJson, "attributes");
        return finish(Envelope(false, "Live-команда изменения атрибутов не выполнена или игрок не подтверждён в live runtime."), "attributes");
    }
    if (mode == "command" || mode == "admincommand") {
        auto commandTemplate = JsonStringField(req.body, "command");
        if (commandTemplate.empty()) commandTemplate = JsonStringField(req.body, "commandTemplate");
        if (commandTemplate.empty()) return finish(Envelope(false, std::string("Не задана команда для ручной выдачи ") + displayName + "."), "command");
        const bool commandUsesSteam = commandTemplate.find("{steamId}") != std::string::npos;
        const bool commandUsesName = commandTemplate.find("{name}") != std::string::npos;
        if (commandUsesSteam || commandUsesName || !runtimeKey.empty() || !steamId.empty() || !name.empty()) {
            const auto liveTarget = ValidatePanelLiveTargetForCommandChannel(
                baseDir, steamId, name, runtimeKey, actionPrefix + "-admin-command", false, commandUsesSteam);
            if (!liveTarget.ok) {
                return finish(Envelope(false, liveTarget.message), "command");
            }
            if (!liveTarget.steamId.empty()) steamId = liveTarget.steamId;
            if (!liveTarget.name.empty()) name = liveTarget.name;
            if (!liveTarget.runtimeKey.empty()) runtimeKey = liveTarget.runtimeKey;
        }
        commandTemplate = std::regex_replace(commandTemplate, std::regex(R"(\{steamId\})"), steamId);
        commandTemplate = std::regex_replace(commandTemplate, std::regex(R"(\{name\})"), name);
        commandTemplate = TrimAscii(commandTemplate);
        if (!commandTemplate.empty() && commandTemplate.front() != '#') commandTemplate = "#" + commandTemplate;
        if (commandTemplate.find('\r') != std::string::npos || commandTemplate.find('\n') != std::string::npos) {
            return finish(Envelope(false, std::string("Команда ") + displayName + " должна быть одной строкой."), "command");
        }
        if (LocalRconEnabled(baseDir)) {
            const auto rcon = LocalRconSendConsoleCommand(baseDir, commandTemplate);
            AppendActionRecord(baseDir, actionPrefix + "_admin_command_server_command", rcon.ok, steamId, name,
                rcon.ok ? rcon.message : (rcon.message.empty() ? rcon.body : rcon.message),
                std::string("{\"command\":\"") + JsonEscape(commandTemplate) + "\",\"transport\":\"server-command\"}");
            if (!rcon.ok) return finish(Envelope(false, rcon.message.empty() ? std::string("Командный канал сервера не принял ") + displayName + "-команду." : rcon.message), "command");
            std::ostringstream data;
            data << "{\"transport\":\"server-command\""
                 << ",\"command\":\"" << JsonEscape(LocalRconCommandText(commandTemplate)) << "\""
                 << ",\"body\":" << JsonValueOrString(rcon.body) << "}";
            return finish(Envelope(true, data.str()), "command");
        }
        if (ArkPanelEnabled(baseDir)) {
            const auto rcon = ArkPanelSendConsoleCommand(baseDir, commandTemplate);
            AppendActionRecord(baseDir, actionPrefix + "_admin_command_ark_panel", rcon.ok, steamId, name,
                rcon.ok ? rcon.message : (rcon.message.empty() ? rcon.body : rcon.message),
                std::string("{\"command\":\"") + JsonEscape(commandTemplate) + "\",\"transport\":\"hosting-panel-command\"}");
            if (!rcon.ok) return finish(Envelope(false, rcon.message.empty() ? std::string("Консоль хостинга не приняла ") + displayName + "-команду." : rcon.message), "command");
            std::ostringstream data;
            data << "{\"transport\":\"hosting-panel-command\""
                 << ",\"command\":\"" << JsonEscape(commandTemplate) << "\""
                 << ",\"status\":" << rcon.status
                 << ",\"panelBody\":" << JsonValueOrString(rcon.body) << "}";
            return finish(Envelope(true, data.str()), "command");
        }
        auto br = BridgeExecute(baseDir, "admin_exec",
            std::string("{\"commandText\":\"") + JsonEscape(commandTemplate) + "\"}", 20000);
        AppendActionRecord(baseDir, actionPrefix + "_admin_command", br.ok, steamId, name, br.ok ? br.body : br.message,
            std::string("{\"command\":\"") + JsonEscape(commandTemplate) + "\"}");
        return finish(br.ok ? Envelope(true, br.body) : Envelope(false, br.message), "command");
    }
    return Envelope(false, "Неподдерживаемый режим ручной выдачи.");
}

std::string WargmManualDeliver(const std::wstring& baseDir, const Request& req) {
    return ShopManualDeliver(baseDir, req, "wargm-shop", "panel-manual", "wargm-manual", "wargm", "Wargm");
}

std::string GameStoresManualDeliver(const std::wstring& baseDir, const Request& req) {
    std::lock_guard<std::recursive_mutex> operationGuard(g_gameStoresOperationMutex);
    GameStoresOperationLease lease(baseDir);
    if (!lease.acquired()) return Envelope(false, lease.error());
    return ShopManualDeliver(baseDir, req, "gamestores-shop", "gamestores-manual", "gamestores-manual", "gamestores", "GameStores");
}

bool WargmSettledKeyExists(const std::vector<std::string>& keys, const std::string& value) {
    const auto key = TrimAscii(value);
    if (key.empty()) return false;
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) return true;
    const auto lowered = ToLowerAscii(key);
    for (const auto& existing : keys) {
        if (ToLowerAscii(existing) == lowered) return true;
    }
    return false;
}

std::string WargmPendingRowKey(const std::string& row) {
    const auto explicitKey = JsonScalarAny(row, { "key", "Key" });
    if (!explicitKey.empty()) return explicitKey;
    const auto opId = JsonScalarAny(row, { "operationId", "OperationId", "id", "Id" });
    if (!opId.empty()) return "operationId:" + opId;
    return {};
}

bool WargmPendingRowSettled(const std::vector<std::string>& keys, const std::string& row) {
    const auto key = WargmPendingRowKey(row);
    if (WargmSettledKeyExists(keys, key)) return true;
    const auto opId = JsonScalarAny(row, { "operationId", "OperationId", "id", "Id" });
    return !opId.empty() && WargmSettledKeyExists(keys, "operationId:" + opId);
}

std::string WargmPendingRowRequestBody(const std::string& row) {
    const auto mode = NormalizeDeliveryMode(JsonScalarAny(row, { "mode", "deliveryMode", "DeliveryMode" }));
    std::ostringstream body;
    bool first = true;
    const auto addString = [&](const std::string& key, const std::string& value) {
        if (value.empty()) return;
        if (!first) body << ",";
        first = false;
        body << "\"" << key << "\":\"" << JsonEscape(value) << "\"";
    };
    const auto addNumber = [&](const std::string& key, const std::string& value) {
        auto raw = TrimAscii(value);
        if (raw.empty()) return;
        bool valid = true;
        for (const char ch : raw) {
            if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' || ch == '.')) {
                valid = false;
                break;
            }
        }
        if (!valid) return;
        if (!first) body << ",";
        first = false;
        body << "\"" << key << "\":" << raw;
    };

    body << "{";
    addString("source", FirstNonEmpty({ JsonScalarAny(row, { "source", "Source" }), "native-auto" }));
    addString("steamId", JsonScalarAny(row, { "steamId", "SteamId", "steam_id" }));
    addString("name", FirstNonEmpty({
        JsonScalarAny(row, { "name", "playerName", "recipientName", "recipient" }),
        JsonScalarAny(row, { "Name", "PlayerName", "RecipientName" })
    }));
    addString("runtimeKey", JsonScalarAny(row, { "runtimeKey", "targetRuntimeKey" }));
    addString("mode", mode.empty() ? "item" : mode);

    if (mode == "vehicle") {
        addString("vehicleId", VehicleAdminToken(JsonScalarAny(row, { "vehicleId", "VehicleId", "VehicleAsset", "vehicleAsset" })));
    } else if (mode == "money" || mode == "gold" || mode == "fame" || mode == "setfame") {
        addNumber("amount", JsonNumberTextField(row, "amount", JsonNumberTextField(row, "Amount", "0")));
        if (mode == "gold") addString("currency", "Gold");
        else addString("currency", JsonScalarAny(row, { "currency", "Currency" }));
    } else if (mode == "skill" || mode == "allskills") {
        addString("skill", FirstNonEmpty({
            JsonScalarAny(row, { "skill", "Skill", "skillName", "SkillName" }),
            mode == "allskills" ? std::string("allskills") : std::string()
        }));
        addNumber("level", JsonNumberTextField(row, "level", JsonNumberTextField(row, "SkillLevel", "0")));
        addNumber("experience", JsonNumberTextField(row, "experience", JsonNumberTextField(row, "SkillExperience", "0")));
    } else if (mode == "attributes") {
        addNumber("strength", JsonNumberTextField(row, "strength", JsonNumberTextField(row, "Strength", "0")));
        addNumber("constitution", JsonNumberTextField(row, "constitution", JsonNumberTextField(row, "Constitution", "0")));
        addNumber("dexterity", JsonNumberTextField(row, "dexterity", JsonNumberTextField(row, "Dexterity", "0")));
        addNumber("intelligence", JsonNumberTextField(row, "intelligence", JsonNumberTextField(row, "Intelligence", "0")));
    } else if (mode == "command") {
        addString("command", JsonScalarAny(row, { "command", "CommandTemplate", "commandTemplate" }));
    } else {
        addString("itemsSpec", JsonScalarAny(row, { "itemsSpec", "ItemsSpec" }));
        addString("itemId", JsonScalarAny(row, { "itemId", "ItemId" }));
        addNumber("quantity", JsonNumberTextField(row, "quantity", JsonNumberTextField(row, "Quantity", "1")));
    }
    body << "}";
    return body.str();
}

std::string WargmDeliverPendingJson(const std::wstring& baseDir) {
    bool expected = false;
    if (!g_wargmDeliveryBusy.compare_exchange_strong(expected, true)) {
        return Envelope(false, "Автовыдача Wargm уже выполняется.");
    }
    struct DeliveryBusyGuard { ~DeliveryBusyGuard() { g_wargmDeliveryBusy = false; } } deliveryGuard;

    const auto config = PluginConfigJson(baseDir, "wargm-shop", "{}");
    if (!JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false))) {
        return Envelope(false, "Магазин Wargm выключен в настройках.");
    }
    if (!JsonBoolField(config, "AutoDeliverPending", JsonBoolField(config, "autoDeliverPending", true))) {
        return Envelope(false, "Автовыдача pending-операций Wargm выключена.");
    }

    const auto rows = ExtractArrayObjects(ModuleStateJson(baseDir, "wargm-pending"));
    const auto settledKeys = WargmSettledKeys(baseDir);
    const int maxPerRun = std::clamp(JsonIntField(config, "AutoDeliveryMaxPerRun",
        JsonIntField(config, "autoDeliveryMaxPerRun", 5)), 1, 50);
    const int delaySeconds = std::clamp(JsonIntField(config, "DeliveryDelaySeconds",
        JsonIntField(config, "deliveryDelaySeconds", 0)), 0, 30);

    std::vector<std::string> remaining;
    int scanned = 0;
    int issued = 0;
    int failed = 0;
    int skipped = 0;

    for (const auto& row : rows) {
        ++scanned;
        if (WargmPendingRowSettled(settledKeys, row)) {
            ++skipped;
            continue;
        }
        if (issued >= maxPerRun) {
            remaining.push_back(row);
            continue;
        }

        const auto body = WargmPendingRowRequestBody(row);
        Request autoReq{ "POST", "/api/wargm/manual-deliver", body };
        const auto response = WargmManualDeliver(baseDir, autoReq);
        const bool ok = JsonBoolField(response, "ok", false);
        const auto opId = JsonScalarAny(row, { "operationId", "OperationId", "id", "Id" });
        const auto key = WargmPendingRowKey(row);
        const auto mode = NormalizeDeliveryMode(JsonScalarAny(row, { "mode", "deliveryMode", "DeliveryMode" }));

        if (ok) {
            ++issued;
            AppendModuleStateRecord(baseDir, "wargm-delivered",
                std::string("{\"utc\":\"") + UtcIsoNow() +
                "\",\"status\":\"delivered\",\"source\":\"native-auto\",\"key\":\"" + JsonEscape(key) +
                "\",\"operationId\":\"" + JsonEscape(opId) +
                "\",\"offerId\":\"" + JsonEscape(JsonScalarAny(row, { "offerId", "OfferId" })) +
                "\",\"title\":\"" + JsonEscape(JsonScalarAny(row, { "title", "Title" })) +
                "\",\"steamId\":\"" + JsonEscape(JsonScalarAny(row, { "steamId", "SteamId", "steam_id" })) +
                "\",\"name\":\"" + JsonEscape(FirstNonEmpty({
                    JsonScalarAny(row, { "name", "playerName", "recipientName", "recipient" }),
                    JsonScalarAny(row, { "Name", "PlayerName", "RecipientName" })
                })) +
                "\",\"mode\":\"" + JsonEscape(mode.empty() ? "item" : mode) +
                "\",\"response\":" + JsonValueOrString(response) + "}");
            if (delaySeconds > 0) Sleep(static_cast<DWORD>(delaySeconds * 1000));
            continue;
        }

        ++failed;
        remaining.push_back(row);
        AppendModuleStateRecord(baseDir, "wargm-attempts",
            std::string("{\"utc\":\"") + UtcIsoNow() +
            "\",\"status\":\"failed\",\"source\":\"native-auto\",\"key\":\"" + JsonEscape(key) +
            "\",\"operationId\":\"" + JsonEscape(opId) +
            "\",\"response\":" + JsonValueOrString(response) + "}");
        if (delaySeconds > 0) Sleep(static_cast<DWORD>(delaySeconds * 1000));
    }

    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < remaining.size(); ++i) {
        if (i) out << ",";
        out << remaining[i];
    }
    out << "]";
    WriteTextFile(ModuleStatePath(baseDir, "wargm-pending"), out.str());

    AppendModuleStateRecord(baseDir, "wargm-shop",
        std::string("{\"utc\":\"") + UtcIsoNow() +
        "\",\"status\":\"deliver-pending-finished\",\"scanned\":" + std::to_string(scanned) +
        ",\"issued\":" + std::to_string(issued) +
        ",\"failed\":" + std::to_string(failed) +
        ",\"skipped\":" + std::to_string(skipped) +
        ",\"remaining\":" + std::to_string(remaining.size()) + "}");

    const bool delivered = failed == 0 && remaining.empty();
    const auto resultJson = std::string("{\"delivered\":") + (delivered ? "true" : "false") +
        ",\"scanned\":" + std::to_string(scanned) +
        ",\"issued\":" + std::to_string(issued) +
        ",\"failed\":" + std::to_string(failed) +
        ",\"skipped\":" + std::to_string(skipped) +
        ",\"remaining\":" + std::to_string(remaining.size()) + "}";
    if (failed > 0) {
        return EnvelopeFailureWithJsonData("Не все pending-операции Wargm были выданы.", resultJson);
    }
    return Envelope(true, resultJson);
}

std::string GameStoresConfirmDeliveredJson(const std::wstring& baseDir) {
    std::lock_guard<std::recursive_mutex> operationGuard(g_gameStoresOperationMutex);
    GameStoresOperationLease lease(baseDir);
    if (!lease.acquired()) return Envelope(false, lease.error());
    const auto config = PluginConfigJson(baseDir, "gamestores-shop", "{}");
    if (!JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false))) {
        return Envelope(false, "Магазин GameStores выключен в настройках.");
    }
    const auto shopId = FirstNonEmpty({ JsonScalarAny(config, { "ShopId", "shopId", "StoreId", "storeId" }), "0" });
    const auto secret = JsonScalarAny(config, { "SecretKey", "secretKey", "ApiKey", "apiKey" });
    const auto serverId = FirstNonEmpty({ JsonScalarAny(config, { "ServerId", "serverId", "GameStoresServerId", "gameStoresServerId" }), "0" });
    if (shopId == "0" || TrimAscii(secret).empty()) {
        return Envelope(false, "Не настроены ShopId или SecretKey GameStores.");
    }

    ReconcileGameStoresDeliveredJournal(baseDir);
    auto delivered = ExtractArrayObjects(ModuleStateJson(baseDir, "gamestores-delivered"));
    std::vector<std::string> deliveredKeys;
    for (const auto& row : delivered) {
        AddGameStoresKey(deliveredKeys, JsonScalarAny(row, { "key", "Key" }));
        const auto bucketId = JsonScalarAny(row, { "bucketId", "BucketId", "id", "Id" });
        if (!bucketId.empty()) AddGameStoresKey(deliveredKeys, "id:" + bucketId);
    }
    for (const auto& entry : GameStoresJournalDeliveredEntries(baseDir)) {
        if (GameStoresKeyExists(deliveredKeys, entry.key) ||
            (!entry.bucketId.empty() && GameStoresKeyExists(deliveredKeys, "id:" + entry.bucketId))) {
            continue;
        }
        delivered.push_back(GameStoresJournalDeliveredRecordJson(entry));
        AddGameStoresKey(deliveredKeys, entry.key);
        if (!entry.bucketId.empty()) AddGameStoresKey(deliveredKeys, "id:" + entry.bucketId);
    }
    const auto confirmedRows = ExtractArrayObjects(ModuleStateJson(baseDir, "gamestores-confirmed"));
    std::vector<std::string> confirmedKeys;
    for (const auto& row : confirmedRows) {
        const auto key = JsonScalarAny(row, { "key", "Key" });
        if (!key.empty()) confirmedKeys.push_back(ToLowerAscii(key));
        const auto bucketId = JsonScalarAny(row, { "bucketId", "BucketId", "id", "Id" });
        if (!bucketId.empty()) confirmedKeys.push_back("bucketid:" + ToLowerAscii(bucketId));
    }

    int scanned = 0;
    int acked = 0;
    int failed = 0;
    for (const auto& row : delivered) {
        ++scanned;
        const auto key = JsonScalarAny(row, { "key", "Key" });
        const auto bucketId = JsonScalarAny(row, { "bucketId", "BucketId", "id", "Id" });
        if (bucketId.empty()) continue;
        const auto loweredKey = ToLowerAscii(key);
        const auto loweredBucket = "bucketid:" + ToLowerAscii(bucketId);
        if ((!loweredKey.empty() && std::find(confirmedKeys.begin(), confirmedKeys.end(), loweredKey) != confirmedKeys.end()) ||
            std::find(confirmedKeys.begin(), confirmedKeys.end(), loweredBucket) != confirmedKeys.end()) {
            continue;
        }

        std::vector<std::pair<std::string, std::string>> query = {
            { "shop_id", shopId },
            { "secret", TrimAscii(secret) },
            { "server", serverId },
            { "gived", "true" },
            { "id", bucketId }
        };
        const auto http = HttpGetText(GameStoresApiUrl(query), std::clamp(JsonIntField(config, "HttpTimeoutSeconds", 20), 3, 60));
        if (http.ok && !GameStoresJsonLooksLikeError(http.body)) {
            ++acked;
            if (!loweredKey.empty()) confirmedKeys.push_back(loweredKey);
            confirmedKeys.push_back(loweredBucket);
            AppendModuleStateRecord(baseDir, "gamestores-confirmed",
                std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"confirmed\",\"key\":\"" +
                JsonEscape(key) + "\",\"bucketId\":\"" + JsonEscape(bucketId) + "\"}");
            AppendModuleStateRecord(baseDir, "gamestores-shop",
                std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"confirmed\",\"key\":\"" +
                JsonEscape(key) + "\",\"bucketId\":\"" + JsonEscape(bucketId) + "\"}");
        } else {
            ++failed;
            AppendModuleStateRecord(baseDir, "gamestores-shop",
                std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"confirm-failed\",\"bucketId\":\"" +
                JsonEscape(bucketId) + "\",\"httpStatus\":" + std::to_string(http.status) + "}");
        }
    }
    return Envelope(true, std::string("{\"confirmed\":true,\"scanned\":") + std::to_string(scanned) +
        ",\"acked\":" + std::to_string(acked) +
        ",\"failed\":" + std::to_string(failed) + "}");
}

std::string GameStoresDeliverPendingJson(const std::wstring& baseDir) {
    std::lock_guard<std::recursive_mutex> operationGuard(g_gameStoresOperationMutex);
    GameStoresOperationLease lease(baseDir);
    if (!lease.acquired()) return Envelope(false, lease.error());
    bool expected = false;
    if (!g_gameStoresDeliveryBusy.compare_exchange_strong(expected, true)) {
        return Envelope(false, "Автовыдача GameStores уже выполняется.");
    }
    struct DeliveryBusyGuard { ~DeliveryBusyGuard() { g_gameStoresDeliveryBusy = false; } } deliveryGuard;

    const auto config = PluginConfigJson(baseDir, "gamestores-shop", "{}");
    if (!JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false))) {
        return Envelope(false, "Магазин GameStores выключен в настройках.");
    }
    if (!JsonBoolField(config, "AutoDeliverPending", JsonBoolField(config, "autoDeliverPending", true))) {
        return Envelope(false, "Автовыдача pending-покупок GameStores выключена.");
    }

    ReconcileGameStoresDeliveredJournal(baseDir);
    const auto shopId = FirstNonEmpty({ JsonScalarAny(config, { "ShopId", "shopId", "StoreId", "storeId" }), "0" });
    const auto serverId = FirstNonEmpty({ JsonScalarAny(config, { "ServerId", "serverId", "GameStoresServerId", "gameStoresServerId" }), "0" });
    const auto rows = ExtractArrayObjects(ModuleStateJson(baseDir, "gamestores-pending"));
    auto settledKeys = GameStoresSettledKeys(baseDir);
    const auto attempts = GameStoresAttemptIndex(baseDir);
    const int maxPerRun = std::clamp(JsonIntField(config, "AutoDeliveryMaxPerRun",
        JsonIntField(config, "autoDeliveryMaxPerRun", 5)), 1, 50);
    const int deliveryDelaySeconds = std::clamp(JsonIntField(config, "DeliveryDelaySeconds",
        JsonIntField(config, "deliveryDelaySeconds", 0)), 0, 86400);
    const int retryDelaySeconds = std::clamp(JsonIntField(config, "RetryDelaySeconds",
        JsonIntField(config, "retryDelaySeconds", 60)), 0, 86400);
    const int maxAttempts = std::max(0, JsonIntField(config, "MaxDeliveryAttempts",
        JsonIntField(config, "maxDeliveryAttempts", 0)));
    const auto now = std::time(nullptr);

    std::vector<std::string> remaining;
    int scanned = 0;
    int issued = 0;
    int failed = 0;
    int skipped = 0;

    for (const auto& row : rows) {
        ++scanned;
        if (GameStoresRowSettled(settledKeys, row, shopId, serverId)) {
            ++skipped;
            continue;
        }
        if (issued >= maxPerRun) {
            remaining.push_back(row);
            ++skipped;
            continue;
        }

        const auto bucketId = JsonScalarAny(row, { "bucketId", "BucketId", "id", "Id" });
        const auto key = GameStoresPendingKey(row, shopId, serverId);
        const auto mode = NormalizeDeliveryMode(JsonScalarAny(row, { "mode", "deliveryMode", "DeliveryMode" }));
        if (key.empty()) {
            ++failed;
            remaining.push_back(row);
            AppendModuleStateRecord(baseDir, "gamestores-attempts",
                std::string("{\"utc\":\"") + UtcIsoNow() +
                "\",\"status\":\"failed\",\"source\":\"native-auto\",\"bucketId\":\"" + JsonEscape(bucketId) +
                "\",\"message\":\"Pending GameStores row has no stable key.\"}");
            continue;
        }

        const auto journal = InspectGameStoresJournal(baseDir, key);
        if (journal.state != GameStoresJournalState::None) {
            ++failed;
            remaining.push_back(row);
            continue;
        }

        if (deliveryDelaySeconds > 0) {
            std::time_t createdAt = 0;
            if (TryParseUtcIsoSeconds(JsonScalarAny(row, { "createdAtUtc", "CreatedAtUtc", "utc", "Utc" }), createdAt) &&
                now > 0 && std::difftime(now, createdAt) < deliveryDelaySeconds) {
                ++skipped;
                remaining.push_back(row);
                continue;
            }
        }

        const auto attemptIt = attempts.find(ToLowerAscii(key));
        if (attemptIt != attempts.end()) {
            if (attemptIt->second.hasUnresolvedDispatch) {
                const auto legacyEntry = CreateGameStoresJournalEntry(
                    key, bucketId,
                    JsonScalarAny(row, { "steamId", "SteamId", "steam_id" }),
                    FirstNonEmpty({
                        JsonScalarAny(row, { "name", "playerName", "recipientName", "recipient" }),
                        JsonScalarAny(row, { "Name", "PlayerName", "RecipientName" })
                    }),
                    "native-auto", "native");
                GameStoresJournalWriteMarker(baseDir, legacyEntry, GameStoresJournalState::Uncertain, "legacy-dispatch-without-journal");
                ++failed;
                remaining.push_back(row);
                AppendModuleStateRecord(baseDir, "gamestores-shop",
                    std::string("{\"utc\":\"") + UtcIsoNow() +
                    "\",\"status\":\"delivery-blocked\",\"key\":\"" + JsonEscape(key) +
                    "\",\"bucketId\":\"" + JsonEscape(bucketId) +
                    "\",\"message\":\"Previous GameStores dispatch has no terminal journal marker; reissue is blocked.\"}");
                continue;
            }
            if (maxAttempts > 0 && attemptIt->second.count >= maxAttempts) {
                ++failed;
                remaining.push_back(row);
                AppendModuleStateRecord(baseDir, "gamestores-attempts",
                    std::string("{\"utc\":\"") + UtcIsoNow() +
                    "\",\"status\":\"max-attempts\",\"source\":\"native-auto\",\"key\":\"" + JsonEscape(key) +
                    "\",\"bucketId\":\"" + JsonEscape(bucketId) +
                    "\",\"attempts\":" + std::to_string(attemptIt->second.count) + "}");
                AppendModuleStateRecord(baseDir, "gamestores-shop",
                    std::string("{\"utc\":\"") + UtcIsoNow() +
                    "\",\"status\":\"max-attempts\",\"key\":\"" + JsonEscape(key) +
                    "\",\"bucketId\":\"" + JsonEscape(bucketId) +
                    "\",\"message\":\"Достигнут лимит попыток выдачи GameStores.\"}");
                continue;
            }
            if (retryDelaySeconds > 0 && attemptIt->second.lastAttemptUtc > 0 &&
                now > 0 && std::difftime(now, attemptIt->second.lastAttemptUtc) < retryDelaySeconds) {
                ++skipped;
                remaining.push_back(row);
                continue;
            }
        }

        const auto journalEntry = CreateGameStoresJournalEntry(
            key, bucketId,
            JsonScalarAny(row, { "steamId", "SteamId", "steam_id" }),
            FirstNonEmpty({
                JsonScalarAny(row, { "name", "playerName", "recipientName", "recipient" }),
                JsonScalarAny(row, { "Name", "PlayerName", "RecipientName" })
            }),
            "native-auto", "native");
        if (!GameStoresJournalWriteMarker(baseDir, journalEntry, GameStoresJournalState::Reserved) ||
            !GameStoresJournalWriteMarker(baseDir, journalEntry, GameStoresJournalState::Dispatching)) {
            ++failed;
            remaining.push_back(row);
            AppendModuleStateRecord(baseDir, "gamestores-shop",
                std::string("{\"utc\":\"") + UtcIsoNow() +
                "\",\"status\":\"delivery-blocked\",\"key\":\"" + JsonEscape(key) +
                "\",\"bucketId\":\"" + JsonEscape(bucketId) +
                "\",\"message\":\"GameStores journal marker was not created; delivery was not sent.\"}");
            continue;
        }

        const auto body = WargmPendingRowRequestBody(row);
        AppendModuleStateRecord(baseDir, "gamestores-attempts",
            std::string("{\"utc\":\"") + UtcIsoNow() +
            "\",\"status\":\"delivering\",\"source\":\"native-auto\",\"key\":\"" + JsonEscape(key) +
            "\",\"bucketId\":\"" + JsonEscape(bucketId) + "\"}");
        Request autoReq{ "POST", "/api/gamestores/manual-deliver", body };
        const auto response = GameStoresManualDeliver(baseDir, autoReq);
        const bool ok = JsonBoolField(response, "ok", false);

        if (ok) {
            if (!GameStoresJournalWriteMarker(baseDir, journalEntry, GameStoresJournalState::Delivered)) {
                ++failed;
                remaining.push_back(row);
                AppendModuleStateRecord(baseDir, "gamestores-attempts",
                    std::string("{\"utc\":\"") + UtcIsoNow() +
                    "\",\"status\":\"uncertain\",\"source\":\"native-auto\",\"key\":\"" + JsonEscape(key) +
                    "\",\"bucketId\":\"" + JsonEscape(bucketId) +
                    "\",\"message\":\"Game grant returned success but delivered journal marker was not saved.\"}");
                continue;
            }
            ++issued;
            AppendModuleStateRecord(baseDir, "gamestores-delivered",
                std::string("{\"utc\":\"") + UtcIsoNow() +
                "\",\"status\":\"delivered\",\"source\":\"native-auto\",\"key\":\"" + JsonEscape(key) +
                "\",\"bucketId\":\"" + JsonEscape(bucketId) +
                "\",\"productId\":\"" + JsonEscape(JsonScalarAny(row, { "productId", "ProductId" })) +
                "\",\"title\":\"" + JsonEscape(JsonScalarAny(row, { "title", "Title" })) +
                "\",\"steamId\":\"" + JsonEscape(JsonScalarAny(row, { "steamId", "SteamId", "steam_id" })) +
                "\",\"name\":\"" + JsonEscape(FirstNonEmpty({
                    JsonScalarAny(row, { "name", "playerName", "recipientName", "recipient" }),
                    JsonScalarAny(row, { "Name", "PlayerName", "RecipientName" })
                })) +
                "\",\"mode\":\"" + JsonEscape(mode.empty() ? "item" : mode) +
                "\",\"response\":" + JsonValueOrString(response) + "}");
            AddGameStoresRowKeys(settledKeys, row, shopId, serverId);
            continue;
        }

        GameStoresJournalWriteMarker(baseDir, journalEntry, GameStoresJournalState::Uncertain, "delivery-result-not-ok");
        ++failed;
        remaining.push_back(row);
        AppendModuleStateRecord(baseDir, "gamestores-attempts",
            std::string("{\"utc\":\"") + UtcIsoNow() +
            "\",\"status\":\"uncertain\",\"source\":\"native-auto\",\"key\":\"" + JsonEscape(key) +
            "\",\"bucketId\":\"" + JsonEscape(bucketId) +
            "\",\"response\":" + JsonValueOrString(response) + "}");
        AppendModuleStateRecord(baseDir, "gamestores-shop",
            std::string("{\"utc\":\"") + UtcIsoNow() +
            "\",\"status\":\"delivery-uncertain\",\"key\":\"" + JsonEscape(key) +
            "\",\"bucketId\":\"" + JsonEscape(bucketId) +
            "\",\"message\":\"Выдача GameStores не подтвердилась; повторная выдача заблокирована до сверки.\"}");
    }

    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < remaining.size(); ++i) {
        if (i) out << ",";
        out << remaining[i];
    }
    out << "]";
    WriteTextFile(ModuleStatePath(baseDir, "gamestores-pending"), out.str());

    AppendModuleStateRecord(baseDir, "gamestores-shop",
        std::string("{\"utc\":\"") + UtcIsoNow() +
        "\",\"status\":\"deliver-pending-finished\",\"scanned\":" + std::to_string(scanned) +
        ",\"issued\":" + std::to_string(issued) +
        ",\"failed\":" + std::to_string(failed) +
        ",\"skipped\":" + std::to_string(skipped) +
        ",\"remaining\":" + std::to_string(remaining.size()) + "}");

    const bool delivered = failed == 0 && remaining.empty();
    const auto resultJson = std::string("{\"delivered\":") + (delivered ? "true" : "false") +
        ",\"scanned\":" + std::to_string(scanned) +
        ",\"issued\":" + std::to_string(issued) +
        ",\"failed\":" + std::to_string(failed) +
        ",\"skipped\":" + std::to_string(skipped) +
        ",\"remaining\":" + std::to_string(remaining.size()) + "}";
    if (failed > 0) {
        return EnvelopeFailureWithJsonData("Не все pending-покупки GameStores были выданы.", resultJson);
    }
    return Envelope(true, resultJson);
}

std::string GameStoresProcessJson(const std::wstring& baseDir) {
    std::lock_guard<std::recursive_mutex> operationGuard(g_gameStoresOperationMutex);
    GameStoresOperationLease lease(baseDir);
    if (!lease.acquired()) return Envelope(false, lease.error());
    const auto sync = GameStoresSyncJson(baseDir);
    const auto deliver = GameStoresDeliverPendingJson(baseDir);
    const auto confirm = GameStoresConfirmDeliveredJson(baseDir);
    return Envelope(true, std::string("{\"sync\":") + sync + ",\"delivery\":" + deliver + ",\"confirm\":" + confirm + "}");
}

bool GameStoresSameClaimTarget(const std::string& row, const std::string& steamId, const std::string& name, const std::string& runtimeKey) {
    const auto rowSteamId = TrimAscii(JsonScalarAny(row, { "steamId", "SteamId", "steam_id" }));
    const auto rowName = TrimAscii(FirstNonEmpty({
        JsonScalarAny(row, { "name", "playerName", "recipientName", "recipient" }),
        JsonScalarAny(row, { "Name", "PlayerName", "RecipientName" })
    }));
    const auto rowRuntimeKey = TrimAscii(JsonScalarAny(row, { "runtimeKey", "targetRuntimeKey" }));

    if (!steamId.empty() && !rowSteamId.empty() && ToLowerAscii(rowSteamId) == ToLowerAscii(steamId)) return true;
    if (!runtimeKey.empty() && !rowRuntimeKey.empty() && ToLowerAscii(rowRuntimeKey) == ToLowerAscii(runtimeKey)) return true;
    if (!name.empty() && !rowName.empty() && ToLowerAscii(rowName) == ToLowerAscii(name)) return true;
    return false;
}

std::string GameStoresDeliverClaimedPendingJson(
    const std::wstring& baseDir,
    const std::string& steamId,
    const std::string& name,
    const std::string& runtimeKey,
    int maxPerRun,
    const std::string& source) {
    std::lock_guard<std::recursive_mutex> operationGuard(g_gameStoresOperationMutex);
    GameStoresOperationLease lease(baseDir);
    if (!lease.acquired()) return Envelope(false, lease.error());
    bool expected = false;
    if (!g_gameStoresDeliveryBusy.compare_exchange_strong(expected, true)) {
        return Envelope(false, "Выдача GameStores уже выполняется.");
    }
    struct DeliveryBusyGuard { ~DeliveryBusyGuard() { g_gameStoresDeliveryBusy = false; } } deliveryGuard;

    const auto config = PluginConfigJson(baseDir, "gamestores-shop", "{}");
    if (!JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false))) {
        return Envelope(false, "Магазин GameStores выключен в настройках.");
    }

    ReconcileGameStoresDeliveredJournal(baseDir);
    const auto shopId = FirstNonEmpty({ JsonScalarAny(config, { "ShopId", "shopId", "StoreId", "storeId" }), "0" });
    const auto serverId = FirstNonEmpty({ JsonScalarAny(config, { "ServerId", "serverId", "GameStoresServerId", "gameStoresServerId" }), "0" });
    auto settledKeys = GameStoresSettledKeys(baseDir);
    const auto attempts = GameStoresAttemptIndex(baseDir);
    const int effectiveMax = std::clamp(maxPerRun > 0 ? maxPerRun : JsonIntField(config, "ManualClaimMaxPerRun",
        JsonIntField(config, "manualClaimMaxPerRun", JsonIntField(config, "AutoDeliveryMaxPerRun",
            JsonIntField(config, "autoDeliveryMaxPerRun", 5)))), 1, 50);
    const int retryDelaySeconds = std::clamp(JsonIntField(config, "RetryDelaySeconds",
        JsonIntField(config, "retryDelaySeconds", 60)), 0, 86400);
    const int maxAttempts = std::max(0, JsonIntField(config, "MaxDeliveryAttempts",
        JsonIntField(config, "maxDeliveryAttempts", 0)));
    const auto now = std::time(nullptr);

    std::vector<std::string> remaining;
    int scanned = 0;
    int matched = 0;
    int issued = 0;
    int failed = 0;
    int skipped = 0;

    for (const auto& row : ExtractArrayObjects(ModuleStateJson(baseDir, "gamestores-pending"))) {
        ++scanned;
        if (GameStoresRowSettled(settledKeys, row, shopId, serverId)) {
            ++skipped;
            continue;
        }
        if (!GameStoresSameClaimTarget(row, steamId, name, runtimeKey)) {
            remaining.push_back(row);
            continue;
        }
        ++matched;
        if (issued >= effectiveMax) {
            remaining.push_back(row);
            ++skipped;
            continue;
        }

        const auto bucketId = JsonScalarAny(row, { "bucketId", "BucketId", "id", "Id" });
        const auto key = GameStoresPendingKey(row, shopId, serverId);
        const auto mode = NormalizeDeliveryMode(JsonScalarAny(row, { "mode", "deliveryMode", "DeliveryMode" }));
        if (key.empty()) {
            ++failed;
            remaining.push_back(row);
            AppendModuleStateRecord(baseDir, "gamestores-attempts",
                std::string("{\"utc\":\"") + UtcIsoNow() +
                "\",\"status\":\"failed\",\"source\":\"" + JsonEscape(source) +
                "\",\"bucketId\":\"" + JsonEscape(bucketId) +
                "\",\"message\":\"Pending GameStores row has no stable key.\"}");
            continue;
        }

        const auto journal = InspectGameStoresJournal(baseDir, key);
        if (journal.state != GameStoresJournalState::None) {
            ++failed;
            remaining.push_back(row);
            continue;
        }

        const auto attemptIt = attempts.find(ToLowerAscii(key));
        if (attemptIt != attempts.end()) {
            if (attemptIt->second.hasUnresolvedDispatch) {
                const auto legacyEntry = CreateGameStoresJournalEntry(
                    key, bucketId,
                    JsonScalarAny(row, { "steamId", "SteamId", "steam_id" }),
                    FirstNonEmpty({
                        JsonScalarAny(row, { "name", "playerName", "recipientName", "recipient" }),
                        JsonScalarAny(row, { "Name", "PlayerName", "RecipientName" })
                    }),
                    source, "native");
                GameStoresJournalWriteMarker(baseDir, legacyEntry, GameStoresJournalState::Uncertain, "legacy-dispatch-without-journal");
                ++failed;
                remaining.push_back(row);
                AppendModuleStateRecord(baseDir, "gamestores-shop",
                    std::string("{\"utc\":\"") + UtcIsoNow() +
                    "\",\"status\":\"claim-blocked\",\"key\":\"" + JsonEscape(key) +
                    "\",\"bucketId\":\"" + JsonEscape(bucketId) +
                    "\",\"message\":\"Previous GameStores dispatch has no terminal journal marker; reissue is blocked.\"}");
                continue;
            }
            if (maxAttempts > 0 && attemptIt->second.count >= maxAttempts) {
                ++failed;
                remaining.push_back(row);
                AppendModuleStateRecord(baseDir, "gamestores-shop",
                    std::string("{\"utc\":\"") + UtcIsoNow() +
                    "\",\"status\":\"claim-max-attempts\",\"key\":\"" + JsonEscape(key) +
                    "\",\"bucketId\":\"" + JsonEscape(bucketId) + "\"}");
                continue;
            }
            if (retryDelaySeconds > 0 && attemptIt->second.lastAttemptUtc > 0 &&
                now > 0 && std::difftime(now, attemptIt->second.lastAttemptUtc) < retryDelaySeconds) {
                ++skipped;
                remaining.push_back(row);
                continue;
            }
        }

        const auto journalEntry = CreateGameStoresJournalEntry(
            key, bucketId,
            JsonScalarAny(row, { "steamId", "SteamId", "steam_id" }),
            FirstNonEmpty({
                JsonScalarAny(row, { "name", "playerName", "recipientName", "recipient" }),
                JsonScalarAny(row, { "Name", "PlayerName", "RecipientName" })
            }),
            source, "native");
        if (!GameStoresJournalWriteMarker(baseDir, journalEntry, GameStoresJournalState::Reserved) ||
            !GameStoresJournalWriteMarker(baseDir, journalEntry, GameStoresJournalState::Dispatching)) {
            ++failed;
            remaining.push_back(row);
            AppendModuleStateRecord(baseDir, "gamestores-shop",
                std::string("{\"utc\":\"") + UtcIsoNow() +
                "\",\"status\":\"claim-blocked\",\"key\":\"" + JsonEscape(key) +
                "\",\"bucketId\":\"" + JsonEscape(bucketId) +
                "\",\"message\":\"GameStores journal marker was not created; delivery was not sent.\"}");
            continue;
        }

        const auto body = WargmPendingRowRequestBody(row);
        AppendModuleStateRecord(baseDir, "gamestores-attempts",
            std::string("{\"utc\":\"") + UtcIsoNow() +
            "\",\"status\":\"claim-delivering\",\"source\":\"" + JsonEscape(source) +
            "\",\"key\":\"" + JsonEscape(key) +
            "\",\"bucketId\":\"" + JsonEscape(bucketId) + "\"}");
        Request autoReq{ "POST", "/api/gamestores/manual-deliver", body };
        const auto response = GameStoresManualDeliver(baseDir, autoReq);
        const bool ok = JsonBoolField(response, "ok", false);

        if (ok) {
            if (!GameStoresJournalWriteMarker(baseDir, journalEntry, GameStoresJournalState::Delivered)) {
                ++failed;
                remaining.push_back(row);
                AppendModuleStateRecord(baseDir, "gamestores-attempts",
                    std::string("{\"utc\":\"") + UtcIsoNow() +
                    "\",\"status\":\"claim-uncertain\",\"source\":\"" + JsonEscape(source) +
                    "\",\"key\":\"" + JsonEscape(key) +
                    "\",\"bucketId\":\"" + JsonEscape(bucketId) +
                    "\",\"message\":\"Game grant returned success but delivered journal marker was not saved.\"}");
                continue;
            }
            ++issued;
            AppendModuleStateRecord(baseDir, "gamestores-delivered",
                std::string("{\"utc\":\"") + UtcIsoNow() +
                "\",\"status\":\"delivered\",\"source\":\"" + JsonEscape(source) +
                "\",\"key\":\"" + JsonEscape(key) +
                "\",\"bucketId\":\"" + JsonEscape(bucketId) +
                "\",\"productId\":\"" + JsonEscape(JsonScalarAny(row, { "productId", "ProductId" })) +
                "\",\"title\":\"" + JsonEscape(JsonScalarAny(row, { "title", "Title" })) +
                "\",\"steamId\":\"" + JsonEscape(JsonScalarAny(row, { "steamId", "SteamId", "steam_id" })) +
                "\",\"name\":\"" + JsonEscape(FirstNonEmpty({
                    JsonScalarAny(row, { "name", "playerName", "recipientName", "recipient" }),
                    JsonScalarAny(row, { "Name", "PlayerName", "RecipientName" })
                })) +
                "\",\"mode\":\"" + JsonEscape(mode.empty() ? "item" : mode) +
                "\",\"response\":" + JsonValueOrString(response) + "}");
            AddGameStoresRowKeys(settledKeys, row, shopId, serverId);
            continue;
        }

        GameStoresJournalWriteMarker(baseDir, journalEntry, GameStoresJournalState::Uncertain, "claim-result-not-ok");
        ++failed;
        remaining.push_back(row);
        AppendModuleStateRecord(baseDir, "gamestores-attempts",
            std::string("{\"utc\":\"") + UtcIsoNow() +
            "\",\"status\":\"claim-uncertain\",\"source\":\"" + JsonEscape(source) +
            "\",\"key\":\"" + JsonEscape(key) +
            "\",\"bucketId\":\"" + JsonEscape(bucketId) +
            "\",\"response\":" + JsonValueOrString(response) + "}");
    }

    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < remaining.size(); ++i) {
        if (i) out << ",";
        out << remaining[i];
    }
    out << "]";
    WriteTextFile(ModuleStatePath(baseDir, "gamestores-pending"), out.str());

    AppendModuleStateRecord(baseDir, "gamestores-shop",
        std::string("{\"utc\":\"") + UtcIsoNow() +
        "\",\"status\":\"claim-finished\",\"source\":\"" + JsonEscape(source) +
        "\",\"steamId\":\"" + JsonEscape(steamId) +
        "\",\"name\":\"" + JsonEscape(name) +
        "\",\"scanned\":" + std::to_string(scanned) +
        ",\"matched\":" + std::to_string(matched) +
        ",\"issued\":" + std::to_string(issued) +
        ",\"failed\":" + std::to_string(failed) +
        ",\"skipped\":" + std::to_string(skipped) +
        ",\"remaining\":" + std::to_string(remaining.size()) + "}");

    const auto resultJson = std::string("{\"claimed\":true") +
        ",\"scanned\":" + std::to_string(scanned) +
        ",\"matched\":" + std::to_string(matched) +
        ",\"issued\":" + std::to_string(issued) +
        ",\"failed\":" + std::to_string(failed) +
        ",\"skipped\":" + std::to_string(skipped) +
        ",\"remaining\":" + std::to_string(remaining.size()) + "}";
    if (failed > 0) {
        return EnvelopeFailureWithJsonData("Не все покупки GameStores были выданы по /pay.", resultJson);
    }
    return Envelope(true, resultJson);
}

std::string GameStoresClaimJson(const std::wstring& baseDir, const Request& req) {
    std::lock_guard<std::recursive_mutex> operationGuard(g_gameStoresOperationMutex);
    GameStoresOperationLease lease(baseDir);
    if (!lease.acquired()) return Envelope(false, lease.error());
    const auto body = req.body.empty() ? std::string("{}") : req.body;
    const auto steamId = TrimAscii(JsonScalarAny(body, { "steamId", "SteamId", "steam_id" }));
    const auto name = TrimAscii(FirstNonEmpty({
        JsonScalarAny(body, { "name", "playerName", "recipientName", "recipient" }),
        JsonScalarAny(body, { "Name", "PlayerName", "RecipientName" })
    }));
    const auto runtimeKey = TrimAscii(JsonScalarAny(body, { "runtimeKey", "targetRuntimeKey" }));
    if (steamId.empty() && name.empty() && runtimeKey.empty()) {
        return Envelope(false, "Не удалось определить игрока для выдачи GameStores.");
    }

    const auto sync = GameStoresSyncJson(baseDir);
    const int maxPerRun = std::clamp(JsonIntField(body, "maxPerRun", JsonIntField(body, "MaxPerRun", 0)), 0, 50);
    const auto delivery = GameStoresDeliverClaimedPendingJson(baseDir, steamId, name, runtimeKey, maxPerRun, "player-claim");
    const auto confirm = GameStoresConfirmDeliveredJson(baseDir);
    return Envelope(true, std::string("{\"sync\":") + sync + ",\"delivery\":" + delivery + ",\"confirm\":" + confirm + "}");
}

struct ScheduledPoint {
    bool ok = false;
    std::string name;
    std::string group;
    double x = 0;
    double y = 0;
    double z = 0;
    int radius = 0;
};

struct ScheduledItemSet {
    bool ok = false;
    std::string name;
    int weight = 1;
    std::vector<GrantItem> items;
};

double JsonDoubleConfigField(const std::string& object, const std::string& key, double fallback = 0.0) {
    auto raw = JsonNumberTextField(object, key, "");
    if (raw.empty() && !key.empty()) {
        std::string alt = key;
        alt[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(alt[0])));
        raw = JsonNumberTextField(object, alt, "");
    }
    if (raw.empty()) return fallback;
    try {
        return std::stod(raw);
    } catch (...) {
        return fallback;
    }
}

std::vector<GrantItem> ParseItemsTextSpec(const std::string& text) {
    std::vector<GrantItem> items;
    std::string normalized = text;
    std::replace(normalized.begin(), normalized.end(), '\r', ';');
    std::replace(normalized.begin(), normalized.end(), '\n', ';');
    std::stringstream ss(normalized);
    std::string part;
    while (std::getline(ss, part, ';')) {
        part = TrimAscii(part);
        if (part.empty()) continue;
        auto sep = part.find('|');
        if (sep == std::string::npos) sep = part.find(':');
        auto itemId = sep == std::string::npos ? part : part.substr(0, sep);
        auto qtyText = sep == std::string::npos ? std::string("1") : part.substr(sep + 1);
        itemId = TrimAscii(itemId);
        if (!IsSafeScumAdminToken(itemId)) continue;
        const int quantity = std::clamp(std::atoi(TrimAscii(qtyText).c_str()), 1, 100);
        items.push_back(GrantItem{ itemId, quantity });
    }
    return items;
}

ScheduledPoint SelectScheduledPoint(const std::string& config, const std::string& group, std::time_t seed) {
    std::vector<ScheduledPoint> matches;
    const auto wanted = ToLowerAscii(TrimAscii(group));
    for (const auto& object : ExtractArrayObjects(JsonRawField(config, "Points"))) {
        ScheduledPoint point;
        point.name = JsonStringField(object, "Name");
        if (point.name.empty()) point.name = JsonStringField(object, "name");
        point.group = JsonStringField(object, "Group");
        if (point.group.empty()) point.group = JsonStringField(object, "group");
        if (!wanted.empty() && ToLowerAscii(TrimAscii(point.group)) != wanted) continue;
        point.x = JsonDoubleConfigField(object, "X", 0);
        point.y = JsonDoubleConfigField(object, "Y", 0);
        point.z = JsonDoubleConfigField(object, "Z", 0);
        point.radius = std::clamp(JsonIntField(object, "Radius", JsonIntField(object, "radius", 0)), 0, 100000);
        point.ok = true;
        matches.push_back(point);
    }
    if (matches.empty()) return {};
    auto selected = matches[static_cast<size_t>(std::llabs(static_cast<long long>(seed)) % matches.size())];
    if (selected.radius > 0) {
        uint64_t value = static_cast<uint64_t>(seed) ^ 0x9E3779B97F4A7C15ULL;
        const auto jitter = [&value](int radius) {
            value = value * 6364136223846793005ULL + 1442695040888963407ULL;
            const int span = radius * 2 + 1;
            return static_cast<int>(value % static_cast<uint64_t>(span)) - radius;
        };
        selected.x += jitter(selected.radius);
        selected.y += jitter(selected.radius);
    }
    return selected;
}

std::vector<ScheduledPoint> SelectScheduledPoints(const std::string& config, const std::string& group, std::time_t seed, int count) {
    std::vector<ScheduledPoint> selected;
    std::vector<std::string> identities;
    count = std::clamp(count, 1, 10);
    for (int attempt = 0; attempt < count * 4 && static_cast<int>(selected.size()) < count; ++attempt) {
        const auto point = SelectScheduledPoint(config, group, seed + attempt);
        if (!point.ok) break;
        const auto identity = ToLowerAscii(TrimAscii(point.name)) + "|" +
            JsonDoubleValue(point.x) + "|" + JsonDoubleValue(point.y) + "|" + JsonDoubleValue(point.z);
        bool duplicate = false;
        for (const auto& existing : identities) {
            if (existing == identity) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        identities.push_back(identity);
        selected.push_back(point);
    }
    return selected;
}

ScheduledItemSet SelectScheduledItemSet(const std::string& config, const std::string& name, std::time_t seed) {
    std::vector<ScheduledItemSet> sets;
    const auto wanted = ToLowerAscii(TrimAscii(name));
    int totalWeight = 0;
    for (const auto& object : ExtractArrayObjects(JsonRawField(config, "ItemSets"))) {
        ScheduledItemSet set;
        set.name = JsonStringField(object, "Name");
        if (set.name.empty()) set.name = JsonStringField(object, "name");
        if (!wanted.empty() && ToLowerAscii(TrimAscii(set.name)) != wanted) continue;
        set.weight = std::max(1, JsonIntField(object, "Weight", JsonIntField(object, "weight", 1)));
        set.items = ParseItemsTextSpec(JsonStringField(object, "ItemsText"));
        if (set.items.empty()) set.items = ParseGrantItems(object);
        if (set.items.empty()) continue;
        set.ok = true;
        totalWeight += set.weight;
        sets.push_back(set);
    }
    if (sets.empty()) return {};
    if (!wanted.empty() || totalWeight <= 0) return sets.front();
    int pick = static_cast<int>(std::llabs(static_cast<long long>(seed)) % totalWeight);
    for (const auto& set : sets) {
        if (pick < set.weight) return set;
        pick -= set.weight;
    }
    return sets.back();
}

std::string ScheduledPointLocation(const ScheduledPoint& point) {
    return JsonDoubleValue(point.x) + " " + JsonDoubleValue(point.y) + " " + JsonDoubleValue(point.z);
}

std::string FillScheduledTemplate(
    std::string text,
    const ScheduledPoint& point,
    const std::string& itemSetName,
    const std::string& worldEventClass = "") {
    const auto replaceAll = [](std::string& target, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = target.find(from, pos)) != std::string::npos) {
            target.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(text, "{X}", point.ok ? JsonDoubleValue(point.x) : "0");
    replaceAll(text, "{Y}", point.ok ? JsonDoubleValue(point.y) : "0");
    replaceAll(text, "{Z}", point.ok ? JsonDoubleValue(point.z) : "0");
    replaceAll(text, "{Point}", point.ok ? (point.name.empty() ? ScheduledPointLocation(point) : point.name) : "карта");
    replaceAll(text, "{Group}", point.group);
    replaceAll(text, "{ItemSet}", itemSetName);
    replaceAll(text, "{WorldEventClass}", worldEventClass);
    replaceAll(text, "{EventClass}", worldEventClass);
    return text;
}

std::string ScheduledWorldEventClass(const std::string& job) {
    auto eventClass = JsonStringField(job, "WorldEventClass");
    if (eventClass.empty()) eventClass = JsonStringField(job, "worldEventClass");
    if (eventClass.empty()) eventClass = JsonStringField(job, "EventClass");
    if (eventClass.empty()) eventClass = JsonStringField(job, "eventClass");
    eventClass = TrimAscii(eventClass);
    if (eventClass.empty()) return "BP_CargoDropEvent";
    return IsSafeScumAdminToken(eventClass) ? eventClass : std::string();
}

struct ScheduledCommandResult {
    bool ok = false;
    int status = 0;
    std::string message;
    std::string body;
    std::string transport;
};

bool ScheduledAdminTextLooksRejected(const std::string& text) {
    const auto lowered = ToLowerAscii(text);
    const std::vector<std::string> patterns = {
        "not authorized",
        "not authorised",
        "not allowed",
        "permission denied",
        "unknown command",
        "cannot spawn",
        "can't spawn",
        "cannot execute",
        "failed to execute",
        "command failed",
        "не авториз",
        "нет прав",
        "неизвестная команда",
        "нельзя выполнить"
    };
    for (const auto& pattern : patterns) {
        if (lowered.find(pattern) != std::string::npos) return true;
    }
    return false;
}

void ApplyScheduledAdminFailureClassifier(ScheduledCommandResult& result) {
    if (!result.ok) return;
    const auto text = result.message + "\n" + result.body;
    if (!ScheduledAdminTextLooksRejected(text)) return;
    result.ok = false;
    if (result.message.empty()) result.message = result.body;
    result.message = "SCUM отклонил admin-команду: " + result.message;
}

ScheduledCommandResult SendScheduledAdminCommand(const std::wstring& baseDir, std::string command, int timeoutMs = 20000) {
    ScheduledCommandResult result;
    command = TrimAscii(command);
    if (command.empty()) {
        result.message = "Команда не задана.";
        return result;
    }
    if (command.front() != '#') command = "#" + command;
    if (command.find('\r') != std::string::npos || command.find('\n') != std::string::npos) {
        result.message = "Команда должна быть одной строкой.";
        return result;
    }
    if (LocalRconEnabled(baseDir)) {
        const auto rcon = LocalRconSendConsoleCommand(baseDir, command);
        result.ok = rcon.ok;
        result.status = rcon.status;
        result.body = rcon.body;
        result.message = rcon.message.empty() ? rcon.body : rcon.message;
        result.transport = "server-command";
        ApplyScheduledAdminFailureClassifier(result);
        return result;
    }
    if (ArkPanelEnabled(baseDir)) {
        const auto rcon = ArkPanelSendConsoleCommand(baseDir, command);
        result.ok = rcon.ok;
        result.status = rcon.status;
        result.body = rcon.body;
        result.message = rcon.message.empty() ? rcon.body : rcon.message;
        result.transport = "hosting-panel-rcon";
        ApplyScheduledAdminFailureClassifier(result);
        return result;
    }
    const auto br = BridgeExecute(baseDir, "admin_exec",
        std::string("{\"commandText\":\"") + JsonEscape(command) + "\"}", timeoutMs);
    result.ok = br.ok;
    result.body = br.body;
    result.message = br.ok ? br.body : br.message;
    result.transport = "ue4ss-bridge";
    ApplyScheduledAdminFailureClassifier(result);
    return result;
}

void SendScheduledAnnouncement(const std::wstring& baseDir, const std::string& message) {
    const auto text = ScumSingleLineMessage(message, 220);
    if (text.empty()) return;
    SendScheduledAdminCommand(baseDir, std::string("#Announce ") + text, 8000);
}

std::string RunScheduledEventJob(const std::wstring& baseDir, const std::string& config, const std::string& job, std::time_t seed) {
    auto name = JsonStringField(job, "Name");
    if (name.empty()) name = JsonStringField(job, "name");
    auto mode = JsonStringField(job, "Mode");
    if (mode.empty()) mode = JsonStringField(job, "mode");
    mode = ToLowerAscii(TrimAscii(mode));
    const auto group = JsonStringField(job, "PointGroup").empty() ? JsonStringField(job, "pointGroup") : JsonStringField(job, "PointGroup");
    const auto itemSetName = JsonStringField(job, "ItemSet").empty() ? JsonStringField(job, "itemSet") : JsonStringField(job, "ItemSet");
    auto commandTemplate = JsonStringField(job, "CommandTemplate");
    if (commandTemplate.empty()) commandTemplate = JsonStringField(job, "commandTemplate");
    auto announcement = JsonStringField(job, "Announcement");
    if (announcement.empty()) announcement = JsonStringField(job, "announcement");
    const auto point = SelectScheduledPoint(config, group, seed);

    if (mode == "airdrop" || mode == "cargodrop" || mode == "cargo-drop") {
        const int pointCount = std::clamp(JsonIntField(job, "PointCount", JsonIntField(job, "pointCount", 1)), 1, 10);
        auto targets = SelectScheduledPoints(config, group, seed, pointCount);
        if (targets.empty()) targets.push_back(ScheduledPoint{});
        const int delayMs = LocalRconEnabled(baseDir)
            ? LocalRconCommandDelayMs(baseDir)
            : ConfigIntValue(baseDir, "ark_panel_command_delay_ms", 0, 0, 5000);
        int requested = 0;
        int sent = 0;
        std::ostringstream results;
        results << "[";
        for (size_t i = 0; i < targets.size(); ++i) {
            auto templateForRun = commandTemplate;
            if (templateForRun.empty()) {
                templateForRun = targets[i].ok ? "ScheduleWorldEvent BP_CargoDropEvent X={X} Y={Y} Z={Z}" : "ScheduleCargoDrop";
            }
            const auto command = FillScheduledTemplate(templateForRun, targets[i], itemSetName, "BP_CargoDropEvent");
            const auto result = SendScheduledAdminCommand(baseDir, command, 20000);
            const auto pointName = targets[i].ok
                ? (targets[i].name.empty() ? ScheduledPointLocation(targets[i]) : targets[i].name)
                : std::string("карта");
            if (requested > 0) results << ",";
            results << "{\"point\":\"" << JsonEscape(pointName)
                    << "\",\"command\":\"" << JsonEscape(command)
                    << "\",\"ok\":" << (result.ok ? "true" : "false")
                    << ",\"message\":\"" << JsonEscape(result.message) << "\"}";
            ++requested;
            if (result.ok) {
                ++sent;
                if (!announcement.empty()) SendScheduledAnnouncement(baseDir, FillScheduledTemplate(announcement, targets[i], itemSetName, "BP_CargoDropEvent"));
            }
            if (i + 1 < targets.size() && delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        results << "]";
        const bool ok = requested > 0 && sent == requested;
        AppendActionRecord(baseDir, "scheduled_airdrop", ok, "", name,
            ok ? "Планировщик вызвал cargo drop." : "Планировщик не смог вызвать все cargo drop.",
            std::string("{\"requested\":") + std::to_string(requested) +
            ",\"sent\":" + std::to_string(sent) +
            ",\"transport\":\"scheduled-admin-command\"}");
        return std::string("{\"ok\":") + (ok ? "true" : "false") +
            ",\"mode\":\"airdrop\",\"job\":\"" + JsonEscape(name) +
            "\",\"requested\":" + std::to_string(requested) +
            ",\"sent\":" + std::to_string(sent) +
            ",\"results\":" + results.str() + "}";
    }

    if (mode == "worldevent" || mode == "world-event" || mode == "event") {
        const auto eventClass = ScheduledWorldEventClass(job);
        if (eventClass.empty()) {
            return std::string("{\"ok\":false,\"mode\":\"worldEvent\",\"job\":\"") + JsonEscape(name) + "\",\"message\":\"Класс world event задан небезопасно.\"}";
        }
        const int pointCount = std::clamp(JsonIntField(job, "PointCount", JsonIntField(job, "pointCount", 1)), 1, 10);
        const auto targets = SelectScheduledPoints(config, group, seed, pointCount);
        if (targets.empty()) {
            return std::string("{\"ok\":false,\"mode\":\"worldEvent\",\"job\":\"") + JsonEscape(name) + "\",\"message\":\"Не найдена точка для группы " + JsonEscape(group) + ".\"}";
        }
        if (commandTemplate.empty()) {
            commandTemplate = "ScheduleWorldEvent {WorldEventClass} X={X} Y={Y} Z={Z}";
        }
        const int delayMs = LocalRconEnabled(baseDir)
            ? LocalRconCommandDelayMs(baseDir)
            : ConfigIntValue(baseDir, "ark_panel_command_delay_ms", 0, 0, 5000);
        int requested = 0;
        int sent = 0;
        std::ostringstream results;
        results << "[";
        for (size_t i = 0; i < targets.size(); ++i) {
            const auto command = FillScheduledTemplate(commandTemplate, targets[i], itemSetName, eventClass);
            const auto result = SendScheduledAdminCommand(baseDir, command, 20000);
            const auto pointName = targets[i].name.empty() ? ScheduledPointLocation(targets[i]) : targets[i].name;
            if (requested > 0) results << ",";
            results << "{\"point\":\"" << JsonEscape(pointName)
                    << "\",\"eventClass\":\"" << JsonEscape(eventClass)
                    << "\",\"command\":\"" << JsonEscape(command)
                    << "\",\"ok\":" << (result.ok ? "true" : "false")
                    << ",\"message\":\"" << JsonEscape(result.message) << "\"}";
            ++requested;
            if (result.ok) {
                ++sent;
                if (!announcement.empty()) SendScheduledAnnouncement(baseDir, FillScheduledTemplate(announcement, targets[i], itemSetName, eventClass));
            }
            if (i + 1 < targets.size() && delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        results << "]";
        const bool ok = requested > 0 && sent == requested;
        AppendActionRecord(baseDir, "scheduled_world_event", ok, "", name,
            ok ? "Планировщик вызвал world event." : "Планировщик не смог вызвать все world events.",
            std::string("{\"eventClass\":\"") + JsonEscape(eventClass) +
            "\",\"requested\":" + std::to_string(requested) +
            ",\"sent\":" + std::to_string(sent) + "}");
        return std::string("{\"ok\":") + (ok ? "true" : "false") +
            ",\"mode\":\"worldEvent\",\"job\":\"" + JsonEscape(name) +
            "\",\"eventClass\":\"" + JsonEscape(eventClass) +
            "\",\"requested\":" + std::to_string(requested) +
            ",\"sent\":" + std::to_string(sent) +
            ",\"results\":" + results.str() + "}";
    }

    if (mode == "spawnitems" || mode == "items" || mode == "spawn-items") {
        if (!point.ok) {
            return std::string("{\"ok\":false,\"mode\":\"spawnItems\",\"job\":\"") + JsonEscape(name) + "\",\"message\":\"Не найдена точка для группы " + JsonEscape(group) + ".\"}";
        }
        const auto set = SelectScheduledItemSet(config, itemSetName, seed);
        if (!set.ok || set.items.empty()) {
            return std::string("{\"ok\":false,\"mode\":\"spawnItems\",\"job\":\"") + JsonEscape(name) + "\",\"message\":\"Не найден набор предметов.\"}";
        }
        const int maxItems = std::clamp(JsonIntField(job, "MaxItemsPerRun", JsonIntField(job, "maxItemsPerRun", 30)), 1, 300);
        const int delayMs = LocalRconEnabled(baseDir)
            ? LocalRconCommandDelayMs(baseDir)
            : ConfigIntValue(baseDir, "ark_panel_command_delay_ms", 0, 0, 5000);
        int requested = 0;
        int sent = 0;
        std::ostringstream results;
        results << "[";
        for (const auto& item : set.items) {
            if (requested >= maxItems) break;
            ++requested;
            const auto command = std::string("#SpawnItem ") + TrimAscii(item.itemId) + " " +
                std::to_string(std::clamp(item.quantity, 1, 100)) + " Location " + ScumLocationArgument(ScheduledPointLocation(point));
            const auto result = SendScheduledAdminCommand(baseDir, command, 20000);
            if (requested > 1) results << ",";
            results << "{\"itemId\":\"" << JsonEscape(item.itemId)
                    << "\",\"quantity\":" << std::clamp(item.quantity, 1, 100)
                    << ",\"ok\":" << (result.ok ? "true" : "false")
                    << ",\"message\":\"" << JsonEscape(result.message) << "\"}";
            if (result.ok) ++sent;
            if (requested < maxItems) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        results << "]";
        const bool ok = requested > 0 && sent == requested;
        if (ok && !announcement.empty()) SendScheduledAnnouncement(baseDir, FillScheduledTemplate(announcement, point, set.name));
        AppendActionRecord(baseDir, "scheduled_spawn_items", ok, "", name,
            ok ? "Планировщик заспавнил набор предметов." : "Планировщик отправил набор предметов не полностью.",
            std::string("{\"itemSet\":\"") + JsonEscape(set.name) +
            "\",\"point\":\"" + JsonEscape(point.name) +
            "\",\"requested\":" + std::to_string(requested) +
            ",\"sent\":" + std::to_string(sent) + "}");
        return std::string("{\"ok\":") + (ok ? "true" : "false") +
            ",\"mode\":\"spawnItems\",\"job\":\"" + JsonEscape(name) +
            "\",\"itemSet\":\"" + JsonEscape(set.name) +
            "\",\"point\":\"" + JsonEscape(point.name) +
            "\",\"requested\":" + std::to_string(requested) +
            ",\"sent\":" + std::to_string(sent) +
            ",\"results\":" + results.str() + "}";
    }

    if (mode == "command" || mode == "admincommand" || mode == "admin-command") {
        if (commandTemplate.empty()) {
            return std::string("{\"ok\":false,\"mode\":\"command\",\"job\":\"") + JsonEscape(name) + "\",\"message\":\"Команда не задана.\"}";
        }
        const auto command = FillScheduledTemplate(commandTemplate, point, itemSetName, ScheduledWorldEventClass(job));
        const auto result = SendScheduledAdminCommand(baseDir, command, 20000);
        if (result.ok && !announcement.empty()) SendScheduledAnnouncement(baseDir, FillScheduledTemplate(announcement, point, itemSetName, ScheduledWorldEventClass(job)));
        AppendActionRecord(baseDir, "scheduled_admin_command", result.ok, "", name, result.message,
            std::string("{\"command\":\"") + JsonEscape(command) + "\",\"transport\":\"" + JsonEscape(result.transport) + "\"}");
        return std::string("{\"ok\":") + (result.ok ? "true" : "false") +
            ",\"mode\":\"command\",\"job\":\"" + JsonEscape(name) +
            "\",\"command\":\"" + JsonEscape(command) +
            "\",\"message\":\"" + JsonEscape(result.message) + "\"}";
    }

    return std::string("{\"ok\":false,\"mode\":\"") + JsonEscape(mode) + "\",\"job\":\"" + JsonEscape(name) + "\",\"message\":\"Неизвестный тип задания.\"}";
}

std::string ScheduledTimesText(const std::string& job) {
    auto text = JsonStringField(job, "ScheduleTimes");
    if (text.empty()) text = JsonStringField(job, "scheduleTimes");
    if (text.empty()) text = JsonStringField(job, "TimesOfDay");
    if (text.empty()) text = JsonStringField(job, "timesOfDay");
    if (text.empty()) text = JsonStringField(job, "RunTimes");
    if (text.empty()) text = JsonStringField(job, "runTimes");
    if (text.empty()) text = JsonStringField(job, "At");
    if (text.empty()) text = JsonStringField(job, "at");
    return TrimAscii(text);
}

bool TryParseScheduleMinute(const std::string& token, int& minuteOut) {
    auto value = TrimAscii(token);
    std::replace(value.begin(), value.end(), '.', ':');
    const auto sep = value.find(':');
    if (sep == std::string::npos) return false;
    try {
        const int hour = std::stoi(value.substr(0, sep));
        const int minute = std::stoi(value.substr(sep + 1));
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
        minuteOut = hour * 60 + minute;
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<int> ParseScheduleTimesMinutes(std::string text) {
    std::replace(text.begin(), text.end(), ',', ' ');
    std::replace(text.begin(), text.end(), ';', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\t', ' ');
    std::stringstream ss(text);
    std::string token;
    std::vector<int> minutes;
    while (ss >> token) {
        int minute = 0;
        if (TryParseScheduleMinute(token, minute)) minutes.push_back(minute);
    }
    std::sort(minutes.begin(), minutes.end());
    minutes.erase(std::unique(minutes.begin(), minutes.end()), minutes.end());
    return minutes;
}

bool ScheduledTimeDue(
    const std::string& key,
    const std::vector<int>& minutes,
    std::time_t now,
    std::map<std::string, std::time_t>& lastRun,
    std::string& triggerOut) {
    if (minutes.empty()) return false;
    std::tm local{};
    if (localtime_s(&local, &now) != 0) return false;
    const int currentMinute = local.tm_hour * 60 + local.tm_min;
    char dateKey[16]{};
    std::snprintf(dateKey, sizeof(dateKey), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    for (const auto minute : minutes) {
        if (minute != currentMinute) continue;
        const auto runKey = key + "@" + dateKey + "@" + std::to_string(minute);
        if (lastRun.find(runKey) != lastRun.end()) return false;
        lastRun[runKey] = now;
        char label[8]{};
        std::snprintf(label, sizeof(label), "%02d:%02d", minute / 60, minute % 60);
        triggerOut = label;
        return true;
    }
    return false;
}

void StartScheduledEventsWorker(const std::wstring& baseDir) {
    bool expected = false;
    if (!g_scheduledEventsWorkerStarted.compare_exchange_strong(expected, true)) return;
    std::thread([baseDir]() {
        std::map<std::string, std::time_t> lastRun;
        while (true) {
            int sleepMs = 5000;
            try {
                const auto config = PluginConfigJson(baseDir, "scheduled-events", "{}");
                sleepMs = std::clamp(JsonIntField(config, "PollIntervalMs", JsonIntField(config, "pollIntervalMs", 15000)), 5000, 600000);
                if (!JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false))) {
                    Sleep(sleepMs);
                    continue;
                }
                const auto jobs = ExtractArrayObjects(JsonRawField(config, "Jobs"));
                const auto now = std::time(nullptr);
                const int maxRuns = std::clamp(JsonIntField(config, "MaxRunsPerTick", JsonIntField(config, "maxRunsPerTick", 1)), 1, 10);
                int runs = 0;
                for (size_t i = 0; i < jobs.size() && runs < maxRuns; ++i) {
                    const auto& job = jobs[i];
                    if (!JsonBoolField(job, "Enabled", JsonBoolField(job, "enabled", false))) continue;
                    auto name = JsonStringField(job, "Name");
                    if (name.empty()) name = JsonStringField(job, "name");
                    const auto key = name.empty() ? ("job-" + std::to_string(i)) : name;
                    const int intervalMinutes = std::max(1, JsonIntField(job, "IntervalMinutes", JsonIntField(job, "intervalMinutes", 60)));
                    const auto scheduleTimes = ScheduledTimesText(job);
                    const auto scheduleMinutes = ParseScheduleTimesMinutes(scheduleTimes);
                    const bool hasSchedule = !scheduleMinutes.empty();
                    bool due = false;
                    std::string trigger = hasSchedule ? "schedule" : "interval";
                    auto it = lastRun.find(key);
                    if (it == lastRun.end()) {
                        const bool runOnStartup = JsonBoolField(job, "RunOnStartup", JsonBoolField(job, "runOnStartup", false));
                        lastRun[key] = now;
                        if (runOnStartup) {
                            due = true;
                            trigger = "startup";
                            if (hasSchedule) {
                                std::string ignored;
                                ScheduledTimeDue(key, scheduleMinutes, now, lastRun, ignored);
                            }
                        }
                    }
                    if (!due) {
                        if (hasSchedule) {
                            if (!ScheduledTimeDue(key, scheduleMinutes, now, lastRun, trigger)) continue;
                        } else {
                            const auto last = lastRun.find(key);
                            if (last != lastRun.end() && now - last->second < static_cast<std::time_t>(intervalMinutes) * 60) {
                                continue;
                            }
                            due = true;
                            trigger = "interval";
                        }
                    }

                    const auto result = RunScheduledEventJob(baseDir, config, job, now + static_cast<std::time_t>(i));
                    lastRun[key] = now;
                    ++runs;
                    AppendModuleStateRecord(baseDir, "scheduled-events",
                        std::string("{\"utc\":\"") + UtcIsoNow() +
                        "\",\"status\":\"job-run\",\"job\":\"" + JsonEscape(key) +
                        "\",\"trigger\":\"" + JsonEscape(trigger) +
                        "\",\"scheduleTimes\":\"" + JsonEscape(scheduleTimes) +
                        "\",\"intervalMinutes\":" + std::to_string(intervalMinutes) +
                        ",\"result\":" + result + "}");
                }
            } catch (...) {
                AppendModuleStateRecord(baseDir, "scheduled-events",
                    std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"worker-error\",\"ok\":false}");
            }
            Sleep(sleepMs);
        }
    }).detach();
}

std::string NativeFileCommandEnvelope(
    const std::string& id,
    bool ok,
    const std::string& message,
    const std::string& dataJson) {
    std::ostringstream out;
    out << "{\"id\":\"" << JsonEscape(id) << "\""
        << ",\"ok\":" << (ok ? "true" : "false")
        << ",\"message\":\"" << JsonEscape(message) << "\""
        << ",\"data\":" << (dataJson.empty() ? "{}" : dataJson) << "}";
    return out.str();
}

bool WaitForVehicleEntityWithin(
    const std::wstring& baseDir,
    const std::string& vehicleId,
    long long beforeEntityId,
    long long& entityIdOut,
    int timeoutMs) {
    const auto started = GetTickCount64();
    const auto limit = static_cast<ULONGLONG>(std::clamp(timeoutMs, 500, 20000));
    while (GetTickCount64() - started <= limit) {
        const auto current = LatestVehicleEntityId(baseDir, vehicleId);
        if (current > beforeEntityId) {
            entityIdOut = current;
            return true;
        }
        Sleep(250);
    }
    entityIdOut = LatestVehicleEntityId(baseDir, vehicleId);
    return entityIdOut > beforeEntityId;
}

std::string HandleNativeFileCommand(const std::wstring& baseDir, const std::string& text) {
    const auto id = JsonStringField(text, "id");
    const auto command = JsonStringField(text, "command");
    auto payload = JsonRawField(text, "payload");
    if (payload.empty()) payload = text;
    if (id.empty()) {
        return NativeFileCommandEnvelope("", false, "native file command id is missing", "{}");
    }
    if (command.empty()) {
        return NativeFileCommandEnvelope(id, false, "native file command is missing", "{}");
    }

    if (command == "vehicle_latest_entity_id") {
        const auto token = VehicleAdminToken(JsonStringField(payload, "vehicleId"));
        if (token.empty()) {
            return NativeFileCommandEnvelope(id, false, "vehicleId is missing", "{}");
        }
        const auto entityId = LatestVehicleEntityId(baseDir, token);
        return NativeFileCommandEnvelope(id, true, "vehicle latest entity id read",
            std::string("{\"vehicleId\":\"") + JsonEscape(token) +
            "\",\"entityId\":" + std::to_string(entityId) + "}");
    }

    if (command == "vehicle_wait_new_entity_id") {
        const auto token = VehicleAdminToken(JsonStringField(payload, "vehicleId"));
        if (token.empty()) {
            return NativeFileCommandEnvelope(id, false, "vehicleId is missing", "{}");
        }
        const auto beforeEntityId = JsonLongLongField(payload, "beforeEntityId", 0);
        const auto timeoutMs = std::clamp(JsonIntField(payload, "timeoutMs", 12000), 500, 20000);
        long long entityId = 0;
        const bool found = WaitForVehicleEntityWithin(baseDir, token, beforeEntityId, entityId, timeoutMs);
        return NativeFileCommandEnvelope(id, found, found ? "vehicle db entity id resolved" : "vehicle db entity id was not found",
            std::string("{\"vehicleId\":\"") + JsonEscape(token) +
            "\",\"beforeEntityId\":" + std::to_string(beforeEntityId) +
            ",\"entityId\":" + std::to_string(entityId) +
            ",\"timeoutMs\":" + std::to_string(timeoutMs) + "}");
    }

    if (command == "vehicle_entity_exists") {
        const auto entityId = JsonLongLongField(payload, "entityId", JsonLongLongField(payload, "vehicleEntityId", 0));
        if (entityId <= 0) {
            return NativeFileCommandEnvelope(id, false, "entityId is missing", "{}");
        }
        bool exists = false;
        std::string error;
        if (!TryReadVehicleEntityExists(baseDir, entityId, exists, error)) {
            return NativeFileCommandEnvelope(id, false, error.empty() ? "vehicle db read failed" : error,
                std::string("{\"entityId\":") + std::to_string(entityId) + "}");
        }
        return NativeFileCommandEnvelope(id, true, "vehicle db entity existence read",
            std::string("{\"entityId\":") + std::to_string(entityId) +
            ",\"exists\":" + (exists ? "true" : "false") + "}");
    }

    // Paid in-game services use this bounded file bridge instead of treating a
    // queued admin command as proof of payment.  The native side issues the
    // supported RCON/host command and confirms the exact balance delta through
    // a read-only SCUM.db observation.  Do not fall back to BridgeExecute here:
    // this request already originated from the UE4SS bridge, so a fallback
    // would create a circular command dependency and can outlive the Lua
    // request timeout.
    if (command == "change_money_verified") {
        const auto steamId = JsonStringField(payload, "steamId");
        const auto name = JsonStringField(payload, "name");
        const auto runtimeKey = JsonStringField(payload, "runtimeKey");
        const auto amount = JsonIntField(payload, "amount", 0);
        auto currency = JsonStringField(payload, "currency");
        if (currency.empty()) currency = "Normal";
        const auto change = ChangeMoneyVerified(baseDir, steamId, name, runtimeKey, amount, currency, false, true);
        std::ostringstream data;
        data << "{\"operationId\":\"" << JsonEscape(change.operationId) << "\""
             << ",\"verified\":" << (change.verified ? "true" : "false")
             << ",\"bridgeOk\":" << (change.bridgeOk ? "true" : "false")
             << ",\"commandAccepted\":" << (change.commandAccepted ? "true" : "false")
             << ",\"uncertain\":" << (change.uncertain ? "true" : "false")
             << ",\"transport\":\"" << JsonEscape(change.transport) << "\""
             << ",\"dbWrite\":" << (change.dbPatched ? "true" : "false")
             << "}";
        return NativeFileCommandEnvelope(id, change.ok, change.message, data.str());
    }

    if (command == "base_loot_access") {
        bool commandOk = false;
        std::string message;
        const auto data = BaseLootAccessJson(baseDir, payload, commandOk, message);
        return NativeFileCommandEnvelope(id, commandOk, message.empty() ? "base loot access checked" : message, data);
    }

    if (command == "base_loot_nearest_flag") {
        bool commandOk = false;
        std::string message;
        const auto data = BaseLootNearestFlagJson(baseDir, payload, commandOk, message);
        return NativeFileCommandEnvelope(id, commandOk, message.empty() ? "base loot nearest flag checked" : message, data);
    }

    return NativeFileCommandEnvelope(id, false, std::string("unknown native file command: ") + command, "{}");
}

void StartNativeFileCommandWorker(const std::wstring& baseDir) {
    bool expected = false;
    if (!g_nativeFileCommandWorkerStarted.compare_exchange_strong(expected, true)) return;
    std::thread([baseDir]() {
        const auto commandPath = ModuleStatePath(baseDir, "native-http-command");
        const auto resultPath = ModuleStatePath(baseDir, "native-http-result");
        while (true) {
            try {
                auto text = TrimAscii(ReadTextFile(commandPath));
                if (!text.empty() && text.front() == '{') {
                    DeleteFileW(commandPath.c_str());
                    WriteTextFile(resultPath, HandleNativeFileCommand(baseDir, text));
                }
            } catch (...) {
                WriteTextFile(resultPath, "{\"id\":\"\",\"ok\":false,\"message\":\"native file command worker exception\",\"data\":{}}");
            }
            Sleep(100);
        }
    }).detach();
}

// Money verification can take tens of seconds because it performs a live
// command plus read-only persistence confirmation.  Keep it off the short
// native-http file lane used by /loot and vehicle entity probes.
std::string HandleNativeMoneyFileCommand(const std::wstring& baseDir, const std::string& text) {
    const auto id = JsonStringField(text, "id");
    const auto command = JsonStringField(text, "command");
    if (command != "change_money_verified") {
        return NativeFileCommandEnvelope(id, false, "native money lane only accepts change_money_verified", "{}");
    }
    return HandleNativeFileCommand(baseDir, text);
}

void StartNativeMoneyFileCommandWorker(const std::wstring& baseDir) {
    bool expected = false;
    if (!g_nativeMoneyFileCommandWorkerStarted.compare_exchange_strong(expected, true)) return;
    std::thread([baseDir]() {
        const auto commandPath = ModuleStatePath(baseDir, "native-money-command");
        const auto resultPath = ModuleStatePath(baseDir, "native-money-result");
        while (true) {
            try {
                auto text = TrimAscii(ReadTextFile(commandPath));
                if (!text.empty() && text.front() == '{') {
                    DeleteFileW(commandPath.c_str());
                    WriteTextFile(resultPath, HandleNativeMoneyFileCommand(baseDir, text));
                }
            } catch (...) {
                WriteTextFile(resultPath, "{\"id\":\"\",\"ok\":false,\"message\":\"native money command worker exception\",\"data\":{\"uncertain\":true}}");
            }
            Sleep(100);
        }
    }).detach();
}

std::string ApiResponse(const std::wstring& baseDir, const Request& req, const std::string& apiKey) {
    (void)apiKey;
    const auto path = ApiPath(req.path);

    if (path == "/api/health") {
        return Envelope(true, "{\"status\":\"ok\",\"name\":\"ScumNeDjinNative\"}");
    }
    if (path == "/api/build") {
        return Envelope(true, "{\"product\":\"ScumNeDjin\",\"runtime\":\"native-version-proxy\",\"version\":\"0.1.0\"}");
    }
    if (path == "/api/status") {
        return Envelope(true, StatusJson(baseDir));
    }
    if (path == "/api/managed-loader/status") {
        return Envelope(true, ManagedLoaderStatusJson(baseDir));
    }
    if (path == "/api/managed-loader/run-probe") {
        if (req.method != "POST") {
            return Envelope(false, "Method not allowed.");
        }
        if (!ConfigBoolValue(baseDir, "managed_loader_enabled", false)) {
            return Envelope(false, "Managed loader is disabled by nedjin.ini.");
        }
        if (!ConfigBoolValue(baseDir, "managed_loader_allow_in_process_probe", false)) {
            return Envelope(false, "Managed loader in-process probe is blocked by safety flag.");
        }
        if (!ConfigBoolValue(baseDir, "managed_loader_allow_api_probe", false)) {
            return Envelope(false, "Managed loader API probe is blocked by safety flag.");
        }

        RunManagedLoaderProbeAsync(baseDir);
        return Envelope(true,
            "{\"status\":\"queued\",\"message\":\"managed loader in-process probe queued\",\"requiredFlags\":[\"managed_loader_enabled\",\"managed_loader_allow_in_process_probe\",\"managed_loader_allow_api_probe\"]}");
    }
    if (path == "/api/server-info") {
        const auto serverName = ServerDisplayName(baseDir);
        return Envelope(true, std::string("{\"name\":\"") + JsonEscape(serverName) +
            "\",\"server\":\"" + JsonEscape(serverName) +
            "\",\"source\":\"ServerSettings.ini-or-SCUM.log\"}");
    }
    if (path == "/api/diagnostics/package") {
        return Envelope(true, DiagnosticPackageText(baseDir), false);
    }
    if (path == "/api/status.performance" || path == "/api/status/performance") {
        return Envelope(true, LatestGlobalStatsJson(baseDir));
    }
    if (path == "/api/ark-panel/status") {
        return Envelope(true, ArkPanelStatusJson(baseDir));
    }
    if (path == "/api/server-control/status") {
        return Envelope(true, ArkPanelStatusJson(baseDir));
    }
    if (path == "/api/server-control") {
        return ServerControlEnvelope(baseDir, req);
    }
    if (path.rfind("/api/server-control/", 0) == 0) {
        Request copy = req;
        copy.body = std::string("{\"action\":\"") + JsonEscape(path.substr(std::string("/api/server-control/").size())) + "\"}";
        return ServerControlEnvelope(baseDir, copy);
    }
    if (path == "/api/server-configs") {
        return Envelope(true, ServerConfigsListJson(baseDir));
    }
    if (path == "/api/server-config") {
        if (req.method == "GET") return ServerConfigReadEnvelope(baseDir, req);
        return ServerConfigSaveEnvelope(baseDir, req);
    }
    if (path == "/api/servers") {
        const auto serverName = ServerDisplayName(baseDir);
        return Envelope(true, std::string("[{\"id\":\"local\",\"name\":\"") + JsonEscape(serverName) +
            "\",\"serverName\":\"" + JsonEscape(serverName) +
            "\",\"server\":\"" + JsonEscape(serverName) +
            "\",\"kind\":\"dedicated\",\"online\":true,\"control\":" + ArkPanelStatusJson(baseDir) + "}]");
    }
    if (path == "/api/players") {
        return Envelope(true, PlayersJson(baseDir, req));
    }
    if (path == "/api/base-loot/access") {
        return BaseLootAccessEnvelope(baseDir, req);
    }
    if (path == "/api/base-loot/nearest-flag") {
        return BaseLootNearestFlagEnvelope(baseDir, req);
    }
    if (path == "/api/plugins") {
        return Envelope(true, PluginsJson(baseDir));
    }
    if (path == "/api/plugin-configs" || path == "/api/pluginconfigs") {
        return Envelope(true, PluginsJson(baseDir));
    }
    if (path == "/api/plugin-manifest" || path == "/api/plugin-schema") {
        return Envelope(true, PluginManifestJson());
    }
    if (path == "/api/plugin") {
        if (req.method == "GET") {
            auto key = CanonicalPluginConfigKey(QueryValue(req.path, "name"));
            if (key.empty()) key = CanonicalPluginConfigKey(QueryValue(req.path, "key"));
            if (key.empty()) return Envelope(true, PluginsJson(baseDir));
            bool legacyAliasOnly = false;
            const auto config = PluginConfigJson(baseDir, key, "{}", &legacyAliasOnly);
            const bool legacyAliasConflict = !legacyAliasOnly && HasLegacyPluginConfigAlias(baseDir, key);
            return Envelope(true, std::string("{\"key\":\"") + JsonEscape(key) + "\",\"config\":" + config +
                ",\"source\":\"" + (legacyAliasOnly ? "ScumNeDjin legacy-alias-read-only" : "ScumNeDjin live-config") +
                "\",\"migrationRequired\":" + (legacyAliasOnly ? "true" : "false") +
                ",\"legacyAliasConflict\":" + (legacyAliasConflict ? "true" : "false") +
                ",\"runtimeConfigActive\":" + (legacyAliasOnly ? "false" : "true") + "}");
        }
        if (req.method == "DELETE") {
            return Envelope(false, "Удаление конфигурации модуля через API отключено: текущий config/overlay сохраняется. Используйте отдельный backup/recovery workflow, не DELETE.");
        }
        if (req.method != "POST") return Envelope(false, "Для /api/plugin разрешены только GET и POST; DELETE отключён для защиты настроек модулей.");
        auto key = CanonicalPluginConfigKey(JsonStringField(req.body, "name"));
        if (key.empty()) key = CanonicalPluginConfigKey(JsonStringField(req.body, "Name"));
        if (key.empty()) key = CanonicalPluginConfigKey(JsonStringField(req.body, "key"));
        if (key.empty()) key = CanonicalPluginConfigKey(JsonStringField(req.body, "Key"));
        auto rawConfig = JsonRawField(req.body, "config");
        if (rawConfig.empty()) rawConfig = JsonRawField(req.body, "Config");
        if (key.empty()) return Envelope(false, "Ключ плагина не указан.");
        if (HasLegacyOnlyPluginConfigAlias(baseDir, key)) return Envelope(false, PluginConfigMigrationRequiredMessage(key));
        if (rawConfig.empty() || rawConfig.front() != '{' || !IsValidJsonObject(rawConfig)) return Envelope(false, "Конфиг плагина должен быть корректным JSON-объектом.");
        std::string backupName;
        const auto saved = WritePluginConfigOverlayAtomically(baseDir, key, rawConfig, backupName);
        return saved ? Envelope(true, std::string("{\"saved\":true,\"key\":\"") + JsonEscape(key) + "\",\"liveConfig\":true,\"backup\":\"" + JsonEscape(backupName) + "\"}") : Envelope(false, "Не удалось атомарно сохранить конфиг плагина; текущие настройки не изменены.");
    }
    if (path == "/api/discord-log-bridge/status" || path == "/api/discord/status") {
        return Envelope(true, DiscordBridgeStatusJson(baseDir));
    }
    if (path == "/api/discord-log-bridge/test" || path == "/api/discord/test") {
        return DiscordBridgeTestResponse(baseDir, req);
    }
    if (path == "/api/discord-log-bridge/channels") {
        return Envelope(true, DiscordBridgeChannelsJson(baseDir));
    }
    if (path == "/api/discord-log-bridge/secret") {
        if (req.method == "GET") return Envelope(true, DiscordSecretStatusJson(baseDir));
        const auto token = JsonStringField(req.body, "botToken");
        const auto saved = WriteTextFile(DiscordSecretOverlayPath(baseDir), std::string("{\"botToken\":\"") + JsonEscape(token) + "\"}");
        return saved ? Envelope(true, DiscordSecretStatusJson(baseDir)) : Envelope(false, "Не удалось сохранить секрет Discord.");
    }
    if (path == "/api/catalog/items") {
        return Envelope(true, WebJsonAssetOrFallback(baseDir, L"spawnable-items.json", ItemCatalogJson()));
    }
    if (path == "/api/catalog/vehicles") {
        return Envelope(true, WebJsonAssetOrFallback(baseDir, L"spawnable-vehicles.json", VehicleCatalogJson()));
    }
    if (path == "/api/catalog/item-icons" || path == "/api/item-icons") {
        return Envelope(true, WebJsonAssetOrFallback(baseDir, L"item-icons-manifest.json", "{}"));
    }
    if (path == "/api/player/skill-catalog") {
        return Envelope(true, SkillCatalogJson());
    }
    if (path == "/api/downloads") {
        return Envelope(true, DownloadsJson());
    }
    if (path == "/api/plugin-config" || path == "/api/pluginconfig") {
        if (req.method == "GET") {
            auto key = CanonicalPluginConfigKey(QueryValue(req.path, "name"));
            if (key.empty()) key = CanonicalPluginConfigKey(QueryValue(req.path, "key"));
            if (key.empty()) return Envelope(false, "Ключ плагина не указан.");
            bool legacyAliasOnly = false;
            const auto config = PluginConfigJson(baseDir, key, "{}", &legacyAliasOnly);
            const bool legacyAliasConflict = !legacyAliasOnly && HasLegacyPluginConfigAlias(baseDir, key);
            return Envelope(true, std::string("{\"key\":\"") + JsonEscape(key) + "\",\"config\":" + config +
                ",\"migrationRequired\":" + (legacyAliasOnly ? "true" : "false") +
                ",\"legacyAliasConflict\":" + (legacyAliasConflict ? "true" : "false") +
                ",\"runtimeConfigActive\":" + (legacyAliasOnly ? "false" : "true") + "}");
        }
        if (req.method != "POST") return Envelope(false, "Для /api/plugin-config разрешены только GET и POST; удаление конфигов отключено для защиты настроек модулей.");

        auto key = CanonicalPluginConfigKey(JsonStringField(req.body, "name"));
        if (key.empty()) key = CanonicalPluginConfigKey(JsonStringField(req.body, "Name"));
        if (key.empty()) key = CanonicalPluginConfigKey(JsonStringField(req.body, "key"));
        if (key.empty()) key = CanonicalPluginConfigKey(JsonStringField(req.body, "Key"));
        const auto rawConfig = JsonRawField(req.body, "config");
        auto rawConfigValue = rawConfig.empty() ? JsonRawField(req.body, "Config") : rawConfig;
        if (key.empty()) return Envelope(false, "Ключ плагина не указан.");
        if (HasLegacyOnlyPluginConfigAlias(baseDir, key)) return Envelope(false, PluginConfigMigrationRequiredMessage(key));
        if (rawConfigValue.empty() || rawConfigValue.front() != '{' || !IsValidJsonObject(rawConfigValue)) return Envelope(false, "Конфиг плагина должен быть корректным JSON-объектом.");
        const auto hardenedConfig = HardenPluginConfigJson(key, rawConfigValue);
        std::string backupName;
        const auto saved = WritePluginConfigOverlayAtomically(baseDir, key, hardenedConfig, backupName);
        return saved ? Envelope(true, std::string("{\"saved\":true,\"key\":\"") + JsonEscape(key) + "\",\"liveConfig\":true,\"backup\":\"" + JsonEscape(backupName) + "\"}") : Envelope(false, "Не удалось атомарно сохранить конфиг плагина; текущие настройки не изменены.");
    }
    if (path == "/api/plugin-state") {
        if (req.method != "POST") return Envelope(false, "Для /api/plugin-state разрешён только POST; удаление конфигов отключено для защиты настроек модулей.");
        auto key = CanonicalPluginConfigKey(JsonStringField(req.body, "name"));
        if (key.empty()) key = CanonicalPluginConfigKey(JsonStringField(req.body, "key"));
        if (key.empty()) return Envelope(false, "Ключ плагина не указан.");
        if (HasLegacyOnlyPluginConfigAlias(baseDir, key)) return Envelope(false, PluginConfigMigrationRequiredMessage(key));
        auto config = PluginConfigJson(baseDir, key, "{}");
        const bool enabled = JsonBoolField(req.body, "enabled", false);
        if (config == "{}") {
            config = std::string("{\"Enabled\":") + (enabled ? "true" : "false") + "}";
        } else {
            auto pos = config.find("\"enabled\"");
            if (pos == std::string::npos) pos = config.find("\"Enabled\"");
            if (pos != std::string::npos) {
                const auto colon = config.find(':', pos);
                const auto valueEnd = config.find_first_of(",}", colon == std::string::npos ? pos : colon);
                if (colon != std::string::npos && valueEnd != std::string::npos) {
                    config.replace(colon + 1, valueEnd - colon - 1, enabled ? "true" : "false");
                }
            } else if (!config.empty() && config.back() == '}') {
                config.pop_back();
                if (TrimAscii(config) != "{") config += ",";
                config += std::string("\"Enabled\":") + (enabled ? "true" : "false") + "}";
            }
        }
        std::string backupName;
        const auto saved = WritePluginConfigOverlayAtomically(baseDir, key, config, backupName);
        return saved ? Envelope(true, std::string("{\"saved\":true,\"key\":\"") + JsonEscape(key) + "\",\"enabled\":" + (enabled ? "true" : "false") + ",\"backup\":\"" + JsonEscape(backupName) + "\"}") : Envelope(false, "Не удалось атомарно сохранить состояние плагина; текущие настройки не изменены.");
    }
    if (path == "/api/reload") {
        const auto br = BridgeExecute(baseDir, "reload_bridge", "{}", 3000);
        std::ostringstream data;
        data << "{\"reloaded\":true,\"liveConfig\":true"
             << ",\"luaRestartRequested\":" << (br.ok ? "true" : "false")
             << ",\"message\":\"Конфиги плагинов читаются при каждом действии. Lua bridge отправляется на live-перезапуск через UE4SS, если bridge сейчас доступен.\""
             << ",\"bridgeMessage\":\"" << JsonEscape(br.message) << "\""
             << ",\"bridgeBody\":" << (br.body.empty() ? "\"\"" : ("\"" + JsonEscape(br.body) + "\""))
             << "}";
        return Envelope(true, data.str());
    }
    if (path == "/api/queue" || path == "/api/command-queue") {
        return Envelope(true, QueueStatusJson(baseDir));
    }
    if (path == "/api/welcome-pack/timers" || path == "/api/welcomepack/timers") {
        const auto state = JsonFileOrNull(ModuleReadableStatePath(baseDir, "welcome-pack-timers"));
        return Envelope(true, state == "null" ? "[]" : state);
    }
    if (path == "/api/welcome-pack/reset-timer" || path == "/api/welcomepack/reset-timer") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto identity = ResolvePlayerIdentity(baseDir, steamId, name);
        const auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
        const auto targetName = identity.name.empty() ? name : identity.name;
        const auto saved = RemoveStateRecordsForIdentity(baseDir, "welcome-pack-timers", targetSteamId, targetName);
        return saved ? Envelope(true, std::string("{\"reset\":\"") + JsonEscape(steamId.empty() ? "all" : steamId) + "\"}") : Envelope(false, "Не удалось сбросить таймеры стартового набора.");
    }
    if (path == "/api/module-state") {
        auto key = SafeConfigKey(QueryValue(req.path, "key"));
        if (key.empty()) key = SafeConfigKey(QueryValue(req.path, "name"));
        if (key.empty()) key = SafeConfigKey(JsonStringField(req.body, "key"));
        if (key.empty()) key = SafeConfigKey(JsonStringField(req.body, "name"));
        if (key.empty()) return Envelope(false, "Ключ состояния не указан.");
        const auto storageKey = StateStorageKey(key);
        if (storageKey == "action-log" || storageKey == "chat" || storageKey == "economy" ||
            storageKey == "discord-log" || storageKey == "server-kill-feed" || storageKey == "kill-feed") {
            return Envelope(true, ModuleStateTailJson(baseDir, storageKey, QueryInt(req.path, "limit", 1000, 1, 5000)));
        }
        return Envelope(true, ModuleStateJson(baseDir, storageKey));
    }
    if (path == "/api/state") {
        auto key = SafeConfigKey(QueryValue(req.path, "name"));
        if (key.empty()) key = SafeConfigKey(QueryValue(req.path, "key"));
        if (key.empty()) key = SafeConfigKey(JsonStringField(req.body, "name"));
        if (key.empty()) key = SafeConfigKey(JsonStringField(req.body, "key"));
        if (key.empty()) return Envelope(false, "Ключ состояния не указан.");
        const auto storageKey = StateStorageKey(key);
        if (storageKey == "action-log" || storageKey == "chat" || storageKey == "economy" ||
            storageKey == "discord-log" || storageKey == "server-kill-feed" || storageKey == "kill-feed") {
            return Envelope(true, ModuleStateTailJson(baseDir, storageKey, QueryInt(req.path, "limit", 1000, 1, 5000)));
        }
        return Envelope(true, ModuleStateJson(baseDir, storageKey));
    }
    if (path == "/api/action-log") {
        return Envelope(true, ModuleStateTailJson(baseDir, "action-log", QueryInt(req.path, "limit", 250, 1, 5000)));
    }
    if (path == "/api/events" || path == "/api/event-log") {
        const auto route = ToLowerAscii(TrimAscii(FirstNonEmpty({
            QueryValue(req.path, "route"),
            QueryValue(req.path, "type"),
            QueryValue(req.path, "name")
        })));
        const int limit = QueryInt(req.path, "limit", 250, 1, 5000);
        if (route == "chat" || route == "global" || route == "local" || route == "squad" || route == "admin") {
            return Envelope(true, ChatStateWithLogIdentityJson(baseDir, limit));
        }
        if (route == "kill" || route == "kills" || route == "combat" || route == "death") {
            return Envelope(true, StateEventsOrLogJson(baseDir, "kill-feed", "kill", { "LogKill", "killed", "was killed", "Died", "Death" }));
        }
        if (route == "economy" || route == "money" || route == "trader") {
            return Envelope(true, StateEventsOrLogJson(baseDir, "economy", "economy", { "Economy", "ATM", "Currency", "Transaction", "Trader:", "Bank:" }));
        }
        if (route == "discord") {
            return Envelope(true, ModuleStateTailJson(baseDir, "discord-log", limit));
        }
        if (route == "wargm" || route == "shop") {
            return Envelope(true, ModuleStateTailJson(baseDir, "wargm-shop", limit));
        }
        if (route == "gamestores" || route == "game-stores" || route == "gamestores-shop") {
            return Envelope(true, ModuleStateTailJson(baseDir, "gamestores-shop", limit));
        }
        return Envelope(true, ModuleStateTailJson(baseDir, "action-log", limit));
    }
    if (path == "/api/welcome-pack/claim") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
        const auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
        const auto targetName = identity.name.empty() ? name : identity.name;
        const auto config = PluginConfigJson(baseDir, "welcome-pack",
            R"JSON({"Enabled":true,"CooldownHours":30000,"Items":[{"ItemId":"Apple_2","Quantity":1},{"ItemId":"Emergency_bandage_Big","Quantity":1}]})JSON");
        if (!JsonBoolField(config, "Enabled", true)) return Envelope(false, "Стартовый набор отключён.");
        const int cooldownHours = std::max(0, JsonIntField(config, "CooldownHours", JsonIntField(config, "cooldownHours", 30000)));
        const int remaining = CooldownRemainingSeconds(baseDir, "welcome-pack-timers", targetSteamId, targetName, cooldownHours * 3600);
        if (remaining > 0) {
            return Envelope(false, std::string("Кулдаун стартового набора активен. Осталось секунд: ") + std::to_string(remaining));
        }
        const auto result = DeliverItemsBatchJson(baseDir, targetSteamId, targetName, runtimeKey, ParseGrantItems(config), "welcome-pack");
        AppendModuleStateRecord(baseDir, "welcome-pack-claims",
            std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"steamId\":\"" + JsonEscape(targetSteamId) + "\",\"name\":\"" + JsonEscape(targetName) + "\",\"result\":" + result + "}");
        if (result.find("\"ok\":true") != std::string::npos) {
            const auto now = std::time(nullptr);
            const auto expiresAt = now + static_cast<std::time_t>(cooldownHours) * 3600;
            AppendModuleStateRecord(baseDir, "welcome-pack-timers",
                std::string("{\"utc\":\"") + UtcIsoNow() +
                "\",\"at\":" + std::to_string(static_cast<long long>(now)) +
                ",\"steamId\":\"" + JsonEscape(targetSteamId) +
                "\",\"name\":\"" + JsonEscape(targetName) +
                "\",\"cooldownHours\":" + std::to_string(cooldownHours) +
                ",\"expiresAtUtc\":\"" + JsonEscape(UtcIsoFromEpoch(expiresAt)) + "\"}");
        }
        return result;
    }
    if (path == "/api/home/list") {
        return Envelope(true, ModuleStateJson(baseDir, "homes"));
    }
    if (path == "/api/home/set") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        const auto label = JsonStringField(req.body, "label");
        return SaveHomeFromLivePlayer(baseDir, steamId, name, runtimeKey, label);
    }
    if (path == "/api/home/teleport") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        auto label = JsonStringField(req.body, "label");
        if (label.empty()) label = JsonStringField(req.body, "Label");
        if (!label.empty()) {
            return BridgePlayerCommand(baseDir, "player_plugin_command", steamId, name, runtimeKey,
                "\"message\":\"/home " + JsonEscape(ScumSingleLineMessage(label)) + "\"", 10000);
        }
        const auto x = JsonNumberTextField(req.body, "x", "");
        const auto y = JsonNumberTextField(req.body, "y", "");
        const auto z = JsonNumberTextField(req.body, "z", "");
        if (x.empty() || y.empty() || z.empty()) {
            return Envelope(false, "Домашняя точка не указана: передай label или явные x/y/z.");
        }
        if ((x == "0" || x == "0.0") && (y == "0" || y == "0.0") && (z == "0" || z == "0.0")) {
            return Envelope(false, "Телепорт в 0,0,0 заблокирован: передай label или реальные координаты.");
        }
        return BridgePlayerCommand(baseDir, "teleport_player", steamId, name, runtimeKey, "\"x\":" + x + ",\"y\":" + y + ",\"z\":" + z, 5000);
    }
    if (path == "/api/fast-travel/routes") {
        const auto config = PluginConfigJson(baseDir, "fast-travel", DefaultFastTravelConfigJson());
        const auto routes = JsonRawField(config, "Outposts");
        return Envelope(true, routes.empty() ? "[]" : routes);
    }
    if (path == "/api/fast-travel/go") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        const auto alias = ToLowerAscii(TrimAscii(JsonStringField(req.body, "alias")));
        std::string ignoredX;
        std::string ignoredY;
        std::string ignoredZ;
        std::string routeName;
        const auto config = PluginConfigJson(baseDir, "fast-travel", DefaultFastTravelConfigJson());
        if (!TryResolveFastTravelAlias(config, alias, ignoredX, ignoredY, ignoredZ, routeName)) {
            return Envelope(false, "Маршрут быстрого перемещения не найден. Укажи код маршрута из настроек fast-travel.");
        }
        const auto message = "/travel " + ScumSingleLineMessage(alias, 64);
        return BridgePlayerCommand(baseDir, "player_plugin_command", steamId, name, runtimeKey,
            std::string("\"message\":\"") + JsonEscape(message) + "\"", 15000);
    }
    if (path == "/api/vehicle-rental") {
        return Envelope(true, std::string("{\"config\":") +
            PluginConfigJson(baseDir, "vehicle-rental", DefaultVehicleRentalConfigJson()) +
            ",\"state\":" + ModuleStateJson(baseDir, "vehicle-rentals") +
            ",\"pendingTaxes\":" + ModuleStateJson(baseDir, "vehicle-rental-pending-taxes") +
            "}");
    }
    if (path == "/api/vehicle-rental/rent") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        auto vehicleId = JsonStringField(req.body, "vehicleId");
        int initialCharge = JsonIntField(req.body, "charge", 0);
        int pricePer10Minutes = 0;
        std::string displayName;
        const auto alias = JsonStringField(req.body, "alias");
        const auto config = PluginConfigJson(baseDir, "vehicle-rental", DefaultVehicleRentalConfigJson());
        int minMinutes = std::max(1, JsonIntField(config, "MinRentalMinutes",
            JsonIntField(config, "minRentalMinutes", JsonIntField(config, "MinMinutes", JsonIntField(config, "minMinutes", 10)))));
        int maxMinutes = std::max(minMinutes, JsonIntField(config, "MaxRentalMinutes",
            JsonIntField(config, "maxRentalMinutes", JsonIntField(config, "MaxMinutes", JsonIntField(config, "maxMinutes", 60)))));
        int defaultMinutes = std::clamp(JsonIntField(config, "DefaultRentalMinutes",
            JsonIntField(config, "defaultRentalMinutes", JsonIntField(config, "DefaultMinutes", JsonIntField(config, "defaultMinutes", 10)))),
            minMinutes,
            maxMinutes);
        std::string resolvedAlias;
        std::string resolvedVehicleId;
        const auto lookup = !alias.empty() ? alias : vehicleId;
        if (TryResolveVehicleAlias(config, lookup, resolvedAlias, resolvedVehicleId, initialCharge, pricePer10Minutes, displayName, defaultMinutes, minMinutes, maxMinutes)) {
            if (vehicleId.empty()) vehicleId = resolvedVehicleId;
        }
        if (resolvedAlias.empty()) return Envelope(false, "Транспорт для аренды не найден в настройках плагина.");
        const int requestedMinutes = JsonIntField(req.body, "minutes", defaultMinutes);
        int minutes = std::clamp(requestedMinutes > 0 ? requestedMinutes : defaultMinutes, minMinutes, maxMinutes);
        const auto message = "/rent " + ScumSingleLineMessage(ToLowerAscii(resolvedAlias), 64) + " " + std::to_string(minutes);
        return BridgePlayerCommand(baseDir, "player_plugin_command", steamId, name, runtimeKey,
            std::string("\"message\":\"") + JsonEscape(message) + "\"", 15000);
    }
    if (path == "/api/vehicle-rental/cleanup") {
        return Envelope(true, VehicleRentalCleanupJson(baseDir));
    }
    if (path == "/api/sector-scan") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        if (steamId.empty() && name.empty() && runtimeKey.empty()) {
            return Envelope(false, "Укажи игрока: /scan проверяет текущий квадрат выбранного игрока.");
        }
        return BridgePlayerCommand(baseDir, "player_plugin_command", steamId, name, runtimeKey, "\"message\":\"/scan\"", 15000);
    }
    if (path == "/api/private-message/send") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        if (runtimeKey.empty()) runtimeKey = JsonStringField(req.body, "targetRuntimeKey");
        const auto message = ScumSingleLineMessage(JsonStringField(req.body, "message"));
        if (message.empty()) return Envelope(false, "Сообщение пустое.");
        return TargetChatEnvelope(baseDir, "private-messages", steamId, name, runtimeKey, message, "private", 6000);
    }
    if (path == "/api/wargm/pending") {
        return Envelope(true, std::string("{\"pending\":") +
            ModuleStateJson(baseDir, "wargm-pending") +
            ",\"delivered\":" + ModuleStateJson(baseDir, "wargm-delivered") +
            ",\"confirmed\":" + ModuleStateJson(baseDir, "wargm-confirmed") +
            ",\"log\":" + ModuleStateTailJson(baseDir, "wargm-shop", QueryInt(req.path, "limit", 500, 1, 5000)) +
            "}");
    }
    if (path == "/api/wargm/manual-deliver") {
        return WargmManualDeliver(baseDir, req);
    }
    if (path == "/api/wargm/sync") {
        return WargmSyncJson(baseDir);
    }
    if (path == "/api/wargm/deliver-pending") {
        return WargmDeliverPendingJson(baseDir);
    }
    if (path == "/api/wargm/confirm-delivered") {
        return WargmConfirmDeliveredJson(baseDir);
    }
    if (path == "/api/gamestores/pending") {
        return Envelope(true, std::string("{\"pending\":") +
            ModuleStateJson(baseDir, "gamestores-pending") +
            ",\"delivered\":" + ModuleStateJson(baseDir, "gamestores-delivered") +
            ",\"confirmed\":" + ModuleStateJson(baseDir, "gamestores-confirmed") +
            ",\"attempts\":" + ModuleStateJson(baseDir, "gamestores-attempts") +
            ",\"log\":" + ModuleStateTailJson(baseDir, "gamestores-shop", QueryInt(req.path, "limit", 500, 1, 5000)) +
            "}");
    }
    if (path == "/api/gamestores/manual-deliver") {
        return GameStoresManualDeliver(baseDir, req);
    }
    if (path == "/api/gamestores/sync") {
        return GameStoresSyncJson(baseDir);
    }
    if (path == "/api/gamestores/deliver-pending") {
        return GameStoresDeliverPendingJson(baseDir);
    }
    if (path == "/api/gamestores/confirm-delivered") {
        return GameStoresConfirmDeliveredJson(baseDir);
    }
    if (path == "/api/gamestores/claim") {
        return GameStoresClaimJson(baseDir, req);
    }
    if (path == "/api/gamestores/process") {
        return GameStoresProcessJson(baseDir);
    }
    if (path == "/api/admin-command-probe" || path == "/api/debug/admin-command-probe") {
        return Envelope(false, "admin-command-probe отключён в публичной сборке: live-probe admin-команд уже приводил к UE4SS crash. Используйте статические дампы/документацию.");
    }
    if (path == "/api/base-element/db-spawn" || path == "/api/base/db-spawn" || path == "/api/player/spawn-base-element") {
        return Envelope(false, "DB-only base/base-element эксперименты удалены из публичной релизной панели.");
    }
    if (path == "/api/scheduled-events/run") {
        auto indexText = QueryValue(req.path, "index");
        if (indexText.empty()) indexText = TrimAscii(JsonRootRawField(req.body, "index"));
        if (indexText.empty()) indexText = TrimAscii(JsonRootRawField(req.body, "Index"));
        if (indexText.empty()) return Envelope(false, "Индекс задания планировщика не задан.");
        if (!indexText.empty() && indexText.front() == '"') indexText = JsonUnquoteRawString(indexText);
        const int index = std::atoi(indexText.c_str());
        if (index < 0) return Envelope(false, "Индекс задания планировщика не задан.");
        const auto forceQuery = ToLowerAscii(TrimAscii(QueryValue(req.path, "force")));
        const bool force = forceQuery == "1" || forceQuery == "true" || forceQuery == "yes" ||
            JsonBoolField(req.body, "force", JsonBoolField(req.body, "Force", false));
        auto args = std::string("{\"index\":") + std::to_string(index) +
            ",\"force\":" + (force ? "true" : "false") + "}";
        auto br = BridgeExecute(baseDir, "scheduled_events_run", args, 45000);
        return br.ok ? Envelope(true, br.body) : EnvelopeFailureWithJsonData(br.message, br.body);
    }
    if (path == "/api/plugin-command") {
        auto command = JsonRootStringField(req.body, "command");
        if (command.empty()) command = JsonRootStringField(req.body, "cmd");
        if (command.empty()) command = JsonRootStringField(req.body, "commandText");
        if (command.empty()) command = JsonRootStringField(req.body, "text");
        if (!command.empty()) {
            auto args = JsonRootRawField(req.body, "args");
            if (args.empty()) args = "{}";
            if (command == "player_plugin_command") {
                auto message = JsonRootStringField(args, "message");
                if (message.empty()) message = JsonRootStringField(args, "command");
                if (message.empty()) message = JsonRootStringField(args, "commandText");
                if (message.empty()) message = JsonRootStringField(req.body, "message");
                const auto steamId = FirstNonEmpty({
                    JsonRootStringField(args, "steamId"),
                    JsonRootStringField(args, "targetSteamId"),
                    JsonRootStringField(req.body, "steamId")
                });
                const auto name = FirstNonEmpty({
                    JsonRootStringField(args, "name"),
                    JsonRootStringField(args, "targetName"),
                    JsonRootStringField(req.body, "name")
                });
                const auto runtimeKey = FirstNonEmpty({
                    JsonRootStringField(args, "runtimeKey"),
                    JsonRootStringField(args, "targetRuntimeKey"),
                    JsonRootStringField(req.body, "runtimeKey")
                });
                message = ScumSingleLineMessage(message);
                if (message.empty()) return Envelope(false, "Команда плагина не задана.");
                if (steamId.empty() && name.empty() && runtimeKey.empty()) {
                    return Envelope(false, "Укажите игрока для выполнения команды плагина.");
                }
                return BridgePlayerCommand(baseDir, "player_plugin_command", steamId, name, runtimeKey,
                    std::string("\"message\":\"") + JsonEscape(message) + "\"", 15000);
            }
            if (command == "native_base_element_db_spawn" || command == "spawn_base_element_db" || command == "db_base_element_spawn") {
                return Envelope(false, "DB-only base/base-element route removed from the public release.");
            }
            auto br = BridgeExecute(baseDir, command, args, 25000);
            return br.ok ? Envelope(true, br.body) : EnvelopeFailureWithJsonData(br.message, br.body);
        }
        const auto message = JsonRootStringField(req.body, "message");
        if (!message.empty()) {
            const auto steamId = JsonRootStringField(req.body, "steamId");
            const auto name = JsonRootStringField(req.body, "name");
            const auto runtimeKey = JsonRootStringField(req.body, "runtimeKey");
            if (steamId.empty() && name.empty() && runtimeKey.empty()) {
                return Envelope(false, "Укажите игрока для выполнения команды плагина.");
            }
            return BridgePlayerCommand(baseDir, "player_plugin_command", steamId, name, runtimeKey,
                std::string("\"message\":\"") + JsonEscape(message) + "\"", 15000);
        }
        return Envelope(false, "Команда плагина не задана.");
    }
    if (path == "/api/player-inventory") {
        auto steamId = QueryValue(req.path, "steamId");
        auto name = QueryValue(req.path, "name");
        if (steamId.empty()) steamId = JsonStringField(req.body, "steamId");
        if (name.empty()) name = JsonStringField(req.body, "name");
        return Envelope(true, PlayerInventoryDbJson(baseDir, steamId, name));
    }
    if (path == "/api/player/wallet") {
        auto steamId = QueryValue(req.path, "steamId");
        auto name = QueryValue(req.path, "name");
        if (steamId.empty()) steamId = JsonStringField(req.body, "steamId");
        if (name.empty()) name = JsonStringField(req.body, "name");
        PlayerWallet wallet;
        if (!ReadPlayerWalletFromDb(baseDir, steamId, name, wallet)) {
            return Envelope(false, wallet.error.empty() ? "player wallet readback failed" : wallet.error);
        }
        return Envelope(true, WalletJson(wallet));
    }
    if (path == "/api/player/attributes") {
        auto steamId = QueryValue(req.path, "steamId");
        auto name = QueryValue(req.path, "name");
        if (steamId.empty()) steamId = JsonStringField(req.body, "steamId");
        if (name.empty()) name = JsonStringField(req.body, "name");
        PrisonerAttributes attributes;
        if (!ReadPrisonerAttributesFromDb(baseDir, steamId, name, attributes)) {
            return Envelope(false, attributes.error.empty() ? "player attributes readback failed" : attributes.error);
        }
        return Envelope(true, AttributesJson(attributes));
    }
    if (path == "/api/player/skills") {
        auto steamId = QueryValue(req.path, "steamId");
        auto name = QueryValue(req.path, "name");
        if (steamId.empty()) steamId = JsonStringField(req.body, "steamId");
        if (name.empty()) name = JsonStringField(req.body, "name");
        PlayerSkillsSnapshot skills;
        if (!ReadPlayerSkillsFromDb(baseDir, steamId, name, skills)) {
            return Envelope(false, skills.error.empty() ? "player prisoner_skill readback failed" : skills.error);
        }
        return Envelope(true, PlayerSkillsJson(skills));
    }
    if (path == "/api/player-details") {
        const auto steamId = QueryValue(req.path, "steamId");
        const auto name = QueryValue(req.path, "name");
        const auto runtimeKey = QueryValue(req.path, "runtimeKey");
        auto br = BridgeExecute(baseDir, "player_details", std::string("{") + PlayerTargetArgs(steamId, name, runtimeKey) + "}", 5000);
        if (!br.ok) return Envelope(false, br.message);
        const auto overlaid = BridgePlayerDetailsWithLogIdentityJson(baseDir, br.body, steamId, name);
        return Envelope(true, overlaid.empty() ? br.body : overlaid);
    }
    if (path == "/api/player/teleport") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        const auto x = JsonIntField(req.body, "x", 0);
        const auto y = JsonIntField(req.body, "y", 0);
        const auto z = JsonIntField(req.body, "z", 0);
        const auto result = BridgePlayerCommand(baseDir, "teleport_player", steamId, name, runtimeKey,
            std::string("\"x\":") + std::to_string(x) +
            ",\"y\":" + std::to_string(y) +
            ",\"z\":" + std::to_string(z), 6000);
        const bool ok = result.find("\"ok\":true") != std::string::npos;
        AppendActionRecord(baseDir, "teleport", ok, steamId, name, ok ? "Телепорт отправлен через live bridge." : "Телепорт отменён: игрок не подтверждён в live runtime.",
            std::string("{\"x\":") + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"z\":" + std::to_string(z) + ",\"preflight\":true}");
        return result;
    }
    if (path == "/api/player/kick") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        const auto reason = JsonStringField(req.body, "reason");
        const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
        auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
        auto targetName = identity.name.empty() ? name : identity.name;
        auto targetRuntimeKey = identity.runtimeKey.empty() ? runtimeKey : identity.runtimeKey;
        const auto liveTarget = ValidatePanelLiveTargetForCommandChannel(
            baseDir, targetSteamId, targetName, targetRuntimeKey, "kick-native-route", false, false, 6000);
        if (!liveTarget.ok) return Envelope(false, liveTarget.message);
        if (!liveTarget.steamId.empty()) targetSteamId = liveTarget.steamId;
        if (!liveTarget.name.empty()) targetName = liveTarget.name;
        if (!liveTarget.runtimeKey.empty()) targetRuntimeKey = liveTarget.runtimeKey;
        const auto target = ArkPanelEnabled(baseDir) ? ScumAdminTargetRef(targetSteamId, targetName) : (!targetSteamId.empty() ? targetSteamId : targetName);
        if (target.empty()) return Envelope(false, "Укажите SteamID64 или безопасное имя игрока.");
        if (ArkPanelEnabled(baseDir)) {
            const auto commandText = std::string("#Kick ") + target + " " + TrimAscii(reason);
            const auto rcon = ArkPanelSendConsoleCommand(baseDir, commandText);
            return rcon.ok
                ? Envelope(true, std::string("{\"transport\":\"hosting-panel-command\",\"command\":\"") + JsonEscape(commandText) + "\",\"status\":" + std::to_string(rcon.status) + ",\"panelBody\":" + JsonValueOrString(rcon.body) + "}")
                : Envelope(false, rcon.message.empty() ? "Консоль хостинга не приняла Kick." : rcon.message);
        }
        auto br = BridgeExecute(baseDir, "admin_exec", std::string("{\"commandText\":\"#Kick ") + JsonEscape(target) + " " + JsonEscape(reason) + "\"}", 5000);
        return br.ok ? Envelope(true, br.body) : Envelope(false, br.message);
    }
    if (path == "/api/player/ban") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        const auto reason = JsonStringField(req.body, "reason");
        const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
        auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
        auto targetName = identity.name.empty() ? name : identity.name;
        auto targetRuntimeKey = identity.runtimeKey.empty() ? runtimeKey : identity.runtimeKey;
        const auto liveTarget = ValidatePanelLiveTargetForCommandChannel(
            baseDir, targetSteamId, targetName, targetRuntimeKey, "ban-native-route", false, false, 6000);
        if (!liveTarget.ok) return Envelope(false, liveTarget.message);
        if (!liveTarget.steamId.empty()) targetSteamId = liveTarget.steamId;
        if (!liveTarget.name.empty()) targetName = liveTarget.name;
        if (!liveTarget.runtimeKey.empty()) targetRuntimeKey = liveTarget.runtimeKey;
        const auto target = ArkPanelEnabled(baseDir) ? ScumAdminTargetRef(targetSteamId, targetName) : (!targetSteamId.empty() ? targetSteamId : targetName);
        if (target.empty()) return Envelope(false, "Укажите SteamID64 или безопасное имя игрока.");
        if (ArkPanelEnabled(baseDir)) {
            const auto commandText = std::string("#Ban ") + target + " " + TrimAscii(reason);
            const auto rcon = ArkPanelSendConsoleCommand(baseDir, commandText);
            return rcon.ok
                ? Envelope(true, std::string("{\"transport\":\"hosting-panel-command\",\"command\":\"") + JsonEscape(commandText) + "\",\"status\":" + std::to_string(rcon.status) + ",\"panelBody\":" + JsonValueOrString(rcon.body) + "}")
                : Envelope(false, rcon.message.empty() ? "Консоль хостинга не приняла Ban." : rcon.message);
        }
        auto br = BridgeExecute(baseDir, "admin_exec", std::string("{\"commandText\":\"#Ban ") + JsonEscape(target) + " " + JsonEscape(reason) + "\"}", 5000);
        return br.ok ? Envelope(true, br.body) : Envelope(false, br.message);
    }
    if (path == "/api/player/grant-item") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        const auto itemId = JsonStringField(req.body, "itemId");
        const auto quantity = JsonIntField(req.body, "quantity", 1);
        return DeliverItemsBatchJson(baseDir, steamId, name, runtimeKey, { GrantItem{ itemId, quantity } }, "panel-grant-item");
    }
    if (path == "/api/player/equip-item") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        const auto itemId = JsonStringField(req.body, "itemId");
        const auto quantity = std::clamp(JsonIntField(req.body, "quantity", 1), 1, 10);
        const auto timeoutMs = std::clamp(JsonIntField(req.body, "timeoutMs", 20000), 3000, 60000);
        if (TrimAscii(itemId).empty()) return Envelope(false, "ID предмета не указан.");
        const auto result = BridgePlayerCommand(baseDir, "equip_item", steamId, name, runtimeKey,
            std::string("\"itemId\":\"") + JsonEscape(itemId) +
            "\",\"quantity\":" + std::to_string(quantity), timeoutMs);
        const bool ok = result.find("\"ok\":true") != std::string::npos;
        AppendActionRecord(baseDir, "panel_equip_item", ok, steamId, name, ok ? result : "Экипировка отменена: игрок не подтверждён в live runtime.",
            std::string("{\"itemId\":\"") + JsonEscape(itemId) + "\",\"quantity\":" + std::to_string(quantity) + ",\"timeoutMs\":" + std::to_string(timeoutMs) + "}");
        return result;
    }
    if (path == "/api/player/item-batch") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        auto items = ParseGrantItems(req.body);
        if (items.empty()) {
            items.push_back(GrantItem{ JsonStringField(req.body, "itemId"), JsonIntField(req.body, "quantity", 1) });
        }
        return DeliverItemsBatchJson(baseDir, steamId, name, runtimeKey, items, "panel-item-batch");
    }
    if (path == "/api/items/prewarm" || path == "/api/item-class/prewarm") {
        const auto itemsSpec = PrewarmItemsSpecFromRequest(req.body);
        if (TrimAscii(itemsSpec).empty()) return Envelope(false, "Предметы для проверки не указаны.");
        const auto timeoutMs = std::clamp(JsonIntField(req.body, "timeoutMs", 30000), 3000, 60000);
        auto br = BridgeExecute(baseDir, "prewarm_items",
            std::string("{\"itemsSpec\":\"") + JsonEscape(itemsSpec) + "\"}", timeoutMs);
        AppendActionRecord(baseDir, "items_prewarm", br.ok, "", "", br.ok ? br.body : br.message,
            std::string("{\"itemsSpec\":\"") + JsonEscape(itemsSpec) + "\",\"timeoutMs\":" + std::to_string(timeoutMs) + "}");
        return br.ok ? Envelope(true, br.body) : Envelope(false, br.message);
    }
    if (path == "/api/player/clear-inventory") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        return BridgePlayerCommand(baseDir, "clear_inventory", steamId, name, runtimeKey, "", 6000);
    }
    if (path == "/api/player/inventory/delete" || path == "/api/player/delete-inventory-item") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        auto entityId = JsonStringField(req.body, "entityId");
        if (entityId.empty()) entityId = JsonStringField(req.body, "itemEntityId");
        if (entityId.empty()) entityId = JsonStringField(req.body, "runtimeId");
        auto itemClass = JsonStringField(req.body, "itemClass");
        if (itemClass.empty()) itemClass = JsonStringField(req.body, "classPath");
        auto assetPath = JsonStringField(req.body, "itemEntitySetup");
        if (assetPath.empty()) assetPath = JsonStringField(req.body, "assetPath");
        if (entityId.empty()) return Envelope(false, "EntityID предмета не указан.");
        return BridgePlayerCommand(baseDir, "delete_inventory_item", steamId, name, runtimeKey,
            std::string("\"entityId\":\"") + JsonEscape(entityId) +
            "\",\"itemEntityId\":\"" + JsonEscape(entityId) +
            "\",\"runtimeId\":\"" + JsonEscape(entityId) +
            "\",\"itemId\":\"" + JsonEscape(JsonStringField(req.body, "itemId")) +
            "\",\"itemClass\":\"" + JsonEscape(itemClass) +
            "\",\"classPath\":\"" + JsonEscape(itemClass) +
            "\",\"itemEntitySetup\":\"" + JsonEscape(assetPath) +
            "\",\"assetPath\":\"" + JsonEscape(assetPath) + "\"", 25000);
    }
    if (path == "/api/player/destroy-inventory-entities") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        auto entitiesText = JsonStringField(req.body, "entitiesText");
        if (entitiesText.empty()) entitiesText = JsonStringField(req.body, "entityIds");
        if (entitiesText.empty()) entitiesText = JsonStringField(req.body, "ids");
        return DestroyInventoryEntitiesEnvelope(baseDir, steamId, name, runtimeKey, entitiesText);
    }
    if (path == "/api/player/change-money") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        const auto amount = JsonIntField(req.body, "amount", 0);
        auto currency = JsonStringField(req.body, "currency");
        if (currency.empty()) currency = "Normal";
        const auto change = ChangeMoneyVerified(baseDir, steamId, name, runtimeKey, amount, currency);
        std::ostringstream data;
        data << "{\"verified\":" << (change.verified ? "true" : "false")
             << ",\"bridgeOk\":" << (change.bridgeOk ? "true" : "false")
             << ",\"dbWrite\":" << (change.dbPatched ? "true" : "false")
             << ",\"liveApply\":\"" << (change.bridgeOk ? "admin-command" : "none") << "\""
             << ",\"message\":\"" << JsonEscape(change.message) << "\""
             << ",\"before\":" << WalletJson(change.before)
             << ",\"after\":" << WalletJson(change.after) << "}";
        return change.ok ? Envelope(true, data.str()) : EnvelopeFailureWithJsonData(change.message, data.str());
    }
    if (path == "/api/player/set-fame") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        const auto amount = JsonIntField(req.body, "amount", 0);
        const auto fame = SetFameVerified(baseDir, steamId, name, runtimeKey, amount);
        std::ostringstream data;
        data << "{\"verified\":" << (fame.verified ? "true" : "false")
             << ",\"bridgeOk\":" << (fame.bridgeOk ? "true" : "false")
             << ",\"dbWrite\":" << (fame.dbPatched ? "true" : "false")
             << ",\"liveApply\":\"" << (fame.bridgeOk ? "admin-command" : "none") << "\""
             << ",\"message\":\"" << JsonEscape(fame.message) << "\""
             << ",\"before\":" << WalletJson(fame.before)
             << ",\"after\":" << WalletJson(fame.after) << "}";
        return fame.ok ? Envelope(true, data.str()) : EnvelopeFailureWithJsonData(fame.message, data.str());
    }
    if (path == "/api/player/change-fame") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        const auto amount = JsonIntField(req.body, "amount", 0);
        const auto fame = ChangeFameVerified(baseDir, steamId, name, runtimeKey, amount);
        std::ostringstream data;
        data << "{\"verified\":" << (fame.verified ? "true" : "false")
             << ",\"bridgeOk\":" << (fame.bridgeOk ? "true" : "false")
             << ",\"dbWrite\":" << (fame.dbPatched ? "true" : "false")
             << ",\"liveApply\":\"" << (fame.bridgeOk ? "admin-command" : "none") << "\""
             << ",\"message\":\"" << JsonEscape(fame.message) << "\""
             << ",\"before\":" << WalletJson(fame.before)
             << ",\"after\":" << WalletJson(fame.after) << "}";
        return fame.ok ? Envelope(true, data.str()) : EnvelopeFailureWithJsonData(fame.message, data.str());
    }
    if (path == "/api/player/set-attributes" || path == "/api/character-stats/apply") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        auto attributes = JsonRawField(req.body, "attributes");
        if (attributes.empty()) attributes = "{}";
        const auto strength = JsonNumberTextField(req.body, "strength", JsonNumberTextField(attributes, "Strength", JsonNumberTextField(attributes, "strength", "0")));
        const auto constitution = JsonNumberTextField(req.body, "constitution", JsonNumberTextField(attributes, "Constitution", JsonNumberTextField(attributes, "constitution", "0")));
        const auto dexterity = JsonNumberTextField(req.body, "dexterity", JsonNumberTextField(attributes, "Dexterity", JsonNumberTextField(attributes, "dexterity", "0")));
        const auto intelligence = JsonNumberTextField(req.body, "intelligence", JsonNumberTextField(attributes, "Intelligence", JsonNumberTextField(attributes, "intelligence", "0")));
        const auto result = BridgePlayerCommand(baseDir, "set_attributes", steamId, name, runtimeKey,
            std::string("\"strength\":") + strength +
            ",\"constitution\":" + constitution +
            ",\"dexterity\":" + dexterity +
            ",\"intelligence\":" + intelligence, 20000);
        const bool ok = result.find("\"ok\":true") != std::string::npos;
        AppendActionRecord(baseDir, "set_attributes_live", ok, steamId, name, ok ? result : "Изменение атрибутов отменено: игрок не подтверждён в live runtime.",
            std::string("{\"requested\":{\"strength\":") + strength +
            ",\"constitution\":" + constitution +
            ",\"dexterity\":" + dexterity +
            ",\"intelligence\":" + intelligence +
            "},\"bridgeOk\":" + (ok ? "true" : "false") +
            ",\"accepted\":" + (ok ? "true" : "false") +
            ",\"liveApply\":\"bridge-admin-command\"}");
        return result;
    }
    if (path == "/api/player/set-skill") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        auto skill = JsonStringField(req.body, "skill");
        if (skill.empty()) skill = JsonStringField(req.body, "skillName");
        const auto level = JsonIntField(req.body, "level", 0);
        const auto experience = JsonIntField(req.body, "experience", JsonIntField(req.body, "skillExperience", 0));
        const auto result = BridgePlayerCommand(baseDir, "set_skill", steamId, name, runtimeKey,
            std::string("\"skill\":\"") + JsonEscape(skill) +
            "\",\"level\":" + std::to_string(std::clamp(level, 0, 4)) +
            ",\"experience\":" + std::to_string(std::max(0, experience)), 20000);
        const bool ok = result.find("\"ok\":true") != std::string::npos;
        AppendActionRecord(baseDir, "set_skill_live", ok, steamId, name, ok ? result : "Изменение навыка отменено: игрок не подтверждён в live runtime.",
            std::string("{\"skill\":\"") + JsonEscape(skill) +
            "\",\"level\":" + std::to_string(std::clamp(level, 0, 4)) +
            ",\"experience\":" + std::to_string(std::max(0, experience)) +
            ",\"liveApply\":\"bridge-admin-command\"}");
        return result;
    }
    if (path == "/api/player/turn-zombie" || path == "/api/player/zombie-infect") {
        return Envelope(false, "Zombie/модельные эксперименты удалены из публичной релизной панели.");
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        auto meshPath = JsonStringField(req.body, "meshPath");
        if (meshPath.empty()) meshPath = JsonStringField(req.body, "MeshPath");
        if (meshPath.empty()) meshPath = JsonStringField(req.body, "ZombieMeshPath");
        auto gender = JsonStringField(req.body, "gender");
        if (gender.empty()) gender = JsonStringField(req.body, "Gender");
        if (gender.empty()) gender = JsonStringField(req.body, "modelGender");
        if (gender.empty()) gender = JsonStringField(req.body, "playerModel");
        auto transformMode = JsonStringField(req.body, "transformMode");
        if (transformMode.empty()) transformMode = JsonStringField(req.body, "TransformMode");
        if (transformMode.empty()) transformMode = JsonStringField(req.body, "mode");
        auto message = JsonStringField(req.body, "message");
        if (message.empty()) message = JsonStringField(req.body, "Message");
        const auto announce = JsonBoolField(req.body, "announce", JsonBoolField(req.body, "Announce", true));
        const auto whisper = JsonBoolField(req.body, "whisper", JsonBoolField(req.body, "Whisper", true));
        const auto result = BridgePlayerCommand(baseDir, "zombie_transform", steamId, name, runtimeKey,
            std::string("\"meshPath\":\"") + JsonEscape(meshPath) +
            "\",\"gender\":\"" + JsonEscape(gender) +
            "\",\"transformMode\":\"" + JsonEscape(transformMode) +
            "\",\"message\":\"" + JsonEscape(message) +
            "\",\"announce\":" + (announce ? "true" : "false") +
            ",\"whisper\":" + (whisper ? "true" : "false"), 25000);
        const bool ok = result.find("\"ok\":true") != std::string::npos;
        AppendActionRecord(baseDir, "zombie_transform", ok, steamId, name, ok ? result : "Zombie transform отменён: игрок не подтверждён в live runtime.",
            std::string("{\"meshPath\":\"") + JsonEscape(meshPath) +
            "\",\"gender\":\"" + JsonEscape(gender) +
            "\",\"transformMode\":\"" + JsonEscape(transformMode) +
            "\",\"announce\":" + (announce ? "true" : "false") +
            ",\"whisper\":" + (whisper ? "true" : "false") + ",\"preflight\":true}");
        return result;
    }
    if (path == "/api/player/spawn-actor" || path == "/api/actor/spawn") {
        return Envelope(false, "UE Actor/SpawnActor эксперименты удалены из публичной релизной панели.");
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        auto actorClass = JsonStringField(req.body, "actorClass");
        if (actorClass.empty()) actorClass = JsonStringField(req.body, "ActorClass");
        if (actorClass.empty()) actorClass = JsonStringField(req.body, "className");
        if (actorClass.empty()) actorClass = JsonStringField(req.body, "actorId");
        if (TrimAscii(actorClass).empty()) return Envelope(false, "Класс актора не указан.");
        const auto identity = ResolvePlayerIdentity(baseDir, steamId, name, runtimeKey);
        const auto targetSteamId = identity.steamId.empty() ? steamId : identity.steamId;
        const auto targetName = identity.name.empty() ? name : identity.name;
        const auto targetRuntimeKey = identity.runtimeKey.empty() ? runtimeKey : identity.runtimeKey;
        auto bridge = BridgeExecute(baseDir, "spawn_actor",
            std::string("{") + PlayerTargetArgs(targetSteamId, targetName, targetRuntimeKey) +
            ",\"actorClass\":\"" + JsonEscape(actorClass) + "\"}", 25000);
        AppendActionRecord(baseDir, "spawn_actor", bridge.ok, targetSteamId, targetName, bridge.ok ? bridge.body : bridge.message,
            std::string("{\"actorClass\":\"") + JsonEscape(actorClass) + "\"}");
        return bridge.ok ? Envelope(true, bridge.body) : Envelope(false, bridge.message.empty() ? "Создание актора не выполнено." : bridge.message);
    }
    if (path == "/api/player/apply-character-pack") {
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        auto pack = JsonStringField(req.body, "pack");
        if (pack.empty()) pack = JsonStringField(req.body, "packName");
        if (pack.empty()) pack = "fullstats";
        const auto result = BridgePlayerCommand(baseDir, "apply_character_pack", steamId, name, runtimeKey,
            std::string("\"pack\":\"") + JsonEscape(pack) + "\"", 20000);
        const bool ok = result.find("\"ok\":true") != std::string::npos;
        AppendActionRecord(baseDir, "apply_character_pack_live", ok, steamId, name, ok ? result : "Character pack отменён: игрок не подтверждён в live runtime.",
            std::string("{\"pack\":\"") + JsonEscape(pack) + "\",\"liveApply\":\"bridge-admin-command\"}");
        return result;
    }
    if (path == "/api/vehicle/destroy" || path == "/api/player/destroy-vehicle") {
        auto vehicleRef = JsonStringField(req.body, "vehicleRef");
        if (vehicleRef.empty()) vehicleRef = JsonStringField(req.body, "entityId");
        if (vehicleRef.empty()) vehicleRef = JsonNumberTextField(req.body, "entityId", "");
        if (vehicleRef.empty()) vehicleRef = QueryValue(req.path, "vehicleRef");
        if (vehicleRef.empty()) vehicleRef = QueryValue(req.path, "entityId");
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        auto reason = JsonStringField(req.body, "reason");
        if (reason.empty()) reason = "panel";
        auto destroy = DestroyVehicleVerified(baseDir, vehicleRef, steamId, name, reason);
        std::ostringstream data;
        data << "{\"verified\":" << (destroy.ok ? "true" : "false")
             << ",\"bridgeOk\":" << (destroy.bridgeOk ? "true" : "false")
             << ",\"entityId\":" << destroy.entityId
             << ",\"existedBefore\":" << (destroy.existedBefore ? "true" : "false")
             << ",\"existsAfter\":" << (destroy.existsAfter ? "true" : "false")
             << ",\"message\":\"" << JsonEscape(destroy.message) << "\""
             << ",\"command\":\"" << JsonEscape(destroy.commandText) << "\""
             << ",\"bridge\":" << (destroy.bridgeBody.empty() ? "{}" : destroy.bridgeBody) << "}";
        return destroy.ok ? Envelope(true, data.str()) : Envelope(false, data.str());
    }
    if (path == "/api/player/spawn-vehicle" || path == "/api/vehicle/spawn") {
        const auto vehicleId = JsonStringField(req.body, "vehicleId");
        const auto steamId = JsonStringField(req.body, "steamId");
        const auto name = JsonStringField(req.body, "name");
        const auto runtimeKey = JsonStringField(req.body, "runtimeKey");
        auto spawn = SpawnVehicleVerified(baseDir, vehicleId, steamId, name, runtimeKey);
        if (!spawn.ok) {
            AppendActionRecord(baseDir, "spawn_vehicle", false, steamId, name, spawn.message,
                std::string("{\"vehicleId\":\"") + JsonEscape(VehicleAdminToken(vehicleId)) + "\",\"attempts\":" + spawn.attemptsJson + "}");
            return Envelope(false, spawn.message + " попытки=" + spawn.attemptsJson);
        }
        const auto destroyRef = spawn.entityId > 0 ? std::to_string(spawn.entityId) : spawn.runtimeRef;
        AppendActionRecord(baseDir, "spawn_vehicle", true, steamId, name, spawn.message,
            std::string("{\"vehicleId\":\"") + JsonEscape(VehicleAdminToken(vehicleId)) +
            "\",\"entityId\":" + std::to_string(spawn.entityId) +
            ",\"runtimeRef\":\"" + JsonEscape(spawn.runtimeRef) +
            "\",\"destroyRef\":\"" + JsonEscape(destroyRef) +
            "\",\"command\":\"" + JsonEscape(spawn.commandText) +
            "\",\"attempts\":" + spawn.attemptsJson + "}");
        return Envelope(true, std::string("{\"ok\":true,\"source\":\"nedjin-verified\",\"command\":\"spawn_vehicle\",\"message\":\"") +
            JsonEscape(spawn.message) +
            "\",\"data\":{\"vehicleId\":\"" + JsonEscape(VehicleAdminToken(vehicleId)) +
            "\",\"entityId\":" + std::to_string(spawn.entityId) +
            ",\"runtimeRef\":\"" + JsonEscape(spawn.runtimeRef) +
            "\",\"destroyRef\":\"" + JsonEscape(destroyRef) +
            "\",\"command\":\"" + JsonEscape(spawn.commandText) +
            "\",\"attempts\":" + spawn.attemptsJson +
            "},\"bridge\":" + (spawn.bridgeBody.empty() ? "{}" : spawn.bridgeBody) + "}");
    }
    if ((path == "/api/chat" || path == "/api/broadcast") && req.method == "POST") {
        const auto message = ScumSingleLineMessage(JsonStringField(req.body, "message"));
        const auto channel = JsonStringField(req.body, "channel");
        auto targetSteam = JsonStringField(req.body, "targetSteamId");
        auto targetName = JsonStringField(req.body, "targetName");
        auto targetRuntimeKey = JsonStringField(req.body, "targetRuntimeKey");
        if (targetRuntimeKey.empty()) targetRuntimeKey = JsonStringField(req.body, "runtimeKey");
        if (IdentityTextLooksUnsafe(targetSteam)) targetSteam.clear();
        if (IdentityTextLooksUnsafe(targetName)) targetName.clear();
        if (message.empty()) return Envelope(false, "Сообщение пустое.");
        if (!targetName.empty() || !targetSteam.empty() || !targetRuntimeKey.empty()) {
            return TargetChatEnvelope(baseDir, "chat", targetSteam, targetName, targetRuntimeKey, message,
                channel.empty() ? "server" : channel, 6000);
        }
        const auto bridgeArgs = std::string("{\"message\":\"") + JsonEscape(message) +
            "\",\"channel\":\"" + JsonEscape(channel.empty() ? "server" : channel) + "\"}";
        const auto bridge = BridgeExecute(baseDir, "chat_broadcast", bridgeArgs, 6000);
        if (bridge.ok) {
            AppendModuleStateRecord(baseDir, "chat",
                std::string("{\"type\":\"chat\",\"timestampUtc\":\"") + UtcIsoNow() +
                "\",\"message\":\"" + JsonEscape(message) +
                "\",\"channel\":\"" + JsonEscape(channel.empty() ? "server" : channel) +
                "\",\"source\":\"panel\",\"targetSteamId\":\"" + JsonEscape(targetSteam) +
                "\",\"targetName\":\"" + JsonEscape(targetName) +
                "\",\"targetRuntimeKey\":\"" + JsonEscape(targetRuntimeKey) +
                "\",\"transport\":\"ue4ss-chat-broadcast\"" +
                ",\"ok\":true}");
            return Envelope(true, std::string("{\"transport\":\"ue4ss-chat-broadcast\",\"bridge\":") +
                (bridge.body.empty() ? "{}" : bridge.body) + "}");
        }
        if (ArkPanelEnabled(baseDir)) {
            auto prefix = std::string("[Panel]");
            const auto normalizedMessage = ToLowerAscii(TrimAscii(message));
            const bool messageHasPanelPrefix =
                normalizedMessage.rfind("[panel]", 0) == 0 ||
                normalizedMessage.rfind("[panel->", 0) == 0;
            const auto commandText = messageHasPanelPrefix
                ? std::string("#Announce ") + message
                : std::string("#Announce ") + prefix + " " + message;
            const auto rcon = ArkPanelSendConsoleCommand(baseDir, commandText);
            AppendModuleStateRecord(baseDir, "chat",
                std::string("{\"type\":\"chat\",\"timestampUtc\":\"") + UtcIsoNow() +
                "\",\"message\":\"" + JsonEscape(message) +
                "\",\"channel\":\"" + JsonEscape(channel.empty() ? "server" : channel) +
                "\",\"source\":\"panel\",\"targetSteamId\":\"" + JsonEscape(targetSteam) +
                "\",\"targetName\":\"" + JsonEscape(targetName) +
                "\",\"targetRuntimeKey\":\"" + JsonEscape(targetRuntimeKey) +
                "\",\"transport\":\"hosting-panel-command-announce\"" +
                ",\"ok\":" + (rcon.ok ? "true" : "false") + "}");
            if (rcon.ok) {
                return Envelope(true, std::string("{\"transport\":\"hosting-panel-command-announce\",\"command\":\"") +
                    JsonEscape(commandText) + "\",\"status\":" + std::to_string(rcon.status) +
                    ",\"panelBody\":" + JsonValueOrString(rcon.body) + "}");
            }
        }
        return Envelope(false, bridge.message.empty()
            ? "Отправка чата через Lua не подтвердилась, консоль хостинга не настроена."
            : bridge.message);
    }
    if (path == "/api/map") {
        const auto mode = ToLowerAscii(TrimAscii(QueryValue(req.path, "mode")));
        const auto live = ToLowerAscii(TrimAscii(QueryValue(req.path, "live")));
        const auto refresh = ToLowerAscii(TrimAscii(QueryValue(req.path, "refresh")));
        const bool wantsLive =
            refresh == "1" || refresh == "true" || refresh == "yes" ||
            mode == "live" ||
            live == "1" || live == "true" || live == "yes";
        if (wantsLive && mode != "log" && live != "0" && live != "false" && live != "no") {
            auto br = BridgeExecute(baseDir, "list_players", "{}", 3000);
            if (br.ok && !br.body.empty()) {
                auto playerBody = BridgePlayersWithLogIdentityJson(baseDir, br.body);
                if (playerBody.empty()) playerBody = br.body;
                const auto playersArray = ExtractPlayersArrayJson(playerBody);
                if (!playersArray.empty()) {
                    return Envelope(true, MapJsonWithPlayers(
                        playersArray,
                        playerBody.find("ue4ss+server-log") != std::string::npos ? "ue4ss positions + server-log identity" : "ue4ss",
                        FlagsDbJson(baseDir),
                        VehiclesDbJson(baseDir)));
                }
            }
        }
        return Envelope(true, MapJson(baseDir));
    }
    if (path == "/api/squads") {
        return Envelope(true, SquadsDbJson(baseDir));
    }
    if (path == "/api/squads/kick") {
        return Envelope(false, "Исключение игрока из отряда через безопасный маршрут пока недоступно в этой версии SCUM. Используйте игровые инструменты управления отрядом.");
    }
    if (path == "/api/flags") {
        return Envelope(true, FlagsDbJson(baseDir));
    }
    if (path == "/api/vehicles") {
        return Envelope(true, VehiclesDbJson(baseDir));
    }
    if (path == "/api/kills") {
        return Envelope(true, StateEventsOrLogJson(baseDir, "kill-feed", "kill", { "LogKill", "killed", "was killed", "Died", "Death" }));
    }
    if (path == "/api/economy") {
        return Envelope(true, EconomyDbJson(baseDir));
    }
    if (path == "/api/chat" || path == "/api/chat-log") {
        return Envelope(true, ChatStateWithLogIdentityJson(baseDir, QueryInt(req.path, "limit", 250, 1, 5000)));
    }
    if (path == "/api/command-trace") {
        return Envelope(true, CommandTraceJson(baseDir));
    }
    if (path == "/api/runtime-log") {
        return Envelope(true, LinesJson(TailLines(ProjectReadableRuntimeLogPath(baseDir), QueryInt(req.path, "limit", 600, 1, 2000), 2 * 1024 * 1024)));
    }
    if (path == "/api/logs/clear") {
        const auto keys = LogClearTargets(req);
        if (keys.empty()) {
            return Envelope(false, "Не выбран журнал для очистки.");
        }
        return Envelope(true, ClearProjectLogsJson(baseDir, keys));
    }
    if (path == "/api/logs") {
        return Envelope(true, std::string("{\"server\":") + LinesJson(TailLines(SavedLogPath(baseDir), QueryInt(req.path, "limit", 600, 1, 2000), 4 * 1024 * 1024)) + ",\"runtime\":" + LinesJson(TailLines(ProjectReadableRuntimeLogPath(baseDir), QueryInt(req.path, "limit", 600, 1, 2000), 2 * 1024 * 1024)) + "}");
    }
    if (path == "/api/server-log") {
        return Envelope(true, LinesJson(TailLines(SavedLogPath(baseDir), QueryInt(req.path, "limit", 600, 1, 2000), 4 * 1024 * 1024)));
    }
    if (path.rfind("/api/", 0) == 0) {
        return Envelope(false, std::string("Unknown API route: ") + path);
    }
    return Envelope(false, "Unknown API route.");
}

void SendAll(SOCKET client, const std::string& bytes) {
    const char* ptr = bytes.data();
    int remaining = static_cast<int>(bytes.size());
    uint64_t retryUntil = GetTickCount64() + 5000;
    while (remaining > 0) {
        const int sent = send(client, ptr, remaining, 0);
        if (sent <= 0) {
            const int err = WSAGetLastError();
            if ((err == WSAEWOULDBLOCK || err == WSAEINTR) && GetTickCount64() < retryUntil) {
                Sleep(10);
                continue;
            }
            break;
        }
        ptr += sent;
        remaining -= sent;
        retryUntil = GetTickCount64() + 5000;
    }
}

void SendResponse(
    SOCKET client,
    int status,
    const std::string& type,
    const std::string& body,
    const std::string& cacheControl = {}) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Error") << "\r\n"
       << "Content-Type: " << type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Access-Control-Allow-Headers: X-API-KEY, Content-Type\r\n"
       << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    if (!cacheControl.empty()) {
        ss << "Cache-Control: " << cacheControl << "\r\n";
    }
    ss << "Connection: close\r\n\r\n" << body;
    SendAll(client, ss.str());
}

void SendBusyResponse(SOCKET client) {
    // This is called on the single accept loop after the handler budget has
    // been reached. Do one non-blocking best-effort write instead of using
    // SendAll's retry window, so a slow peer cannot stall new connections for
    // several seconds while the panel is already under pressure.
    u_long nonBlocking = 1;
    ioctlsocket(client, FIONBIO, &nonBlocking);
    static constexpr const char body[] = "Panel is busy. Retry shortly.";
    std::ostringstream ss;
    ss << "HTTP/1.1 503 Service Unavailable\r\n"
       << "Content-Type: text/plain; charset=utf-8\r\n"
       << "Content-Length: " << (sizeof(body) - 1) << "\r\n"
       << "Cache-Control: no-store\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    const auto response = ss.str();
    send(client, response.data(), static_cast<int>(response.size()), 0);
}

std::string StaticCacheControl(const std::wstring& filePath) {
    const auto extension = ToLowerAscii(WideToUtf8(fs::path(filePath).extension().wstring()));
    return extension == ".html" ? "no-cache" : "public, max-age=3600";
}

std::string ReceiveRequest(SOCKET client) {
    std::string raw;
    char buf[4096];
    int contentLength = 0;
    uint64_t retryUntil = GetTickCount64() + 5000;
    while (true) {
        const int got = recv(client, buf, sizeof(buf), 0);
        if (got <= 0) {
            if (got == SOCKET_ERROR) {
                const int err = WSAGetLastError();
                if ((err == WSAEWOULDBLOCK || err == WSAEINTR) && GetTickCount64() < retryUntil) {
                    Sleep(10);
                    continue;
                }
            }
            break;
        }
        raw.append(buf, got);
        retryUntil = GetTickCount64() + 5000;
        const auto headerEnd = raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            const auto header = raw.substr(0, headerEnd + 4);
            const auto len = HeaderValue(header, "content-length");
            contentLength = len.empty() ? 0 : atoi(len.c_str());
            if (static_cast<int>(raw.size() - headerEnd - 4) >= contentLength) break;
        }
        if (raw.size() > 1024 * 1024) break;
    }
    return raw;
}

void ConfigureHttpClientSocket(SOCKET client) {
    u_long blocking = 0;
    ioctlsocket(client, FIONBIO, &blocking);
    const DWORD timeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

void HandleClient(SOCKET client, std::wstring baseDir, std::string apiKey, int port) {
    ConfigureHttpClientSocket(client);
    try {
        auto raw = ReceiveRequest(client);
        if (raw.empty()) {
            SendResponse(client, 408, "text/plain; charset=utf-8", "Request timeout");
            closesocket(client);
            return;
        }
        auto req = ParseRequest(raw);
        if (req.method == "OPTIONS") {
            SendResponse(client, 200, "text/plain", "");
            closesocket(client);
            return;
        }
        const auto apiPath = ApiPath(req.path);
        if (apiPath.rfind("/api/", 0) == 0) {
            if (apiPath != "/api/health") {
#ifdef SCUM_NEDJIN_PUBLIC_REQUIRE_CONFIGURED_API_KEY
                if (apiKey.empty()) {
                    SendResponse(client, 503, "application/json; charset=utf-8", Envelope(false, "Configure a unique local API key in ScumNeDjin/nedjin.ini, then restart the server."));
                    closesocket(client);
                    return;
                }
#endif
                const auto supplied = HeaderValue(raw, "x-api-key");
                if (!apiKey.empty() && supplied != apiKey) {
                    SendResponse(client, 401, "application/json; charset=utf-8", Envelope(false, "API key is missing or invalid."));
                    closesocket(client);
                    return;
                }
            }
            SendResponse(client, 200, "application/json; charset=utf-8", ApiResponse(baseDir, req, apiKey), "no-store");
            closesocket(client);
            return;
        }

        const auto rel = NormalizeRelativePath(req.path);
        const auto webRoot = JoinPath(JoinPath(baseDir, L"ScumNeDjin"), L"web");
        auto filePath = JoinPath(webRoot, rel);
        if (!FileExists(filePath)) {
            filePath = JoinPath(webRoot, L"index.html");
        }
        auto body = ReadTextFile(filePath);
        if (body.empty() && !FileExists(filePath)) {
            SendResponse(client, 404, "text/plain; charset=utf-8", "Not found");
        } else {
            SendResponse(client, 200, MimeType(filePath), body, StaticCacheControl(filePath));
        }
    } catch (...) {
        SendResponse(client, 500, "application/json; charset=utf-8", Envelope(false, "Native HTTP handler failed."));
        WriteNativeHttpState(baseDir, "handler-error", port);
    }
    closesocket(client);
}

void ServerThread(std::wstring baseDir, int port) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return;
    }
    const auto apiKey = ConfigApiKey(baseDir);

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        WriteNativeHttpState(baseDir, "socket-error", port, std::to_string(WSAGetLastError()));
        return;
    }
    u_long yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        WriteNativeHttpState(baseDir, "bind-error", port, std::to_string(WSAGetLastError()));
        closesocket(server);
        return;
    }
    if (listen(server, SOMAXCONN) == SOCKET_ERROR) {
        WriteNativeHttpState(baseDir, "listen-error", port, std::to_string(WSAGetLastError()));
        closesocket(server);
        return;
    }

    u_long nonBlocking = 1;
    ioctlsocket(server, FIONBIO, &nonBlocking);

    WriteNativeHttpState(baseDir, "listening", port);

    uint64_t lastAcceptErrorStateTick = 0;
    while (true) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            const int err = WSAGetLastError();
            const auto now = GetTickCount64();
            // WSAEWOULDBLOCK is the normal idle path for this non-blocking
            // listener. Persisting it every five seconds caused continuous
            // disk churn even when the panel was unused.
            if (err != WSAEWOULDBLOCK && err != WSAEINTR && now - lastAcceptErrorStateTick > 5000) {
                WriteNativeHttpState(baseDir, "accept-error", port, std::to_string(err));
                lastAcceptErrorStateTick = now;
            }
            Sleep(100);
            continue;
        }
        if (!TryAcquireHttpClientSlot()) {
            SendBusyResponse(client);
            closesocket(client);
            continue;
        }
        try {
            std::thread([client, baseDir, apiKey, port]() {
                HttpClientSlotGuard guard;
                HandleClient(client, baseDir, apiKey, port);
            }).detach();
        } catch (...) {
            g_httpActiveClients.fetch_sub(1, std::memory_order_release);
            g_httpThreadStartFailures.fetch_add(1, std::memory_order_relaxed);
            WriteNativeHttpState(baseDir, "thread-error", port);
            closesocket(client);
        }
    }
}

void StartWargmWorker(const std::wstring& baseDir) {
    bool expected = false;
    if (!g_wargmWorkerStarted.compare_exchange_strong(expected, true)) return;
    std::thread([baseDir]() {
        std::time_t lastPoll = 0;
        const auto requestPath = ModuleStatePath(baseDir, "wargm-sync-request");
        const auto confirmRequestPath = ModuleStatePath(baseDir, "wargm-confirm-request");
        while (true) {
            Sleep(1000);
            try {
                bool shouldSync = false;
                bool shouldConfirm = false;
                if (FileExists(requestPath)) {
                    shouldSync = true;
                    DeleteFileQuiet(requestPath);
                    AppendModuleStateRecord(baseDir, "wargm-shop",
                        std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"sync-request-picked\"}");
                }
                if (FileExists(confirmRequestPath)) {
                    shouldConfirm = true;
                    DeleteFileQuiet(confirmRequestPath);
                    AppendModuleStateRecord(baseDir, "wargm-shop",
                        std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"confirm-request-picked\"}");
                }

                const auto config = PluginConfigJson(baseDir, "wargm-shop", "{}");
                const int interval = JsonIntField(config, "PollIntervalSeconds", JsonIntField(config, "pollIntervalSeconds", 0));
                const auto now = std::time(nullptr);
                if (interval > 0 && JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false)) &&
                    (lastPoll == 0 || now - lastPoll >= interval)) {
                    shouldSync = true;
                }

                if (shouldSync) {
                    lastPoll = now;
                    const auto result = WargmSyncJson(baseDir);
                    const auto deliverResult = WargmDeliverPendingJson(baseDir);
                    const auto confirmResult = WargmConfirmDeliveredJson(baseDir);
                    AppendModuleStateRecord(baseDir, "wargm-shop",
                        std::string("{\"utc\":\"") + UtcIsoNow() +
                        "\",\"status\":\"worker-sync-finished\",\"result\":" + result +
                        ",\"deliverResult\":" + deliverResult +
                        ",\"confirmResult\":" + confirmResult + "}");
                }
                if (shouldConfirm) {
                    const auto result = WargmConfirmDeliveredJson(baseDir);
                    AppendModuleStateRecord(baseDir, "wargm-shop",
                        std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"worker-confirm-finished\",\"result\":" + result + "}");
                }
            } catch (...) {
                AppendModuleStateRecord(baseDir, "wargm-shop",
                    std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"worker-error\"}");
            }
        }
    }).detach();
}

void StartGameStoresWorker(const std::wstring& baseDir) {
    bool expected = false;
    if (!g_gameStoresWorkerStarted.compare_exchange_strong(expected, true)) return;
    std::thread([baseDir]() {
        std::time_t lastPoll = 0;
        const auto requestPath = ModuleStatePath(baseDir, "gamestores-sync-request");
        const auto confirmRequestPath = ModuleStatePath(baseDir, "gamestores-confirm-request");
        const auto claimRequestPath = ModuleStatePath(baseDir, "gamestores-claim-request");
        while (true) {
            Sleep(1000);
            try {
                bool shouldSync = false;
                bool shouldConfirm = false;
                std::string claimRequest;
                if (FileExists(requestPath)) {
                    shouldSync = true;
                    DeleteFileQuiet(requestPath);
                    AppendModuleStateRecord(baseDir, "gamestores-shop",
                        std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"sync-request-picked\"}");
                }
                if (FileExists(confirmRequestPath)) {
                    shouldConfirm = true;
                    DeleteFileQuiet(confirmRequestPath);
                    AppendModuleStateRecord(baseDir, "gamestores-shop",
                        std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"confirm-request-picked\"}");
                }
                if (FileExists(claimRequestPath)) {
                    claimRequest = TrimAscii(ReadTextFile(claimRequestPath));
                    DeleteFileQuiet(claimRequestPath);
                    AppendModuleStateRecord(baseDir, "gamestores-shop",
                        std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"claim-request-picked\"}");
                }

                const auto config = PluginConfigJson(baseDir, "gamestores-shop", "{}");
                const int interval = JsonIntField(config, "PollIntervalSeconds", JsonIntField(config, "pollIntervalSeconds", 0));
                const bool requirePlayerClaim = JsonBoolField(config, "RequirePlayerClaim", JsonBoolField(config, "requirePlayerClaim", false));
                const auto now = std::time(nullptr);
                if (interval > 0 && JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false)) &&
                    (lastPoll == 0 || now - lastPoll >= interval)) {
                    shouldSync = true;
                }

                if (!claimRequest.empty()) {
                    Request claimReq{ "POST", "/api/gamestores/claim", claimRequest };
                    const auto result = GameStoresClaimJson(baseDir, claimReq);
                    AppendModuleStateRecord(baseDir, "gamestores-shop",
                        std::string("{\"utc\":\"") + UtcIsoNow() +
                        "\",\"status\":\"worker-claim-finished\",\"result\":" + result + "}");
                    continue;
                }

                if (shouldSync) {
                    lastPoll = now;
                    std::lock_guard<std::recursive_mutex> operationGuard(g_gameStoresOperationMutex);
                    const auto result = GameStoresSyncJson(baseDir);
                    const auto deliverResult = requirePlayerClaim
                        ? Envelope(true, "{\"skipped\":true,\"reason\":\"require-player-claim\"}")
                        : GameStoresDeliverPendingJson(baseDir);
                    const auto confirmResult = GameStoresConfirmDeliveredJson(baseDir);
                    AppendModuleStateRecord(baseDir, "gamestores-shop",
                        std::string("{\"utc\":\"") + UtcIsoNow() +
                        "\",\"status\":\"worker-sync-finished\",\"result\":" + result +
                        ",\"deliverResult\":" + deliverResult +
                        ",\"confirmResult\":" + confirmResult + "}");
                }
                if (shouldConfirm) {
                    std::lock_guard<std::recursive_mutex> operationGuard(g_gameStoresOperationMutex);
                    const auto result = GameStoresConfirmDeliveredJson(baseDir);
                    AppendModuleStateRecord(baseDir, "gamestores-shop",
                        std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"worker-confirm-finished\",\"result\":" + result + "}");
                }
            } catch (...) {
                AppendModuleStateRecord(baseDir, "gamestores-shop",
                    std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"status\":\"worker-error\"}");
            }
        }
    }).detach();
}

std::string DecodeScumSaveLogBytes(const std::string& bytes) {
    if (bytes.size() >= 2) {
        if (static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
            std::string out;
            for (size_t i = 2; i + 1 < bytes.size(); i += 2) out.push_back(bytes[i]);
            return out;
        }
        if (static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF) {
            std::string out;
            for (size_t i = 2; i + 1 < bytes.size(); i += 2) out.push_back(bytes[i + 1]);
            return out;
        }
    }
    size_t nulCount = 0;
    for (const char ch : bytes) {
        if (ch == '\0') ++nulCount;
    }
    if (!bytes.empty() && nulCount * 3 > bytes.size()) {
        std::string out;
        out.reserve(bytes.size() / 2 + 1);
        for (size_t i = 0; i < bytes.size(); i += 2) {
            if (bytes[i] != '\0') out.push_back(bytes[i]);
        }
        return out;
    }
    return bytes;
}

std::string ReadScumSaveLogRange(const std::wstring& path, uintmax_t offset, size_t maxBytes = 512 * 1024) {
    std::ifstream file(fs::path(path), std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto endPos = file.tellg();
    if (endPos <= 0) return {};
    const auto fileSize = static_cast<uintmax_t>(endPos);
    if (offset >= fileSize) return {};
    const auto readSize = static_cast<size_t>(std::min<uintmax_t>(fileSize - offset, maxBytes));
    if (offset % 2 != 0) --offset;
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::string buffer(readSize, '\0');
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<size_t>(file.gcount()));
    return DecodeScumSaveLogBytes(buffer);
}

std::string StripScumLogTimestamp(std::string line, std::string* timestampUtc = nullptr) {
    line = TrimAscii(line);
    if (line.size() >= 22 && line[0] == '[' &&
        std::isdigit(static_cast<unsigned char>(line[1])) &&
        std::isdigit(static_cast<unsigned char>(line[2])) &&
        std::isdigit(static_cast<unsigned char>(line[3])) &&
        std::isdigit(static_cast<unsigned char>(line[4])) &&
        line[5] == '.' && line[8] == '.' && line[11] == '-' &&
        line[14] == '.' && line[17] == '.' && line[20] == ':') {
        if (timestampUtc) {
            *timestampUtc = line.substr(1, 4) + "-" + line.substr(6, 2) + "-" + line.substr(9, 2) +
                "T" + line.substr(12, 2) + ":" + line.substr(15, 2) + ":" + line.substr(18, 2) + "Z";
        }
        const auto logPrefix = line.find("LogSCUM:");
        if (logPrefix != std::string::npos) return TrimAscii(line.substr(logPrefix + 8));
        const auto secondBracket = line.find(']', 22);
        if (secondBracket != std::string::npos) return TrimAscii(line.substr(secondBracket + 1));
        return TrimAscii(line.substr(22));
    }
    if (line.size() >= 20 &&
        std::isdigit(static_cast<unsigned char>(line[0])) &&
        std::isdigit(static_cast<unsigned char>(line[1])) &&
        std::isdigit(static_cast<unsigned char>(line[2])) &&
        std::isdigit(static_cast<unsigned char>(line[3])) &&
        line[4] == '.' && line[7] == '.' && line[10] == '-' &&
        line[13] == '.' && line[16] == '.' && line[19] == ':') {
        if (timestampUtc) {
            *timestampUtc = line.substr(0, 4) + "-" + line.substr(5, 2) + "-" + line.substr(8, 2) +
                "T" + line.substr(11, 2) + ":" + line.substr(14, 2) + ":" + line.substr(17, 2) + "Z";
        }
        return TrimAscii(line.substr(20));
    }
    if (timestampUtc) *timestampUtc = UtcIsoNow();
    return line;
}

std::string KillLogActorName(std::string value) {
    value = TrimAscii(value);
    const auto colon = value.find(':');
    if (colon != std::string::npos && colon + 1 < value.size()) {
        value = value.substr(colon + 1);
    }
    const auto profileSuffix = value.rfind('(');
    if (profileSuffix != std::string::npos) {
        value = value.substr(0, profileSuffix);
    }
    value = TrimAscii(value);
    return value.empty() ? "Unknown" : value;
}

std::string KillFeedPrefix(const std::wstring& baseDir) {
    const auto config = PluginConfigJson(baseDir, "server-kill-feed",
        R"JSON({"Enabled":true,"Prefix":"WILD","IncludeWeapon":true,"AnnounceSuicides":true})JSON");
    auto prefix = TrimAscii(JsonStringField(config, "Prefix"));
    if (prefix.empty()) prefix = TrimAscii(JsonStringField(config, "prefix"));
    if (prefix.empty()) prefix = "WILD";
    if (prefix.front() == '[') return prefix;
    return "[" + prefix + "]";
}

bool ServerKillFeedEnabled(const std::wstring& baseDir) {
    const auto config = PluginConfigJson(baseDir, "server-kill-feed", R"JSON({"Enabled":true})JSON");
    return JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", true));
}

bool ParseScumDeathPayload(
    const std::string& payload,
    std::string& message,
    std::string& victim,
    std::string& killer,
    std::string& weapon,
    bool& suicideOrDeath) {
    static const std::regex deathPattern(
        R"(^\s*Died:\s*(.*?)\s*\((\d+)\),\s*Killer:\s*(.*?)\s*\((\d+)\)\s*Weapon:\s*(.*?)\s*(?:\[[^\]]*\])?\s*$)",
        std::regex_constants::icase);
    std::smatch match;
    if (std::regex_match(payload, match, deathPattern)) {
        victim = TrimAscii(match[1].str());
        killer = TrimAscii(match[3].str());
        weapon = TrimAscii(match[5].str());
        const auto victimId = TrimAscii(match[2].str());
        const auto killerId = TrimAscii(match[4].str());
        suicideOrDeath = killer.empty() || killer == victim || killerId == victimId || killer == "None" || killer == "Unknown";
    } else {
        static const std::regex scumLogKilledPattern(
            R"(^.*?'([^']+?)'\s+was killed by\s+'([^']+?)'.*$)",
            std::regex_constants::icase);
        if (!std::regex_match(payload, match, scumLogKilledPattern)) return false;
        const auto victimRaw = TrimAscii(match[1].str());
        const auto killerRaw = TrimAscii(match[2].str());
        victim = KillLogActorName(victimRaw);
        killer = KillLogActorName(killerRaw);
        weapon.clear();
        suicideOrDeath = killerRaw == victimRaw || killer.empty() || killer == victim || killer == "None" || killer == "Unknown";
    }
    if (suicideOrDeath) {
        message = victim + " погиб";
    } else {
        message = killer + " убил " + victim;
    }
    if (!weapon.empty() && weapon != "None" && weapon != "Unknown") {
        message += " (" + weapon + ")";
    }
    return true;
}

std::vector<std::wstring> KillLogFiles(const std::wstring& baseDir) {
    std::vector<std::wstring> files;
    std::error_code ec;
    const auto scumLog = fs::path(SavedLogPath(baseDir));
    if (fs::exists(scumLog, ec) && scumLog.has_filename()) {
        files.push_back(scumLog.wstring());
    }
    ec.clear();
    const auto dir = fs::path(SaveFilesLogsPath(baseDir));
    if (fs::exists(dir, ec)) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec || !entry.is_regular_file(ec)) continue;
            const auto name = WideToUtf8(entry.path().filename().wstring());
            const auto lower = ToLowerAscii(name);
            if ((lower.rfind("kill_", 0) == 0 || lower.rfind("event_kill_", 0) == 0) &&
                lower.size() > 4 && lower.find(".log") != std::string::npos) {
                files.push_back(entry.path().wstring());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

void ProcessKillLogLine(const std::wstring& baseDir, const std::wstring& filePath, const std::string& line) {
    std::string timestampUtc;
    const auto payload = StripScumLogTimestamp(line, &timestampUtc);
    if (payload.empty() || ContainsCi(payload, "Game version")) return;

    std::string message;
    std::string victim;
    std::string killer;
    std::string weapon;
    bool suicideOrDeath = false;
    if (!ParseScumDeathPayload(payload, message, victim, killer, weapon, suicideOrDeath)) return;

    const auto config = PluginConfigJson(baseDir, "server-kill-feed",
        R"JSON({"Enabled":true,"AnnounceSuicides":true,"IncludeWeapon":true})JSON");
    if (suicideOrDeath && !JsonBoolField(config, "AnnounceSuicides", JsonBoolField(config, "announceSuicides", true))) {
        return;
    }

    const auto fullMessage = KillFeedPrefix(baseDir) + " " + message;
    const auto record = std::string("{\"type\":\"kill\",\"timestampUtc\":\"") + JsonEscape(timestampUtc.empty() ? UtcIsoNow() : timestampUtc) +
        "\",\"message\":\"" + JsonEscape(message) +
        "\",\"broadcastMessage\":\"" + JsonEscape(fullMessage) +
        "\",\"killerName\":\"" + JsonEscape(killer) +
        "\",\"victimName\":\"" + JsonEscape(victim) +
        "\",\"weapon\":\"" + JsonEscape(weapon) +
        "\",\"source\":\"server-log\",\"logFile\":\"" + JsonEscape(WideToUtf8(fs::path(filePath).filename().wstring())) +
        "\",\"raw\":\"" + JsonEscape(payload) + "\"}";
    AppendModuleStateRecord(baseDir, "kill-feed", record);
    AppendModuleStateRecord(baseDir, "server-kill-feed", record);

    if (!ServerKillFeedEnabled(baseDir)) return;
    auto br = BridgeExecute(baseDir, "kill_feed_broadcast",
        std::string("{\"message\":\"") + JsonEscape(fullMessage) + "\"}",
        2500);
    AppendActionRecord(baseDir, "server_kill_feed_log_broadcast", br.ok, "", "",
        br.ok ? "kill feed sent to global chat" : br.message,
        std::string("{\"message\":\"") + JsonEscape(fullMessage) +
        "\",\"source\":\"server-log\",\"bridgeOk\":" + (br.ok ? "true" : "false") + "}");
}

void StartKillLogWorker(const std::wstring& baseDir) {
    bool expected = false;
    if (!g_killLogWorkerStarted.compare_exchange_strong(expected, true)) return;
    std::thread([baseDir]() {
        std::map<std::wstring, uintmax_t> offsets;
        bool firstScan = true;
        while (true) {
            try {
                const auto files = KillLogFiles(baseDir);
                for (const auto& path : files) {
                    const auto size = FileSizeOrZero(path);
                    auto it = offsets.find(path);
                    if (it == offsets.end()) {
                        offsets[path] = firstScan ? size : 0;
                        continue;
                    }
                    if (size < it->second) it->second = 0;
                    if (size <= it->second) continue;
                    const auto text = ReadScumSaveLogRange(path, it->second);
                    it->second = size;
                    for (const auto& line : SplitLines(text)) {
                        ProcessKillLogLine(baseDir, path, line);
                    }
                }
                firstScan = false;
            } catch (...) {
                AppendActionRecord(baseDir, "server_kill_feed_log_worker", false, "", "",
                    "kill log worker caught an exception", "{}");
            }
            Sleep(2000);
        }
    }).detach();
}

std::string NormalizeDedupeText(std::string value) {
    value = TrimAscii(std::move(value));
    std::string out;
    out.reserve(value.size());
    bool pendingSpace = false;
    for (const auto ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isspace(c)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

long long DiscordTimestampBucket(const std::string& timestamp, int windowSeconds) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(timestamp.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;
        const auto epoch = _mkgmtime(&tm);
        if (epoch > 0) return static_cast<long long>(epoch) / std::max(1, windowSeconds);
    }
    return 0;
}

std::string DiscordRecordKey(const std::string& record, int duplicateWindowSeconds) {
    auto timestamp = JsonStringField(record, "utc");
    if (timestamp.empty()) timestamp = JsonStringField(record, "timestampUtc");
    auto route = NormalizeDedupeText(JsonStringField(record, "route"));
    if (route.empty()) route = "system";
    auto message = JsonStringField(record, "message");
    if (message.empty()) message = JsonStringField(record, "description");
    if (message.empty()) message = JsonStringField(record, "content");
    auto actor = FirstNonEmpty({
        JsonStringField(record, "steamId"),
        JsonStringField(record, "SteamId"),
        JsonStringField(record, "steam"),
        JsonStringField(record, "player"),
        JsonStringField(record, "name")
    });
    return std::to_string(DiscordTimestampBucket(timestamp, duplicateWindowSeconds))
        + "|" + route
        + "|" + NormalizeDedupeText(actor)
        + "|" + NormalizeDedupeText(message);
}

std::string DiscordRecordPayload(const std::string& config, const std::string& record) {
    auto username = JsonStringField(config, "Username");
    if (username.empty()) username = "SCUM NeDjin";
    const auto avatar = JsonStringField(config, "AvatarUrl");
    auto route = JsonStringField(record, "route");
    if (route.empty()) route = "system";
    auto message = JsonStringField(record, "message");
    if (message.empty()) message = JsonStringField(record, "description");
    if (message.empty()) message = JsonStringField(record, "content");
    auto title = JsonStringField(record, "title");
    if (title.empty()) {
        if (route == "chat") title = "Чат сервера";
        else if (route == "combat") title = "Событие боя";
        else if (route == "presence") title = "Онлайн";
        else title = "SCUM NeDjin";
    }
    auto timestamp = JsonStringField(record, "utc");
    if (timestamp.empty()) timestamp = JsonStringField(record, "timestampUtc");
    if (timestamp.empty()) timestamp = UtcIsoNow();
    std::ostringstream payload;
    payload << "{\"username\":\"" << JsonEscape(username) << "\"";
    if (!avatar.empty()) payload << ",\"avatar_url\":\"" << JsonEscape(avatar) << "\"";
    payload << ",\"embeds\":[{\"title\":\"" << JsonEscape(title)
            << "\",\"description\":\"" << JsonEscape(message)
            << "\",\"timestamp\":\"" << JsonEscape(timestamp) << "\"";
    if (route == "combat") payload << ",\"color\":15158332";
    else if (route == "chat") payload << ",\"color\":3447003";
    else if (route == "presence") payload << ",\"color\":5763719";
    else payload << ",\"color\":9807270";
    payload << "}]}";
    return payload.str();
}

void StartDiscordLogWorker(const std::wstring& baseDir) {
    bool expected = false;
    if (!g_discordLogWorkerStarted.compare_exchange_strong(expected, true)) return;
    std::thread([baseDir]() {
        std::map<std::string, bool> seen;
        bool primed = false;
        while (true) {
            try {
                const auto config = PluginConfigJson(baseDir, "discord-log", "{}");
                const bool enabled = JsonBoolField(config, "Enabled", JsonBoolField(config, "enabled", false));
                const int minDelayMs = std::max(0, JsonIntField(config, "MinPostIntervalMs", JsonIntField(config, "minPostIntervalMs", 800)));
                const int timeoutSeconds = JsonIntField(config, "HttpTimeoutSeconds", JsonIntField(config, "httpTimeoutSeconds", 10));
                const int duplicateWindowSeconds = std::clamp(JsonIntField(config, "DuplicateWindowSeconds", JsonIntField(config, "duplicateWindowSeconds", 15)), 1, 300);
                const auto state = ModuleStateTailJson(baseDir, "discord-log", 400);
                const auto records = ExtractArrayObjects(state);
                if (!primed || !enabled) {
                    for (const auto& record : records) {
                        const auto key = DiscordRecordKey(record, duplicateWindowSeconds);
                        if (!key.empty()) seen[key] = true;
                    }
                    primed = true;
                    Sleep(2000);
                    continue;
                }
                for (const auto& record : records) {
                    if (JsonStringField(record, "source") == "native-discord-worker") continue;
                    auto message = JsonStringField(record, "message");
                    if (message.empty()) message = JsonStringField(record, "description");
                    if (message.empty()) message = JsonStringField(record, "content");
                    if (message.empty()) continue;
                    auto route = JsonStringField(record, "route");
                    if (route.empty()) route = "system";
                    const auto key = DiscordRecordKey(record, duplicateWindowSeconds);
                    if (key.empty() || seen[key]) continue;
                    seen[key] = true;
                    const auto webhook = DiscordWebhookForRoute(config, route);
                    if (webhook.empty()) {
                        AppendModuleStateRecord(baseDir, "discord-log",
                            std::string("{\"utc\":\"") + UtcIsoNow() +
                            "\",\"route\":\"system\",\"message\":\"Discord webhook не настроен для route=" + JsonEscape(route) +
                            "\",\"ok\":false,\"source\":\"native-discord-worker\"}");
                        continue;
                    }
                    const auto post = HttpPostJson(webhook, DiscordRecordPayload(config, record), timeoutSeconds);
                    AppendModuleStateRecord(baseDir, "discord-log",
                        std::string("{\"utc\":\"") + UtcIsoNow() +
                        "\",\"route\":\"" + JsonEscape(route) +
                        "\",\"message\":\"" + JsonEscape(message) +
                        "\",\"ok\":" + (post.ok ? "true" : "false") +
                        ",\"status\":" + std::to_string(post.status) +
                        ",\"result\":\"" + JsonEscape(post.message) +
                        "\",\"source\":\"native-discord-worker\"}");
                    if (minDelayMs > 0) Sleep(static_cast<DWORD>(minDelayMs));
                }
                if (seen.size() > 2000) {
                    seen.clear();
                    for (const auto& record : records) {
                        const auto key = DiscordRecordKey(record, duplicateWindowSeconds);
                        if (!key.empty()) seen[key] = true;
                    }
                }
            } catch (...) {
                AppendModuleStateRecord(baseDir, "discord-log",
                    std::string("{\"utc\":\"") + UtcIsoNow() + "\",\"route\":\"system\",\"message\":\"Discord worker exception\",\"ok\":false,\"source\":\"native-discord-worker\"}");
            }
            Sleep(2000);
        }
    }).detach();
}


}

void StartHttpServer(const std::wstring& baseDir) {
    bool expected = false;
    if (!g_httpServerStarted.compare_exchange_strong(expected, true)) return;

    g_httpClientLimit.store(
        ConfigIntValue(baseDir, "http_max_clients", 32, 4, 128),
        std::memory_order_relaxed);
    StartNativeFileCommandWorker(baseDir);
    StartNativeMoneyFileCommandWorker(baseDir);
    StartWargmWorker(baseDir);
    StartGameStoresWorker(baseDir);
    StartScheduledEventsWorker(baseDir);
    StartKillLogWorker(baseDir);
    StartDiscordLogWorker(baseDir);
    for (const int port : ConfigHttpPorts(baseDir)) {
        try {
            std::thread(ServerThread, baseDir, port).detach();
        } catch (...) {
            WriteNativeHttpState(baseDir, "listener-thread-error", port);
        }
    }
}
}
