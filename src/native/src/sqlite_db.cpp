#include "sqlite_db.h"
#include "path_utils.h"

#include <windows.h>

#include <filesystem>
#include <cctype>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace nedjin {
namespace {

struct sqlite3;
struct sqlite3_stmt;

constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;
constexpr int SQLITE_INTEGER = 1;
constexpr int SQLITE_FLOAT = 2;
constexpr int SQLITE_TEXT = 3;
constexpr int SQLITE_BLOB = 4;
constexpr int SQLITE_NULL = 5;
constexpr int SQLITE_OPEN_READONLY = 0x00000001;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
constexpr int SQLITE_OPEN_URI = 0x00000040;

using sqlite3_open_v2_fn = int(__cdecl*)(const char*, sqlite3**, int, const char*);
using sqlite3_close_fn = int(__cdecl*)(sqlite3*);
using sqlite3_prepare_v2_fn = int(__cdecl*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
using sqlite3_step_fn = int(__cdecl*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int(__cdecl*)(sqlite3_stmt*);
using sqlite3_column_count_fn = int(__cdecl*)(sqlite3_stmt*);
using sqlite3_column_name_fn = const char*(__cdecl*)(sqlite3_stmt*, int);
using sqlite3_column_type_fn = int(__cdecl*)(sqlite3_stmt*, int);
using sqlite3_column_int64_fn = long long(__cdecl*)(sqlite3_stmt*, int);
using sqlite3_column_double_fn = double(__cdecl*)(sqlite3_stmt*, int);
using sqlite3_column_text_fn = const unsigned char*(__cdecl*)(sqlite3_stmt*, int);
using sqlite3_column_bytes_fn = int(__cdecl*)(sqlite3_stmt*, int);
using sqlite3_errmsg_fn = const char*(__cdecl*)(sqlite3*);
using sqlite3_busy_timeout_fn = int(__cdecl*)(sqlite3*, int);

struct SqliteApi {
    HMODULE dll{};
    sqlite3_open_v2_fn open_v2{};
    sqlite3_close_fn close{};
    sqlite3_prepare_v2_fn prepare_v2{};
    sqlite3_step_fn step{};
    sqlite3_finalize_fn finalize{};
    sqlite3_column_count_fn column_count{};
    sqlite3_column_name_fn column_name{};
    sqlite3_column_type_fn column_type{};
    sqlite3_column_int64_fn column_int64{};
    sqlite3_column_double_fn column_double{};
    sqlite3_column_text_fn column_text{};
    sqlite3_column_bytes_fn column_bytes{};
    sqlite3_errmsg_fn errmsg{};
    sqlite3_busy_timeout_fn busy_timeout{};
    std::string error;
    bool attempted = false;
};

std::mutex g_sqliteMutex;
SqliteApi g_sqlite;

template <typename T>
bool LoadProc(HMODULE dll, const char* name, T& target, std::string& error) {
    target = reinterpret_cast<T>(GetProcAddress(dll, name));
    if (!target) {
        error = std::string("sqlite export missing: ") + name;
        return false;
    }
    return true;
}

bool LoadSqliteApiLocked(const std::wstring& baseDir, std::string& errorOut) {
    if (g_sqlite.attempted) {
        errorOut = g_sqlite.error;
        return g_sqlite.dll != nullptr;
    }

    g_sqlite.attempted = true;
    const std::wstring candidates[] = {
        JoinPath(JoinPath(baseDir, L"ScumNeDjin"), L"bin\\e_sqlite3.dll"),
        JoinPath(baseDir, L"e_sqlite3.dll"),
        L"e_sqlite3.dll"
    };

    for (const auto& path : candidates) {
        g_sqlite.dll = LoadLibraryW(path.c_str());
        if (g_sqlite.dll) break;
    }

    if (!g_sqlite.dll) {
        g_sqlite.error = "e_sqlite3.dll not found.";
        errorOut = g_sqlite.error;
        return false;
    }

    std::string error;
    const bool ok =
        LoadProc(g_sqlite.dll, "sqlite3_open_v2", g_sqlite.open_v2, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_close", g_sqlite.close, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_prepare_v2", g_sqlite.prepare_v2, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_step", g_sqlite.step, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_finalize", g_sqlite.finalize, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_column_count", g_sqlite.column_count, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_column_name", g_sqlite.column_name, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_column_type", g_sqlite.column_type, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_column_int64", g_sqlite.column_int64, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_column_double", g_sqlite.column_double, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_column_text", g_sqlite.column_text, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_column_bytes", g_sqlite.column_bytes, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_errmsg", g_sqlite.errmsg, error) &&
        LoadProc(g_sqlite.dll, "sqlite3_busy_timeout", g_sqlite.busy_timeout, error);

    if (!ok) {
        g_sqlite.error = error;
        errorOut = g_sqlite.error;
        return false;
    }

    return true;
}

std::string DbError(sqlite3* db, const std::string& fallback) {
    if (!db || !g_sqlite.errmsg) return fallback;
    const char* text = g_sqlite.errmsg(db);
    return text && *text ? std::string(text) : fallback;
}

std::string JsonNumber(double value) {
    std::ostringstream ss;
    ss << value;
    return ss.str();
}

} // namespace

bool SqliteQueryRowsJson(
    const std::wstring& baseDir,
    const std::wstring& dbPath,
    const std::string& sql,
    int maxRows,
    std::string& jsonOut,
    std::string& errorOut) {
    jsonOut = "[]";
    errorOut.clear();

    std::lock_guard<std::mutex> lock(g_sqliteMutex);
    if (!LoadSqliteApiLocked(baseDir, errorOut)) return false;
    if (!FileExists(dbPath)) {
        errorOut = "SCUM.db not found.";
        return false;
    }

    sqlite3* db{};
    const auto pathUtf8 = WideToUtf8(dbPath);
    int rc = g_sqlite.open_v2(pathUtf8.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    if (rc != SQLITE_OK || !db) {
        errorOut = DbError(db, "sqlite open failed.");
        if (db) g_sqlite.close(db);
        return false;
    }

    g_sqlite.busy_timeout(db, 1500);

    sqlite3_stmt* stmt{};
    rc = g_sqlite.prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        errorOut = DbError(db, "sqlite prepare failed.");
        g_sqlite.close(db);
        return false;
    }

    const int columnCount = g_sqlite.column_count(stmt);
    std::ostringstream out;
    out << "[";
    int rows = 0;
    bool firstRow = true;
    while ((rc = g_sqlite.step(stmt)) == SQLITE_ROW) {
        if (maxRows > 0 && rows >= maxRows) break;
        if (!firstRow) out << ",";
        firstRow = false;
        out << "{";
        for (int i = 0; i < columnCount; ++i) {
            if (i) out << ",";
            const char* name = g_sqlite.column_name(stmt, i);
            out << "\"" << JsonEscape(name ? name : ("c" + std::to_string(i))) << "\":";
            switch (g_sqlite.column_type(stmt, i)) {
            case SQLITE_INTEGER:
                out << g_sqlite.column_int64(stmt, i);
                break;
            case SQLITE_FLOAT:
                out << JsonNumber(g_sqlite.column_double(stmt, i));
                break;
            case SQLITE_TEXT: {
                const auto* text = g_sqlite.column_text(stmt, i);
                out << "\"" << JsonEscape(text ? reinterpret_cast<const char*>(text) : "") << "\"";
                break;
            }
            case SQLITE_BLOB:
                out << "\"<blob:" << g_sqlite.column_bytes(stmt, i) << ">\"";
                break;
            case SQLITE_NULL:
            default:
                out << "null";
                break;
            }
        }
        out << "}";
        ++rows;
    }

    if (rc != SQLITE_DONE && !(maxRows > 0 && rows >= maxRows)) {
        errorOut = DbError(db, "sqlite step failed.");
        g_sqlite.finalize(stmt);
        g_sqlite.close(db);
        return false;
    }

    out << "]";
    g_sqlite.finalize(stmt);
    g_sqlite.close(db);
    jsonOut = out.str();
    return true;
}

} // namespace nedjin
