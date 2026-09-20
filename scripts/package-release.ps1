param(
    [ValidateSet('Auto', 'Array128', 'Legacy48')]
    [string]$Variant = 'Auto',
    [string]$BuildDir = '',
    [string]$DistDir = 'dist',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$Generator = 'Ninja',
    [Alias('QtKit')][string]$CMakePrefixPath = '',
    [string]$CMakePath = '',
    [string]$CTestPath = '',
    [string]$CxxCompiler = '',
    [string]$MakeProgram = '',
    [ValidateRange(1, 128)][int]$Parallel = 6,
    [switch]$SkipTests,
    [switch]$NoZip,
    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Resolve-RepoPath {
    param([string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Read-CMakeCacheValue {
    param([string]$CachePath, [string]$Name)
    if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) { return '' }
    $line = Get-Content -LiteralPath $CachePath | Where-Object { $_ -match "^$([regex]::Escape($Name)):" } | Select-Object -First 1
    if (-not $line) { return '' }
    return ($line -replace '^[^=]+=', '').Trim()
}

function Find-CacheValue {
    param([string]$Name)
    foreach ($cache in $toolCaches) {
        $value = Read-CMakeCacheValue $cache $Name
        if ($value) { return $value }
    }
    return ''
}

function Resolve-Tool {
    param([string]$ExplicitPath, [string]$Name)
    if ($ExplicitPath) {
        $resolved = Resolve-RepoPath $ExplicitPath
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) { throw "Tool does not exist: $resolved" }
        return $resolved
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $command) { throw "Required command '$Name' was not found. Specify its tool path." }
    return $command.Source
}

function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Command failed (exit $LASTEXITCODE): $Command $($Arguments -join ' ')" }
}

# Follow the checkout's CMake default, not a hard-coded Git branch name.
$rootCmake = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($Variant -eq 'Auto') {
    if ($rootCmake -match '(?s)option\(\s*MICARRAY_BUILD_LEGACY_48\b[^)]*\bOFF\s*\)') { $Variant = 'Array128' }
    else { $Variant = 'Legacy48' }
}
$is128 = $Variant -eq 'Array128'
$appName = if ($is128) { 'MicArray128' } else { 'MicArray' }
$sourcePath = if ($is128) { Join-Path $repoRoot 'array128' } else { $repoRoot }
if (-not (Test-Path -LiteralPath (Join-Path $sourcePath 'CMakeLists.txt'))) { throw "Variant source is missing: $sourcePath" }
if (-not $BuildDir) {
    $BuildDir = if ($is128) { 'build-array128' } else { 'build\Desktop_Qt_6_11_1_MinGW_64_bit_Release' }
}
$buildPath = Resolve-RepoPath $BuildDir
$distPath = Resolve-RepoPath $DistDir
$cachePath = Join-Path $buildPath 'CMakeCache.txt'
$cacheSource = Read-CMakeCacheValue $cachePath 'CMAKE_HOME_DIRECTORY'
if ($cacheSource) {
    $cacheSource = [IO.Path]::GetFullPath($cacheSource)
    $cacheLegacy = Read-CMakeCacheValue $cachePath 'MICARRAY_BUILD_LEGACY_48'
    if ($is128 -and $cacheSource -eq $repoRoot -and $cacheLegacy -eq 'OFF') {
        $sourcePath = $repoRoot # Also accept a root-configured M128 build tree.
    }
    if ($cacheSource -ne $sourcePath -or
        ($is128 -and $cacheLegacy -eq 'ON') -or
        (-not $is128 -and $cacheLegacy -eq 'OFF')) {
        throw 'BuildDir belongs to another source/variant. Choose a separate BuildDir; existing files were not removed.'
    }
    $cachedGenerator = Read-CMakeCacheValue $cachePath 'CMAKE_GENERATOR'
    if (-not $PSBoundParameters.ContainsKey('Generator') -and $cachedGenerator) { $Generator = $cachedGenerator }
}
$testTargets = if ($is128) { @('test_array128') } else {
    @('test_protocol', 'test_device', 'test_media', 'test_app', 'test_ui', 'test_analysis')
}
$variantOptions = @()
if ($sourcePath -eq $repoRoot -and $rootCmake -match 'option\(\s*MICARRAY_BUILD_LEGACY_48') {
    $legacyValue = if ($is128) { 'OFF' } else { 'ON' }
    $variantOptions += "-DMICARRAY_BUILD_LEGACY_48=$legacyValue"
}
$executableSubdir = if ($is128 -and $sourcePath -eq $repoRoot) { 'array128' } else { '' }
$plan = [ordered]@{
    Variant = $Variant; Target = $appName; SourceDir = $sourcePath; BuildDir = $buildPath
    DistDir = $distPath; Configuration = $Configuration; Generator = $Generator
    ConfigureOptions = @($variantOptions); TestTargets = @($testTargets)
    ExecutableSubdir = $executableSubdir; SkipTests = [bool]$SkipTests; Zip = -not [bool]$NoZip
}
if ($PlanOnly) { $plan | ConvertTo-Json -Depth 4; return }

$toolCaches = @($cachePath, (Join-Path $repoRoot 'build-array128/CMakeCache.txt'),
    (Join-Path $repoRoot 'build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug/CMakeCache.txt'))
if (-not $CMakePrefixPath) { $CMakePrefixPath = Find-CacheValue 'CMAKE_PREFIX_PATH' }
if (-not $CMakePrefixPath) {
    $qtCmake = Find-CacheValue 'Qt6_DIR'
    if ($qtCmake) { $CMakePrefixPath = [IO.Path]::GetFullPath((Join-Path $qtCmake '../../..')) }
}
if (-not $CMakePrefixPath) { throw 'Qt kit not found. Pass -CMakePrefixPath (and -CxxCompiler for a MinGW kit).' }
# CMAKE_PREFIX_PATH may contain several prefixes. Locate the Qt kit among them.
$qtBin = ''
foreach ($prefix in ($CMakePrefixPath -split ';')) {
    $candidate = Join-Path (Resolve-RepoPath $prefix) 'bin'
    if (Test-Path -LiteralPath (Join-Path $candidate 'windeployqt.exe')) { $qtBin = $candidate; break }
}
if (-not $qtBin) { throw 'No windeployqt.exe found under CMakePrefixPath. Point it at a Qt Windows kit.' }
$qtTools = Join-Path (Split-Path -Parent (Split-Path -Parent $qtBin)) '../Tools'
if (-not $CMakePath) { $CMakePath = Find-CacheValue 'CMAKE_COMMAND' }
if (-not $CMakePath -and (Test-Path -LiteralPath (Join-Path $qtTools 'CMake_64/bin/cmake.exe'))) {
    $CMakePath = Join-Path $qtTools 'CMake_64/bin/cmake.exe'
}
$cmake = Resolve-Tool $CMakePath 'cmake'
$ctestCandidate = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$ctest = if ($CTestPath) { Resolve-Tool $CTestPath 'ctest' }
    elseif (Test-Path -LiteralPath $ctestCandidate) { $ctestCandidate } else { '' }
if (-not $CxxCompiler) { $CxxCompiler = Find-CacheValue 'CMAKE_CXX_COMPILER' }
if (-not $CxxCompiler -and $qtBin -match 'mingw') {
    $mingwCandidates = @(Get-ChildItem -LiteralPath $qtTools -Directory -Filter 'mingw*' -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'bin/g++.exe' } | Where-Object { Test-Path -LiteralPath $_ })
    if ($mingwCandidates.Count -ne 1) { throw 'Cannot uniquely select a Qt MinGW toolchain. Pass -CxxCompiler; do not mix MSYS/Qt runtimes.' }
    $CxxCompiler = $mingwCandidates[0]
}
if ($CxxCompiler) { $CxxCompiler = Resolve-Tool $CxxCompiler 'g++' }
if (-not $MakeProgram) { $MakeProgram = Read-CMakeCacheValue $cachePath 'CMAKE_MAKE_PROGRAM' }
if (-not $MakeProgram -and $Generator -like 'Ninja*') {
    $kitNinja = Join-Path $qtTools 'Ninja/ninja.exe'
    $MakeProgram = if (Test-Path -LiteralPath $kitNinja) { $kitNinja } else { Resolve-Tool '' 'ninja' }
}
if ($MakeProgram) { $MakeProgram = Resolve-Tool $MakeProgram 'ninja' }

$originalPath = $env:PATH
Push-Location $repoRoot
try {
    $runtimePaths = @($qtBin, (Split-Path -Parent $cmake))
    if ($CxxCompiler) { $runtimePaths = @((Split-Path -Parent $CxxCompiler)) + $runtimePaths }
    if ($MakeProgram) { $runtimePaths += Split-Path -Parent $MakeProgram }
    $env:PATH = ($runtimePaths -join ';') + ';' + $originalPath
    $cmakeArgs = @('-S', $sourcePath, '-B', $buildPath, '-G', $Generator,
        "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_PREFIX_PATH=$CMakePrefixPath") + $variantOptions
    if ($CxxCompiler) { $cmakeArgs += "-DCMAKE_CXX_COMPILER=$CxxCompiler" }
    if ($MakeProgram) { $cmakeArgs += "-DCMAKE_MAKE_PROGRAM=$MakeProgram" }
    Write-Host "==> Configure $Variant / $Configuration"
    Invoke-Checked $cmake $cmakeArgs
    Write-Host "==> Build $appName"
    Invoke-Checked $cmake @('--build', $buildPath, '--config', $Configuration, '--parallel', "$Parallel", '--target', $appName)
    if (-not $SkipTests) {
        Write-Host '==> Build and run all tests for this variant'
        Invoke-Checked $cmake (@('--build', $buildPath, '--config', $Configuration, '--parallel', "$Parallel", '--target') + $testTargets)
        if (-not $ctest) { $ctest = Resolve-Tool '' 'ctest' }
        Invoke-Checked $ctest @('--test-dir', $buildPath, '--output-on-failure', '-C', $Configuration)
    }

    $binaryRoot = if ($executableSubdir) { Join-Path $buildPath $executableSubdir } else { $buildPath }
    $exePath = Join-Path $binaryRoot "$appName.exe"
    if ($Generator -match 'Visual Studio|Multi-Config') { $exePath = Join-Path $binaryRoot "$Configuration/$appName.exe" }
    if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) { throw "Built executable was not found: $exePath" }
    # Unique final directory: no recursive deletion or replacement of old packages.
    $packageName = "$appName-$Configuration-$(Get-Date -Format 'yyyyMMdd-HHmmss-fff')-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
    $packagePath = Join-Path $distPath $packageName
    $zipPath = "$packagePath.zip"
    New-Item -ItemType Directory -Path $packagePath | Out-Null
    Copy-Item -LiteralPath $exePath -Destination $packagePath
    $deployMode = if ($Configuration -eq 'Debug') { '--debug' } else { '--release' }
    Write-Host '==> Deploy Qt runtime'
    Invoke-Checked (Join-Path $qtBin 'windeployqt.exe') @($deployMode, '--compiler-runtime', '--no-translations',
        '--dir', $packagePath, (Join-Path $packagePath "$appName.exe"))
    foreach ($required in @('Qt6Core', 'Qt6Gui', 'Qt6Widgets', 'Qt6Network', 'Qt6Multimedia')) {
        $suffix = if ($Configuration -eq 'Debug') { 'd.dll' } else { '.dll' }
        if (-not (Test-Path -LiteralPath (Join-Path $packagePath "$required$suffix"))) { throw "Incomplete Qt deployment: $required$suffix" }
    }
    $platformPlugin = if ($Configuration -eq 'Debug') { 'platforms/qwindowsd.dll' } else { 'platforms/qwindows.dll' }
    if (-not (Test-Path -LiteralPath (Join-Path $packagePath $platformPlugin))) { throw 'Missing Windows platform plugin.' }
    $testStatus = if ($SkipTests) { 'SKIPPED (caller requested -SkipTests)' } else { 'PASSED (all variant CTest targets)' }
    $manifest = @("$appName Release Package", "Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')",
        "Variant: $Variant", "Configuration: $Configuration", "SourceDir: $sourcePath", "BuildDir: $buildPath",
        "QtBin: $qtBin", "Tests: $testStatus", "Executable SHA256: $((Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash)",
        '', "Run: $appName.exe")
    if ($is128) {
        $manifest += @('M128 v1: 128 channels, 48000 Hz, signed PCM24.',
            'PC: 192.168.10.1/24, local UDP port 5000.', 'FPGA source: 192.168.10.2:5000.',
            'Video: USB camera attached to the PC. UDP is receive-only; no TCP video.',
            'SYS3 UART: Windows native serial I/O, 115200 8N1. Connect manually; no automatic commands.',
            'Manual s/h/r/t/c commands require fresh SYS3 identification; r/t are blocked while recording.',
            'Allow UDP for THIS executable path if required; this package does not modify firewall rules.',
            'Import the real 128-element geometry before acoustic localization/beamforming.',
            'Digital selftest PASS does not certify MEMS acoustics or USB video.')
    } else {
        $manifest += @('Legacy 48-channel host.', 'Default TCP device endpoints:',
            '  Wired: 192.168.2.10:9968', '  Wireless: 192.168.5.1:9968')
    }
    $manifest | Set-Content -LiteralPath (Join-Path $packagePath 'RELEASE.txt') -Encoding UTF8
    if (-not $NoZip) {
        Write-Host '==> Create zip package'
        Compress-Archive -LiteralPath $packagePath -DestinationPath $zipPath
    }
    Write-Host "Package directory: $packagePath"
    if (-not $NoZip) { Write-Host "Package zip:       $zipPath" }
}
finally {
    $env:PATH = $originalPath
    Pop-Location
}
