# Creating a receiver save (research, not implemented)

Goal: let a profile that has never played a game receive a shared save, so the
receiver no longer has to play once first. Nothing in this document is
implemented. All findings below were made read-only on the 11.40 console on
2026-10-10; copies of system files stayed on the developer PC and are not in Git.

## What a PS5 save consists of

For user `<user>` and game `<title>`, one save slot `<slot>` is made of:

1. `/user/home/<user>/savedata_prospero/<title>/sdimg_<slot>`: the encrypted PFS
   container. Bytes 0x800-0x85f hold the sealed key that `/dev/pfsmgr` unseals
   (already used by `common/mount.c`). The container includes `sce_sys/param.sfo`
   with title, slot, account ID and user ID (user ID at offset 0x660 per public
   garlic-savemgr release notes).
2. A row in the per-user registry
   `/system_data/savedata_prospero/<user>/db/user/savedata.db` (SQLite, one row per
   slot; 113 rows for 114 slot files on the test profile). Columns:
   `title_id, dir_name, main_title, sub_title, detail, tmp_dir_name, is_broken,
   user_param, blocks, free_blocks, size_kib, mtime, fake_broken, account_id,
   user_id, faked_owner, cloud_icon_url, cloud_revision, game_title_id,
   system_blocks, convert_performance, account_id_in_usb_path`. The text and size
   columns mirror the container's param.sfo and allocation (example: 3 MiB Crash
   slot = 48 blocks, size_kib 3072).
3. `/user/home/<user>/savedata_prospero_meta/user/<title>/<slot>_icon0.png`: the
   menu icon.

`/system_data/savedata/<user>/db/user/savedata.db` is the separate PS4/legacy
registry (CUSA/SLUS rows only) and is not involved.

Consequence for current sharing: restoring into an existing slot keeps the
receiver's registry row, so the system menu may show the receiver's old slot
title until the game saves again. Games load the container contents.

## Available system functions

The pinned SDK (pacbrew v0.40.2) ships stubs for `libSceFsInternalForVsh` that
include `sceFsInitCreatePprPfsSaveDataOpt`, `sceFsCreatePprPfsSaveDataImage`
(PS5 format), `sceFsInitCreatePfsSaveDataOpt`, `sceFsCreatePfsSaveDataImage`
(PS4 format), `sceFsTransactionSaveDataReadParam/WriteParam` and
`sceFsUfsAllocateSaveData`, and for `libSceRegMgr` (`sceRegMgrGetBin`, used by
garlic-savemgr to read the account ID). No `libSceSaveData` stubs are shipped.
Argument layouts of the create functions are not documented; garlic-savemgr
(no license published, so none of its code is used here) reports creating PS5
images this way.

## Proposed design (each step needs its own hardware gate)

1. Create the container: `sceFsCreatePprPfsSaveDataImage` into a staging path,
   size taken from the package's recorded source capacity.
2. Generate a sealed key for this console (pfsmgr) and mount the new container
   with the existing mount path.
3. Write the package payload plus a `sce_sys/param.sfo` built for the receiver:
   title, slot and texts from the package's source metadata, account and user ID
   from this console (registry / profile), never the sender's.
4. Unmount, verify, then move the container into `savedata_prospero/<title>/`.
5. Register it: either insert one row into the per-user registry while no game is
   running (back up the database first, verify with a read-back, keep a rollback
   copy), or rely on Safe Mode "Rebuild Database" if testing shows it rescans
   container metadata. Which one is needed is unknown.

## Unknowns and risks

- Argument layouts and return codes of the create functions on 7.00 and 11.40.
- Whether the system or the game accepts a container whose registry row was added
  externally, and whether the shell caches the database while running.
- A malformed registry row could hide or break other saves of that profile.

## Safe test plan

- Use the empty profile `1eb70485` on the 11.40 console (no PS5 saves) and a small
  game, so no real save can be affected.
- Back up the full `savedata.db` and the profile's `savedata_prospero` folder first.
- Stage 1: create and mount an empty container in `/data/pscloud`, unmount, delete.
  No system folders touched.
- Stage 2: only after stage 1 works on both firmwares, place one container for the
  test profile and register it; confirm the game sees and loads it; restore the
  database backup afterwards if anything looks wrong.
