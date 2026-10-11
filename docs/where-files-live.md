## Where PSCloud keeps your files

Everything PSCloud stores on the PS5 is in one folder, `/data/pscloud/`. (Up to 0.22 the
log, cloud sign-in, CA bundle and tool settings sat beside it as `/data/pscloud*`; the
dashboard moves them in automatically the first time it starts.) The dashboard's
**Storage** page shows each location with its size and file count, and **Free up space**.

| Location in `/data/pscloud/` | What is in it | Kept automatically |
|---|---|---|
| `spool/` | **Local backups.** `*.zip.ready` = waiting to upload, `*.zip.sent` = uploaded, each with a `.identity` file naming the profile, game and checksum. | All, unless "Keep only the newest backup of each game" is on. Backups not yet uploaded are never deleted. |
| `share/` | **Shared saves** exported or received. | The 2 newest per game. |
| `rollback/` | **Restore safety copies**: your save as it was before each restore or replace. Replaces store `restore-<ID>-<profile>-<id>/before-N.img` plus `info.txt` (game, profile, date, slots). | The 3 newest per game and profile. |
| `downloads/` | **Cloud downloads.** | All |
| `staging/`, `*-stage-*` | **Working files** used while a backup, check or replace runs. | Removed when the operation finishes. |
| `upload.conf`, `ca.pem`, `preferences.conf`, `google.conf`, other `*.conf` | **Settings** and cloud sign-in, readable only by PSCloud. | All |
| `pscloud.log` | **Activity log**, shown on the Activity page. | All |

Your PS5's own saves stay in `/user/home/<profile>/savedata_prospero/`; PSCloud only reads
them, except when you choose Restore or Replace.

**USB drives.** Plug in an exFAT USB drive (it appears as `/mnt/usb0` to `/mnt/usb7`).
PSCloud only writes inside the drive's `PSCloud` folder:

```
PSCloud/<Game name> (<ID>)/portable-<ID>-<snapshot>.zip
```

(0.20.0 to 0.22.0 used `PSCloud/Backups/…` and `PSCloud/Shared saves/…`; those files are
still listed.) A save whose contents are already on the drive is not copied again.

Every copy is written to a temporary file first, then read back and compared by
SHA-256 before it counts as copied. A shared save ZIP placed anywhere under `PSCloud/`
on the drive can be received from the Storage page.
