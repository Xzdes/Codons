[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipTests,
    [switch]$Package
)

$ErrorActionPreference = 'Stop'
foreach ($dependency in @('cmake', 'gcc', 'mingw32-make')) {
    if (-not (Get-Command $dependency -ErrorAction SilentlyContinue)) {
        throw "Required tool is not on PATH: $dependency"
    }
}
$buildDirectory = Join-Path $PSScriptRoot 'build'
& cmake -S $PSScriptRoot -B $buildDirectory -G 'MinGW Makefiles' -DCMAKE_C_COMPILER=gcc "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
& cmake --build $buildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
if (-not $SkipTests) {
    & ctest --test-dir $buildDirectory --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}
if ($Package) {
    # Chocolatey may also install a cpack command. Use CMake's companion executable.
    $cpackExecutable = Join-Path (Split-Path -Parent (Get-Command cmake).Source) 'cpack.exe'
    if (-not (Test-Path -LiteralPath $cpackExecutable)) { throw 'CMake packaging tool was not found' }
    & $cpackExecutable --config (Join-Path $buildDirectory 'CPackConfig.cmake') -B $buildDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed' }
}
Write-Host "Ready: $(Join-Path $buildDirectory 'codons.exe')"
