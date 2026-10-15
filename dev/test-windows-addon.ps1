[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$node = (Get-Command node -CommandType Application -ErrorAction Stop).Source
$version = & $node -p 'process.versions.node + " " + process.arch'
if ($LASTEXITCODE -ne 0 -or $version -ne '24.19.0 x64') {
    throw 'Use the already installed Node 24.19.0 x64 runtime; this script never installs a host runtime or toolchain.'
}
$verifiedStatus = (& (Join-Path $repo 'dev.ps1') status | Out-String).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Managed container validation failed.' }
Write-Output $verifiedStatus
function Invoke-Docker([string[]]$Arguments) {
    & docker @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Docker failed with exit code $LASTEXITCODE." }
}
$raw = & docker container inspect idydb-dev
if ($LASTEXITCODE -ne 0) { throw 'The existing idydb-dev container is required.' }
$container = ($raw | ConvertFrom-Json)[0]
if ($container.Id -ne ($verifiedStatus -split '\s+')[0] -or !$container.State.Running) {
    throw 'The verified idydb-dev container must still be running.'
}
$rawManifest = & docker exec $container.Id cat /build/windows-addon-runtime/latest.json
if ($LASTEXITCODE -ne 0) { throw 'Cross build the Node addon with dev/build-windows-addon.sh first.' }
$manifest = ($rawManifest | Out-String) | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.node -ne '24.19.0' -or $manifest.architecture -ne 'x64' -or
    $manifest.napi -ne 8 -or $manifest.run -notmatch '^run-[0-9]{8}T[0-9]{6}Z-[0-9]+$') {
    throw 'The Windows addon runtime manifest is invalid.'
}
$hostRun = Join-Path $repo "build\windows-runtime\addon-$($manifest.run)-host-$([DateTime]::UtcNow.Ticks)"
New-Item -ItemType Directory -Force -Path $hostRun | Out-Null
Invoke-Docker @('cp', "$($container.Id):/build/windows-addon-runtime/$($manifest.run)/.", $hostRun)
$unexpected = Get-ChildItem -LiteralPath $hostRun -Recurse -File | Where-Object {
    $_.Extension -notin @('.node', '.dll', '.js', '.json', '.txt')
}
if ($unexpected) { throw 'The addon payload contains a non-runtime file; contents were preserved.' }
$priorPath = $env:PATH
try {
    foreach ($configuration in @('production', 'instrumented')) {
        $directory = Join-Path $hostRun $configuration
        $env:PATH = "$(Join-Path $directory 'build\Release');$priorPath"
        Push-Location $directory
        try {
            if ($configuration -eq 'production') {
                & $node -e 'const a = require("./build/Release/idydb.node"); if (a.__testFailNextSync !== undefined || a.__testSyncFailures !== undefined) throw Error("test hooks present in production addon");'
                if ($LASTEXITCODE -ne 0) { throw 'Production addon hook isolation failed.' }
                & $node --expose-gc (Join-Path $directory 'test.js')
            } else {
                & $node --expose-gc (Join-Path $directory 'test-failure.js')
            }
            if ($LASTEXITCODE -ne 0) { throw "Windows $configuration addon tests failed with exit code $LASTEXITCODE." }
        } finally { Pop-Location }
    }
    Write-Output 'Windows Node-API regression and injected persistence/GC failure tests passed on Node 24.19.0 x64.'
    Write-Output "Runtime artifacts: $hostRun"
} finally { $env:PATH = $priorPath }
