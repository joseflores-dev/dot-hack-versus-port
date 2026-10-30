# Arranca out-win\hackversus.exe, va del título a una pelea CPU VS CPU con el teclado (fallback de cellPad:
# Z = X, flechas = cruceta), deja correr la pelea y guarda capturas. Log en out-win\run.log / run.err.
# uso: powershell -File tools\win_fight.ps1 [-FightSecs 40] [-Env "AUDIO_RATE=1","HV_PUMP_THREAD=0"]
param([int]$FightSecs = 40, [string[]]$Env = @("AUDIO_RATE=1"), [double]$AfterLoad = 8, [switch]$Probe, [switch]$Attach, [int]$Right = 0, [int]$Scan = 0, [int]$Burst = 0, [switch]$CostumeProbe, [string]$Keys = "", [string]$PreKeys = "")
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
# El aviso de autoguardado ("✕ Skip", ~14 s si se espera): pantalla oscura con texto blanco en el recuadro del centro.
function NoticeText {
    $p.Refresh(); if ($p.MainWindowHandle -eq [IntPtr]::Zero) { return 0 }
    $r = New-Object K+R; [K]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $w = $r.Ri - $r.L; $h = $r.B - $r.T; if ($w -le 0) { return 0 }
    $bmp = New-Object System.Drawing.Bitmap $w, $h; $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $n = 0
    for ($i = 0; $i -lt 40; $i++) { for ($j = 0; $j -lt 12; $j++) {
        $c = $bmp.GetPixel([int]($w * (0.30 + 0.30 * $i / 40)), [int]($h * (0.38 + 0.12 * $j / 12)))
        if ($c.R -gt 190 -and $c.G -gt 190 -and $c.B -gt 190) { $n++ } } }
    $g.Dispose(); $bmp.Dispose(); return $n
}
# Pantallas reconocidas por píxeles (coordenadas relativas, calibradas con capturas reales):
#   título: el logo blanco ".hack//Versus" (franja 25-33 % del alto); la opción resaltada es una barra azul
#   (STORY 50 %, VERSUS 56,7 %, OPTION 63,5 % del alto, entre 44 y 60 % del ancho; resaltada ~(44,113,197), fondo cian);
#   selección de personaje: el texto blanco "CHARACTER SELECT" arriba a la izquierda;
#   pelea: las dos barras de vida naranjas arriba.
function Grab { $p.Refresh(); if ($p.MainWindowHandle -eq [IntPtr]::Zero) { return $null }; $r = New-Object K+R; [K]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $w = $r.Ri - $r.L; $h = $r.B - $r.T; if ($w -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap $w, $h; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $g.Dispose(); return $bmp }
function IsTitle { $b = Grab; if (-not $b) { return $false }; $n = 0
    for ($i = 0; $i -lt 50; $i++) { for ($j = 0; $j -lt 10; $j++) {
        $c = $b.GetPixel([int]($b.Width * (0.25 + 0.5 * $i / 50)), [int]($b.Height * (0.25 + 0.08 * $j / 10)))
        if ($c.R -gt 220 -and $c.G -gt 220 -and $c.B -gt 220) { $n++ } } }
    $b.Dispose(); return $n -ge 10 }
function MenuRow { $b = Grab; if (-not $b) { return -1 }; $best = -1; $bn = 2; $k = 0   # 0 STORY, 1 VERSUS, 2 OPTION
    foreach ($y in 0.507, 0.574, 0.642) { $n = 0
        for ($i = 0; $i -lt 40; $i++) { $c = $b.GetPixel([int]($b.Width * (0.44 + 0.16 * $i / 40)), [int]($b.Height * $y))
            if ($c.B -gt 150 -and $c.G -gt 80 -and $c.G -lt 150 -and $c.R -lt 90) { $n++ } }
        if ($n -gt $bn) { $bn = $n; $best = $k }; $k++ }
    $b.Dispose(); return $best }   # la fila con más azul de barra resaltada; el fondo cian (verde > 170) no cuenta
function IsCharSelect { $b = Grab; if (-not $b) { return $false }; $n = 0
    for ($x = 180; $x -lt 880; $x += 10) { $c = $b.GetPixel([int]($b.Width * $x / 1920), [int]($b.Height * 115 / 1080)); if ($c.R -gt 200 -and $c.G -gt 200 -and $c.B -gt 200) { $n++ } }
    $b.Dispose(); return $n -ge 12 }
function IsFight { $b = Grab; if (-not $b) { return $false }; $ok = 0
    foreach ($x in 400, 600, 1300, 1500) { $c = $b.GetPixel([int]($b.Width * $x / 1920), [int]($b.Height * 132 / 1080)); if ($c.R -gt 180 -and $c.B -lt 90 -and $c.G -gt 30) { $ok++ } }
    $b.Dispose(); return $ok -ge 3 }
function WaitFor([scriptblock]$cond, [double]$secs) { $t = Get-Date; while (((Get-Date) - $t).TotalSeconds -lt $secs) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 300 }; return $false }
# Hasta el título; el aviso de autoguardado se salta con ✕ (solo cuando está en pantalla).
$lastx = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $tl + 60) {
    if (IsTitle) { break }
    # ✕ cada 2 s mientras el aviso siga en pantalla (una sola, demasiado temprano, se pierde)
    if (((Get-Date) - $lastx).TotalSeconds -gt 2 -and (Brightness) -lt 40 -and (NoticeText) -ge 6) { Press x 0.3; $lastx = Get-Date }
    Start-Sleep -Milliseconds 300
}
Start-Sleep -Milliseconds 1200   # el menú ignora el pad durante su fundido
Shot "1_titulo"
# Abajo hasta que VERSUS MODE esté resaltado (nunca ✕ sobre STORY MODE: entra al hub y pide el nombre).
WaitFor { (MenuRow) -ge 0 } 6 | Out-Null   # el fundido del título: esperar a que haya una opción resaltada
for ($i = 0; $i -lt 8; $i++) { $row = MenuRow; if ($row -eq 1) { break }; if ($row -eq 2) { Press up 1.0 } else { Press down 1.0 } }
if ((MenuRow) -ne 1) { Shot "error_titulo"; Stop-Process -Id $p.Id -Force; Write-Output "no se pudo elegir VERSUS MODE"; exit 1 }
# VERSUS MODE -> Offline -> CPU VS CPU (pausas cortas; si la selección no aparece, se reintenta el último ✕)
Press x 2.5; Press x 3; Press down 0.8; Press down 0.8; Press x 1
if (-not (WaitFor { IsCharSelect } 12)) { Press x 1; WaitFor { IsCharSelect } 10 | Out-Null }
Start-Sleep -Milliseconds 1500; Shot "2_personajes"
# personaje P1, P2, Start Battle, escenario, Yes, carga
# -Scan N: recorre el carrusel de P1 (N pasos a la derecha, una captura por paso) y cierra el juego.
if ($Scan -gt 0) {
    for ($i = 1; $i -le $Scan; $i++) { Press right 1.2; Shot ("scan_{0:D2}" -f $i) }
    Stop-Process -Id $p.Id -Force; Write-Output "scan listo"; exit
}
# -Right N: N veces a la derecha en el carrusel antes de elegir al jugador 1; el 2 queda como está
for ($i = 0; $i -lt $Right; $i++) { Press right 0.6 }
function RunKeys([string]$list, [string]$tag) { $k = 0; foreach ($key in $list.Split(",")) { $key = $key.Trim(); $k++
    if ($key -eq "snap") { $req = Join-Path $root "out-win\snap.req"; New-Item -ItemType File -Force $req | Out-Null
        $ts = Get-Date; while ((Test-Path $req) -and ((Get-Date) - $ts).TotalSeconds -lt 60) { Start-Sleep -Milliseconds 200 }; continue }
    if ($key -eq "quit") { Stop-Process -Id $p.Id -Force; Write-Output "keys listo"; exit }
    Press $key 1.0; Shot ("{0}{1:D2}_{2}" -f $tag, $k, $key) } }
# -PreKeys: teclas en el carrusel antes de elegir al jugador 1 (mismas teclas especiales que -Keys)
if ($PreKeys) { RunKeys $PreKeys "pk" }
Shot "2b_p1"
Press x 2
# -Keys "down,down,x,right,x,up,up": teclas en el menú del jugador 1 (después de elegirlo), una captura por tecla.
#   Color 2 de un personaje: "down,down,x,right,x,up,up" (PC Colour Settings, siguiente, confirmar, volver a Start Battle)
#   "snap": pide un volcado de memoria (HV_MEMSNAP=out-win) y espera que termine; "quit": cierra el juego y sale.
if ($Keys) { $k = 0; foreach ($key in $Keys.Split(",")) { $key = $key.Trim(); $k++
    if ($key -eq "snap") { $req = Join-Path $root "out-win\snap.req"; New-Item -ItemType File -Force $req | Out-Null
        $ts = Get-Date; while ((Test-Path $req) -and ((Get-Date) - $ts).TotalSeconds -lt 60) { Start-Sleep -Milliseconds 200 }; continue }
    if ($key -eq "quit") { Stop-Process -Id $p.Id -Force; Write-Output "keys listo"; exit }
    Press $key 1.0; Shot ("k{0:D2}_{1}" -f $k, $key) } }
# -CostumeProbe: explora el menú "PC Costume Settings" con una captura por paso y cierra el juego.
if ($CostumeProbe) {
    Shot "c0_menu"; Press down 1; Shot "c1_down"; Press x 2; Shot "c2_x"; Press right 1.2; Shot "c3_right"
    Press right 1.2; Shot "c4_right"; Press x 2; Shot "c5_x"; Press down 1; Shot "c6_down"
    Stop-Process -Id $p.Id -Force; Write-Output "costume probe listo"; exit
}
Press x 2.5; Press x 2.5; Press x 3; Press x 2; Press x 2   # una ✕ de más: a veces el "Yes" se come una
# hasta que aparezcan las barras de vida (✕ cada 4 s por si alguna se perdió)
$tw = Get-Date
while (-not (WaitFor { IsFight } 4)) { if (((Get-Date) - $tw).TotalSeconds -gt 60) { break }; Press x 0.2 }
$tf = ((Get-Date) - $t0).TotalSeconds
Start-Sleep -Seconds 2; Shot "3_pelea"
for ($i = 1; $i -le $Burst; $i++) { Start-Sleep -Milliseconds 700; Shot ("burst_{0:D2}" -f $i) }   # -Burst N: N capturas seguidas
New-Item -ItemType File -Force (Join-Path $root "out-win\prof.go") | Out-Null   # HV_PROF_GO
Wait-Until ($tf + $FightSecs); Shot "4_pelea"
Stop-Process -Id $p.Id -Force
Write-Output "listo: pelea desde ~$([int]$tf) s"
