# ttf23dmm - any TTF/OTF to 3D Movie Maker "3D Words"

Turns a TrueType/OpenType font into a `.vxp` expansion for v3DMM that adds a new 3D font to 3D Movie Maker.

    pip install -r requirements.txt
    python3 ttf23dmm.py MyFont.ttf -o MyFont.vxp [options]

Same 224 slots as 3dmm's fonts (character codes 32..255).  Codes become characters through a code page
(`--charset`, default `cp1252` like 3dmm's fonts)
3D text only reads one byte per character, so there's no room for more.
Other alphabets work by choosing a code page (`--charset cp1251` puts Cyrillic in the upper slots; you type them with the matching keyboard layout/code page).

## Options
| option | default | |
|---|---|---|
| `--name` | font's name | internal font name (and preview thumbnail text) |
| `--shading flat\|smooth` | flat | flat = every face hard, like default 3dmm fonts; smooth = curved edges shaded smooth (like blender's "auto smooth") |
| `--crease` | 40 | smooth only: edges sharper than this many degrees stay hard (caps always do) |
| `--bevel` | 0 | bevel size in 3DMM units (clamped per glyph so thin strokes do not collapse) |
| `--bevel-steps` | 1 | 1 = chamfer, 2+ = rounded bevel |
| `--cap-height` | 5.4 | capital height in 3DMM units (stock: 5.4) |
| `--depth` | 2.5 | extrusion depth (stock: 2.5) |
| `--detail` | 0.03 | curve flattening tolerance; smaller = rounder and heavier |
| `--spacing` | 0 | extra advance for every character |
| `--charset` | cp1252 | any Python codec |
| `--axis wght=700` | | for variable fonts: pick an instance |
| `--face-index` | 0 | for font collections (.ttc) |
| `--author` | | written into the vxp's .cfg |
