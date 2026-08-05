param(
    [string]$ServiceName = 'HikSdkHttpBridge'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$output = Join-Path $root 'src\HikSdkHttpBridge\bin\x86\Release\net48'
$exe = Join-Path $output 'hik-sdk-http-bridge.exe'
$config = Join-Path $output 'config.json'
if (-not (Test-Path $exe)) { throw 'Run deploy\build.ps1 first.' }
$settings = Get-Content -Raw -Encoding UTF8 $config | ConvertFrom-Json
$prefix = "http://$($settings.server.bind):$($settings.server.port)/"
$urlAccount = 'NT AUTHORITY\LOCAL SERVICE'
& netsh http add urlacl "url=$prefix" "user=$urlAccount"
$binary = '"{0}" service-run --config "{1}"' -f $exe, $config
$serviceAccount = 'NT AUTHORITY\LocalService'
& sc.exe create $ServiceName 'binPath=' $binary 'start=' 'auto' 'obj=' $serviceAccount
& sc.exe description $ServiceName 'HCNetSDK to fragmented MP4 local HTTP bridge'
& sc.exe start $ServiceName
