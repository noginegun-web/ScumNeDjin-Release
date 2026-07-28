# SCUM Trader Manager — UE4SS C++ module

This module is an experimental C++ layer for working with the SCUM trader
lifecycle through the normal economy objects. A visible NPC or a created actor
is not a functional trader until its economy registration and client UI have
both been verified.

## Status and limits

The module builds separately and requires a real server for end-to-end
validation. Do not install economy-object changes without a named backup and a
separate maintenance window.

## Build

You need Visual Studio 2022 with the C++ toolchain, CMake 3.22+ and a local
`UE4SS.lib` matching the active x64 UE4SS runtime. Pass that library explicitly:

```powershell
$ue4ssImport = 'D:\path\to\UE4SS.lib'
$ue4ssImportArg = "-DSCUM_NEDJIN_UE4SS_IMPORT_LIB=$ue4ssImport"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  $ue4ssImportArg
cmake --build build --config Release
```

The output is created in `build/Mods/SCUMTraderManager`. Do not add generated
DLLs, import libraries or game files to Git. The common build instructions and
runtime constraints are in
[`docs/UE4SS_CPPMOD_BUILDING.md`](../../../docs/UE4SS_CPPMOD_BUILDING.md).
