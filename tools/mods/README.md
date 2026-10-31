# Character mods / Mods de personajes

*English below · [Español](#español)*

Tools to turn an edited character model into a patch (`.hvpatch`) that the port applies as an extra colour of that
character. A patch only holds what you changed; the port builds the full model from each player's own disc.
Needs Python 3.

## Steps

1. **Find the model** (`<code>chrpsn.xfbin` in `data/chr/`; Kite is `1kit01`):
   ```
   python tools/cpk_list.py <DATA.CPK> chrpsn
   ```
   `DATA.CPK` is in the disc (`PS3_GAME/USRDIR/DATA/HDD/`) or in the game's HDD install.
2. **Extract it unpacked:**
   ```
   python tools/mods/extract_entry.py <DATA.CPK> data/chr/1kit01chrpsn.xfbin kite.xfbin
   ```
3. **Edit it** with your XFBIN/NUD tools (meshes are NDP3, textures NTP3). Two rules:
   - the edited file must be **exactly the same size** as the extracted one (pad with zeros at the end, or shrink
     textures you do not need to make room);
   - packed again, it must fit the original entry. If it does not, `log.txt` says `does not fit`.
4. **Make the patch** (carousel slot and colour index are optional; with them it prints the line for you):
   ```
   python tools/mods/make_patch.py <DATA.CPK> data/chr/1kit01chrpsn.xfbin kite_edited.xfbin my_kite.hvpatch 3 2
   ```
   Carousel slots: Sora 0, Sakuya 1, Haseo 2, Kite 3, BlackRose 4, Tsukasa 5, Ovan 6, Tokio 7. Colour index 2 is the
   third colour.
5. **Install:** put `my_kite.hvpatch` in `mods/` and the printed line in `hv_mods.txt`, both in
   `Android/data/com.hackversus.port/files/PS3/` (Android) or `data/` next to `hackversus.exe` (Windows):
   ```
   subst 1kit01 2 3 BE07800 7393184 mods/my_kite.hvpatch
   ```
   On its next start the port builds `mods/my_kite.hvpatch.built` (delete it after changing the patch). With
   `HV_CHRLOG=1` in `hv_env.txt` the log shows what the mods do.

The new colour has no tint of its own: the model's textures are used as they are.


## Models from .hack//G.U. (PS2)

`gu/` reads the character models of the PS2 G.U. games (CCS files) and exports them as OBJ + PNG in their rest pose:
```
python tools/mods/gu/export_obj.py <file.ccs> <out_folder>
```
The CCS files are in the disc's `DATA/DATA.CVM` (a CRI ROFS image: a 0x1800-byte header followed by a plain ISO9660
image), each one gzip-compressed. Haseo's 3rd form in Vol. 3 Redemption is `/SPC/EV/1HSW03.CCS` (cutscene model).
`gu/ccs.py` is the reader (format notes from NCDyson's StudioCCS, checked against the Redemption files). Bones use
row vectors, degrees, rotation order Z·Y·X, in cm; vertices are s16 · scale/256 / 16 in bone units; Z is up.

To turn such a model into a Versus colour, it goes onto the Versus fighter's skeleton bone by bone (both games use
the same 3ds Max Biped names), keeping G.U.'s proportions and its own weights; triangles are faced by the CCS's
stored normals and made double-sided (G.U. draws both sides, Versus culls the back). The released Haseo 3rd Form
mod was made that way.


## Extra weapons (PC Costume Settings)

The weapon selector can list more weapons than the title has. A "carrier" weapon of the title stays in the list and
the extra ones are steps of the cursor right after it; while one is chosen, the carrier's file is served from its
patch. Lines in `hv_mods.txt`:
```
weaponicons <offset> <64 bytes hex> <carrier's name>
weapon <carrier> <offset hex> <length> mods/<file>.hvpatch <name>
```
`weapon`: the carrier's number in the title's list (`weapon_04`, Empty Skies, is 4; twin blades are 0-11), the CPK
entry of its file (`data/weapon/wdg00.xfbin`) and the patch; one line per extra weapon, in the order they are shown.
`weaponicons` (optional) lets each extra weapon show its own picture and name: the port finds the carrier's picture
in the weapon pictures atlas (`csel_wpn_icon`, 1024 wide, DXT5) by those 64 bytes at `<offset>` from its start, and
the name by its text; `<file>.icon` next to each patch is its 256x128 picture as DXT5 blocks. Without these lines
nothing changes. The released twin blades mod (16 weapons from .hack//G.U. Vol. 3) shows Haseo, Kite, Sora and
Sakuya the same list.

## Español

Herramientas para convertir un modelo de personaje editado en un parche (`.hvpatch`) que el port agrega como un color
extra de ese personaje. El parche solo guarda lo que cambiaste; el port arma el modelo completo a partir del disco
de cada jugador. Hace falta Python 3.

1. **Busca el modelo** (`<código>chrpsn.xfbin` en `data/chr/`; Kite es `1kit01`):
   `python tools/cpk_list.py <DATA.CPK> chrpsn`. El `DATA.CPK` está en el disco (`PS3_GAME/USRDIR/DATA/HDD/`) o en
   la instalación del juego en el HDD.
2. **Sácalo descomprimido:**
   `python tools/mods/extract_entry.py <DATA.CPK> data/chr/1kit01chrpsn.xfbin kite.xfbin`
3. **Edítalo** con tus herramientas de XFBIN/NUD (mallas NDP3, texturas NTP3). Dos reglas:
   - el archivo editado tiene que medir **exactamente lo mismo** que el extraído (rellena con ceros al final, o achica
     texturas que no necesites para hacer espacio);
   - comprimido otra vez, tiene que caber en la entrada original. Si no cabe, `log.txt` dice `does not fit`.
4. **Haz el parche** (posición en el carrusel e índice de color son opcionales; con ellos imprime la línea):
   `python tools/mods/make_patch.py <DATA.CPK> data/chr/1kit01chrpsn.xfbin kite_editado.xfbin mi_kite.hvpatch 3 2`.
   Posiciones: Sora 0, Sakuya 1, Haseo 2, Kite 3, BlackRose 4, Tsukasa 5, Ovan 6, Tokio 7. El índice de color 2 es el
   tercer color.
5. **Instálalo:** `mi_kite.hvpatch` en `mods/` y la línea impresa en `hv_mods.txt`, ambos en
   `Android/data/com.hackversus.port/files/PS3/` (Android) o en `data/` junto a `hackversus.exe` (Windows).
   En el siguiente arranque el port arma `mods/mi_kite.hvpatch.built` (bórralo si cambias el parche). Con
   `HV_CHRLOG=1` en `hv_env.txt` el log muestra lo que hacen los mods.

El color nuevo no tiene tinte propio: se usan las texturas del modelo tal cual.

### Modelos de .hack//G.U. (PS2)

`gu/` lee los modelos de personajes de los G.U. de PS2 (archivos CCS) y los exporta a OBJ + PNG en su pose de reposo:
`python tools/mods/gu/export_obj.py <archivo.ccs> <carpeta_salida>`.
Los CCS están en `DATA/DATA.CVM` del disco (imagen ROFS de CRI: cabecera de 0x1800 bytes y luego un ISO9660 normal),
cada uno comprimido con gzip. Haseo 3ª forma en Vol. 3 Redemption es `/SPC/EV/1HSW03.CCS` (modelo de escenas).
`gu/ccs.py` es el lector (formato según StudioCCS de NCDyson, comprobado con los archivos de Redemption). Huesos:
vector fila, grados, orden Z·Y·X, en cm; vértices = s16 · escala/256 / 16 en unidades de hueso; Z hacia arriba.

Para hacerlo un color de Versus, el modelo se pasa al esqueleto del luchador hueso por hueso (los dos juegos usan los
mismos nombres del Biped de 3ds Max), conservando las proporciones y los pesos de G.U.; los triángulos se orientan
con las normales guardadas en el CCS y van con sus dos caras (G.U. dibuja ambas, Versus descarta la de atrás). Así
se hizo el mod publicado de Haseo 3ª forma.

### Armas extra (PC Costume Settings)

El selector de armas puede mostrar más armas de las que trae el juego. Un arma del juego hace de "soporte": sigue en
la lista y las extra son pasos del cursor justo después de ella; mientras una está elegida, el archivo del soporte se
sirve desde su parche. Líneas de `hv_mods.txt`: `weaponicons <offset> <64 bytes hex> <nombre del soporte>` y
`weapon <soporte> <offset hex> <largo> mods/<archivo>.hvpatch <nombre>` (una por arma extra, en el orden en que se
muestran; el soporte es su número en la lista del juego: `weapon_04`, Empty Skies, es 4; las dagas dobles son 0-11).
Con `weaponicons` cada arma extra muestra su imagen (`<archivo>.icon`, 256x128 en bloques DXT5) y su nombre. Sin estas
líneas no cambia nada. El mod de dagas publicado (16 armas de .hack//G.U. Vol. 3) da la misma lista a Haseo, Kite,
Sora y Sakuya.
