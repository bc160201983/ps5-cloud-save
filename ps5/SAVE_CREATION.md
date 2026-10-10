# Receiver saves: findings and decision

Question: can a profile that has never played a game receive a shared save?

Decision: no. The receiver must start the game once so the game itself creates
its save; sharing then fills that save. PSCloud does not create save containers.
Creating them would require handling the console's save encryption outside the
system's own save process, which this project does not do.

All findings below were made read-only on the 11.40 console on 2026-10-10.
Copies of system files used for the investigation stayed on the developer PC and
are not in Git.

## How a PS5 save is laid out

For user `<user>` and game `<title>`, each save slot `<slot>` consists of:

1. `/user/home/<user>/savedata_prospero/<title>/sdimg_<slot>`: the encrypted
   container. Its `sce_sys/param.sfo` holds title, slot texts, account ID and
   user ID. Sharing only ever writes game files inside an existing container and
   keeps the receiver's `sce_sys` untouched.
2. One row in the per-user PS5 save registry
   `/system_data/savedata_prospero/<user>/db/user/savedata.db` (SQLite, table
   `savedata`; 113 rows for 114 slot files on the test profile). Columns:
   `title_id, dir_name, main_title, sub_title, detail, tmp_dir_name, is_broken,
   user_param, blocks, free_blocks, size_kib, mtime, fake_broken, account_id,
   user_id, faked_owner, cloud_icon_url, cloud_revision, game_title_id,
   system_blocks, convert_performance, account_id_in_usb_path`. The text and size
   columns mirror the container's metadata (example: a 3 MiB Crash slot is
   48 blocks, `size_kib` 3072).
3. `/user/home/<user>/savedata_prospero_meta/user/<title>/<slot>_icon0.png`: the
   slot icon shown in the system menu.

Other locations seen:

- `/system_data/savedata/<user>/db/user/savedata.db`: the separate PS4/legacy
  registry (CUSA/SLUS/app rows only), matching `/user/home/<user>/savedata`.
- `/user/home/<user>/savedata_prospero_for_cloud`: used by the system's own cloud
  feature (only a small settings file on the test profile).

## Save keys

Every container is encrypted with a key that is sealed to the console
that owns it. PSCloud only uses the system's own unsealing (see
`ps5/common/mount.c`) to open an existing container on the same console, works
on a staged copy, and never exports, imports or converts keys. Portable packages
contain only game files; the receiver's container, sealed key and `sce_sys`
metadata stay its own. This is why sharing works across consoles and accounts,
and also why a receiver without its own save cannot be supplied one.

## Effects on sharing

- A shared restore replaces only container contents. The receiver's registry row
  is kept, so the system menu can show the receiver's previous slot title until
  the game saves again. The game loads the new data (confirmed with Wolverine,
  11.40 to 7.00).
- If the game has created only some of the package's slots, use "Skip save slots
  this profile does not have"; the remaining slots can be imported after the game
  has created them.

## Possible improvement (not implemented)

Guide the receiver through the game's own first save: detect that the profile has
no save for the package's game, ask the user to start the game until it saves
once and close it, then continue the import and check automatically.
