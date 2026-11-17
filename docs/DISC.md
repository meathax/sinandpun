# R2VE01 disc facts (measured 2026-11-17)

The input image is `orig/Sin & Punishment - Star Successor (USA).nkit.iso`.
- SHA-256: `f23d58b1d1199f0dfea0aa03b5501c80ddcf1b87288b7b16825b09d1659d7d9b`
- nodtool reports it as NKit lossless: unencrypted, no hashes.
- Game ID R2VE01, Disc 1, Revision 0. Internal title "Sin and Punishment 2".

It is extracted to `orig/R2VE01/` with `tools/bin/nodtool.exe extract` (880 MB). That folder is not tracked.

## Code

| File | SHA-256 | Notes |
|---|---|---|
| sys/main.dol | 2c750625d647608d9f3f48d75bb45c2f357de44689a167c8074f5ace1054da60 | entry 0x80006310 (`__start`); .text 0x8001F240 size 0x2B5EB8; .bss to 0x804AF6F0 |
| files/gs2.sel | 75e33b651ec7b569b0dfcce13ed9694c1c23f3e3dced1a5a70e7864434381e1e | Static symbol export list (`AppWiiRelRso.elf`); about 1.8k main.dol names, e.g. `Ng::Cam::setMtxPrjOrth` |
| files/stg00.rso | 009041ef511a086ba8374be56f473c96f055c97b1cf66320e3c5d1d408bc2b76 | 6708 exports |
| files/stg01.rso | 465eae042d9472b00be9d1e436d668efed80dedbe2c46f5a09f6e498bf0549c2 | 3492 exports (path `C:\dev3\gs2\src\stg01\stg01.plf`) |
| files/stg02.rso | 2dba55df854900cf46b94c00220d7e0b03e1adeda9a6fad3772e6cc47150e0cd | 3343 exports |
| files/stg03.rso | b8fcb7660b566e1451a9b02d652e3d7763655e2446846d2c2cd58c2e08290dfe | 3020 exports |
| files/stg04.rso | 03ce28c4ede4981e4f5af8b1ebf9be208f27339249af7456c7b40421075a6975 | 2508 exports |
| files/stg05.rso | b8ce258de11add6dd1674e75bd7dedeef2aa9c6931a37182e5b92f6a502c5d27 | 4249 exports |
| files/stg06.rso | 98efe15912c59d7af2bbc4f39eb075ca9abf7a6ddfc9fc14b6eb6694ae2601be | 6040 exports |
| files/stg07.rso | d22f7c63c9dae7d4f9865e191a2a8d8507161a0ecca043363af47f80e5b51f3f | 3424 exports |

Recompute with
`sha256sum orig/R2VE01/sys/main.dol orig/R2VE01/files/*.rso`.

## Implications

- **Symbols.** The game is built on Treasure's in-house `Ng` engine (C++, CodeWarrior-mangled
  names). The SEL file plus the RSO exports give real names for the camera, projection, actor
  and enemy classes. That covers the code that interpolation, HUD filtering and aiming hooks
  need.
- **Stage code is in RSO modules, not RELs.** The WiiCompiled translator handles one DOL plus one
  fixed-address REL; it has no RSO parser. Each stg0N.rso must be relocated to a stable guest
  base and translated separately, and the game's RSO link calls (RSOLinkList and related) must
  place modules at those bases or be replaced by an HLE. DolRecomp's REL-folder mode
  (stable virtual addresses) is the design reference. dtk can read RSOs (`dtk rso info`).
- **Data.** Assets are U8 `.arc` archives, NW4R `.brfnt`/`.brtex` and `sfx2.brsar`, with streamed
  audio in `files/stream/`.
