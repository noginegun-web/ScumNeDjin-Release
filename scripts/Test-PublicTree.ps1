[CmdletBinding()]
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$resolvedRoot = (Resolve-Path -LiteralPath $Root).Path
$blockedExtensions = @('.bak', '.db', '.dll', '.dmp', '.exe', '.exp', '.ilk', '.lib', '.mdmp', '.pdb', '.pem', '.pfx', '.sqlite', '.ucas', '.utoc')
$blockedNames = @('.env', 'scum.db')
$blockedSegments = @('.git', '.vs', 'Saved', 'bin', 'logs', 'obj', 'private', 'state')
$violations = New-Object System.Collections.Generic.List[string]

$requiredSourceFiles = @(
    'src/native/tests/config_io_tests.cpp',
    'src/ue4ss_cppmods/ScumNedjinManagedBridgeMod/CMakeLists.txt',
    'src/ue4ss_cppmods/ScumNedjinManagedBridgeMod/src/dllmain.cpp',
    'src/ue4ss_cppmods/SCUMTraderManager/CMakeLists.txt',
    'src/ue4ss_cppmods/SCUMTraderManager/src/dllmain.cpp',
    'src/ue4ss_cppmods/SCUMTraderManager/src/SCUMTraderManager.cpp',
    'src/ue4ss_cppmods/SCUMTraderManager/include/SCUMTraderManager.hpp',
    'src/ue4ss_cppmods/SCUMTraderManager/include/ue4ss_abi_shim.hpp'
)

foreach ($relative in $requiredSourceFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $resolvedRoot $relative) -PathType Leaf)) {
        $violations.Add("missing required source: $relative")
    }
}

Get-ChildItem -LiteralPath $resolvedRoot -File -Recurse -Force | ForEach-Object {
    # A local Git checkout is metadata, not distributable project content.
    # Still flag any nested .git directory that could be accidentally packaged.
    $relative = $_.FullName.Substring($resolvedRoot.Length).TrimStart('\', '/')
    $segments = $relative -split '[\\/]'
    if ($segments.Count -gt 0 -and $segments[0] -eq '.git') {
        return
    }
    if ($blockedExtensions -contains $_.Extension.ToLowerInvariant()) {
        $violations.Add("blocked extension: $relative")
    }
    if ($blockedNames -contains $_.Name.ToLowerInvariant()) {
        $violations.Add("blocked file name: $relative")
    }
    if ($segments | Where-Object { $blockedSegments -contains $_ }) {
        $violations.Add("blocked path segment: $relative")
    }
}

$bridgeWebJs = Join-Path $resolvedRoot 'ue4ss/ScumNeDjin/web/app.js'
if (-not (Test-Path -LiteralPath $bridgeWebJs -PathType Leaf)) {
    $violations.Add('missing native web panel JavaScript')
}

$managedProjectFiles = @(Get-ChildItem -LiteralPath $resolvedRoot -Filter '*.csproj' -File -Recurse -Force)
if ($managedProjectFiles.Count -gt 0) {
    $managedProjectFiles | ForEach-Object {
        $relative = $_.FullName.Substring($resolvedRoot.Length).TrimStart('\', '/')
        $violations.Add("managed project is outside the public runtime boundary: $relative")
    }
}

$solutionFiles = @(Get-ChildItem -LiteralPath $resolvedRoot -Filter '*.sln' -File -Recurse -Force)
if ($solutionFiles.Count -gt 0) {
    $solutionFiles | ForEach-Object {
        $relative = $_.FullName.Substring($resolvedRoot.Length).TrimStart('\', '/')
        $violations.Add("solution file is outside the public runtime boundary: $relative")
    }
}

$bridgeSafetyPath = Join-Path $resolvedRoot 'ue4ss/ScumNeDjin/configs/bridge-safety.json'
$bridgeMainPath = Join-Path $resolvedRoot 'ue4ss/Mods/ScumNeDjinBridge/scripts/main.lua'
if ((Test-Path -LiteralPath $bridgeSafetyPath -PathType Leaf) -and (Test-Path -LiteralPath $bridgeMainPath -PathType Leaf)) {
    $bridgeSafety = Get-Content -LiteralPath $bridgeSafetyPath -Raw | ConvertFrom-Json
    $expectedFlags = @{
        EnableRuntimeCacheLoop = $false
        EnableJoinWarmupRuntimeScan = $false
        AllowCommandRuntimeScanDuringJoinSettle = $false
        AllowPanelRuntimeScanDuringJoinSettle = $false
        AllowControllerFindAllPlayerScan = $false
        EnablePostJoinPlayerCacheSeed = $true
        PostJoinPlayerCacheSeedMaxAttempts = 1
    }
    foreach ($name in $expectedFlags.Keys) {
        $property = $bridgeSafety.PSObject.Properties[$name]
        if ($null -eq $property -or $property.Value -ne $expectedFlags[$name]) {
            $violations.Add("unexpected bridge safety value: $name")
        }
    }

    $bridgeSource = Get-Content -LiteralPath $bridgeMainPath -Raw
    foreach ($marker in @('refreshPlayerInfoCacheFromGameStatePlayerArrayOnly', 'deferGameStrict', 'POST_JOIN_CACHE_SEED_MAX_ATTEMPTS = 1')) {
        if (-not $bridgeSource.Contains($marker)) {
            $violations.Add("missing bridge safety marker: $marker")
        }
    }
    foreach ($blockedMarker in @('FindAllOf("Item")', 'FindAllOf("Controller")')) {
        if ($bridgeSource.Contains($blockedMarker)) {
            $violations.Add("blocked live object scan marker: $blockedMarker")
        }
    }
}

$nativeHttpPath = Join-Path $resolvedRoot 'src/native/src/http_server.cpp'
if (Test-Path -LiteralPath $nativeHttpPath -PathType Leaf) {
    $nativeHttpSource = Get-Content -LiteralPath $nativeHttpPath -Raw
    foreach ($rawRoute in @('/api/rcon', '/api/web-rcon', '/api/command', '/api/local-rcon', '/api/server-command', '/api/ark-rcon')) {
        if ($nativeHttpSource.Contains('"' + $rawRoute + '"')) {
            $violations.Add("public raw command route is present: $rawRoute")
        }
    }
}

if ($violations.Count -gt 0) {
    $violations | Sort-Object | ForEach-Object { Write-Error $_ }
    throw "Public tree check failed with $($violations.Count) violation(s)."
}

Write-Output "Public tree check passed: $resolvedRoot"
