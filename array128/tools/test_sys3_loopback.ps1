#requires -Version 7.0
param(
    [string]$HostPort = 'COM17',
    [string]$PeerPort = 'COM18',
    [string]$TestExe = 'build-array128/test_array128.exe',
    [string]$OutputRoot = 'build-array128/sys3-loopback',
    [switch]$ApplicationSmoke,
    [switch]$BacklogRecovery
)

$ErrorActionPreference = 'Stop'
if ($ApplicationSmoke -and $BacklogRecovery) { throw 'ApplicationSmoke and BacklogRecovery are mutually exclusive.' }
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
# Refuse physical/unknown ports. Both ports must be the A/B ends of one
# configured com0com pair; this test sends r/t/c ONLY to the local emulator.
$entries = @(Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Services\com0com\Parameters' | ForEach-Object { Get-ItemProperty -LiteralPath $_.PSPath })
$hostEntry = @($entries | Where-Object { $_.PortName -eq $HostPort })
$peerEntry = @($entries | Where-Object { $_.PortName -eq $PeerPort })
if ($hostEntry.Count -ne 1 -or $peerEntry.Count -ne 1 -or $HostPort -eq $PeerPort) { throw 'Both ports must be distinct com0com endpoints.' }
if ($hostEntry[0].PSChildName -notmatch '^CNC[AB](\d+)$') { throw 'Invalid host endpoint.' }
$pair = $Matches[1]
if ($peerEntry[0].PSChildName -notmatch "^CNC[AB]$pair`$") { throw 'Endpoints are not the same com0com pair.' }
$exe = [IO.Path]::GetFullPath((Join-Path $repoRoot $TestExe))
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Build test_array128 first.' }
$output = [IO.Path]::GetFullPath((Join-Path $repoRoot "$OutputRoot/run-$([guid]::NewGuid().ToString('N'))"))
New-Item -ItemType Directory -Path $output | Out-Null
$testLog = Join-Path $output 'qt-tests.txt'
$peer = [IO.Ports.SerialPort]::new($PeerPort,115200,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$peer.Handshake = [IO.Ports.Handshake]::None; $peer.DtrEnable = $false; $peer.RtsEnable = $false; $peer.WriteTimeout = 500
if ($BacklogRecovery) { $peer.WriteBufferSize = 65536 }
$hostProbe = [IO.Ports.SerialPort]::new($HostPort,115200)
$oldPort = $env:M128_SYS3_TEST_PORT
$oldPlatform = $env:QT_QPA_PLATFORM
$oldReady = $env:M128_SYS3_TEST_READY
$oldBacklog = $env:M128_SYS3_BACKLOG_TEST
$receivedCommands = [Collections.Generic.List[string]]::new()
$process = $null
try {
    $peer.Open(); $hostProbe.Open(); $hostProbe.Close() # fail if either is already in use
    $env:M128_SYS3_TEST_PORT = $HostPort
    $env:QT_QPA_PLATFORM = if ($ApplicationSmoke) { 'windows' } else { 'offscreen' }
    $env:M128_SYS3_TEST_READY = Join-Path $output 'host-ready'
    $env:M128_SYS3_BACKLOG_TEST = if ($BacklogRecovery) { '1' } else { $null }
    $arguments = if ($ApplicationSmoke) { @('--serial-port',$HostPort,'--smoke-seconds','5','--report',(Join-Path $output 'report.json'),'--screenshot',(Join-Path $output 'serial.png')) }
        elseif ($BacklogRecovery) { @('sys3VirtualSerialBacklogRecovery','-o',"$testLog,txt") }
        else { @('sys3VirtualSerialRoundTrip','-o',"$testLog,txt") }
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden
    $watch = [Diagnostics.Stopwatch]::StartNew()
    [uint32[]]$words = New-Object uint32[] 34
    $words[2] = 0x0000FBFF; $words[7] = 7; $words[8] = 0x600; $words[12] = 0x2C00A
    $words[13] = 0x001CC916; $words[20] = 0x302E; $words[21] = 0x8006; $words[27] = 1
    $commandCount = 0; $nextFrame = 200; $backlogSent = $false; $backlogBytes = 0
    $normalFrameInterval = if ($BacklogRecovery) { 200 } else { 250 }
    while (-not $process.HasExited -and $watch.ElapsedMilliseconds -lt 18000) {
        foreach ($command in $peer.ReadExisting().ToCharArray()) {
            if ('shrtc'.IndexOf($command) -lt 0) { throw "Unexpected command byte: $([int]$command)" }
            if ($BacklogRecovery -and $command -ne 's') { throw "Unexpected backlog-recovery command: $command" }
            $receivedCommands.Add([string]$command); $commandCount++
            $words[5] = $commandCount -bor ([int]$command -shl 16)
            if ($command -eq 'r') { $words[8] = 0x600; $words[27] = 1 }
            if ($command -eq 't') { $words[8] = $words[8] -bxor 0x808; $words[27]++ }
            if ($command -eq 'c') { $words[2] = $words[2] -band 0xFFF87FFF }
        }
        $ready = if ($ApplicationSmoke) { $watch.ElapsedMilliseconds -ge 1500 } else { Test-Path -LiteralPath $env:M128_SYS3_TEST_READY }
        if ($BacklogRecovery -and $ready -and -not $backlogSent) {
            # One native write queues 29,952 bytes on the isolated virtual
            # endpoint: substantially more than the client's 4,096-byte read.
            # Keep device time independent of host time while replaying this
            # burst, then continue it monotonically in the recovery frames.
            $burst = [Text.StringBuilder]::new(96 * 312)
            for ($frame = 0; $frame -lt 96; $frame++) {
                $words[0]++; $words[1] += 250; $words[25] += 4000
                $line = 'SYS3 ' + (($words | ForEach-Object { $_.ToString('X8') }) -join ' ') + "`r`n"
                [void]$burst.Append($line)
            }
            $backlogBytes = $burst.Length
            $peer.Write($burst.ToString())
            $backlogSent = $true
            $nextFrame = $watch.ElapsedMilliseconds + 200
        }
        if ($ready -and $watch.ElapsedMilliseconds -ge $nextFrame) {
            $words[0]++; $words[25] += 4000
            if ($BacklogRecovery) { $words[1] += 200 } else { $words[1] = [uint32]$watch.ElapsedMilliseconds }
            $line = 'SYS3 ' + (($words | ForEach-Object { $_.ToString('X8') }) -join ' ') + "`r`n"
            # Deliberately split in the middle of a hex word.
            $peer.Write($line.Substring(0,37)); Start-Sleep -Milliseconds 2; $peer.Write($line.Substring(37))
            $nextFrame = $watch.ElapsedMilliseconds + $normalFrameInterval
        }
        Start-Sleep -Milliseconds 10
    }
    if (-not $process.HasExited) { $process.Kill(); throw 'Virtual serial test timeout.' }
    $process.WaitForExit()
    if ($ApplicationSmoke) {
        $report = Get-Content -Raw -LiteralPath (Join-Path $output 'report.json') | ConvertFrom-Json
        if ($process.ExitCode -ne 0 -or $receivedCommands.Count -ne 0 -or -not $report.sys3_uart.current -or [long]$report.sys3_uart.valid_frames -lt 4) { throw 'Application serial smoke failed.' }
        "PASS: application received SYS3 on virtual pair; zero automatic commands; evidence $output"
    } elseif ($BacklogRecovery) {
        Get-Content -LiteralPath $testLog
        if ($process.ExitCode -ne 0 -or -not $backlogSent -or $backlogBytes -lt 20000 -or ($receivedCommands -join '') -ne 's') { throw 'Serial backlog recovery failed.' }
        "PASS: $HostPort <-> $PeerPort; $backlogBytes-byte burst recovered; received only manual s; evidence $output"
    } else {
        Get-Content -LiteralPath $testLog
        if ($process.ExitCode -ne 0 -or ($receivedCommands -join '') -ne 'shrtc') { throw 'Serial round trip failed.' }
        "PASS: $HostPort <-> $PeerPort; bytes received exactly shrtc; evidence $output"
    }
}
finally {
    if ($process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    if ($peer.IsOpen) { $peer.Close() }; $peer.Dispose(); $hostProbe.Dispose()
    $env:M128_SYS3_TEST_PORT = $oldPort; $env:QT_QPA_PLATFORM = $oldPlatform; $env:M128_SYS3_TEST_READY = $oldReady
    $env:M128_SYS3_BACKLOG_TEST = $oldBacklog
}
