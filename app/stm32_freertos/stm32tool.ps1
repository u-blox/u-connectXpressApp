param(
    [Parameter(Mandatory)][ValidateSet('find-vcp', 'log')][string]$Action,
    [string]$Port,
    [int]$Baud = 115200
)

function Find-Vcp {
    $m = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.Status -eq 'OK' -and $_.Name -match 'STLink Virtual COM Port \((COM\d+)\)' } |
        Select-Object -First 1
    if ($m) { [regex]::Match($m.Name, 'COM\d+').Value }
}

if ($Action -eq 'find-vcp') {
    $p = Find-Vcp
    if (-not $p) { exit 1 }
    Write-Output $p
    exit 0
}

if (-not $Port) { $Port = Find-Vcp }
if (-not $Port) { Write-Error 'No ST-Link VCP found'; exit 1 }

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'one'
$sp.DtrEnable = $true
$sp.Encoding = [System.Text.Encoding]::GetEncoding(28591)
$sp.Open()
Write-Host "[$Port @ $Baud] Ctrl-C quits. In bridge mode Ctrl-] returns to the app shell."
[Console]::TreatControlCAsInput = $true
try {
    while ($true) {
        if ($sp.BytesToRead -gt 0) { [Console]::Write($sp.ReadExisting()) }
        if ([Console]::KeyAvailable) {
            $k = [Console]::ReadKey($true)
            if ($k.Key -eq 'C' -and ($k.Modifiers -band 'Control')) { break }
            $c = $k.KeyChar
            if ($k.Key -eq 'Enter') { $c = "`r" }
            if ($c -ne [char]0) { $sp.Write([string]$c) }
        } else {
            Start-Sleep -Milliseconds 5
        }
    }
} finally {
    [Console]::TreatControlCAsInput = $false
    $sp.Close()
}
