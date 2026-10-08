# FW 11.40 controlled cloud recovery test

The first recovery path is deliberately limited to PPSA02433 archives containing
exactly one uncompressed `ue4savegame.dpx.sav` file, maximum 16 MiB. It verifies
ZIP structure, CRC32 and an independently supplied SHA-256. It rejects other
games, compressed ZIPs, multiple entries, metadata, traversal and symlink paths.
This is a same-console test, not cross-console compatibility or automatic sync.

## Download only (does not change a mounted save)

1. Download `pscloud-recovery` from a successful GitHub Actions run.
2. Copy `pscloud-download.conf.example` to `pscloud-download.conf` and upload to
   `/data/pscloud-download.conf`. The example selects the cloud archive already
   verified against the original. Keep the existing `/data/pscloud-upload.conf`
   and `/data/pscloud-ca.pem`; download reuses their WebDAV credentials and TLS CA.
3. Send `pscloud-download.elf` to port 9021. Confirm the SHA-256 verified event.
   The archive is saved to `/data/pscloud/downloads/`. Failed downloads are not
   published. Stale `.part` files after a crash require manual inspection/removal.
4. Read the loader output and `/data/pscloud.log`. A transmission result alone
   does not establish successful execution.

## Controlled restore (writes exactly one destination payload file)

1. Keep an independent original-save backup. Close the game and mount a disposable
   same-console PPSA02433 destination save through a working Save Mounter session.
   Confirm title, user and slot in the mounter; the payload cannot verify those
   identities or detect game closure. Keep the mounter connected throughout.
2. Confirm the mount contains the expected `ue4savegame.dpx.sav` and `sce_sys`.
   Copy `pscloud-restore.conf.example` to `pscloud-restore.conf`. Set TARGET to
   that actual `/mnt/pfs/...` mount, and both confirmations to `yes`. Upload to
   `/data/pscloud-restore.conf`. Do not use the encrypted `/user/home` save image.
3. Send `pscloud-restore.elf` only after this destination is identified. It
   revalidates the downloaded archive, exports the destination into a unique
   `/data/pscloud/rollback/pre-restore-*.zip`, and flushes it before replacement.
   If backup creation fails, it does not replace the destination. The rollback
   ZIP excludes `sce_sys`; destination-specific metadata is preserved in place.
4. The replacement is staged, flushed and renamed over `ue4savegame.dpx.sav`.
   It leaves other files and `sce_sys` intact. Atomic rename/directory fsync
   support on the mounted filesystem and game compatibility need console testing.
   If a durability failure is reported after rename, do not retry blindly; keep
   the rollback and inspect the log. This is not a transactional save-system API.
5. After success, unmount in Save Mounter, then start the game and confirm the
   expected progress loads. Only that game-load test verifies restoreability.
   Download and preserve the rollback archive before further experiments.

For a different snapshot, compute SHA-256 from your trusted independent original
and update BACKUP and SHA256 in both configs. CRC32 alone is not authentication.
This first path has no remote manifest discovery, automatic rollback, destination
metadata remapping, game-exit mounting or cross-console restore.
