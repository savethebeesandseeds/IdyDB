[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
# The regular launcher validates ownership labels, image, mounts and permissions.
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
if ($container.Id -ne ($verifiedStatus -split '\s+')[0]) { throw 'The container changed after validation; it was preserved.' }
if (!$container.State.Running) { throw 'Start the existing idydb-dev container first.' }
$rawManifest = & docker exec $container.Id cat /build/windows-runtime/latest.json
if ($LASTEXITCODE -ne 0) { throw 'Build the runtime payload with dev/build-windows.sh first.' }
$manifest = ($rawManifest | Out-String) | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.run -notmatch '^run-[0-9]{8}T[0-9]{6}Z-[0-9]+$') {
    throw 'The runtime payload manifest is invalid.'
}
$expected = @('big-static', 'tiny-static', 'small-static', 'big-shared')
if (($manifest.configurations -join ',') -ne ($expected -join ',')) { throw 'Unexpected runtime configuration list.' }
$runtimeRoot = Join-Path $repo 'build\windows-runtime'
$hostRunName = "$($manifest.run)-host-$([DateTime]::UtcNow.Ticks)"
$hostRun = Join-Path $runtimeRoot $hostRunName
New-Item -ItemType Directory -Force -Path $hostRun | Out-Null
Invoke-Docker @('cp', "$($container.Id):/build/windows-runtime/$($manifest.run)/.", $hostRun)
$unexpected = Get-ChildItem -LiteralPath $hostRun -Recurse -File | Where-Object {
    $_.Extension -notin @('.exe', '.dll', '.db', '.idydb', '.json', '.txt')
}
if ($unexpected) { throw 'Runtime payload contains an unexpected non-runtime file; contents were preserved.' }
$runtimeExecutables = @('idydb_core_tests.exe', 'idydb_query_tests.exe',
    'idydb_persistence_tests.exe', 'idydb_c_api_test.exe', 'idydb_failure_tests.exe',
    'idydb_directory_tests.exe', 'idydb_platform_tests.exe', 'idydb_compatibility_tests.exe',
    'consumer_c.exe', 'consumer_cpp.exe')
$logDirectory = Join-Path $hostRun 'logs'
New-Item -ItemType Directory -Path $logDirectory | Out-Null
$script:runtimeSequence = 0
function Quote-RuntimeArgument([string]$Value) {
    if ($Value.Contains([char]0)) { throw 'A runtime argument contains NUL.' }
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}
function Invoke-RuntimeTest([string]$Executable, [string]$Label, [string[]]$Arguments = @()) {
    $fullExecutable = [IO.Path]::GetFullPath($Executable)
    $allowedRoot = [IO.Path]::GetFullPath($hostRun).TrimEnd('\') + '\'
    if (!$fullExecutable.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($fullExecutable) -notin $runtimeExecutables) {
        throw 'Only the expected executables in this copied runtime payload may run.'
    }
    $script:runtimeSequence++
    $logName = '{0:D3}-{1}' -f $script:runtimeSequence, ($Label -replace '[^a-zA-Z0-9_.-]', '_')
    $stdout = Join-Path $logDirectory "$logName-stdout.txt"
    $stderr = Join-Path $logDirectory "$logName-stderr.txt"
    $start = @{
        FilePath = $fullExecutable
        WorkingDirectory = (Split-Path -Parent $fullExecutable)
        WindowStyle = 'Hidden'
        PassThru = $true
        RedirectStandardOutput = $stdout
        RedirectStandardError = $stderr
    }
    if ($Arguments.Count) { $start.ArgumentList = ($Arguments | ForEach-Object { Quote-RuntimeArgument $_ }) -join ' ' }
    $ownedProcess = Start-Process @start
    try {
        if (!$ownedProcess.WaitForExit(120000)) {
            # Kill only the Process object created above; never enumerate processes
            # or terminate by executable name, a guessed PID, or a process tree.
            if (!$ownedProcess.HasExited) { $ownedProcess.Kill() }
            if (!$ownedProcess.WaitForExit(5000)) {
                throw "$Label timed out and its owned PID $($ownedProcess.Id) did not exit after termination. Logs: $stdout ; $stderr"
            }
            throw "$Label exceeded its 120-second timeout; its owned process was terminated. Logs: $stdout ; $stderr"
        }
        $ownedProcess.WaitForExit()
        if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout | Write-Output }
        if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr | Write-Output }
        if ($null -eq $ownedProcess.ExitCode -or $ownedProcess.ExitCode -ne 0) {
            throw "$Label failed with exit code $($ownedProcess.ExitCode). Logs: $stdout ; $stderr"
        }
    } finally { $ownedProcess.Dispose() }
}
$priorFixtureDirectory = $env:IDYDB_FIXTURE_DIR
$env:IDYDB_FIXTURE_DIR = Join-Path $hostRun 'fixtures'
$completed = 0
try {
    foreach ($configuration in $expected) {
        $directory = Join-Path $hostRun $configuration
        Push-Location $directory
        try {
            foreach ($file in $runtimeExecutables) {
                Write-Output "Windows $configuration / $file"
                Invoke-RuntimeTest (Join-Path $directory $file) "$configuration-$file"
                $completed++
            }
            Invoke-RuntimeTest (Join-Path $directory 'idydb_compatibility_tests.exe') "$configuration-read-linux" @('--read', (Join-Path $hostRun 'linux-fixtures'))
        } finally { Pop-Location }
    }
    # Generate one Windows-written pair, then let the native Linux build read it.
    $written = Join-Path $hostRun 'windows-fixtures'
    Invoke-RuntimeTest (Join-Path $hostRun 'big-static\idydb_compatibility_tests.exe') 'write-windows-fixtures' @('--write', $written)
    Invoke-Docker @('exec', $container.Id, 'mkdir', '-p', '/build/windows-interop-received')
    $received = "/build/windows-interop-received/$hostRunName"
    Invoke-Docker @('cp', $written, "$($container.Id):$received")
    Invoke-Docker @('exec', $container.Id, '/build/windows-interop-linux/idydb_compatibility_tests', '--read', $received)
    Write-Output "Windows runtime checks passed: $completed tests; four Linux-to-Windows reads; one Windows-to-Linux read."
    Write-Output "Runtime artifacts and fixtures: $hostRun"
} finally { $env:IDYDB_FIXTURE_DIR = $priorFixtureDirectory }
