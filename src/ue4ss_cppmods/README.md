# UE4SS C++ modules

This directory contains the complete source for the optional UE4SS C++ modules
shipped by this project:

- `ScumNedjinManagedBridgeMod` — managed bridge entry point;
- `SCUMTraderManager` — experimental trader-lifecycle module.

Each module has an independent CMake project. Build it with an x64 UE4SS import
library that matches the exact UE4SS runtime used on the target server. The
repository intentionally carries source only: it does not contain UE4SS
binaries, import libraries, game files or build output.

See [../../docs/UE4SS_CPPMOD_BUILDING.md](../../docs/UE4SS_CPPMOD_BUILDING.md)
for the reproducible commands and runtime safety rules.
