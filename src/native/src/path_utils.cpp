#include "path_utils.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

namespace warden {

namespace {

std::atomic<uint64_t> g_exactWriteSequence{ 0 };

std::wstring ExactWriteTempPath(const std::wstring& path) {
    const auto target = fs::path(path);
    const auto sequence = g_exactWriteSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    return (target.parent_path() / (target.filename().wstring() +
        L".tmp.exact." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(GetTickCount64()) +
        L"." + std::to_wstring(sequence))).wstring();
}

bool WriteAllAndFlush(HANDLE file, const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const auto remaining = text.size() - offset;
        const auto chunk = static_cast<DWORD>(std::min<size_t>(remaining, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(file, text.data() + offset, chunk, &written, nullptr) || written != chunk) {
            return false;
        }
        offset += written;
    }
    return FlushFileBuffers(file) != FALSE;
}

bool CreateExactTempFile(const std::wstring& path, const std::string& text, std::wstring& temporary) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        temporary = ExactWriteTempPath(path);
        HANDLE file = CreateFileW(
            temporary.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            temporary.clear();
            return false;
        }

        const bool written = WriteAllAndFlush(file, text);
        const bool closed = CloseHandle(file) != FALSE;
        if (written && closed) return true;
        DeleteFileW(temporary.c_str());
        temporary.clear();
        return false;
    }
    temporary.clear();
    return false;
}

}

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    HMODULE module{};
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleDirectory),
        &module);
    GetModuleFileNameW(module, path, MAX_PATH);
    return fs::path(path).parent_path().wstring();
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    return (fs::path(left) / fs::path(right)).wstring();
}

bool FileExists(const std::wstring& path) {
    return fs::exists(path) && fs::is_regular_file(path);
}

bool DirectoryExists(const std::wstring& path) {
    return fs::exists(path) && fs::is_directory(path);
}

void EnsureDirectory(const std::wstring& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
}

std::string ReadTextFile(const std::wstring& path) {
    const auto livePath = fs::path(path).wstring() + L".live";
    std::ifstream file(FileExists(livePath) ? fs::path(livePath) : fs::path(path), std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool ReadTextFileExact(const std::wstring& path, std::string& text) {
    text.clear();
    std::ifstream file(fs::path(path), std::ios::binary);
    if (!file) return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    if (file.bad()) return false;
    text = ss.str();
    return true;
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    EnsureDirectory(fs::path(path).parent_path().wstring());
    {
        std::ofstream file(fs::path(path), std::ios::binary | std::ios::trunc);
        if (file) {
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.flush();
            if (file.good()) {
                DeleteFileW((fs::path(path).wstring() + L".live").c_str());
                return true;
            }
        }
    }

    const auto target = fs::path(path);
    const auto tmp = target.parent_path() / (
        target.filename().wstring() +
        L".tmp." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(GetTickCount64()));

    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file.good()) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }

    if (FileExists(path)) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED)) {
        return true;
    }

    DeleteFileW(path.c_str());
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED)) {
        return true;
    }

    std::error_code ec;
    fs::remove(tmp, ec);
    const auto livePath = fs::path(path).wstring() + L".live";
    std::ofstream liveFile(fs::path(livePath), std::ios::binary | std::ios::trunc);
    if (!liveFile) {
        return false;
    }
    liveFile.write(text.data(), static_cast<std::streamsize>(text.size()));
    liveFile.flush();
    return liveFile.good();
}

bool WriteTextFileAtomicallyExact(const std::wstring& path, const std::string& text, const std::wstring& backupPath) {
    try {
        EnsureDirectory(fs::path(path).parent_path().wstring());
    } catch (...) {
        return false;
    }

    std::wstring temporary;
    if (!CreateExactTempFile(path, text, temporary)) return false;

    const bool targetExists = FileExists(path);
    bool replaced = false;
    if (targetExists) {
        if (backupPath.empty() || FileExists(backupPath)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        replaced = ReplaceFileW(
            path.c_str(),
            temporary.c_str(),
            backupPath.c_str(),
            REPLACEFILE_WRITE_THROUGH,
            nullptr,
            nullptr) != FALSE;
    } else if (backupPath.empty()) {
        replaced = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    }

    if (!replaced) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::string Sha256Hex(const std::string& text) {
    if (text.size() > static_cast<size_t>(std::numeric_limits<ULONG>::max())) return {};

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};

    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD returned = 0;
    const bool propertiesOk =
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &returned, 0) == 0 &&
        returned == sizeof(objectLength) &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &returned, 0) == 0 &&
        returned == sizeof(hashLength) &&
        objectLength > 0 && hashLength > 0;
    if (!propertiesOk) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    std::vector<UCHAR> object(objectLength);
    std::vector<UCHAR> hash(hashLength);
    BCRYPT_HASH_HANDLE handle = nullptr;
    if (BCryptCreateHash(algorithm, &handle, object.data(), objectLength, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    const auto data = text.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<char*>(text.data()));
    const bool hashOk =
        BCryptHashData(handle, data, static_cast<ULONG>(text.size()), 0) == 0 &&
        BCryptFinishHash(handle, hash.data(), hashLength, 0) == 0;
    BCryptDestroyHash(handle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!hashOk) return {};

    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (const auto byte : hash) out << std::setw(2) << static_cast<unsigned int>(byte);
    return out.str();
}

bool DeleteFileQuiet(const std::wstring& path) {
    std::error_code ec;
    const auto removedLive = fs::remove(fs::path(path).wstring() + L".live", ec);
    ec.clear();
    const auto removedBase = fs::remove(path, ec);
    return removedLive || removedBase;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8]{};
                sprintf_s(buf, "\\u%04x", ch);
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    return out;
}

std::string UtcIsoNow() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &time);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

std::string UrlDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const auto hex = value.substr(i + 1, 2);
            char* end{};
            const long code = strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0') {
                out.push_back(static_cast<char>(code));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return out;
}

std::wstring NormalizeRelativePath(const std::string& path) {
    std::string clean = path;
    const auto q = clean.find('?');
    if (q != std::string::npos) {
        clean.resize(q);
    }
    clean = UrlDecode(clean);
    if (clean.empty() || clean == "/") {
        clean = "/index.html";
    }
    while (!clean.empty() && clean.front() == '/') {
        clean.erase(clean.begin());
    }
    fs::path rel = Utf8ToWide(clean);
    fs::path normalized;
    for (const auto& part : rel) {
        const auto s = part.wstring();
        if (s == L"." || s.empty()) {
            continue;
        }
        if (s == L"..") {
            continue;
        }
        normalized /= part;
    }
    return normalized.wstring();
}

}
