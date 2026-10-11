[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipTests
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
Write-Host "Ready: $(Join-Path $buildDirectory 'codons.exe')"
