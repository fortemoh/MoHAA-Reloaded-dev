# The Competitive HUD's built-in data

`cg_hudstyle 1` draws Project: Omaha's Competitive HUD from cgame
(`code/cgame/cg_hud_*.cpp`). It needs no files of its own at run time: its
text faces and pictures are paths compiled into cgame, in
`code/cgame/cg_hud_data.h`, which `hudgen.py` makes from the files here.
The header is committed, so the build needs neither the script nor these
files; they are kept to make it again.

| Path | What it is |
|---|---|
| `fonts/Oswald-Medium.ttf`, `fonts/Oswald-SemiBold.ttf` | [Oswald](https://github.com/googlefonts/OswaldFont), SIL Open Font License 1.1 (`fonts/OFL.txt`). Only the outlines of the Windows-1252 characters (0x20 to 0xFF, the game's text) and the faces' metrics go into the header, no kerning (the Omaha HUD had none). |
| `silhouettes/*.png` | The weapon, grenade and kill-feed pictures of Project: Omaha's HUD (white with alpha), used with its author's permission. Traced into polygons: the outline of alpha 0.5 between the texel centers (marching squares), simplified by 0.35 texel at most. |
| `hudgen.py` | Writes `../cg_hud_data.h`. |

Make the header again after changing a font or a picture:

```bash
python3 code/cgame/hud/hudgen.py
```

It needs Python 3 with Pillow and numpy. cgame rasterizes the paths with
`stb_truetype` (`code/thirdparty/stb/stb_truetype.h`) into its atlas at the
exact pixel size each is drawn at; the shapes (circles, rings, the team
icons' layers, the radar's marks, the damage indicator's crescents) are made
in `cg_hud_atlas.cpp`.

The font's license goes with the release packages, since cgame carries the
font: `misc/release/licenses/oswald/OFL.txt`.
