# .hack//Versus — port nativo para Android

Port **nativo** (sin emulador) del juego de lucha **.hack//Versus** (PlayStation 3, BLJS93008, con parche al inglés) a
Android ARM64, hecho por recompilación estática con [ps3recomp](https://github.com/sp00nznet/ps3recomp): el código PPU
del juego se traduce a C++ y se compila para el teléfono, los programas SPU se traducen igual y la GPU RSX se
emula sobre Vulkan. Probado en un Samsung Galaxy S22 Ultra (Snapdragon 8 Gen 1, Adreno 730, Android 16).

![Pelea: Sora vs Sora en Aura's Garden](docs/pelea.png)

## Estado

| Parte | Estado |
|---|---|
| Arranque, intro, menús, selección de personaje | Funciona |
| Peleas (Versus, CPU vs CPU, Práctica) | Funciona, ~25–27 fps con ahorro de batería activado (el juego va a 30) |
| Menús y pantalla "Stereoscopic" | 30 fps (tope del juego) |
| Story Mode (hub ALTIMIT OS "VS THE WORLD") | Entra; el nombre del personaje lo rellena el port (ver opciones) |
| Sonido (música CRI, voces y golpes del driver "nus" en SPU) | Funciona |
| Guardado | Funciona (`/sdcard/PS3/dev_hdd0/home/00000001/savedata`) |
| Mando Bluetooth (DualShock 4 y compatibles) | Funciona, mapeo completo incluidos L3/R3 |
| Pad táctil en pantalla | Funciona (botón **PAD** para ocultarlo) |
| Online, trofeos reales, música del usuario | No (stubs) |

![Título](docs/titulo.png)

## Requisitos

- Teléfono ARM64 con Vulkan 1.1 y Android 12 o superior. Con menos de 8 GB de RAM no se ha probado.
- **Tu propia copia del juego.** Este repositorio y el APK no contienen datos del juego: ni la ISO, ni el ejecutable,
  ni texturas, modelos o sonido. Hay que extraerlos del disco original (ver abajo).

## Instalación

1. Descarga `hackversus.apk` de la sección **Releases** e instálalo.
2. Dale a la app el permiso "Acceso a todos los archivos" (Ajustes → Aplicaciones → .hack//Versus → Permisos), o por adb:
   ```
   adb shell appops set com.hackversus.port MANAGE_EXTERNAL_STORAGE allow
   ```
3. Prepara los datos del juego en `/sdcard/PS3/`:
   - `EBOOT.ELF`: el ejecutable **descifrado** del juego. La forma más sencilla es dejar que un emulador lo vuelque:
     en ARMSX3/RPCS3 activa `Core: PPU Debug` y arranca el juego una vez; el ELF queda en
     `cache/ppu_progs/BLJS93008-*/exec.elf`.
   - `vfs/PS3_GAME/`: el contenido del disco. En el PC: `python tools/iso_extract.py <tu.iso> all disc` y copia
     `disc/PS3_GAME` a `/sdcard/PS3/vfs/PS3_GAME` (unos 4 GB; `adb push` sirve).
4. Abre la app. La primera vez instala sus datos y compila los shaders: tarda más; las siguientes arranca en ~20 s.

Estructura final en el teléfono:
```
/sdcard/PS3/
  EBOOT.ELF
  vfs/PS3_GAME/USRDIR/...      (DATA.CPK, PARAM.SFO, etc.)
  dev_hdd0/home/00000001/savedata/   (partidas, las crea el juego)
  hv_env.txt                    (opcional, ver abajo)
```
El registro de la app está en `/sdcard/Android/data/com.hackversus.port/files/log.txt`.

## Controles

- **Mando Bluetooth**: empareja un DualShock 4 (o compatible) con Android antes de abrir la app; el port lo asigna al
  puerto 1 con el mapeo de la PS3 (✕○□△, L1/R1/L2/R2, L3/R3, START/SELECT, cruceta y sticks).
- **Pad táctil**: siempre activo como jugador 1; el botón **PAD** (arriba al centro) lo oculta o lo muestra.
- Los controles del juego están en el propio juego: en una pelea, START → "Basic Operation", "Command List" y
  "Battle Key Config".

## Opciones (`/sdcard/PS3/hv_env.txt`)

Una `VARIABLE=valor` por línea. Las útiles:

| Variable | Efecto |
|---|---|
| `PS3_OSK_TEXT=NOMBRE` | Nombre que el port escribe cuando el juego pide texto (Story Mode: máximo 8 caracteres, el juego no deja cambiarlo después). Por defecto `PLAYER`. |
| `HV_KEEP_ASPECT=1` | 16:9 con barras en vez de estirar a toda la pantalla. |
| `HV_INTRO=orig` | Intro completa (por defecto se muestran solo los logos de Bandai Namco y CRIWARE). |
| `HV_AUTOSKIP=1` | Pulsa ✕ solo hasta pasar la intro (solo tiene sentido sin partida guardada). |
| `HV_JC_PAR=n` | Hilos para los jobs SPU (3 por defecto). |
| `HV_AUDIO_LEGACY_PACE=1` | Mezclador de audio original de ps3recomp (por si el nuevo suena peor en tu equipo). |

## Rendimiento

Con el ahorro de batería activado (núcleos a 1,2–1,8 GHz) las peleas van a 25–27 fps y los menús a 30. Sin ahorro
de batería el teléfono sube de frecuencia y el juego llega al tope de 30 con margen. El cuello de botella real es la CPU
(hilo principal del juego, ~170 jobs SPU de skinning por cuadro y el procesamiento de los comandos RSX), no la GPU, que
va al ~45 %.

## Cómo se construye

Todo el entorno es portátil y vive en la carpeta del proyecto; nada se instala en el sistema.

1. **Toolchain** (no incluido): Android NDK r30, CMake ≥ 3.28, Ninja, JDK 17, build-tools 35.0.1 y `android.jar`
   de API 35, Python 3.10 con `pip install -r ps3recomp-0.18.0/requirements.txt`, y en `sysroot/` SDL2 y glslang
   compilados para `arm64-v8a`.
2. **Lift del juego** (una vez, en el PC; `game/EBOOT.ELF` es tu ELF descifrado):
   ```
   P=.venv/Scripts/python; R=ps3recomp-0.18.0/tools
   $P $R/ppu_loader.py game/EBOOT.ELF -o analysis/
   $P $R/find_functions.py game/EBOOT.ELF --seed-json analysis/EBOOT.functions.json --output analysis/functions.json
   $P $R/extract_spu_images.py game/EBOOT.ELF --output spu_programs/
   $P $R/ppu_lifter.py game/EBOOT.ELF --functions analysis/functions.json --hle-stubs analysis/EBOOT.imports.json \
       --code-end 0x7bf7b0 --chunks 64 -o recomp/
   ```
   Los jobs SPURS (skinning) no están en el ELF: se vuelcan en el teléfono con `SPU_DUMP_MISS=<dir>` la primera vez
   que el juego los usa y van a `spu_jobs/*.elf`.
3. **APK**: `tools/build_apk.sh` → `out/hackversus.apk` (configura CMake con el NDK, compila el runtime, los 64 archivos
   PPU traducidos y los SPU, enlaza `libmain.so`, compila la actividad Java y firma con una clave de depuración).
   El código PPU completo recompila en unos 3 minutos con 16 núcleos.

Los scripts de `tools/` (`goto_fight.sh`, `measure_fight.sh`, `prof_threads_fast.py`, …) sirven para llegar a una
pelea sin tocar el teléfono, medir fps y perfilar; llevan escrito el identificador adb del equipo con que se desarrolló,
cámbialo por el tuyo.

## Qué cambia respecto a ps3recomp 0.18.0

El toolkit va incluido en `ps3recomp-0.18.0/` con las modificaciones de este port (licencia MIT, ver su `LICENSE`).
Entre otras: backend Vulkan para Android (render passes fusionados, lotes de dibujos, present asíncrono, profundidad
leída como color en compute), escalarización de los bancos de registros de los shaders para Adreno, jobs SPURS en
paralelo con hilos, colas de eventos SPU del driver de sonido, VMX y FP escalar en NEON/sin comprobaciones de NaN,
fetch de vértices e índices en NEON, `cellOskDialog` funcional, guardado, pad táctil y mando. Los detalles técnicos
están en los comentarios del código.

## Aviso legal

.hack//Versus es propiedad de CyberConnect2 y Bandai Namco. Este repositorio no contiene datos del juego (ni la ISO,
ni el ejecutable, ni texturas, modelos o sonido) y el APK no funciona sin una copia legítima del disco. El APK sí
contiene el código del juego traducido a ARM64 por la recompilación estática. Es un trabajo de aficionado sin relación
con los titulares de los derechos; si ellos lo piden, se retira.
