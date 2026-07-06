param(
    [string]$BuildDir = "build\Desktop_Qt_6_11_1_MinGW_64_bit_Release",
    [string]$DistDir = "dist",
    [string]$Configuration = "Release",
    [string]$Generator = "Ninja",
    [string]$CMakePrefixPath = "",
    [switch]$SkipTests,
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Join-Path $PSScriptRoot "..") $Path))
}

function Read-CMakeCacheValue {
    param(
        [string]$CachePath,
        [string]$Name
    )
    if (-not (Test-Path -LiteralPath $CachePath)) {
        return ""
    }
    $line = Get-Content -LiteralPath $CachePath | Where-Object { $_ -match "^$([regex]::Escape($Name)):" } | Select-Object -First 1
    if (-not $line) {
        return ""
    }
    return ($line -replace "^[^=]+=", "").Trim()
}

function Require-Command {
    param([string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "Required command '$Name' was not found in PATH."
    }
    return $command.Source
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildPath = Resolve-RepoPath $BuildDir
$distPath = Resolve-RepoPath $DistDir
$stageRoot = Join-Path $distPath "MicArray"
$packageName = "MicArray-$Configuration-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
$packagePath = Join-Path $distPath $packageName
$zipPath = "$packagePath.zip"
$debugCache = Join-Path $repoRoot "build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug\CMakeCache.txt"

Push-Location $repoRoot
try {
    Require-Command "cmake" | Out-Null

    if ([string]::IsNullOrWhiteSpace($CMakePrefixPath)) {
        $CMakePrefixPath = Read-CMakeCacheValue -CachePath $debugCache -Name "CMAKE_PREFIX_PATH"
    }

    $cmakeArgs = @(
        "-S", ".",
        "-B", $buildPath,
        "-G", $Generator,
        "-DCMAKE_BUILD_TYPE=$Configuration"
    )
    if (-not [string]::IsNullOrWhiteSpace($CMakePrefixPath)) {
        $cmakeArgs += "-DCMAKE_PREFIX_PATH=$CMakePrefixPath"
    }

    Write-Host "==> Configure $Configuration build"
    & cmake @cmakeArgs

    Write-Host "==> Build MicArray"
    & cmake --build $buildPath --config $Configuration --target MicArray

    if (-not $SkipTests) {
        Write-Host "==> Build tests"
        & cmake --build $buildPath --config $Configuration --target test_protocol test_device test_media

        Write-Host "==> Run tests"
        & ctest --test-dir $buildPath --output-on-failure -C $Configuration
    }

    $cachePath = Join-Path $buildPath "CMakeCache.txt"
    $qmake = Read-CMakeCacheValue -CachePath $cachePath -Name "QT_QMAKE_EXECUTABLE"
    if ([string]::IsNullOrWhiteSpace($qmake)) {
        $qmake = Read-CMakeCacheValue -CachePath $debugCache -Name "QT_QMAKE_EXECUTABLE"
    }
    if ([string]::IsNullOrWhiteSpace($qmake)) {
        throw "Could not locate QT_QMAKE_EXECUTABLE from CMake cache. Pass -CMakePrefixPath or configure the project with Qt first."
    }

    $qtBin = Split-Path -Parent $qmake
    $windeployqt = Join-Path $qtBin "windeployqt.exe"
    if (-not (Test-Path -LiteralPath $windeployqt)) {
        throw "windeployqt.exe was not found at '$windeployqt'."
    }

    $exePath = Join-Path $buildPath "MicArray.exe"
    if (-not (Test-Path -LiteralPath $exePath)) {
        throw "Built executable was not found at '$exePath'."
    }

    if (Test-Path -LiteralPath $stageRoot) {
        Remove-Item -LiteralPath $stageRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $stageRoot | Out-Null

    Copy-Item -LiteralPath $exePath -Destination $stageRoot

    Write-Host "==> Deploy Qt runtime"
    & $windeployqt --release --compiler-runtime --no-translations (Join-Path $stageRoot "MicArray.exe")

    $manifest = @(
        "MicArray Release Package",
        "Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')",
        "Configuration: $Configuration",
        "BuildDir: $buildPath",
        "QtBin: $qtBin",
        "",
        "Run: MicArray.exe",
        "Default device endpoints:",
        "  Wired: 192.168.2.10:9968",
        "  Wireless: 192.168.5.1:9968"
    )
    $manifest | Set-Content -LiteralPath (Join-Path $stageRoot "RELEASE.txt") -Encoding UTF8

    if (Test-Path -LiteralPath $packagePath) {
        Remove-Item -LiteralPath $packagePath -Recurse -Force
    }
    Move-Item -LiteralPath $stageRoot -Destination $packagePath

    if (-not $NoZip) {
        if (Test-Path -LiteralPath $zipPath) {
            Remove-Item -LiteralPath $zipPath -Force
        }
        Write-Host "==> Create zip package"
        Compress-Archive -LiteralPath $packagePath -DestinationPath $zipPath -Force
    }

    Write-Host ""
    Write-Host "Package directory: $packagePath"
    if (-not $NoZip) {
        Write-Host "Package zip:       $zipPath"
    }
}
finally {
    Pop-Location
}
