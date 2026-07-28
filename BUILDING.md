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
  -DSCUM_NEDJIN_PUBLIC_REQUIRE_CONFIGURED_API_KEY=ON
cmake --build build/native --config Release
```

Панель:

```powershell
dotnet build ScumWarden.sln -c Release
node --check src/ScumWarden.Server/wwwroot/app.js
node --check ue4ss/ScumNeDjin/web/app.js
```

Перед упаковкой проверьте, что `version.dll` собран из этого дерева и что
конфигурационный шаблон требует уникальный локальный ключ доступа. Каталоги
предметов, иконок и карты в публичной поставке намеренно пустые: добавляйте
только собственные или законно полученные локальные данные согласно
[ASSET_POLICY.md](ASSET_POLICY.md).
