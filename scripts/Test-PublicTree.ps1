[CmdletBinding()]
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$resolvedRoot = (Resolve-Path -LiteralPath $Root).Path
$blockedExtensions = @('.bak', '.db', '.dmp', '.exe', '.mdmp', '.pdb', '.pem', '.pfx', '.sqlite', '.ucas', '.utoc')
$blockedNames = @('.env', 'scum.db')
$blockedSegments = @('.git', '.vs', 'Saved', 'bin', 'logs', 'obj', 'private', 'state')
$violations = New-Object System.Collections.Generic.List[string]

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

if ($violations.Count -gt 0) {
    $violations | Sort-Object | ForEach-Object { Write-Error $_ }
    throw "Public tree check failed with $($violations.Count) violation(s)."
}

Write-Output "Public tree check passed: $resolvedRoot"
