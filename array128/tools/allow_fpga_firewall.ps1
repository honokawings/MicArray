#requires -RunAsAdministrator
# Run only after explicit user approval. Does not disable or modify other rules.
$ErrorActionPreference = 'Stop'
$m128RuleName = 'MicArray128-UDP5000-FPGA'
$m128Program = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../build-array128/MicArray128.exe')).Path
$m128Expected = 'D:\Workspace\QT\MicArray\build-array128\MicArray128.exe'
if ($m128Program -ine $m128Expected) { throw 'Executable path is outside the explicitly approved target.' }
$m128Existing = Get-NetFirewallRule -Name $m128RuleName -ErrorAction SilentlyContinue
if (-not $m128Existing) {
    New-NetFirewallRule -Name $m128RuleName `
        -DisplayName 'MicArray128 FPGA UDP 5000 (192.168.10.2 only)' `
        -Direction Inbound -Action Allow -Protocol UDP -LocalPort 5000 `
        -RemoteAddress 192.168.10.2 -RemotePort 5000 -Program $m128Program `
        -Profile Any | Out-Null
}
$m128Rule = Get-NetFirewallRule -Name $m128RuleName
$m128App = $m128Rule | Get-NetFirewallApplicationFilter
$m128Port = $m128Rule | Get-NetFirewallPortFilter
$m128Address = $m128Rule | Get-NetFirewallAddressFilter
if ($m128App.Program -ine $m128Program -or $m128Rule.Direction -ne 'Inbound' `
    -or $m128Rule.Action -ne 'Allow' -or $m128Rule.Enabled -ne 'True' `
    -or $m128Port.Protocol -ne 'UDP' -or $m128Port.LocalPort -ne '5000' `
    -or $m128Port.RemotePort -ne '5000' -or $m128Address.RemoteAddress -ne '192.168.10.2') {
    throw 'An existing rule differs from the approved scope; nothing else was changed.'
}
Write-Output 'Approved MicArray128 UDP rule is installed and verified.'
