# Modern controller prompts

Options > Controls > Overview > Button prompts selects Xbox One (default) or
PlayStation. `[input] button_prompts = "xbox_one" | "playstation"` persists it.
The option changes presentation, not controls. Game prompts follow the first
configured gamepad binding; an unbound action displays a dash. Options-menu
hints follow that menu's fixed positional controls.

The R2VE01 shared BTN sheet contains all 32 standard Remote, Nunchuk, Classic
and GameCube button sprites. Its source is `texture.arc`, NARC entry BTN at
file offset `0x6daad0`, size 262400. TEX0 at +128 is 256x256 GX_RGBA8, one level,
with 262144 bytes at +192; XXH64(seed 0) is `7d9cbe2a23379258`.
`comdat.arc` BTN SPRX at `0x50720` defines 32 40-byte frames starting at +0x54.
Their UV cells are encoded in `button_prompt_atlas.h`; positions and UVs stay
unchanged. The original images are never distributed or modified.

The GX worker registers a source-hash replacement through Aurora. Style and
binding changes replace that registration and invalidate redundant bindings.
Storage remains alive until unregistering the old registration. Texture
replacement files need not be enabled. Only the exact BTN hash matches.

Remote A is both Confirm and Lock-on; Remote B is both Cancel and Fire in
`virtual_remote.cpp`. Those two shared icons show both configured bindings
when different. Classic prompts have distinct menu and combat icons. This
avoids silently mislabelling Remote prompts with just one of two valid inputs.

`tools/gen_button_prompts.py` generates original glyph artwork with Pillow and
the existing Apache-2.0 Roboto font. Face symbols use vector shapes. Generated
pixels are embedded, so releases need no extra texture pack or game assets.

Coverage is the shared BTN prompts and recomp menu hints. Text or controller
illustrations baked into other textures, videos and the Wii HOME library are
separate assets and are not replaced by this sheet. Full-game visual coverage
still requires a playthrough; do not infer it from the atlas tests.

Verification (2026-11-21): native `WiiCompiled.exe` rebuilt successfully;
`button_prompt_tests` and `controls_settings_tests` passed. The former checks
the source hash against locally extracted BTN bytes, actual Classic A/B and
shoulder mappings, PlayStation versus Xbox pixels, rebinding/unbound actions,
GX RGBA8 tiling and transparent unused cells. The latter checks preference
persistence and tolerant fallback. Both preferences booted to game menus in
isolated data directories. Captures under `build/prompt-tests/*-live.png` do
not show a button prompt and therefore are not visual coverage proof. The
decoded replacement atlases were separately rendered and inspected there.
