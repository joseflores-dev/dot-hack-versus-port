# Arranca out-win\hackversus.exe, va del título a una pelea CPU VS CPU con el teclado (fallback de cellPad:
# Z = X, flechas = cruceta), deja correr la pelea y guarda capturas. Log en out-win\run.log / run.err.
# uso: powershell -File tools\win_fight.ps1 [-FightSecs 40] [-Env "AUDIO_RATE=1","HV_PUMP_THREAD=0"]
param([int]$FightSecs = 40, [string[]]$Env = @("AUDIO_RATE=1"), [double]$AfterLoad = 8, [switch]$Probe, [switch]$Attach)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class K { [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Ri, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra); }
"@
[K]::SetProcessDPIAware() | Out-Null
$shots = Join-Path $root "out-win\shots"
New-Item -ItemType Directory -Force $shots | Out-Null
foreach ($kv in $Env) { $k, $v = $kv -split "=", 2; Set-Item -Path "env:$k" -Value $v }
# -Attach: el juego ya lo lanzó otro (tools/win_fight.sh), con su salida en out-win\run.log / run.err.
if ($Attach) { while (-not ($p = Get-Process hackversus -ErrorAction SilentlyContinue | Select-Object -First 1)) { Start-Sleep -Milliseconds 200 } }
else {
$p = Start-Process (Join-Path $root "out-win\hackversus.exe") -ArgumentList "`"$root\game\EBOOT.ELF`"", "`"$root\disc`"" `
     -PassThru -RedirectStandardOutput (Join-Path $root "out-win\run.log") -RedirectStandardError (Join-Path $root "out-win\run.err")
}
$t0 = Get-Date
Remove-Item (Join-Path $root "out-win\prof.go") -ErrorAction SilentlyContinue
function Wait-Until([double]$s) { while (((Get-Date) - $t0).TotalSeconds -lt $s) { Start-Sleep -Milliseconds 100 } }
function Focus { $p.Refresh(); if ($p.MainWindowHandle -eq [IntPtr]::Zero) { return }; [K]::ShowWindow($p.MainWindowHandle, 9) | Out-Null; [K]::SetForegroundWindow($p.MainWindowHandle) | Out-Null }
function Shot([string]$name) {
    $p.Refresh(); if ($p.MainWindowHandle -eq [IntPtr]::Zero) { return }; $r = New-Object K+R; [K]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $w = $r.Ri - $r.L; $h = $r.B - $r.T; if ($w -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $h; $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $bmp.Save((Join-Path $shots "$name.png")); $g.Dispose(); $bmp.Dispose()
}
$VK = @{ x = 0x5A; o = 0x58; down = 0x28; up = 0x26; left = 0x25; right = 0x27; start = 0x0D }
function Press([string]$b, [double]$wait) {
    Focus; Start-Sleep -Milliseconds 80
    [K]::keybd_event([byte]$VK[$b], 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 140
    [K]::keybd_event([byte]$VK[$b], 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds ([int]($wait * 1000))
}
# Con partida guardada el juego la carga (cellSaveData LOAD) y luego pasa el aviso de autoguardado hasta el título.
$log = Join-Path $root "out-win\run.log"
while (-not (Select-String -Path $log -Pattern "LOAD complete" -Quiet -ErrorAction SilentlyContinue)) {
    Start-Sleep -Milliseconds 250; if (((Get-Date) - $t0).TotalSeconds -gt 120) { Write-Output "sin carga de partida"; break } }
$tl = ((Get-Date) - $t0).TotalSeconds
if ($Probe) { for ($i = 0; $i -lt 10; $i++) { Wait-Until ($tl + 2 * $i); Shot ("p{0:D2}" -f (2 * $i)) }; Stop-Process -Id $p.Id -Force; Write-Output "partida cargada a los $([int]$tl) s"; return }
# El título: pantalla clara (brillo medio > 150) después del fundido a negro que sigue al aviso de autoguardado.
function Brightness {
    $p.Refresh(); if ($p.MainWindowHandle -eq [IntPtr]::Zero) { return 0 }
    $r = New-Object K+R; [K]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $w = $r.Ri - $r.L; $h = $r.B - $r.T; if ($w -le 0) { return 0 }
    $bmp = New-Object System.Drawing.Bitmap $w, $h; $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $sum = 0; $n = 0
    for ($y = 1; $y -le 5; $y++) { for ($x = 1; $x -le 7; $x++) {
        $c = $bmp.GetPixel([int]($w * $x / 8), [int]($h * $y / 6)); $sum += ($c.R + $c.G + $c.B) / 3; $n++ } }
    $g.Dispose(); $bmp.Dispose(); return $sum / $n
}
$dark = $false
while (((Get-Date) - $t0).TotalSeconds -lt $tl + 60) {
    $b = Brightness
    if ($b -lt 40) { $dark = $true } elseif ($dark -and $b -gt 150) { break }
    Start-Sleep -Milliseconds 500
}
Start-Sleep -Seconds 3   # el menú ignora el pad durante su fundido
Shot "1_titulo"
# título -> VERSUS MODE -> Offline -> CPU VS CPU
Press down 1.5; Press x 4; Press x 5; Press down 1.2; Press down 1.2; Press x 7; Shot "2_personajes"
# personaje P1, P2, Start Battle, escenario, Yes, carga
Press x 4; Press x 5; Press x 5; Press x 6; Press x 3; Press x 5   # una ✕ de más: a veces el "Yes" se come una
$tf = ((Get-Date) - $t0).TotalSeconds
Wait-Until ($tf + 20); Shot "3_pelea"
New-Item -ItemType File -Force (Join-Path $root "out-win\prof.go") | Out-Null   # HV_PROF_GO
Wait-Until ($tf + 20 + $FightSecs); Shot "4_pelea"
Stop-Process -Id $p.Id -Force
Write-Output "listo: pelea desde ~$([int]$tf) s"
