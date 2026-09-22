# Run elevated on a machine without existing hikbridge/HikSdkHttpBridge registrations.
# Creates only temporary test registrations and removes them in finally.
param([string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build-win32'))
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$manager = (Resolve-Path (Join-Path $PSScriptRoot '..\deploy\windows\windows-service.cmd')).Path
$names = @('hikbridge', 'HikSdkHttpBridge')
foreach ($name in $names) {
    if (Get-CimInstance Win32_Service -Filter "Name='$name'") { throw "Existing service $name; refusing to disturb it." }
}
if (@(Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -in $names }).Count) {
    throw 'Existing bridge tasks; refusing to disturb them.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (!(New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this integration test as administrator.'
}
function Require($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Manage([string]$Action, [string]$Directory, [bool]$ExpectSuccess = $true) {
    $batch = Join-Path $Directory 'windows-service.cmd'
    & cmd.exe /d /c ('""{0}" {1} /nopause"' -f $batch, $Action.ToLowerInvariant())
    if ($ExpectSuccess) { Require ($LASTEXITCODE -eq 0) "Manager failed: $Action" }
    else { Require ($LASTEXITCODE -ne 0) 'Expected installation failure' }
}
function Wait-Healthy([int]$Port, [int]$DifferentPid = 0) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $item = Get-CimInstance Win32_Service -Filter "Name='hikbridge'"
        if ($item -and $item.State -eq 'Running' -and $item.ProcessId -ne $DifferentPid) {
            try {
                $response = Invoke-RestMethod "http://127.0.0.1:$Port/healthz" -TimeoutSec 1 -Proxy $null
                if ($response.status -eq 'ok') { return $item }
            } catch {}
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Service did not become healthy'
}

# Include spaces and Chinese characters, without relying on script-file encoding.
$directoryName = 'service-test ' + [char]0x4e2d + [char]0x6587
$package = Join-Path $build $directoryName
$badPackage = Join-Path $build 'service-test-failure'
$listener = $null
$client = $null
try {
    foreach ($directory in @($package, $badPackage)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $build 'hik-sdk-http-bridge.exe') -Destination $directory
        Copy-Item -LiteralPath $manager -Destination $directory
    }
    $portProbe = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
    $portProbe.Start()
    $port = $portProbe.LocalEndpoint.Port
    $portProbe.Stop()
    $config = Get-Content -LiteralPath (Join-Path $build 'config.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $config.server.port = $port
    $config.sdk.directory = Join-Path $build 'hcnetsdk'
    $config.ffmpeg.path = Join-Path $build 'ffmpeg\ffmpeg.exe'
    $config.ffmpeg.hardwareAcceleration = 'off'
    $config | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $package 'config.json') -Encoding UTF8

    # Seed an old SYSTEM startup task, then verify successful migration removes it.
    $action = New-ScheduledTaskAction -Execute (Join-Path $package 'hik-sdk-http-bridge.exe') -Argument ('run --config "{0}"' -f (Join-Path $package 'config.json'))
    Register-ScheduledTask -TaskName 'HikSdkHttpBridge' -Action $action -Trigger (New-ScheduledTaskTrigger -AtStartup) -User SYSTEM -RunLevel Highest | Out-Null
    Start-ScheduledTask -TaskName 'HikSdkHttpBridge'
    Start-Sleep -Seconds 2
    Manage Install $package
    $first = Wait-Healthy $port
    Require (@(Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -eq 'HikSdkHttpBridge' }).Count -eq 0) 'Legacy task survived migration'
    Require ($first.PathName.Contains($package)) 'Unicode image path was not preserved'
    Require ($first.StartMode -eq 'Auto') 'Service is not automatic'
    Require ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\hikbridge').DelayedAutoStart -ne 1) 'Service must use ordinary automatic startup'
    Write-Host 'PASS migration, Unicode path, configured port, automatic startup'

    Manage Install $package
    $second = Wait-Healthy $port $first.ProcessId
    Write-Host 'PASS repeat installation'

    # A valid package with an occupied port passes validation but fails startup.
    $listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $config.server.port = $listener.LocalEndpoint.Port
    $config | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $badPackage 'config.json') -Encoding UTF8
    Manage Install $badPackage $false
    $restored = Wait-Healthy $port
    Require ($restored.PathName -eq $second.PathName) 'Original service path was not restored'
    $listener.Stop()
    $listener = $null
    Write-Host 'PASS failed installation rollback'

    $client = New-Object Net.Sockets.TcpClient('127.0.0.1', $port)
    $partial = [Text.Encoding]::ASCII.GetBytes("GET /healthz HTTP/1.1`r`n")
    $client.GetStream().Write($partial, 0, $partial.Length)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $control = New-Object ServiceProcess.ServiceController('hikbridge')
    try { $control.Stop(); $control.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(10)) } finally { $control.Dispose() }
    Require ($timer.Elapsed.TotalSeconds -lt 10) 'Stopping with partial HTTP request took too long'
    Start-Sleep -Seconds 6
    Require ((Get-CimInstance Win32_Service -Filter "Name='hikbridge'").State -eq 'Stopped') 'Normal stop triggered failure recovery'
    $client.Dispose()
    $client = $null
    Start-Service hikbridge
    $running = Wait-Healthy $port
    Write-Host 'PASS stop with partial request and manual restart'

    Stop-Process -Id $running.ProcessId -Force
    $recovered = Wait-Healthy $port $running.ProcessId
    Require ($recovered.ProcessId -ne $running.ProcessId) 'Failure recovery did not replace the process'
    Write-Host 'PASS crash recovery'

    Manage Uninstall $package
    Require (!(Get-CimInstance Win32_Service -Filter "Name='hikbridge'")) 'Service remained after uninstall'
    Require (Test-Path -LiteralPath (Join-Path $package 'config.json')) 'Uninstall removed configuration'
    Write-Host 'PASS uninstall preserves configuration'
} finally {
    if ($client) { $client.Dispose() }
    if ($listener) { $listener.Stop() }
    Manage Uninstall $package
}
