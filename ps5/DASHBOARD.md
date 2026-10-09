# PSCloud dashboard

## Current behavior — 0.11.1

Dashboard HTML and authenticated `/api/health` remain available while a slow
operation runs. Save/cloud operations are still serialized: competing API calls
return a busy response immediately. Up to eight bounded client threads are used;
idle browser sockets no longer block all requests. Cloud lists have a 60-second
browser cache; Refresh bypasses it. WebDAV upload completion requires SHA-256
readback of the remote archive before committing the queue entry.

Cloud Settings includes a Google device-sign-in developer preview. This is NOT
an active Google transfer provider yet: a registered TV/device OAuth client and
real authorization tests are needed. WebDAV stays active and its configuration
is untouched. See [Google integration status](GOOGLE_DRIVE.md).

Choose any discovered PS5/PPSA game and user, select Whole game, close the game
and confirm, then Back up now. All active save containers are included, not only
Slot 0/profile. Up to 128 slots and a 512 MiB ZIP are supported; larger or unsafe
inventories fail without publishing a partial snapshot. PS4/CUSA is not supported.
V2 ZIPs store sorted encrypted containers and a per-file checksum manifest.
Legacy Crash two-slot V1 archives remain readable and unchanged backups deduplicate.

Cloud settings provide WebDAV connection testing and persistent PS5 preferences.
Automatic upload after a manual backup can be disabled: new backups then stay
queued until Upload / Upload all. PC imports stay queued regardless of that switch.
Activity has search, filters, recent-log download and optional visible-page refresh.
Game-close detection and cross-console sharing remain unavailable.

Generic whole-game restore is experimental and same-console only. Every existing
destination slot must match the archived inventory, encryption keys, sizes and
verified SFO game/slot/account identity. Originals are preserved in rollback.
Crash UE4 recovery can migrate payloads into recreated registered containers;
other games with changed keys are rejected. Deleted database entries are not
created, extra slots are not deleted, and no account remapping is performed.
Never interrupt power or launch a game during restore. Retain rollback files.

Older milestone notes below describe the narrower behavior at those releases.

## PC transfers and upload queue (0.7.0)

The library shows pending backup count and a list of queued snapshots. Each
validated item has individual upload and PC download controls; the main button
uploads all pending items. Counts refresh after import/upload. Large lists are
bounded while the total count is retained.

Open Crash Bandicoot 4, choose Whole game, select an original PSCloud ZIP from
your PC, and click Add to queue. Imported ZIPs are validated and kept locally;
they do not upload or restore automatically. ZIPs must be unmodified, stored
PSCloud whole-game exports under the current 512 MiB limit for the selected game and PS5 user.
Do not extract/repack the archive. Per-slot ZIPs remain downloadable but PC
import initially supports only identity-bearing whole-game ZIPs.

Cloud snapshots offer Download to PC, Download to PS5 and Restore. Imported
queue backups offer Restore to PS5 without requiring a cloud upload first.
Restore requires explicit closure/overwrite confirmation and matching current
encrypted keys and image sizes. Both incoming filesystems are checked before
replacement. Full original rollback images are retained in a unique directory
under `/data/pscloud/rollback/whole-restore-<id>/`.

Whole-game restore is experimental: host fixtures test success, rejection and
partial-failure rollback, but actual PS5 overwrite/game-load validation remains
pending. Never interrupt power. If an operation fails or a `.restore-active`
marker survives interruption, keep the game closed and preserve rollback files
for inspection. Automatic interrupted-transaction recovery is not implemented.

Download the `pscloud-dashboard` artifact from a successful build on `main`.
Deploy its CA bundle to `/data/pscloud-ca.pem` if not already present. Send
`pscloud-dashboard.elf` to the PS5 ELF loader, then open `http://PS5-IP:8082` from
a phone or browser. Opening the page establishes a browser session automatically;
no pairing code is required. Session cookies are HttpOnly and SameSite=Strict,
and state-changing requests require a same-origin request marker. Anyone who
can access this private-LAN dashboard can open a session. Do not expose it publicly.

Use Cloud settings to enter the existing PS5Backups WebDAV URL, username and an
app password. The server verifies the connection before saving settings with
owner-only permissions. The password is not returned to the browser or logged.
Credentials travel over local HTTP between the browser and console: use only a
trusted private LAN, with no port forwarding. Cloud transport verifies HTTPS.

## Backup

Choose a game/user card and a save slot, close the game, tick the confirmation,
then use Back up now. The PS5 stages/mounts/exports/unmounts and uploads its queue.
It skips an unchanged archive if a matching intact ready/sent local snapshot is
present. Changed progress creates a separate version. If cloud transport fails,
the ready archive remains queued; Upload queued backups retries it later.

Cloud layout:
`PS5Backups/Crash Bandicoot 4 - PPSA02433/User-USER_ID/SAVE_NAME/`

Each archive is uploaded first. A checksum/identity record in `.pscloud` is
uploaded last. Only identity-committed versions appear in the dashboard. These
records are not signed or a substitute for provider-side immutability; downloaded
bytes are checked against the recorded SHA-256. Old flat files are not deleted or
automatically assigned to a slot. Listing currently caps results at 48 versions.

## Download and experimental restore

Download to PS5 verifies the commit's user/title/slot, archive structure, CRC and
SHA-256. Restore additionally requires game-closed and replacement confirmations.
It supports only the tested PPSA02433 single-file save format. It copies the
existing encrypted destination image to staging and to an encrypted rollback,
mounts the staged copy, replaces its payload while preserving sce_sys, checks
unmount, then checks the original again and atomically replaces that selected
image. It does not recreate deleted saves, alter save databases, remap accounts,
delete other files, or support cross-console imports.

The new staged-image write-back route requires a separate hardware/game-load
test. Keep independent originals. After a failure, inspect Activity before
retrying; a failed mount/unmount retains the active marker and staged image.
Encrypted rollback copies remain in `/data/pscloud/rollback`. Directory fsync
failures after replacement are reported explicitly and require inspection.

The dashboard handles one operation at a time. It does not automatically start
after reboot or detect game closure. Avoid running other save mounters while
using it. Startup still requires an ELF loader; normal dashboard operations run
on the PS5 and can be triggered from a phone without a PC.
# Whole-game ZIP snapshots (0.6.0)

For the validated Crash Bandicoot 4 game/user with both progress and profile
saves, “Whole game” is the default backup selection. One stored ZIP contains
`sdimg_PlayerSaveSlot0Save`, `sdimg_PlayerSaveProfileSaveData` and `manifest.txt`
with hashes and identity. The encrypted images retain their console-specific
metadata. No original save is mounted, changed or deleted. Both must remain
unchanged throughout copying; a missing/unsafe image aborts the entire snapshot.
Unchanged whole-game ZIPs are skipped. Cloud storage uses a `WholeGame` folder
under the existing game/user folder. Existing per-slot snapshots remain usable.

These encrypted whole-game archives can now be downloaded and imported through
PSCloud 0.7.0. Whole-game restore has its own guarded path; the single-slot
restorer never treats a whole-game ZIP as a single payload. Do not replace
console files manually or delete your original saves as a test.

Cloud version cards show the console-recorded creation date in the browser's
local timezone, newest first. Undated older commits show “Date unavailable”.
