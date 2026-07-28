#pragma once

#include <string>

namespace warden {

std::wstring ModuleDirectory();
std::wstring JoinPath(const std::wstring& left, const std::wstring& right);
bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
void EnsureDirectory(const std::wstring& path);
std::string ReadTextFile(const std::wstring& path);
// Config/ledger files must never follow the legacy .live fallback used by generic state files.
bool ReadTextFileExact(const std::wstring& path, std::string& text);
bool WriteTextFile(const std::wstring& path, const std::string& text);
// Same-directory temp write plus atomic replacement/creation of the exact target only.
// It never deletes the target and never writes a .live fallback.
bool WriteTextFileAtomicallyExact(const std::wstring& path, const std::string& text, const std::wstring& backupPath = {});
std::string Sha256Hex(const std::string& text);
bool DeleteFileQuiet(const std::wstring& path);
std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
std::string JsonEscape(const std::string& value);
std::string UtcIsoNow();
std::string UrlDecode(const std::string& value);
std::wstring NormalizeRelativePath(const std::string& path);

}
