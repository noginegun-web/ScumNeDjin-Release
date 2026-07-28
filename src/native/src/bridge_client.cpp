#include "bridge_client.h"
#include "path_utils.h"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace nedjin {

namespace {

std::wstring BridgeDir(const std::wstring& baseDir) {
    return JoinPath(baseDir, L"nedjin_bridge");
}

std::string RandomId() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<unsigned long long> dist;
    std::ostringstream ss;
    ss << std::hex << dist(gen) << dist(gen);
    return ss.str();
}

bool IsHeartbeatFresh(const std::wstring& heartbeatPath, long long* ageMsOut = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(heartbeatPath.c_str(), GetFileExInfoStandard, &attrs)) {
        return false;
    }
    FILETIME nowFileTime{};
    GetSystemTimeAsFileTime(&nowFileTime);
    ULARGE_INTEGER now{}, file{};
    now.LowPart = nowFileTime.dwLowDateTime;
    now.HighPart = nowFileTime.dwHighDateTime;
    file.LowPart = attrs.ftLastWriteTime.dwLowDateTime;
    file.HighPart = attrs.ftLastWriteTime.dwHighDateTime;
    const auto diff100ns = now.QuadPart > file.QuadPart ? now.QuadPart - file.QuadPart : 0;
    const auto ageMs = static_cast<long long>(diff100ns / 10000ULL);
    if (ageMsOut) {
        *ageMsOut = ageMs;
    }
    return ageMs < 15000;
}

std::string ExtractJsonString(const std::string& text, const std::string& key) {
    const auto needle = "\"" + key + "\"";
    auto pos = text.find(needle);
    if (pos == std::string::npos) return {};
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = text.find('"', pos);
    if (pos == std::string::npos) return {};
    std::string out;
    bool esc = false;
    for (++pos; pos < text.size(); ++pos) {
        char ch = text[pos];
        if (esc) {
            switch (ch) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(ch); break;
            }
            esc = false;
            continue;
        }
        if (ch == '\\') {
            esc = true;
            continue;
        }
        if (ch == '"') {
            break;
        }
        out.push_back(ch);
    }
    return out;
}

}

std::string BridgeStatusJson(const std::wstring& baseDir) {
    const auto dir = BridgeDir(baseDir);
    const auto heartbeat = JoinPath(dir, L"heartbeat.json");
    long long ageMs = -1;
    const bool online = IsHeartbeatFresh(heartbeat, &ageMs);
    std::ostringstream ss;
    ss << "{\"online\":" << (online ? "true" : "false")
       << ",\"bridgePath\":\"" << JsonEscape(WideToUtf8(dir)) << "\"";
    if (ageMs >= 0) {
        ss << ",\"heartbeatAgeSeconds\":" << (static_cast<double>(ageMs) / 1000.0);
    } else {
        ss << ",\"heartbeatAgeSeconds\":null";
    }
    ss << ",\"message\":\"" << (online ? "Bridge heartbeat свежий." : "Bridge heartbeat не найден или устарел.") << "\"}";
    return ss.str();
}

BridgeResult BridgeExecute(const std::wstring& baseDir, const std::string& command, const std::string& argsJson, int timeoutMs) {
    static std::mutex bridgeMutex;
    // The HTTP server gives every request a client slot before it reaches this
    // function.  Waiting here for an unrelated 25-45 second bridge command
    // used to let concurrent requests exhaust all slots, including / and
    // /api/health.  Keep the single-writer guarantee for command.json, but
    // reject a concurrent command immediately so its HTTP slot is released.
    std::unique_lock<std::mutex> lock(bridgeMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return { false, {}, "Bridge уже выполняет другую команду. Повторите запрос через несколько секунд." };
    }

    const auto dir = BridgeDir(baseDir);
    EnsureDirectory(dir);
    const auto heartbeat = JoinPath(dir, L"heartbeat.json");
    if (!IsHeartbeatFresh(heartbeat)) {
        return { false, {}, "Bridge heartbeat не найден или устарел." };
    }

    const auto commandPath = JoinPath(dir, L"command.json");
    const auto resultPath = JoinPath(dir, L"result.json");
    DeleteFileQuiet(resultPath);
    DeleteFileQuiet(commandPath);

    const auto id = RandomId();
    std::ostringstream request;
    request << "{\"id\":\"" << id
            << "\",\"command\":\"" << JsonEscape(command)
            << "\",\"args\":" << (argsJson.empty() ? "{}" : argsJson)
            << ",\"createdUtc\":\"" << UtcIsoNow()
            << "\",\"timeoutMs\":" << timeoutMs << "}";

    if (!WriteTextFile(commandPath, request.str())) {
        return { false, {}, "Не удалось записать файл команды bridge." };
    }

    const DWORD started = GetTickCount();
    while (GetTickCount() - started < static_cast<DWORD>(timeoutMs)) {
        if (FileExists(resultPath)) {
            std::string body;
            for (int attempt = 0; attempt < 20; ++attempt) {
                Sleep(5);
                body = ReadTextFile(resultPath);
                const auto trimmed = body;
                if (trimmed.find("\"ok\"") != std::string::npos &&
                    (!trimmed.empty() && (trimmed.back() == '}' || trimmed.back() == '\n' || trimmed.back() == '\r'))) {
                    break;
                }
            }
            DeleteFileQuiet(resultPath);
            if (body.empty()) {
                return { false, {}, "Bridge вернул пустой результат." };
            }
            const auto resultId = ExtractJsonString(body, "id");
            if (resultId != id) {
                continue;
            }
            const auto message = ExtractJsonString(body, "message");
            const bool ok = body.find("\"ok\":true") != std::string::npos;
            return { ok, body, message.empty() ? (ok ? "ok" : "bridge вернул ошибку") : message };
        }
        Sleep(10);
    }

    DeleteFileQuiet(commandPath);
    return { false, {}, "Команда bridge не ответила вовремя." };
}

}
