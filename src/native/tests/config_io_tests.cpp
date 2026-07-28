#include "path_utils.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

bool Expect(bool condition, const std::string& message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << "\n";
    return false;
}

bool ReadExact(const std::wstring& path, const std::string& expected) {
    std::string actual;
    return nedjin::ReadTextFileExact(path, actual) && actual == expected;
}

}

int wmain() {
    std::error_code error;
    const auto root = fs::temp_directory_path(error) /
        (L"scum-nedjin-config-io-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root, error);
    if (!Expect(!error, "create isolated test directory")) return 1;

    const auto target = (root / L"module.json").wstring();
    const auto backup = target + L".bak.test";
    const auto blockedBackup = target + L".bak.blocked";
    const auto live = target + L".live";

    bool ok = true;
    ok &= Expect(nedjin::Sha256Hex("abc") == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", "SHA-256 known vector");
    ok &= Expect(nedjin::WriteTextFileAtomicallyExact(target, "old-config"), "create exact target");
    ok &= Expect(ReadExact(target, "old-config"), "created target has full content");
    ok &= Expect(nedjin::WriteTextFileAtomicallyExact(live, "stale-live"), "create unrelated live file for exact-read test");
    ok &= Expect(ReadExact(target, "old-config"), "exact read ignores stale .live file");

    ok &= Expect(nedjin::WriteTextFileAtomicallyExact(target, "new-config", backup), "atomically replace target with backup");
    ok &= Expect(ReadExact(target, "new-config"), "replacement has full new content");
    ok &= Expect(ReadExact(backup, "old-config"), "backup has exact prior content");
    ok &= Expect(!nedjin::FileExists(live) || ReadExact(live, "stale-live"), "config writer did not create or alter .live fallback");

    ok &= Expect(nedjin::WriteTextFileAtomicallyExact(blockedBackup, "sentinel"), "create occupied backup destination");
    ok &= Expect(!nedjin::WriteTextFileAtomicallyExact(target, "must-not-replace", blockedBackup), "refuse replacement when backup destination exists");
    ok &= Expect(ReadExact(target, "new-config"), "refused replacement preserves target");

    HANDLE locked = CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ok &= Expect(locked != INVALID_HANDLE_VALUE, "open target without write sharing");
    if (locked != INVALID_HANDLE_VALUE) {
        const auto lockedBackup = target + L".bak.locked";
        ok &= Expect(!nedjin::WriteTextFileAtomicallyExact(target, "locked-write", lockedBackup), "locked target fails closed");
        CloseHandle(locked);
        ok &= Expect(ReadExact(target, "new-config"), "locked write preserves target");
        ok &= Expect(!nedjin::FileExists(lockedBackup), "locked write does not create backup");
    }

    if (!ok) return 1;
    std::cout << "config_io_tests: ok\n";
    std::cout << "artifacts=" << nedjin::WideToUtf8(root.wstring()) << "\n";
    return 0;
}
