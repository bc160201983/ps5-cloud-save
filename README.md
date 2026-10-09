# PS5 Cloud Save — development milestone 0.7.0

## Version 0.7.0 — PC transfers, queue management and experimental whole-game restore

- Verified cloud ZIP downloads to PC or PS5; local queue downloads to PC.
- Import original, unmodified PSCloud whole-game ZIPs from PC into the private
  PS5 queue (16 MiB limit). Game/user/format, CRC and manifest hashes are checked;
  arbitrary, compressed, repacked or mismatched ZIPs are rejected.
- Queue count/list with individual upload, upload-all, PC download and restore.
- Experimental whole-game restore from cloud or imported local queue backups:
  requires explicit game-closed/overwrite confirmation, matching existing
  per-slot sealed keys and image sizes, checked filesystem preflight for both
  incoming images, unchanged destination checks and full encrypted rollback.
- Ordinary partial commit failures roll back replaced images. Interrupted
  restores retain a persistent marker and rollback files, blocking subsequent
  save operations. Keep the game closed after a failed/interrupted restore;
  do not restart it until recovery is inspected. Multi-image restore is not
  atomic across power loss; automatic interruption recovery is not implemented.
- All 76 host tests and PS5 builds passed, including PC import/restore, wrong-key
  rejection and partial-commit rollback fixtures. Live firmware 11.40 testing
  verified PC ZIP download/import, local queue download, queue counts, individual
  upload and upload-all. These checks did not restore or modify console saves.
  Real console overwrite/game-load validation has NOT been performed for
  whole-game restore; that remains experimental until a controlled hardware test.

### 0.6.3 cloud-aware unchanged backups

Backing up an unchanged save queues its verified existing archive for a cloud
presence check. If both its identity commit and archive are still present, no
ZIP is uploaded. If either was deleted, the same local archive is re-uploaded
without creating a new version. Authentication/network/server errors retain the
retry job and report pending upload rather than pretending the cloud copy exists.
Only the selected matching snapshot is reconsidered; deleted historical versions
are not all resurrected. This applies to whole-game and per-slot dashboard backups.
All 72 host tests and PS5 builds passed. On firmware 11.40 with Koofr, an empty
whole-game cloud folder was repopulated from the existing verified local archive.
A repeated unchanged backup kept one cloud version. Cloud ZIP CRC/SHA-256 checks
passed and both original console images matched before and after testing.

### 0.6.2 uploaded-archive deduplication

Queue validation selects an existing `.ready` or `.sent` file by directory
inventory before opening it. This avoids the console's missing-target descriptor
behavior when checking an uploaded archive. Whole-game backup fails closed on
an archive verification error rather than publishing a duplicate. Firmware 11.40
console retesting verified a matching uploaded archive with zero hash errors and
skipped repeated unchanged game backups. The cloud ZIP's SHA-256, CRC and both
included images were checked; original console images matched before and after.
All 70 host tests and PS5 builds passed.

### 0.6.1 validation follow-up

Whole-game console export and Koofr ZIP upload are verified: both encrypted
images and the manifest pass cloud checksum and ZIP CRC checks. Snapshot dates
are visible in the dashboard. The first repeat test found duplicate archives;
additional deduplication diagnostics identified the uploaded-file lookup issue.
Version 0.6.2 fixes it and passes the console repeat check. Earlier test-created
duplicate snapshots are retained rather than deleted.

## Version 0.6.0

- Whole-game backup for Crash Bandicoot 4 on firmware 11.40: one ZIP holds both
  encrypted progress and profile images, plus an identity/hash manifest. Neither
  original is mounted or modified. Missing slots or changing sources abort.
- Whole-game snapshots use `User-<id>/WholeGame/`; older per-slot backups remain
  selectable and unchanged. The ZIP is same-console encrypted data, not a portable
  decrypted save. Whole-game restore/import is deliberately disabled pending
  recovery validation; download these archives through your cloud provider.
- Snapshot dates include local date, time and timezone, sorted newest first.
  Legacy snapshots without dates display “Date unavailable”. Dates reflect the
  console clock, not proof of when game progress changed.
- Koofr WebDAV connection and dashboard uploads are console-confirmed. Per-slot
  console-managed export and unchanged-save skipping are also console-tested.
  Whole-game ZIP creation/upload, CRC/SHA-256 integrity and dated dashboard cards
  are now console-tested, as is unchanged uploaded-snapshot skipping in 0.6.2.

This is the first tested component of a proposed standalone PS5 cloud-save app.
It is NOT an installable PS5 application or a complete save-sync implementation.
PS5 ELF binaries are available as GitHub Actions artifacts. Firmware 11.40 staged
save export, direct Nextcloud upload and verified cloud download have been tested
on the console. Automatic game-close detection and startup are still pending.

## Version 0.4.0

- New identity-tagged backups use game/user/save-slot WebDAV folders.
- Checksum-based duplicate skipping is scoped to the exact user/title/save slot.
- A cloud identity commit records SHA-256 after the archive upload succeeds.
- A PS5-hosted Nextcloud dashboard is being implemented in the next release.
- Requested development changes are published to `main` with VERSION kept current.

```text
PS5Backups/
  Crash Bandicoot 4 - PPSA02433/
    User-1eb70483/
      PlayerSaveSlot0Save/
      PlayerSaveProfileSaveData/
```

Existing flat backups are preserved. They have not been automatically classified
or moved. Folder routing and duplicate skipping require the new identity format.
These commits are an initial protocol, not signed or provider-enforced immutable
manifests. The folder/deduplication host tests and PS5 builds have passed;
their live console checks remain pending.

## Version 0.5.0

- Responsive embedded dashboard on PS5 port 8082, with pairing-code protection.
- Nextcloud app-password connection check and private on-console settings.
- Game cards, separate user/save-slot selection, committed cloud version listing,
  verified download, backup/upload from one button, and recent activity.
- Experimental console-managed same-console restore: mount a staged image, apply
  the verified payload, check unmount, preserve an encrypted rollback, and replace
  only the selected original image if it has not changed. Hardware validation is
  required before relying on this new restore path.
- Identity metadata is under each slot's hidden `.pscloud` directory. The main
  folder contains the ZIP versions; timestamps come from the console clock.

See [ps5/DASHBOARD.md](ps5/DASHBOARD.md). Backup/restore is initially supported only
for the tested Crash Bandicoot 4 (`PPSA02433`) profile and slot 0 saves. Other games
are displayed read-only until validated. The dashboard's local HTTP interface is
for a trusted private LAN; cloud connections use verified HTTPS. Pairing does not
encrypt local HTTP traffic. No automatic startup or game-close detection yet.

## Version 0.5.1

- Added a local design preview for use while the console is offline:
  `python tools/preview_dashboard.py 8085`, then open
  `http://127.0.0.1:8085/#token=preview`.
- Preview uses sample versions, discards submitted settings, and performs no
  console/cloud operations. It is not evidence of live hardware success.
- Fixed Windows UTF-8 reading in the preview and limited failed restore cleanup
  to temporary files created by the current operation.
- Version 0.5.0 passed all 62 host tests and all PS5 cross-builds. Visual browser
  inspection confirmed the library, save-slot controls and version actions.
- The dashboard and new encrypted-image restore require console/game-load tests;
  these were deferred because the console is off. Core staged export and direct
  WebDAV transport were previously tested successfully on firmware 11.40.

## Version 0.5.2

- Live 11.40 testing confirmed dashboard startup, pairing and settings reads.
- Fixed save discovery filling its 64-entry limit before the supported Crash
  saves: supported saves are listed first, the limit is now 256, and truncation
  is reported explicitly.
- Added safe WebDAV transport/status diagnostics and a dashboard User-Agent.
- Added pairing-protected `/api/stop` for graceful dashboard upgrades.
- Nextcloud connection through the dashboard, folder/deduplication checks and
  the new managed restore remain under hardware testing. Saves were not modified
  during the initial dashboard checks.

## Version 0.5.3

- Cleaner, equal-height game cards: internal replay/ghost/profile filenames no
  longer expand the library. Save categories appear inside supported game details.
- Real English game names and icons are read from installed `/user/appmeta`
  metadata. No remote artwork service or save mounting is needed for these cards.
- Removed manual pairing. Opening the page establishes an automatic HttpOnly,
  SameSite=Strict browser session; mutations retain cross-site request checks.
  Anyone on the trusted LAN who opens the dashboard can use it. Keep it private
  and do not forward the dashboard port to the internet.
- Unsupported game buttons stay disabled after another operation completes.
- Nextcloud dashboard verification and managed restore remain under testing.

## Added in 0.3

- Controlled recovery source for the verified PPSA02433 single-file save format:
  HTTPS download with a pinned SHA-256 and restore after destination backup.
  See [ps5/RESTORE_TEST.md](ps5/RESTORE_TEST.md). Game-load validation is pending.

- Next transport work: optional `pscloud-upload.elf` target for direct PS5
  HTTPS/WebDAV uploads, local configuration and queue watching. Requires separate
  PS5 curl/OpenSSL libraries; console upload/download have passed on 11.40.
  See [ps5/UPLOAD_TEST.md](ps5/UPLOAD_TEST.md). Google Drive is still pending.

- PS5 notifications for start, progress, completion and failure in our diagnostic
  and new mounted-save export payload; timestamped logs in /data/pscloud.log.
- Firmware 11.40 manual export of an already-mounted save to an uncompressed ZIP.
- Root sce_sys exclusion, archive CRCs, unique backup names and failed-copy cleanup.
- The GitHub build produces separate pscloud-probe and pscloud-export artifacts.
- See [ps5/EXPORT_TEST.md](ps5/EXPORT_TEST.md) for the next console test.
- Firmware 11.40 diagnostic enumeration has been confirmed on the user's console.
  Manual and console-managed staged exports have passed. Firmware 7.00 is deferred.

## Added in 0.2

- PS5 diagnostic source and SDK Makefile targeting elfldr, plus a build workflow.
- Firmware reporting and read-only save-directory enumeration; a local report is written.
- Windows/Linux Python ELF sender for port 9021.
- Five new host tests (11 total). See ps5/TESTING.md for console steps.
- The initial PS5 diagnostic cross-build succeeded; directory enumeration was tested on firmware 11.40.

## Implemented

Native C upload worker using libcurl, with direct HTTPS PUT to an existing WebDAV
folder (for example Nextcloud). Watches a persistent queue, retries failed uploads
with capped backoff, checks server certificates, prevents concurrent workers using
an advisory lock, refuses symlink jobs, and retains local archives after success.
No PC relay is part of the intended PS5 deployment architecture.

## Not implemented yet

Automatic game-exit detection; signed cloud manifests; Google Drive OAuth and API;
cross-console import; conflict selection; rest-mode
operation; automatic startup. Host transport tests use a local mock server;
direct console-to-Nextcloud upload and download hash checks have also passed.
Google Drive does not expose this WebDAV API and cannot be used by
setting its URL in this worker.

## Build and test on Linux / Windows WSL

Install a C compiler, make, libcurl development headers (libcurl >= 7.85), Python 3,
and OpenSSL development headers/CLI. On Ubuntu these are build-essential libcurl4-openssl-dev libssl-dev python3
openssl. Then:

```sh
make
make test
```

Tests start a local HTTPS mock server with a temporary CA certificate. They verify
successful byte transfer, no repeat after acknowledgement, retry after process
restart, authentication failure retention, untrusted certificate rejection,
symlink/incomplete job handling, and rejection of plaintext HTTP.

## Try the upload component with an existing WebDAV folder

Use a dedicated cloud app password. Set these environment variables locally;
do not put credentials into the source or send them in chat:

```sh
export PSCLOUD_URL='https://YOUR-HOST/remote.php/dav/files/YOUR-USER/PS5Backups'
export PSCLOUD_USER='YOUR-USER'
read -r -s -p 'App password: ' PSCLOUD_PASSWORD
export PSCLOUD_PASSWORD
mkdir -m 700 spool
./cloud-worker spool --watch
```

For a single pass use --once. Exit codes: 0 successful/empty pass; 1 retained
failed jobs; 2 configuration/startup error. The remote folder must already exist.
Redirects are not followed. For a private CA, set PSCLOUD_CA_BUNDLE to its PEM path;
certificate checking must remain enabled.

To test with a disposable archive in another terminal:

```sh
cp example.zip spool/console-user-game-UNIQUE-ID.zip.part
mv spool/console-user-game-UNIQUE-ID.zip.part spool/console-user-game-UNIQUE-ID.zip.ready
```

Use a fresh globally unique ID for every backup across ALL consoles. The final
object becomes console-user-game-UNIQUE-ID.zip. After a successful 2xx response,
the local file becomes .zip.sent and is retained. Do not reuse names or modify
ready/sent archives. A crash after remote acceptance but before local commit
can re-upload the same bytes to the same object; this is intentional. Remote
versioning may record that repeat.

## Production limitations

The upload worker trusts its private spool producer. It does not yet create or
validate ZIPs, provide end-to-end encryption, verify uploaded hashes by download,
or enforce cloud-side immutable objects. A partial PUT may be visible on some
providers until retry. A production restore client MUST only list committed,
verified backups through the future manifest/commit protocol. Local .sent files
are not automatically pruned, so disk usage must be monitored during development.
A 2xx response means server acceptance, not verified restoreability.

## Next milestone

Work on firmware 11.40 first. Validate the mounted-save exporter and then a
restore to a disposable same-console save before introducing automatic detection. Then add Google Drive authentication,
upload/list/download, restore verification, and cross-console imports. Do not copy
sce_sys metadata blindly into the destination save. Never restore during gameplay.

See docs.md for the intended integration contract and validation gates.
