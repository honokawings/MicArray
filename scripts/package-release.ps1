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
    [string]$FusionModelDir = '',
    [string]$OnnxRuntimeDir = '',
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

function Get-Sha256 {
    param([string]$Path)
    # .NET is available in both Windows PowerShell 5.1 and PowerShell 7 even
    # when their module search paths have been isolated by a launcher.
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}

# Read-only protobuf inspection avoids executing a model/runtime or requiring
# Python merely to validate the fixed ONNX deployment contract in -PlanOnly.
function Read-ProtoVarint {
    param([byte[]]$Bytes, [ref]$Offset, [long]$End)
    [uint64]$value = 0
    for ($shift = 0; $shift -lt 63; $shift += 7) {
        if ($Offset.Value -ge $End) { throw 'Truncated ONNX protobuf varint.' }
        $next = $Bytes[$Offset.Value]; $Offset.Value++
        $value = $value -bor ([uint64]($next -band 127) -shl $shift)
        if (($next -band 128) -eq 0) { return [long]$value }
    }
    throw 'Unsupported or overflowing ONNX protobuf varint.'
}

function Read-ProtoFields {
    param([byte[]]$Bytes, [long]$Start, [long]$Length)
    if ($Start -lt 0 -or $Length -lt 0 -or $Start -gt $Bytes.Length - $Length) { throw 'ONNX protobuf bounds are invalid.' }
    [long]$offset = $Start; [long]$end = $Start + $Length
    while ($offset -lt $end) {
        $key = Read-ProtoVarint $Bytes ([ref]$offset) $end
        $number = $key -shr 3; $wire = $key -band 7
        if ($number -lt 1) { throw 'Invalid ONNX protobuf field number.' }
        [long]$value = 0; [long]$size = 0; [long]$dataStart = $offset
        switch ($wire) {
            0 { $value = Read-ProtoVarint $Bytes ([ref]$offset) $end }
            1 { $size = 8 }
            2 { $size = Read-ProtoVarint $Bytes ([ref]$offset) $end; $dataStart = $offset }
            5 { $size = 4 }
            default { throw 'Unsupported ONNX protobuf wire type.' }
        }
        if ($size -gt $end - $offset) { throw 'Truncated ONNX protobuf data.' }
        if ($wire -ne 0) { $offset += $size }
        [pscustomobject]@{ Number = $number; Wire = $wire; Start = $dataStart; Length = $size; Value = $value }
    }
}

function Get-OneProtoField {
    param([object[]]$Fields, [int]$Number, [int]$Wire)
    $matches = @($Fields | Where-Object { $_.Number -eq $Number -and $_.Wire -eq $Wire })
    if ($matches.Count -ne 1) { throw "ONNX contract requires exactly one field $Number with wire type $Wire." }
    return $matches[0]
}

function Assert-JsonArray {
    param($Actual, [object[]]$Expected, [string]$Name)
    if ($Actual -isnot [Array] -or $Actual.Count -ne $Expected.Count) { throw "Invalid fusion metadata array: $Name" }
    for ($index = 0; $index -lt $Expected.Count; ++$index) {
        if ($Expected[$index] -is [string]) {
            if ($Actual[$index] -isnot [string] -or $Actual[$index] -cne $Expected[$index]) { throw "Invalid fusion metadata array: $Name" }
        } elseif ($Actual[$index] -isnot [ValueType] -or $Actual[$index] -is [bool] -or [double]$Actual[$index] -ne [double]$Expected[$index]) {
            throw "Invalid fusion metadata numeric array: $Name"
        }
    }
}

function Read-OnnxTensorContract {
    param([byte[]]$Bytes, [object]$ValueInfo)
    $fields = @(Read-ProtoFields $Bytes $ValueInfo.Start $ValueInfo.Length)
    $name = Get-OneProtoField $fields 1 2
    $type = Get-OneProtoField $fields 2 2
    $tensor = Get-OneProtoField @(Read-ProtoFields $Bytes $type.Start $type.Length) 1 2
    $tensorFields = @(Read-ProtoFields $Bytes $tensor.Start $tensor.Length)
    $element = Get-OneProtoField $tensorFields 1 0
    $shape = Get-OneProtoField $tensorFields 2 2
    $dimensions = @()
    foreach ($dimension in @(Read-ProtoFields $Bytes $shape.Start $shape.Length)) {
        if ($dimension.Number -ne 1 -or $dimension.Wire -ne 2) { throw 'Unsupported ONNX tensor dimension.' }
        $value = Get-OneProtoField @(Read-ProtoFields $Bytes $dimension.Start $dimension.Length) 1 0
        $dimensions += $value.Value
    }
    return [pscustomobject]@{ Name = [Text.Encoding]::UTF8.GetString($Bytes, $name.Start, $name.Length); Element = $element.Value; Shape = $dimensions }
}

function Assert-OnnxContract {
    param([string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $graph = Get-OneProtoField @(Read-ProtoFields $bytes 0 $bytes.Length) 7 2
    $fields = @(Read-ProtoFields $bytes $graph.Start $graph.Length)
    $inputContract = Read-OnnxTensorContract $bytes (Get-OneProtoField $fields 11 2)
    $outputContract = Read-OnnxTensorContract $bytes (Get-OneProtoField $fields 12 2)
    if ($inputContract.Name -cne 'rgb_srp' -or $inputContract.Element -ne 1 -or ($inputContract.Shape -join ',') -ne '1,4,192,320' -or
        $outputContract.Name -cne 'detections' -or $outputContract.Element -ne 1 -or ($outputContract.Shape -join ',') -ne '1,1260,9') {
        throw 'ONNX graph input/output name, float32 type or fixed shape differs from the fusion contract.'
    }
}

function Read-U32 {
    param([byte[]]$Bytes, [long]$Offset)
    if ($Offset -lt 0 -or $Offset -gt $Bytes.Length - 4) { throw 'Invalid PE file bounds.' }
    return [BitConverter]::ToUInt32($Bytes, [int]$Offset)
}

function Convert-PeRva {
    param([long]$Rva, [object[]]$Sections, [long]$FileLength)
    foreach ($section in $Sections) {
        if ($Rva -ge $section.Address -and $Rva -lt $section.Address + $section.Size) {
            $offset = $section.Raw + ($Rva - $section.Address)
            if ($offset -ge 0 -and $offset -lt $FileLength) { return $offset }
        }
    }
    throw 'PE export points outside file sections.'
}

function Read-PeString {
    param([byte[]]$Bytes, [long]$Offset)
    $end = $Offset
    while ($end -lt $Bytes.Length -and $end - $Offset -le 512 -and $Bytes[$end] -ne 0) { ++$end }
    if ($end -ge $Bytes.Length -or $end - $Offset -gt 512) { throw 'Invalid PE string.' }
    return [Text.Encoding]::ASCII.GetString($Bytes, $Offset, $end - $Offset)
}

function Assert-X64Runtime {
    param([string]$Path, [switch]$RequireOrtApi)
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 256 -or $bytes[0] -ne 77 -or $bytes[1] -ne 90) { throw 'ONNX runtime is not a Windows PE DLL.' }
    $pe = Read-U32 $bytes 60
    if ((Read-U32 $bytes $pe) -ne 17744 -or $pe -gt $bytes.Length - 144) { throw 'ONNX runtime PE header is invalid.' }
    if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 34404 -or [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 523) {
        throw 'ONNX runtime must be Windows x64 (AMD64 PE32+), matching the 128-channel package.'
    }
    $count = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $optionalSize = [BitConverter]::ToUInt16($bytes, $pe + 20)
    $sectionStart = $pe + 24 + $optionalSize
    $sections = @()
    for ($i = 0; $i -lt $count; ++$i) {
        $section = $sectionStart + $i * 40
        $sections += [pscustomobject]@{ Address = (Read-U32 $bytes ($section + 12)); Size = (Read-U32 $bytes ($section + 16)); Raw = (Read-U32 $bytes ($section + 20)) }
    }
    $versionText = ''
    if ($RequireOrtApi) {
        $export = Convert-PeRva (Read-U32 $bytes ($pe + 24 + 112)) $sections $bytes.Length
        $names = Read-U32 $bytes ($export + 24)
        if ($names -gt 100000) { throw 'Invalid ONNX runtime PE export count.' }
        $table = Convert-PeRva (Read-U32 $bytes ($export + 32)) $sections $bytes.Length
        $found = $false
        for ($i = 0; $i -lt $names; ++$i) {
            $offset = Convert-PeRva (Read-U32 $bytes ($table + $i * 4)) $sections $bytes.Length
            if ((Read-PeString $bytes $offset) -ceq 'OrtGetApiBase') { $found = $true; break }
        }
        if (-not $found) { throw 'Runtime DLL does not export OrtGetApiBase.' }
        $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
        if ($version.CompanyName -notmatch 'Microsoft' -or $version.FileVersion -notmatch '^1\.(\d+)\.' -or [int]$Matches[1] -lt 30) {
            throw 'Use an official Microsoft ONNX Runtime 1.30+ x64 CPU DLL (C API 30 required).'
        }
        $versionText = $version.FileVersion
    }
    $imports = @()
    $importRva = Read-U32 $bytes ($pe + 24 + 120)
    if ($importRva) {
        $table = Convert-PeRva $importRva $sections $bytes.Length
        for ($i = 0; $i -lt 4096; ++$i) {
            $descriptor = $table + $i * 20
            $nameRva = Read-U32 $bytes ($descriptor + 12)
            if (-not $nameRva -and -not (Read-U32 $bytes ($descriptor + 16))) { break }
            $imports += (Read-PeString $bytes (Convert-PeRva $nameRva $sections $bytes.Length)).ToLowerInvariant()
        }
        if ($i -eq 4096) { throw 'Invalid PE import table.' }
    }
    # Delay-loaded DLLs also count as dependencies; this is binary inspection,
    # never LoadLibrary or execution of untrusted initializers during planning.
    $delayRva = Read-U32 $bytes ($pe + 24 + 112 + 13 * 8)
    if ($delayRva) {
        $table = Convert-PeRva $delayRva $sections $bytes.Length
        for ($i = 0; $i -lt 4096; ++$i) {
            $descriptor = $table + $i * 32
            $attributes = Read-U32 $bytes $descriptor
            $nameRva = Read-U32 $bytes ($descriptor + 4)
            if (-not $nameRva) { break }
            if ($attributes -ne 1) { throw 'Unsupported VA-based delayed PE import.' }
            $imports += (Read-PeString $bytes (Convert-PeRva $nameRva $sections $bytes.Length)).ToLowerInvariant()
        }
        if ($i -eq 4096) { throw 'Invalid delayed PE import table.' }
    }
    $systemLibraries = @('kernel32.dll', 'advapi32.dll', 'ntdll.dll', 'ole32.dll', 'oleaut32.dll', 'user32.dll', 'gdi32.dll',
        'shell32.dll', 'shlwapi.dll', 'ws2_32.dll', 'bcrypt.dll', 'crypt32.dll', 'secur32.dll', 'rpcrt4.dll', 'dbghelp.dll',
        'setupapi.dll', 'dxgi.dll', 'd3d12.dll', 'd3d11.dll', 'dwrite.dll', 'version.dll', 'psapi.dll', 'winmm.dll',
        'combase.dll', 'normaliz.dll', 'powrprof.dll', 'ucrtbase.dll')
    $vcLibraries = @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
        'vcruntime140.dll', 'vcruntime140_1.dll', 'concrt140.dll')
    foreach ($dependency in $imports) {
        if ($dependency -notin $systemLibraries -and $dependency -notin $vcLibraries -and
            $dependency -ne 'onnxruntime_providers_shared.dll' -and $dependency -notmatch '^(api-ms-win-|ext-ms-win-)[a-z0-9-]+\.dll$') {
            throw "Unsupported runtime DLL dependency '$dependency'. Supply the official CPU runtime; unknown dependencies are not executed or copied."
        }
    }
    return [pscustomobject]@{ Version = $versionText; Imports = @($imports | Select-Object -Unique) }
}

function Get-FusionPackage {
    if (-not $FusionModelDir -and -not $OnnxRuntimeDir) {
        return [pscustomobject]@{ Plan = [ordered]@{ Enabled = $false; Files = @(); Capability = 'No model or ONNX Runtime is included; neural inference is not ready.' }; Copies = @() }
    }
    if (-not $is128) { throw 'FusionModelDir/OnnxRuntimeDir are supported only for Array128.' }
    if (-not $FusionModelDir -or -not $OnnxRuntimeDir) { throw 'Provide FusionModelDir and OnnxRuntimeDir together; partial neural packages are not supported.' }
    $modelRoot = Resolve-RepoPath $FusionModelDir; $runtimeRoot = Resolve-RepoPath $OnnxRuntimeDir
    if (-not (Test-Path -LiteralPath $modelRoot -PathType Container) -or -not (Test-Path -LiteralPath $runtimeRoot -PathType Container)) {
        throw 'Explicit fusion model and runtime directories must both exist.'
    }
    $modelPath = Join-Path $modelRoot 'fusion.onnx'; $metadataPath = Join-Path $modelRoot 'fusion.json'
    foreach ($path in @($modelPath, $metadataPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required fusion file is missing: $path" }
    }
    if ((Get-Item -LiteralPath $metadataPath).Length -gt 1MB -or (Get-Item -LiteralPath $modelPath).Length -gt 512MB) { throw 'Fusion files exceed supported size limits.' }
    $metadata = Get-Content -LiteralPath $metadataPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $expected = @{
        format = 'micarray128-yolox-fusion-v1'; architecture = 'YOLOX-Nano-derived-RGB-SRP-point-active'; domain = 'simulation_only'
        input_name = 'rgb_srp'; input_dtype = 'float32'; output_name = 'detections'; rgb_normalization = 'uint8_div_255'
        srp_normalization = 'cpp_response_0_1'; srp_resize = 'bilinear_half_pixel_align_corners_false'
        box_format = 'normalized_cxcywh'; source_point_format = 'normalized_video_uv'
        source_point_origin = 'independent_learned_regression_head_not_box_center_or_srp_peak'
        confidence_rule = 'objectness_times_class_0'; projection = 'pinhole'
    }
    foreach ($key in $expected.Keys) { if ($metadata.$key -isnot [string] -or $metadata.$key -cne $expected[$key]) { throw "Invalid fusion metadata field: $key" } }
    Assert-JsonArray $metadata.input_shape @(1, 4, 192, 320) 'input_shape'
    Assert-JsonArray $metadata.output_shape @(1, 1260, 9) 'output_shape'
    Assert-JsonArray $metadata.channel_order @('R', 'G', 'B', 'SRP') 'channel_order'
    Assert-JsonArray $metadata.output_layout @('cx', 'cy', 'width', 'height', 'objectness', 'class_0', 'source_x', 'source_y', 'active') 'output_layout'
    Assert-JsonArray $metadata.class_names @('visible_candidate_simulation_domain') 'class_names'
    Assert-JsonArray $metadata.training_srp_grid @(72, 45) 'training_srp_grid'
    # Raw SRP includes local peak refinement; even changing the search/NMS
    # separation changes the model input, so preserve the training contract.
    if ($metadata.training_srp_settings -isnot [pscustomobject]) {
        throw 'Invalid fusion metadata object: training_srp_settings'
    }
    $expectedSrp = [ordered]@{ max_sources = 4; minimum_source_separation_deg = 4.0; sound_speed_m_s = 343.0; window_samples = 2048 }
    foreach ($key in $expectedSrp.Keys) {
        $value = $metadata.training_srp_settings.$key
        if ($value -isnot [ValueType] -or $value -is [bool] -or
            [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or
            [double]$value -ne [double]$expectedSrp[$key]) {
            throw "Invalid fusion metadata field: training_srp_settings.$key"
        }
    }
    if ($metadata.srp_color_map_or_focus_transform -isnot [bool] -or $metadata.srp_color_map_or_focus_transform) {
        throw 'Invalid fusion metadata shape, layout, class or raw SRP preprocessing contract.'
    }
    if ($metadata.release_status -like '*interface_only*') { throw 'An interface-only model is not a deployable fusion model; complete independent evaluation first.' }
    if (@($metadata.geometry_coordinates_m).Count -ne 128 -or $metadata.geometry_coordinates_source_sha256 -isnot [string] -or $metadata.geometry_coordinates_source_sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw 'Fusion model must include the complete 128-channel geometry contract and its source SHA-256.'
    }
    foreach ($coordinate in $metadata.geometry_coordinates_m) {
        if (@($coordinate).Count -ne 3) { throw 'Fusion geometry requires exactly three coordinates per channel.' }
        foreach ($value in $coordinate) {
            if ($value -isnot [ValueType] -or $value -is [bool] -or [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or [Math]::Abs([double]$value) -gt 2) {
                throw 'Fusion geometry contains a nonnumeric, nonfinite or out-of-range coordinate.'
            }
        }
    }
    $modelHash = (Get-Sha256 $modelPath).ToLowerInvariant()
    if ($metadata.model_sha256 -isnot [string] -or $metadata.model_sha256 -notmatch '^[0-9a-fA-F]{64}$' -or $modelHash -cne $metadata.model_sha256.ToLowerInvariant()) { throw 'Fusion model SHA-256 does not match fusion.json.' }
    Assert-OnnxContract $modelPath
    # Only explicit flat or official release lib/ layouts are accepted. There
    # is no recursive discovery, PATH search, virtualenv scan or DLL execution.
    $flatRuntime = Join-Path $runtimeRoot 'onnxruntime.dll'; $officialRuntime = Join-Path $runtimeRoot 'lib/onnxruntime.dll'
    $hasFlat = Test-Path -LiteralPath $flatRuntime -PathType Leaf; $hasOfficial = Test-Path -LiteralPath $officialRuntime -PathType Leaf
    if ($hasFlat -eq $hasOfficial) { throw 'Runtime directory must contain exactly one onnxruntime.dll at its root or lib/.' }
    $runtimePath = if ($hasFlat) { $flatRuntime } else { $officialRuntime }
    $runtimeInfo = Assert-X64Runtime $runtimePath -RequireOrtApi
    $runtimeVersion = $runtimeInfo.Version
    $runtimeLicense = Join-Path $runtimeRoot 'LICENSE'; $runtimeNotices = Join-Path $runtimeRoot 'ThirdPartyNotices.txt'
    foreach ($path in @($runtimeLicense, $runtimeNotices)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -lt 20) { throw 'The explicit runtime directory must include LICENSE and ThirdPartyNotices.txt.' }
    }
    if ((Get-Content -LiteralPath $runtimeLicense -Raw) -notmatch 'MIT License') { throw 'ONNX Runtime MIT license text is missing.' }
    $yoloLicense = Join-Path $repoRoot 'array128/fusion/LICENSE.yolox'; $yoloNotice = Join-Path $repoRoot 'array128/fusion/THIRD_PARTY.md'
    foreach ($path in @($yoloLicense, $yoloNotice)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'Repository YOLOX Apache license/derivation notice is missing.' } }
    $copies = @(
        [pscustomobject]@{ Source = $modelPath; Destination = 'fusion/fusion.onnx' },
        [pscustomobject]@{ Source = $metadataPath; Destination = 'fusion/fusion.json' },
        [pscustomobject]@{ Source = $runtimePath; Destination = 'onnxruntime.dll' },
        [pscustomobject]@{ Source = $runtimeLicense; Destination = 'licenses/onnxruntime/LICENSE' },
        [pscustomobject]@{ Source = $runtimeNotices; Destination = 'licenses/onnxruntime/ThirdPartyNotices.txt' },
        [pscustomobject]@{ Source = $yoloLicense; Destination = 'licenses/yolox/LICENSE' },
        [pscustomobject]@{ Source = $yoloNotice; Destination = 'licenses/yolox/THIRD_PARTY.md' }
    )
    $sharedProvider = Join-Path (Split-Path -Parent $runtimePath) 'onnxruntime_providers_shared.dll'
    if (Test-Path -LiteralPath $sharedProvider -PathType Leaf) {
        $sharedInfo = Assert-X64Runtime $sharedProvider
        $copies += [pscustomobject]@{ Source = $sharedProvider; Destination = 'onnxruntime_providers_shared.dll' }
    } elseif ($runtimeInfo.Imports -contains 'onnxruntime_providers_shared.dll') {
        throw 'The explicit runtime imports onnxruntime_providers_shared.dll but that file is missing.'
    }
    foreach ($copy in $copies) {
        if ((Get-Item -LiteralPath $copy.Source).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Fusion package files must be regular files, not links or reparse points.' }
        $copy | Add-Member -NotePropertyName SHA256 -NotePropertyValue (Get-Sha256 $copy.Source)
    }
    return [pscustomobject]@{
        Plan = [ordered]@{ Enabled = $true; Domain = 'simulation_only'; ModelSHA256 = $modelHash; RuntimeVersion = $runtimeVersion
            TrainingSrpGrid = @(72, 45); TrainingSrpSettings = $expectedSrp
            Files = @($copies | ForEach-Object { $_.Destination }); Capability = 'EXPERIMENTAL SIMULATION ONLY. Real-world semantics, hardware mapping and physical super-resolution are not certified.'
            Prerequisite = 'Microsoft Visual C++ 2015-2022 (or newer compatible) x64 Redistributable is required for ONNX Runtime. It is not downloaded, installed or copied from the system by this script.'
            RuntimeImports = @($runtimeInfo.Imports); FileSHA256 = @($copies | ForEach-Object { [ordered]@{ File = $_.Destination; SHA256 = $_.SHA256 } }) }
        Copies = $copies
    }
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
$testTargets = if ($is128) { @('test_array128', 'test_array128_imaging') } else {
    @('test_protocol', 'test_device', 'test_media', 'test_app', 'test_ui', 'test_analysis')
}
if ($is128) {
    $arrayCmake = Get-Content -LiteralPath (Join-Path $repoRoot 'array128/CMakeLists.txt') -Raw
    foreach ($target in @('test_array128_fusion', 'test_array128_fusionworker', 'test_array128_fusioncontext', 'test_array128_fusionpipeline')) {
        if ($arrayCmake -match "qt_add_executable\(\s*$([regex]::Escape($target))\b") { $testTargets += $target }
    }
}
$fusionPackage = Get-FusionPackage
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
    Fusion = $fusionPackage.Plan
}
if ($PlanOnly) { $plan | ConvertTo-Json -Depth 5; return }

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
    foreach ($copy in $fusionPackage.Copies) {
        if ((Get-Sha256 $copy.Source) -ne $copy.SHA256) { throw 'A fusion/runtime/license file changed after package validation. No zip will be produced.' }
        $destination = Join-Path $packagePath $copy.Destination
        $parent = Split-Path -Parent $destination
        if (-not (Test-Path -LiteralPath $parent -PathType Container)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
        Copy-Item -LiteralPath $copy.Source -Destination $destination
        if ((Get-Sha256 $destination) -ne $copy.SHA256) { throw 'Fusion package copy failed its SHA-256 verification.' }
    }
    $testStatus = if ($SkipTests) { 'SKIPPED (caller requested -SkipTests)' } else { 'PASSED (all variant CTest targets)' }
    $manifest = @("$appName Release Package", "Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')",
        "Variant: $Variant", "Configuration: $Configuration", "SourceDir: $sourcePath", "BuildDir: $buildPath",
        "QtBin: $qtBin", "Tests: $testStatus", "Executable SHA256: $(Get-Sha256 $exePath)",
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
        if ($fusionPackage.Plan.Enabled) {
            $manifest += @('', 'OPTIONAL RGB+SRP YOLOX FUSION: EXPERIMENTAL / SIMULATION ONLY.',
                'No certification of real-world semantic classes, physical channel mapping, or physical super-resolution.',
                'The only class is visible_candidate_simulation_domain; it is NOT a verified person/equipment detector.',
                'Audio/video pairing uses approximate host arrival times, not hardware synchronization.',
                'Model: fusion/fusion.onnx; metadata: fusion/fusion.json. Enable explicitly in the application.',
                "Model SHA256: $($fusionPackage.Plan.ModelSHA256)", "ONNX Runtime version: $($fusionPackage.Plan.RuntimeVersion)",
                $fusionPackage.Plan.Prerequisite,
                'If ONNX Runtime cannot load, verify this prerequisite and all DLLs; SRP remains available without neural fusion.',
                'Licenses: licenses/yolox (Apache-2.0); licenses/onnxruntime (MIT and ThirdPartyNotices).')
            $fusionPackage.Plan | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $packagePath 'FUSION-PACKAGE.json') -Encoding UTF8
        } else {
            $manifest += @('', 'No neural model or ONNX Runtime is included. SRP localization remains available.',
                'Neural fusion is disabled by default and is not ready until an explicit compatible model/runtime is supplied.')
        }
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
