# SAVEDATA — virtual memory stick and PARAM.SFO

`runtime/savedata/save_store.*` implements the `ms0:/PSP/SAVEDATA/` layout
used by `sceUtilitySavedata`. It is deliberately small: no KIRK encryption,
no secure-save hashes, no save-picker UI. KIRK save encryption remains
deferred and must never ship key material.

## Layout

```
<ms0 root>/PSP/SAVEDATA/<gameName><saveName>/
    PARAM.SFO
    <fileName>        game data file
    ICON0.PNG         optional
    ICON1.PNG         optional
    PIC1.PNG          optional
    SND0.AT3          optional
```

`saveName == "<>"` means the directory is only `<gameName>`. Components are
validated when read or written: no empty name, `.`/`..`, path separators,
control bytes or names longer than 63 bytes. Symlinks inside a save are
rejected rather than followed. Files are written through a sibling `.tmp`
file and renamed, so a crash does not leave a half-written save.

## PARAM.SFO

The SFO reader/writer is the vendored MIT `third_party/yakumo` implementation
(`mhp3rd::savedata::ParamSfo`). The store builds the same key set a PSP save
uses:

| Key | Format | Max length |
|---|---|---|
| `TITLE` | UTF-8 | 128 |
| `SAVEDATA_TITLE` | UTF-8 | 128 |
| `SAVEDATA_DETAIL` | UTF-8 | 1024 |
| `PARENTAL_LEVEL` | integer | 4 |
| `CATEGORY` | UTF-8 (`MS`) | 4 |
| `SAVEDATA_DIRECTORY` | UTF-8 | 64 |
| `SAVEDATA_FILE_LIST` | binary, 99 x 32 bytes | 3168 |
| `SAVEDATA_PARAMS` | binary, zeroed | 128 |

Every file list entry is `filename[13] + hash[16] + pad[3]`. The hash and
`SAVEDATA_PARAMS` are written as zeroes because encryption is deferred; a
save written by this runtime is unencrypted and must be labelled as such.
`parse -> serialize` is byte-identical for exported PSP saves; the
`tests.test_savedata` fixture case enforces it.

## HLE modes

`sceUtilitySavedataInitStart` accepts the 0x600-byte parameter layout and
validates `size`, mode, game/save/file names and the parameter range.

| Mode | Behaviour |
|---|---|
| 2 `LOAD` | Reads `PARAM.SFO`, restores title/savedata-title/detail/parental, sets `bind=1021`, copies the data file into `dataBuf` and sets `dataSize`; copies icon buffers when the guest supplies them |
| 3 `SAVE` | Writes the data file (`dataSize`, clamped to `dataBufSize`), supplied icons/picture/sound and a fresh `PARAM.SFO` |
| 4 `LISTLOAD` | Headless save picker: candidates are the existing save directories named by the zero-terminated 20-byte `saveNameList` (`"<>"` expands to every save of the game; an absent/empty list falls back to `gameName + saveName`). `focus` selects FIRSTLIST/LASTLIST/LATEST/OLDEST/FIRSTDATA/LASTDATA (unknown and FIRSTLIST select index 0; LATEST/OLDEST compare the `PARAM.SFO` mtime). An empty list completes with `0x80110307`; otherwise the selected name is written back and the LOAD path runs |
| 8 `SIZES` | Existing T6k path: msFree/msData/utilityData from the real save directory |
| 11 `LIST` | Fills the guest `idList` (`maxCount`, `resultCount`, `entries`) with save directory names matching `gameName + saveName`; `*` and `?` wildcards are honoured, entries are sorted, and the name drops the game prefix |
| others | Named counted gap (`savedata mode N is not implemented yet`), result `0x80110300` |

Load errors: `0x80110306` broken SFO, `0x80110307` missing save directory,
`0x80110308` bad parameter/buffer, `0x80110309` missing data file. Save
failures return `0x80110383` (no space) or `0x80110388` (bad parameter).
`LIST` returns `0x80110328` when `idList` or its entries are missing or
invalid; an empty match is a successful request with `resultCount = 0`.
Nothing is silently created or zero-filled on a failed load.

## PPSSPP interchange

The layout matches PPSSPP's memstick directory (`PSP/SAVEDATA/<dir>/`), so a
save directory can be copied between the emulator's memstick and this
runtime. The user's exported saves live under gitignored `savestates/` and
are never committed; tests copy them into a scratch root first.

TC-12.5 mounts the exported SYSTEM/STORY saves, lists them, parses each
`PARAM.SFO`, rewrites every file and compares bytes. TC-12.6 was run on
PPSSPP 1.20.4 with a **clean boot** (not the supplied `.ppst`, which resumes
before a battle and has no save menu): PPSSPP listed `ULUS10592GUN9THSYS` and
`ULUS10592GUN9THSTORY00`, loaded the 61,888-byte STORY file and both icons,
then did the same after the tree was rewritten by our store. Every rewritten
file has the same SHA-256 as the export. Evidence:
`docs/qa/2026-10-31-t12b-savedata.md`. The `.ppst` container itself is not
parsed here; a reimplementation would need explicit sign-off.

## Named gaps

- KIRK/secure save encryption and `SAVEDATA_PARAMS` hashes are deferred.
  Unencrypted saves may not load on real PSP hardware or PPSSPP in secure
  mode; they do load through this HLE.
- Delete/auto/secure modes, `FILES`/`GETSIZE`, `SAVEDATA_FILE_LIST`
  updates beyond one data file and the save-picker UI are not implemented.
- There is no save-picker UI. `LISTLOAD` auto-cancels an empty list with
  `0x80110307` and auto-selects the `focus` entry when saves exist; a real
  picker would need the missing renderer path.
- The save-picker modes `LISTSAVE` (5), `LISTDELETE` (6) and
  `LISTALLDELETE` (7) are not implemented. The title executes LISTLOAD on
  the new-game path with a 32-slot `saveNameList`.
- `LIST` entry timestamps come from the save's `PARAM.SFO` mtime (the
  directory when the SFO is missing) through the host filesystem clock.
- On device the memory-stick root is supplied by the future Android wiring;
  the desktop runner takes `--ms`. Only host/PPSSPP interchange is verified
  today.
- The PPSSPP oracle is a manual clean-boot run; there is no CI PPSSPP run.

## Tests

```sh
python3 -m unittest tests.test_savedata -v
python3 -m unittest tests.test_hle -v
```

`tests.test_savedata` builds `savedata_tests` and `hle_kernel_tests`, runs
the synthetic store/SFO contracts and the exported-save fixture case (named
skip `savedata_fixture_missing` when absent), then runs the HLE save/load
round-trip.
