#pragma once

#include <string>

namespace warden {

struct BridgeResult {
    bool ok{};
    std::string body;
    std::string message;
};

BridgeResult BridgeExecute(const std::wstring& baseDir, const std::string& command, const std::string& argsJson, int timeoutMs);
std::string BridgeStatusJson(const std::wstring& baseDir);

}
