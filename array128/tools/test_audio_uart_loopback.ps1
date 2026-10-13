#requires -Version 7.0
param(
    [string]$HostPort = 'COM17',
    [string]$PeerPort = 'COM18',
    [string]$TestExe = 'build-array128/test_array128.exe',
    [string]$OutputRoot = 'build-array128/uart-audio-loopback'
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
# This runner is deliberately restricted to an already configured com0com pair.
$entries = @(Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Services\com0com\Parameters' | ForEach-Object { Get-ItemProperty -LiteralPath $_.PSPath })
$hostEntry = @($entries | Where-Object { $_.PortName -eq $HostPort })
$peerEntry = @($entries | Where-Object { $_.PortName -eq $PeerPort })
if ($hostEntry.Count -ne 1 -or $peerEntry.Count -ne 1 -or $HostPort -eq $PeerPort) { throw 'Both ports must be distinct com0com endpoints.' }
if ($hostEntry[0].PSChildName -notmatch '^CNC[AB](\d+)$') { throw 'Invalid host endpoint.' }
$pair = $Matches[1]
if ($peerEntry[0].PSChildName -notmatch "^CNC[AB]$pair`$") { throw 'Endpoints must belong to the same com0com pair.' }
$exe = [IO.Path]::GetFullPath((Join-Path $repoRoot $TestExe))
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Build test_array128 first.' }
$output = [IO.Path]::GetFullPath((Join-Path $repoRoot "$OutputRoot/run-$([guid]::NewGuid().ToString('N'))"))
if (-not $output.StartsWith($repoRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Evidence must stay in the repository.' }
New-Item -ItemType Directory -Path $output | Out-Null
$testLog = Join-Path $output 'qt-tests.txt'
$peer = [IO.Ports.SerialPort]::new($PeerPort, 115200, [IO.Ports.Parity]::None, 8, [IO.Ports.StopBits]::One)
$peer.Handshake = [IO.Ports.Handshake]::None; $peer.DtrEnable = $false; $peer.RtsEnable = $false; $peer.WriteTimeout = 500
$probe = [IO.Ports.SerialPort]::new($HostPort, 115200)
$oldPort = $env:M128_UART_AUDIO_TEST_PORT
$oldReady = $env:M128_UART_AUDIO_READY
$oldEvidence = $env:M128_UART_AUDIO_EVIDENCE
$oldPlatform = $env:QT_QPA_PLATFORM
$received = [Collections.Generic.List[string]]::new()
$wire = [Collections.Generic.List[string]]::new()
$process = $null
try {
    $peer.Open(); $probe.Open(); $probe.Close()
    $env:M128_UART_AUDIO_TEST_PORT = $HostPort
    $env:M128_UART_AUDIO_READY = Join-Path $output 'host-ready'
    $env:M128_UART_AUDIO_EVIDENCE = $output
    $env:QT_QPA_PLATFORM = 'offscreen'
    $process = Start-Process -FilePath $exe -ArgumentList 'audioUartVirtualSerialRoundTrip','-o',"$testLog,txt" -WindowStyle Hidden -PassThru
    [uint32[]]$sys = New-Object uint32[] 34
    $sys[2] = 0x0000FBFF; $sys[7] = 7; $sys[8] = 0x7600; $sys[27] = 1
    [uint32[]]$audio = New-Object uint32[] 14
    $audio[1] = 5; $audio[2] = $audio[3] = 128
    $audio[4] = $audio[5] = $audio[6] = $audio[7] = 0x400000
    $watch = [Diagnostics.Stopwatch]::StartNew(); $nextSys = 100; $buffer = ''
    while (-not $process.HasExited -and $watch.ElapsedMilliseconds -lt 18000) {
        foreach ($byte in $peer.ReadExisting().ToCharArray()) {
            if ($byte -eq "`r") { continue }
            if ($byte -ne "`n") { $buffer += $byte; continue }
            $command = $buffer; $buffer = ''; $received.Add($command); $wire.Add("TX $command")
            $gainResponse = $command.StartsWith('@GAIN')
            switch -Regex ($command) {
                '^@I2S\?$|^@GAIN\?$' {
                    $audio[3] = $audio[2]; $audio[6] = $audio[4]; $audio[7] = $audio[5]; $audio[1] = 5
                }
                '^@I2S=(\d+)$' {
                    $channel = [int]$Matches[1]; if ($channel -gt 127) { throw 'Out-of-range command emitted.' }
                    $audio[2] = 128 + $channel; $audio[1] = 6
                }
                '^@I2S=OFF$' { $audio[2] = $audio[2] -band 127; $audio[1] = 6 }
                '^@GAIN=([0-7][0-9A-F]{5}),([0-7][0-9A-F]{5})$' {
                    $audio[4] = [Convert]::ToUInt32($Matches[1],16); $audio[5] = [Convert]::ToUInt32($Matches[2],16); $audio[1] = 6
                }
                default { throw "Unexpected TX: $command" }
            }
            $audio[0]++; $audio[8] += 4800; $audio[9] += 4800; $audio[12] += 4800; $audio[11] = 0xFFF123
            $prefix = if ($gainResponse) { 'GAIN1 ' } else { 'I2S1 ' }
            $line = $prefix + (($audio | ForEach-Object { $_.ToString('X8') }) -join ' ') + "`r`n"
            $wire.Add('RX ' + $line.TrimEnd())
            $peer.Write($line.Substring(0,37)); Start-Sleep -Milliseconds 2; $peer.Write($line.Substring(37))
        }
        if ((Test-Path -LiteralPath $env:M128_UART_AUDIO_READY) -and $watch.ElapsedMilliseconds -ge $nextSys) {
            $sys[0]++; $sys[1] = [uint32]$watch.ElapsedMilliseconds
            $line = 'SYS3 ' + (($sys | ForEach-Object { $_.ToString('X8') }) -join ' ') + "`r`n"
            $wire.Add('RX ' + $line.TrimEnd()); $peer.Write($line); $nextSys = $watch.ElapsedMilliseconds + 250
        }
        Start-Sleep -Milliseconds 10
    }
    if (-not $process.HasExited) { $process.Kill(); throw 'Virtual UART audio test timeout.' }
    $process.WaitForExit(); Get-Content -LiteralPath $testLog
    $expected = @('@I2S?','@I2S=127','@I2S?','@I2S=OFF','@I2S?','@GAIN?','@GAIN=200000,300000','@GAIN?','@GAIN?')
    if ($process.ExitCode -ne 0 -or ($received -join '|') -cne ($expected -join '|')) { throw "UART audio round-trip failed: $($received -join ' | ')" }
    $report = Get-Content -Raw -LiteralPath (Join-Path $output 'report.json') | ConvertFrom-Json
    if ([long]$report.sys3_uart.invalid_lines -ne 0 -or -not $report.sys3_uart.audio_uart.current) { throw 'Invalid/stale report.' }
    "PASS: $HostPort <-> $PeerPort; nine exact newline-terminated audio commands; evidence $output"
}
finally {
    if ($process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    [IO.File]::WriteAllLines((Join-Path $output 'wire.log'), $wire)
    if ($peer.IsOpen) { $peer.Close() }; $peer.Dispose(); $probe.Dispose()
    $env:M128_UART_AUDIO_TEST_PORT = $oldPort; $env:M128_UART_AUDIO_READY = $oldReady
    $env:M128_UART_AUDIO_EVIDENCE = $oldEvidence; $env:QT_QPA_PLATFORM = $oldPlatform
}
