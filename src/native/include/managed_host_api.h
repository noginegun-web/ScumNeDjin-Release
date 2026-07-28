#pragma once

extern "C" {

__declspec(dllexport) int __stdcall ScumNedjin_GetHostInfoJson(char* buffer, int capacity);
__declspec(dllexport) int __stdcall ScumNedjin_GetRuntimeSnapshotJson(char* buffer, int capacity);
__declspec(dllexport) int __stdcall ScumNedjin_GetReadOnlyRuntimeProbeJson(char* buffer, int capacity);
__declspec(dllexport) int __stdcall ScumNedjin_GetUe4ssSurfaceJson(char* buffer, int capacity);
__declspec(dllexport) int __stdcall ScumNedjin_ReadStateFileJson(const char* fileNameUtf8, char* buffer, int capacity);
__declspec(dllexport) int __stdcall ScumNedjin_WriteManagedStatusJson(const char* moduleNameUtf8, const char* jsonUtf8, char* buffer, int capacity);
__declspec(dllexport) int __stdcall ScumNedjin_AppendManagedEventJson(const char* moduleNameUtf8, const char* jsonUtf8, char* buffer, int capacity);
__declspec(dllexport) int __stdcall ScumNedjin_EnqueueManagedCommandJson(const char* moduleNameUtf8, const char* commandTypeUtf8, const char* idempotencyKeyUtf8, const char* jsonUtf8, char* buffer, int capacity);

}
