# .hack//Versus — port nativo para Android y Windows

*[English](README.md) · Español*

Port **nativo** del juego de lucha de PS3 .hack//Versus a **Android (ARM64)** y **Windows (x64)**. No es un emulador:
el código del juego se tradujo a C++ por recompilación estática con
[ps3recomp](https://github.com/sp00nznet/ps3recomp) y se compiló para cada plataforma; el chip gráfico de la PS3 se
reproduce sobre Vulkan.

> [!IMPORTANT]
> No se incluye ningún archivo del juego. Necesitas tu propia copia legal de **.hack//Versus** (PS3, BLJS93008) como
> imagen de disco (`.iso`). Proyecto de aficionado, sin relación con CyberConnect2, Bandai Namco ni Sony.

![Pelea: Sora vs Sora en Aura's Garden](docs/pelea.png)

## Contenido

- [Estado](#estado)
- [Descargar y jugar](#descargar-y-jugar)
- [Controles](#controles)
- [Opciones](#opciones-hv_envtxt)
- [Mods de personajes](#mods-de-personajes-experimental)
- [Problemas conocidos](#problemas-conocidos)
- [Compilar desde el código](#compilar-desde-el-código)
- [Créditos y licencia](#créditos-y-licencia)

## Estado

| | Android | Windows |
|---|---|---|
| Peleas | 30 fps estables (el tope del juego), con ahorro de batería | 30 fps estables |
| Menús, guardado, sonido | Sí | Sí |
| Mandos | Bluetooth (DualShock 4 y compatibles) + pad táctil en pantalla | Cualquier mando de SDL + teclado |
| Story Mode, online | Sin recorrer entero / sin online | Igual |

Probado en un Samsung Galaxy S22 Ultra (Snapdragon 8 Gen 1, Android 16) y en un PC con Windows 10 (Ryzen 7 5700X3D,
RTX 3050). Los informes de fallos y los pull requests son bienvenidos.

## Descargar y jugar

Descarga desde **[Releases](../../releases)**.

### Disco compatible

| Disco | Estado |
|---|---|
| BLJS93008 (Japón, el único lanzamiento) con el parche de traducción al inglés | Probado a fondo |
| BLJS93008 sin parchear | Debería funcionar (textos en japonés), sin probar |

El ejecutable del juego ya traducido viene con el port: no hay que descifrar nada ni usar firmware de PS3.

### Android

Hace falta un teléfono ARM64 con Vulkan 1.1, Android 12 o superior, 8 GB de RAM recomendados y unos 2 GB libres para
la instalación del juego en su HDD (la ISO se lee en su sitio, no se copia).

1. Instala `hackversus.apk` (permite "apps desconocidas" si te lo pide).
2. Copia tu `.iso` a cualquier carpeta del teléfono (Descargas, por ejemplo).
3. Abre la app, toca **Elegir ISO** y selecciona la imagen. El primer arranque tarda unos minutos (el juego instala
   ~2 GB en su HDD virtual y se compilan los shaders); después arranca en unos 20 segundos.

<details>
<summary>Otras formas de darle el juego, y dónde guarda los datos</summary>

- Deja la ISO en `/sdcard/PS3/` y toca **Dar permiso** para conceder acceso a todos los archivos (la misma carpeta que
  usa el emulador ARMSX3).
- O extrae el disco en un PC (`python tools/iso_extract.py <tu.iso> all disc`) y copia `disc/PS3_GAME` a
  `Android/data/com.hackversus.port/files/PS3/vfs/PS3_GAME`.

```
Android/data/com.hackversus.port/files/
  PS3/dev_hdd0/game/BLJS93008/           instalación del juego en el HDD
  PS3/dev_hdd0/home/00000001/savedata/   partidas guardadas
  PS3/hv_env.txt                         opciones
  PS3/hv_mods.txt                        mods de personajes
  log.txt                                registro de la sesión
```
Desinstalar la app borra esta carpeta, **partidas incluidas**: haz una copia antes.
</details>

### Windows

Hace falta Windows 10/11 de 64 bits, una CPU con AVX2 (Intel Haswell / AMD Zen o posterior) y una GPU con Vulkan 1.1.

1. Descomprime `hackversus-windows.zip` donde quieras.
2. Abre `hackversus.exe` y elige tu `.iso` (o arrastra la ISO sobre el exe). La recuerda para la próxima vez.

Windows monta la ISO en solo lectura mientras el juego corre. Partidas, cachés y `log.txt` van a `data/` junto al exe.

## Controles

| | |
|---|---|
| Mando | Distribución de PS3: ✕○□△, L1/R1/L2/R2, L3/R3, START/SELECT, cruceta y sticks. En Android, emparéjalo antes de abrir la app. |
| Pad táctil (Android) | Siempre activo como jugador 1; el botón **PAD** lo oculta. |
| Teclado (Windows) | Flechas = cruceta · Z/X/A/S = ✕/○/□/△ · Q/W = L1/R1 · 1/2 = L2/R2 · Enter = START · Tab = SELECT |
| Ventana (Windows) | F11 o Alt+Enter alternan pantalla completa; Esc sale de pantalla completa. |

Las listas de movimientos están en el juego: en una pelea, START → "Command List".

## Opciones (`hv_env.txt`)

Un archivo de texto con un `VARIABLE=valor` por línea, en `files/PS3/` en Android o junto a `hackversus.exe` en
Windows.

| Variable | Efecto |
|---|---|
| `PS3_OSK_TEXT=NOMBRE` | Nombre que se escribe solo cuando el Story Mode lo pide (máximo 8 caracteres). Por defecto `PLAYER`. |
| `HV_KEEP_ASPECT=1` | 16:9 con barras negras en vez de estirar la imagen. |
| `HV_INTRO=orig` | Intro completa (por defecto solo los logos de Bandai Namco y CRIWARE). `HV_INTRO=0,17` se salta también los logos. |
| `HV_PIPE_PREWARM=0` | No precrear los pipelines gráficos al arrancar (activado por defecto; evita tirones la primera vez que aparece un efecto). |
| `HV_JC_PAR=n` | Hilos para los trabajos SPU (3 por defecto). |
| `HV_AUDIO_LEGACY_PACE=1` | El mezclador de audio original de ps3recomp. |
| `HV_LOG=0` | No escribir `log.txt`. |
| `PS3RECOMP_VK_FULLSCREEN=0` | Windows: arrancar en ventana. |

## Mods de personajes (experimental)

El port puede cambiar el modelo de un personaje por otro **como un color extra** en la selección de personaje, sin
tocar los colores originales. Los mods se listan en `hv_mods.txt` (`files/PS3/` en Android, `data/` en Windows):

```
# subst <modelo> <índice de color> <posición en el carrusel> <offset en DATA.CPK, hex> <largo> <archivo>
subst 1kit01 2 3 BE07800 7393184 mods/mi_kite.bin
```

Esa línea agrega el **color 3** a Kite (posición 3 del carrusel) y, cuando se elige ese color, entrega
`mods/mi_kite.bin` en lugar de los datos del modelo original. El reemplazo tiene que medir lo mismo y tener el mismo
formato que la entrada original de `DATA.CPK`. El port no distribuye archivos de mods. Disponible desde el APK v0.4,
y en Windows compilando desde el código.

## Problemas conocidos

- El Story Mode pide un nombre con el teclado en pantalla; el port lo rellena (`PS3_OSK_TEXT`) y luego no se puede
  cambiar.
- Sin online, trofeos ni música personalizada.
- Tras sesiones largas el teléfono se calienta y baja su velocidad; es el teléfono, no el port.

**Próximamente:** Versus online entre PC y Android con un servidor de lobby de reemplazo (los servidores originales
ya no existen).

## Compilar desde el código

Todo vive en la carpeta del proyecto; no se instala nada en el sistema.

<details>
<summary>Pasos</summary>

1. **Herramientas** (no incluidas): Android NDK r30, CMake ≥ 3.28, Ninja, JDK 17, build-tools 35.0.1 y el
   `android.jar` de la API 35, Python 3.10 con `pip install -r ps3recomp-0.18.0/requirements.txt`, y SDL2 + glslang
   para `arm64-v8a` en `sysroot/`.
2. **Traducir el juego** una vez (`game/EBOOT.ELF` es el ejecutable descifrado, por ejemplo volcado por un emulador):
   ```
   P=.venv/Scripts/python; R=ps3recomp-0.18.0/tools
   $P $R/ppu_loader.py game/EBOOT.ELF -o analysis/
   $P $R/find_functions.py game/EBOOT.ELF --seed-json analysis/EBOOT.functions.json --output analysis/functions.json
   $P $R/extract_spu_images.py game/EBOOT.ELF --output spu_programs/
   $P $R/ppu_lifter.py game/EBOOT.ELF --functions analysis/functions.json --hle-stubs analysis/EBOOT.imports.json \
       --code-end 0x7bf7b0 --chunks 64 -o recomp/
   ```
   Los trabajos SPURS (skinning) se vuelcan en el teléfono con `SPU_DUMP_MISS=<carpeta>` a `spu_jobs/*.elf`.
3. **APK:** `tools/build_apk.sh` → `out/hackversus.apk`. Los scripts insertan los ganchos del port en el código
   generado (`tools/apply_recomp_hooks.py`) antes de compilar.
   Los releases usan una compilación guiada por perfil: `HV_PGO=gen` para un APK instrumentado, jugar una pelea,
   unir el perfil con `llvm-profdata merge -o pgo/fight.profdata hackversus.profraw` y compilar con
   `HV_PGO=use:<ruta>`.
4. **Windows:** con [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (ucrt, x86_64) en `toolchain/`, ejecuta una
   vez `tools/build_win_deps.sh` y luego `tools/build_win.sh` → `out-win/hackversus.exe`.

Ayudas para probar en `tools/`: `goto_title.sh`, `goto_fight.sh`, `measure_fight.sh` (teléfono por adb, el
dispositivo se detecta solo) y `win_fight.sh` (PC, llega a una pelea CPU contra CPU con el teclado).
</details>

<details>
<summary>Qué cambió respecto a ps3recomp 0.18.0</summary>

El toolkit está en `ps3recomp-0.18.0/` con los cambios de este port (licencia MIT). Lo principal: un backend Vulkan
para Android (render passes fusionados, lotes de dibujo, hilo de envío, present asíncrono, precreación de pipelines),
registros de shader escalarizados para Adreno, trabajos SPURS en hilos paralelos, colas de eventos SPU para el driver
de sonido, VMX y coma flotante escalar en NEON, lectura de vértices e índices con NEON, un `cellOskDialog` funcional,
guardado, pad táctil y mando, y un gancho de lectura de archivos que usan los mods de personajes. Para Windows: el
backend Vulkan compilado con mingw, SDL2 para mando y audio, un hilo de callbacks GCM y el teclado como respaldo. El
lector de ISO (`hackversus/isofs.c`) se engancha bajo la API de archivos del sistema. Los detalles están en los
comentarios del código.
</details>

## Créditos y licencia

- CyberConnect2 y Bandai Namco, creadores de .hack//Versus.
- [sp00nznet](https://github.com/sp00nznet/ps3recomp), por ps3recomp.
- El equipo del parche de traducción al inglés de .hack//Versus.
- SDL2, glslang, el Android NDK y llvm-mingw.

El código de este repositorio está bajo la [licencia MIT](LICENSE); los componentes de terceros mantienen sus propias
licencias. No otorga ningún derecho sobre el juego, que pertenece a CyberConnect2 y Bandai Namco. El repositorio no
contiene datos del juego; las compilaciones contienen el código del juego traducido por recompilación estática y
necesitan una copia legítima del disco para funcionar. Si los titulares de los derechos lo piden, se retira.
