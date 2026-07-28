# Сборка из исходников

Требования:

- .NET SDK 8;
- CMake 3.24+;
- MSVC Build Tools с x64 C++ toolchain;
- Node.js для синтаксической проверки панели.

Все временные каталоги можно направить на рабочий диск через `TEMP`, `TMP`,
`DOTNET_CLI_HOME` и `NUGET_PACKAGES`. Не добавляйте в Git рабочие конфиги,
журналы, базы данных или материалы, права на распространение которых не
подтверждены.

Нативная часть:

```powershell
cmake -S src/native -B build/native -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  -DSCUM_NEDJIN_PUBLIC_REQUIRE_CONFIGURED_API_KEY=ON `
  -DSCUM_NEDJIN_BUILD_TESTS=ON
cmake --build build/native --config Release
ctest --test-dir build/native -C Release --output-on-failure
```

Панель:

```powershell
dotnet build ScumWarden.sln -c Release
node --check src/ScumWarden.Server/wwwroot/app.js
node --check ue4ss/ScumNeDjin/web/app.js
```

## C++‑модули UE4SS

Исходники модулей лежат в `src/ue4ss_cppmods`. Для сборки нужен локально
собранный `UE4SS.lib`, совпадающий по версии и архитектуре с UE4SS на целевом
сервере. Библиотеки UE4SS и игровые файлы в этот репозиторий не входят.

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

Подробности, ограничения и структура артефактов описаны в
[docs/UE4SS_CPPMOD_BUILDING.md](docs/UE4SS_CPPMOD_BUILDING.md).

Перед упаковкой проверьте, что `version.dll` собран из этого дерева и что
конфигурационный шаблон требует уникальный локальный ключ доступа. Каталоги
предметов, иконок и карты в публичной поставке намеренно пустые: добавляйте
только собственные или законно полученные локальные данные согласно
[ASSET_POLICY.md](ASSET_POLICY.md).
