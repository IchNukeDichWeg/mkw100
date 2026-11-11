# Position icons 13th–128th

`rank013.tpl` … `rank128.tpl` are 64×64 RGBA8 Wii TPL textures for the race HUD's position
counter past 12th (`runtime/src/hle/rank_icons.cpp`). The build copies this folder next to the
executable as `rank_icons/`; without it the HUD falls back to the overlay's vector digits.

- **13th–24th:** by **hammerbro101**, "Wii - Mario Kart Wii - Positions (24 Players)",
  <https://www.deviantart.com/hammerbro101/art/Wii---Mario-Kart-Wii---Positions-%2824-Players%29-1281677667>
  (fan art), converted to TPL unchanged.
- **25th–128th:** generated text icons in the same style (smoothed, premultiplied-alpha
  downscale), from the MKW100 Rank128 pack of 2026-11-11.
- **1st–12th** are the game's own textures and are not included.
