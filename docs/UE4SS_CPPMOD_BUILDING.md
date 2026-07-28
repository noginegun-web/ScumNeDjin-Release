# Building the UE4SS C++ modules

## Prerequisites

- Visual Studio 2022 Build Tools with the x64 C++ toolchain;
- CMake 3.22 or newer;
- a locally built x64 `UE4SS.lib` that matches the UE4SS runtime installed on
  the target SCUM server;
- a test server and a verified backup for any runtime deployment.

The repository intentionally does not redistribute UE4SS binaries, import
libraries, server executables or game content. Supplying a mismatched import
library may compile a DLL that cannot safely load in the target process.

## Configure and build

From the repository root, point CMake to the local import library explicitly:

```powershell
$ue4ssImport = 'D:\path\to\UE4SS.lib'
$ue4ssImportArg = "-DSCUM_NEDJIN_UE4SS_IMPORT_LIB=$ue4ssImport"

cmake -S src/ue4ss_cppmods/ScumNedjinManagedBridgeMod `
  -B build/ue4ss-managed-bridge -G "Visual Studio 17 2022" -A x64 `
  $ue4ssImportArg
cmake --build build/ue4ss-managed-bridge --config Release

cmake -S src/ue4ss_cppmods/SCUMTraderManager `
  -B build/ue4ss-trader-manager -G "Visual Studio 17 2022" -A x64 `
  $ue4ssImportArg
cmake --build build/ue4ss-trader-manager --config Release
```

The generated module trees are written below the chosen build directories. Do
not copy a build created for one UE4SS version or game update to another
server without rebuilding and validating it.

## Runtime discipline

- Keep gameplay-time probes narrow: do not introduce global UObject scans or
  periodic reflection scans into a live server.
- Drive player-sensitive automation from the normal bridge cache and guarded
  game-thread work; panel transport success alone is not gameplay evidence.
- Treat trader work as incomplete until an actual client can interact with the
  registered economy object and its inventory.
- Before installing a native DLL, preserve a named backup and make the change
  in a controlled restart window.
