# Compatibility entry point; the shared release script owns build/test/deploy logic.
param(
    [string]$QtKit = '',
    [string]$QtTools = '',
    [string]$BuildDir = '',
    [string]$DistDir = 'dist',
    [string]$FusionModelDir = '',
    [string]$OnnxRuntimeDir = '',
    [switch]$SkipTests,
    [switch]$NoZip,
    [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
$releaseOptions = @{ Variant = 'Array128'; DistDir = $DistDir; SkipTests = $SkipTests; NoZip = $NoZip; PlanOnly = $PlanOnly }
if ($QtKit) { $releaseOptions.CMakePrefixPath = $QtKit }
if ($BuildDir) { $releaseOptions.BuildDir = $BuildDir }
if ($FusionModelDir) { $releaseOptions.FusionModelDir = $FusionModelDir }
if ($OnnxRuntimeDir) { $releaseOptions.OnnxRuntimeDir = $OnnxRuntimeDir }
if ($QtTools) {
    $releaseOptions.CMakePath = Join-Path $QtTools 'CMake_64/bin/cmake.exe'
    $releaseOptions.CxxCompiler = Join-Path $QtTools 'mingw1310_64/bin/g++.exe'
    $releaseOptions.MakeProgram = Join-Path $QtTools 'Ninja/ninja.exe'
}
& (Join-Path $PSScriptRoot '../scripts/package-release.ps1') @releaseOptions
