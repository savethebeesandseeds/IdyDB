param(
    [ValidateSet('start', 'setup', 'build', 'test', 'test-addon', 'check', 'status', 'stop')]
    [string]$Action = 'status'
)
$ErrorActionPreference = 'Stop'
$containerName = 'idydb-dev'
$buildVolume = 'idydb-build'
$baseImage = 'debian:13-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f'
$workspacePath = (Resolve-Path -LiteralPath $PSScriptRoot).Path

function Invoke-Docker {
    & docker @args
    if ($LASTEXITCODE -ne 0) { throw "Docker command failed (exit $LASTEXITCODE)." }
}

function Get-ManagedContainer {
    $rawContainer = & docker container inspect $containerName 2>&1 | Out-String
    $inspectExitCode = $LASTEXITCODE
    if ($inspectExitCode -ne 0) {
        if ($rawContainer -match '(?im)No such (container|object):\s*idydb-dev\s*$') { return $null }
        throw "Cannot inspect $containerName (exit $inspectExitCode). $($rawContainer.Trim())"
    }
    $container = ($rawContainer | ConvertFrom-Json)[0]
    $sourceMount = @($container.Mounts | Where-Object Destination -eq '/workspace')
    $buildMount = @($container.Mounts | Where-Object Destination -eq '/build')
    if ($container.Config.Labels.'com.idydb.managed' -ne 'true' -or
        $container.Config.Labels.'com.idydb.config' -ne 'v1' -or
        $container.Config.Image -ne $baseImage -or
        ($container.Config.Cmd -join ' ') -ne 'sleep infinity' -or
        $container.Config.WorkingDir -ne '/workspace' -or
        $sourceMount.Count -ne 1 -or $sourceMount[0].Type -ne 'bind' -or
        $sourceMount[0].Source.Replace('/', '\').TrimEnd('\') -ine $workspacePath.TrimEnd('\') -or
        $buildMount.Count -ne 1 -or $buildMount[0].Type -ne 'volume' -or
        $buildMount[0].Name -ne $buildVolume -or $container.Mounts.Count -ne 2 -or
        $container.HostConfig.RestartPolicy.Name -ne 'no' -or
        $container.HostConfig.Privileged -or
        ($container.HostConfig.Devices -and $container.HostConfig.Devices.Count -ne 0) -or
        ($container.HostConfig.DeviceRequests -and $container.HostConfig.DeviceRequests.Count -ne 0) -or
        @($container.HostConfig.PortBindings.PSObject.Properties).Count -ne 0) {
        throw "Existing $containerName does not match this project configuration; it has been preserved."
    }
    return $container
}

$container = Get-ManagedContainer
if ($Action -eq 'status') {
    if ($container) { Invoke-Docker container inspect $containerName --format '{{.Id}} {{.State.Status}}' }
    else { Write-Output "$containerName does not exist." }
    exit 0
}
if ($Action -eq 'stop') {
    if ($container) { Invoke-Docker stop $container.Id }
    exit 0
}
if (-not $container) {
    $rawVolume = & docker volume inspect $buildVolume 2>&1 | Out-String
    $volumeInspectExitCode = $LASTEXITCODE
    if ($volumeInspectExitCode -eq 0) {
        $volume = ($rawVolume | ConvertFrom-Json)[0]
        if ($volume.Labels.'com.idydb.managed' -ne 'true') {
            throw "Existing $buildVolume is unmanaged; it has been preserved."
        }
    } elseif ($rawVolume -match '(?im)get idydb-build:\s*no such volume\s*$' -or
              $rawVolume -match '(?im)No such volume:?\s*(idydb-build)?\s*$') {
        Invoke-Docker volume create --label com.idydb.managed=true $buildVolume
    } else {
        throw "Cannot inspect $buildVolume (exit $volumeInspectExitCode). $($rawVolume.Trim())"
    }
    Invoke-Docker create --name $containerName --label com.idydb.managed=true --label com.idydb.config=v1 `
        --mount "type=bind,source=$workspacePath,target=/workspace" `
        --mount "type=volume,source=$buildVolume,target=/build" `
        --workdir /workspace --restart no $baseImage sleep infinity
    $container = Get-ManagedContainer
}
if (-not $container.State.Running) { Invoke-Docker start $container.Id }
switch ($Action) {
    'setup' { Invoke-Docker exec $container.Id sh /workspace/setup.sh }
    'build' {
        Invoke-Docker exec $container.Id cmake -S /workspace/idydb -B /build/cmake -DBUILD_TESTING=ON
        Invoke-Docker exec $container.Id cmake --build /build/cmake --parallel 2
    }
    'test' { Invoke-Docker exec $container.Id ctest --test-dir /build/cmake --output-on-failure }
    'test-addon' { Invoke-Docker exec $container.Id sh /workspace/dev/test-addon.sh }
    'check' { Invoke-Docker exec $container.Id sh /workspace/dev/check.sh }
}
