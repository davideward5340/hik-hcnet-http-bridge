param(
    [string]$ServiceName = 'hikbridge'
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$config = Join-Path $root 'src\HikSdkHttpBridge\bin\x86\Release\net48\config.json'
& sc.exe stop $ServiceName
Start-Sleep -Seconds 2
& sc.exe delete $ServiceName
if (Test-Path $config) {
    $settings = Get-Content -Raw -Encoding UTF8 $config | ConvertFrom-Json
    $prefix = "http://$($settings.server.bind):$($settings.server.port)/"
    & netsh http delete urlacl url=$prefix
}
