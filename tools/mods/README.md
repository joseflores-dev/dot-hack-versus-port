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
