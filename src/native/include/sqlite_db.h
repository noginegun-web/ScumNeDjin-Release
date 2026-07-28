#pragma once

#include <string>

namespace warden {

bool SqliteQueryRowsJson(
    const std::wstring& baseDir,
    const std::wstring& dbPath,
    const std::string& sql,
    int maxRows,
    std::string& jsonOut,
    std::string& errorOut);

}
