param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string]$SdkPackage,
    [Parameter(Mandatory = $true)]
    [string]$Runtime,
    [switch]$BuildTests,
    [string]$BuildDirectory = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$vs = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (!(Test-Path -LiteralPath $vs) -or !(Test-Path -LiteralPath $cmake) -or !(Test-Path -LiteralPath $ninja)) {
    throw 'Visual Studio 2022 C++ Build Tools with CMake and Ninja is required'
}

$build = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $root 'build-win32' }
$sdkPackagePath = (Resolve-Path -LiteralPath $SdkPackage).Path
$runtimePath = (Resolve-Path -LiteralPath $Runtime).Path
$tests = if ($BuildTests) { 'ON' } else { 'OFF' }
$line = '"{0}" -arch=x86 -host_arch=x64 && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE={5} -DHIK_WIN32_SDK_PACKAGE="{6}" -DHIK_WIN32_RUNTIME="{7}" -DHIK_BUILD_TESTS={8} && "{1}" --build "{3}" --parallel' -f $vs, $cmake, $root, $build, $ninja, $Configuration, $sdkPackagePath, $runtimePath, $tests
& cmd.exe /d /c $line
if ($LASTEXITCODE -ne 0) { throw 'Windows x86 build failed' }

$binary = Join-Path $build 'hik-sdk-http-bridge.exe'
& $binary --validate-config --config (Join-Path $build 'config.json')
if ($LASTEXITCODE -ne 0) { throw 'Windows configuration validation failed' }
if ($BuildTests) {
    $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
    & $ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Windows lifecycle tests failed' }
}
Write-Host "Windows x86 build completed: $binary"
