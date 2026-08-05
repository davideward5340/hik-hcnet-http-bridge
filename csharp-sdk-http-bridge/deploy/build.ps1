$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
dotnet build (Join-Path $root 'HikSdkHttpBridge.sln') -c Release -p:Platform=x86
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
$output = Join-Path $root 'src\HikSdkHttpBridge\bin\x86\Release\net48'
Write-Host "Build output: $output"
