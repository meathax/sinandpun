# Wii first-run setup

Open `WiiCompiled.exe` and select a North American R2VE01 disc image. Then select
your own raw 16-byte Wii common key file. Dropping an image onto the setup screen
also opens the key picker. The app reads the key during extraction and clears
the temporary key buffers after initializing decryption. It stores the imported
game and stage modules in its data folder. An already prepared game plays
without the key.

RVZ and GCZ use an installed DolphinTool executable selected in the setup
screen or specified by `--dolphin-tool` or `SIN_DOLPHIN_TOOL`. Conversion
temporarily needs space for a full ISO. The game deletes its conversion scratch
directory on completion, failure or normal cancellation.

For windowless setup, use `--prepare --game <image> --data <folder> --keys <file>`.
For RecompLauncher, set `SIN_WII_KEY_FILE` before starting the launcher.
RecompLauncher's current schema has no Wii shared-key input. A verified
extracted folder can be prepared without a key. Launcher picker support for
the compressed formats is separate from the game's importer.

Never commit, embed, package, upload or log Wii disc keys. Packages also exclude
DolphinTool, which players install independently. Use synthetic keys for import
fixtures. Keep older artifacts that embedded keys out of release staging.
The packaging allowlist and archive audit cover loose key files. Before release,
check the rebuilt binary and ZIP against former embedded key signatures without
copying or printing the signatures.

The manifest's verified dump entry is the unmodified USA ISO, 4,699,979,776 bytes,
CRC32 `7df9d581`. Compressed, scrubbed and extracted copies are different
representations. The [Redump catalog mirror](https://data.spludlow.co.uk/redump/redump/Nintendo%20-%20Wii/Sin%20%26%20Punishment%20-%20Star%20Successor%20%28USA%29)
and [GameTDB](https://www.gametdb.com/Wii/R2VE01) also agree on SHA-1
`76de816f77798136c870bd4a57d00eccc61d6cc3`.

Source builds include the pinned [Recomp Achievements SDK](https://github.com/elripalda/RecompAchievements)
and expose bounded Wii MEM1/MEM2 reads at guest retrace. A launcher-hosted
Sin ruleset and verified save-load hooks are still required for game-specific
unlocks and ranked play. Connected sessions are marked casual until that
save tracking is verified.
