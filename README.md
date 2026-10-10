# PS5 Cloud Save — development milestone 0.12.4

## Version 0.12.4 — responsive busy page and all-games export measurement

While the console is running an operation the dashboard now shows a banner with
the latest activity line and retries every 1.5 s, instead of a blank page.

Measured on the 11.40 console (read-only staged exports of every game save, user
with 24 titles): all 11 installed games exported; the other 13 were refused with
"requires readable installed game version" because they have no /user/appmeta
entry (saves of uninstalled games). Package sizes were 0.00–4.00 MiB except
TEKKEN 8 at 229.93 MiB (ghost/replay slots; use slot selection). No imports or
live restores were run for these packages. Cross-console results are pending.

## Version 0.12.3 — selective slot export for large saves (host-tested only)

Sharing export accepts an optional `slots` list (dashboard `/api/share-export`
parameter, comma-separated slot names) so only chosen slots are packaged. Unknown,
duplicate or malformed names refuse the export; check/restore never accept a
selection and import exactly what the package contains, leaving other receiver
slots untouched. Measured on the 11.40 console (read-only staged exports): a
342 MiB Alan Wake 2 container set packaged to 1.58 MiB because containers are
mostly unused space, while TEKKEN 8 stays 230 MiB (already-compressed Unreal
files; ghost/replay slots are about 165 MiB of it), which is what the selection
is for. Not yet verified on a console; there is no UI control yet and no
compression.

## Version 0.12.2 — live restore commit hardening (host-tested only)

Generic live restore now re-checks that no save is mounted immediately before the
restore journal is written, discards its temporary `.pscloud-*.new` copies from the
live save directory on any failure, and verifies that the imported payload tree
contains exactly the package files (no stale leftovers). Rollback copies and the
journal behavior are unchanged. These changes are covered by host tests only; they
have not been run on a console, and generic live restore in-game remains unverified.

## Version 0.12.1 — metadata-independent sharing and compact cloud backups

Generic sharing anchors ownership to the selected existing local user/game/slot,
matching installed version, original container identity/hash and local sealed
keys. It guards the entire sce_sys namespace before/after payload import rather
than requiring one SFO schema. Readable title/slot/account fields must still agree;
malformed recognized PSF, symlinks, changing metadata and unsafe packages are
refused. Opaque or fieldless local metadata is retained byte-for-byte. No foreign
metadata is imported. There are no per-game or system-slot exceptions in this path.

The new **Compact cloud backups** preference uses actual game-data packages for
whole-game queue/upload/download/PC import and local-first protected restore on
7.00 or 11.40. Existing settings retain encrypted mode until explicitly switched;
old encrypted snapshots remain readable. Compact backups use portable- filenames
and can also be used in the sharing importer. Deduplication checks the complete
package hash, and uploads retain HTTPS readback verification. No ZIP compression
is added: savings come from omitting encrypted container allocation/unused space.
Restore requires existing local containers: start the game, make an initial save,
close it, then restore. This is not a full disaster-recovery image or save-database
creation. Legacy encrypted restore stays firmware-gated to 11.40.

Platform ownership is preserved using recipient containers; arbitrary internal
game-account bindings/encryption are NOT converted. Blind ID replacement is never
performed. Same-account operation is not proof of all-game compatibility. Generic
live restore keeps rollback and still requires the user's in-game verification.
Validation: all 131 host tests and both PS5 build jobs passed. Version 0.12.1
was deployed on the 11.40 and 7.00 consoles; both dashboards report the expected
version and accept the compact preference while preserving existing settings.
Compact backups are enabled on both test consoles. With Wolverine closed on
11.40, all ten slots exported to a 4,020,677-byte compact package instead of the
48,630,283-byte encrypted whole-game ZIP (about 92% smaller). The selected package
uploaded successfully to Koofr with readback verification, and local-first staged
restore passed with all live saves untouched. No live restore or in-game progress
verification was performed for this new package. New compact cloud hardware
backup/restore on 7.00 remains untested; that console has no cloud account
configured. Internal game-account conversion is not implemented.

## Version 0.12.0 — generic portable sharing and dual-firmware dashboard

Sharing no longer hard-codes Crash or two payload files. The new portable format
discovers up to 128 slots, exports regular nested game-data files, excludes
sce_sys, and checks a strict stored ZIP layout plus slot SHA-256/CRC integrity.
The dashboard starts on firmware 7.00 and 11.40 and adds Export for sharing,
Import package, staged Check compatibility, and explicitly confirmed Restore.
Local sharing does not require a cloud login. Existing normal encrypted backups
are distinct from portable packages and are not accepted for sharing.

Imports require the same PPSA ID and installed contentVersion, existing receiver
slots with the same names and at least the source container capacity, and valid
local SFO identity/account. Recipient metadata and keys are preserved.
System-memory slots (sce_sdmemory) with structurally valid SFO but no normal
identity fields, or an exact zero-filled 3 KiB metadata placeholder, are
anchored to the existing local game/user container and
installed version; their SFO and sealed keys are preserved. This is save-type
handling, not a per-game exception. Normal slots still require full SFO identity.
Additional recipient slots remain unchanged. All payload work occurs on copied containers;
successful export/check removes its known temporary copies after clean unmounts,
while live-restore rollback and failed/uncertain stages are retained.
live replacement rechecks originals and retains before-N.img rollback with a
durable safety journal. A partial failure attempts verified rollback. A power
loss across multiple slot renames is not atomic; an uncertain journal blocks
retry and requires inspection. Automatic crash recovery is not implemented.

Game-level account binding, regional differences or hidden dependencies can
still prevent in-game loading; container checks do not guarantee every game.
There is no per-game allowlist, but unsafe/mismatched inputs are refused. Saves
must already exist on the receiver; save/database creation is not implemented.
512 MiB total limit and 4096 files per slot. Ordinary 7.00 cloud backup/restore
remains firmware-gated pending its separate format/hardware validation.

All 127 host tests and native builds passed. On 2026-10-09, both dashboards
ran v0.12.0. The generic dashboard export/download/import/staged-check flow
passed for Astro Bot in both 7.00 → 11.40 and 11.40 → 7.00 directions, and for
Crash in the 7.00 → 11.40 direction. Both save-layout classes unmounted cleanly;
live-image hashes, recipient metadata/keys and payload checks passed. No generic
live replacement was performed. Generic live restore/game-load behavior remains
unverified. The user confirmed the separate Crash 7.00 → 11.40 manual v0.11.9
restore works in-game. This is not universal compatibility evidence.

## Version 0.11.9 — manual Crash sharing from 7.00 to 11.40

The standalone sharing tool now permits export on 7.00 as well as 11.40.
An explicit `MODE=restore` on 11.40 prepares both primary Crash containers using
the recipient's metadata/keys, requires the selected package SHA-256 and restore
confirmation, retains rollback copies, journals replacement and attempts verified
rollback on a partial failure. Extra slots remain untouched. No automatic live
replacement is performed by deployment, export or check modes.

All 114 host tests and native builds passed. On 2026-10-09, export from the
7.00 console and staged import into the 11.40 console both passed: payload
checksums, recipient metadata and sealed keys were preserved, both slots
unmounted cleanly, and all live saves stayed unchanged.
This new live restore path is implemented but not yet console/game-load verified.
The previous reverse-direction staged test is not evidence for a live restore.
Dashboard sharing is still unavailable. See [manual sharing](ps5/SHARING.md).

## Version 0.11.8 — Crash portable sharing staged pilot

A separate stage-only sharing tool exports Crash's primary progress/profile
payloads without console encryption keys or SFO/profile metadata. A strict
portable ZIP parser checks canonical layout, payload limits, CRC and SHA-256.
Recipient checks use copies of its own existing containers, preserve metadata
and sealed keys, and verify that live image hashes remain unchanged. The pilot
does not replace live containers, add database entries or enable dashboard
cross-console restore. Other games/extra slots are not supported by this format.

A diagnostic-only payload ran successfully on firmware 7.00 and enumerated save
folders. All 112 host tests and native PS5 builds passed. On 2026-10-09, native
Crash export passed on firmware 11.40: both copies mounted/unmounted successfully,
and a roughly 102 KiB portable package was produced. On firmware 7.00, importing
it into copies of the receiving profile's containers passed on-console payload
checksum, unchanged SFO, unchanged sealed-key and live-original hash checks. Both
games had the same installed version. No live containers were replaced; actual
recipient restore/game-load compatibility and dashboard integration remain
unverified or unimplemented. See [sharing pilot instructions](ps5/SHARING.md).

## Version 0.11.7 — local-first restore and optional backup housekeeping

Whole-game restore checks local queue/sent/download archives by checksum and
identity before downloading archive bytes. The dashboard passes the selected
snapshot SHA-256, enabling a matching spool archive to restore without a cloud
request. Missing/corrupt/mismatched local archives fall back to verified cloud
retrieval. A changed cloud version is rejected rather than silently substituted.
Identical destination images can return an explicit no-change result without
mounting or rewriting saves; closure confirmation and active-marker guards remain.

Cloud snapshots have a confirmed Delete from cloud action. It deletes only that
archive and cloud identity, never PS5 saves, rollback or local copies. Deletion is
blocked during an upload. Optional Keep latest local history retains one uploaded
archive per game/user/save group, preserving pending uploads, downloaded caches
and rollback. It is off by default and enabling it requires confirmation. Cleanup
skips active restore/mount markers, oversized inventories and unverifiable newest
copies. It runs after successful uploads or when the preference is enabled.

Important: a native restore stalled after staged metadata validation on 11.40.
Read-only hashing confirmed the live save still matched its pre-restore rollback.
The precise native blocking call is unconfirmed; additional close/unmount entry
logs are implemented. This milestone does NOT claim to fix every native unmount
stall. Do not clear active safety markers or force-stop a mounted restore.
All 108 host tests and native PS5 builds passed, including local-first/no-change
restore without cloud availability, selected cloud deletion with confirmation,
local retention preserving pending/rollback, and pending PC-import isolation.
After the user-confirmed normal restart, read-only checks found the original
unchanged, no commit journal and an empty staging mount directory. The old mount
marker was moved recoverably into its existing rollback folder; images were not
deleted. v0.11.7 was deployed on firmware 11.40 on 2026-10-09. Read-only checks
confirmed an idle dashboard, populated game inventory and cleanup off by default.
Native changed-save restore, cloud deletion and cleanup still need user testing.
Automatic game-close backup remains unimplemented and unavailable.

## Version 0.11.6 — selected-game upload and usable dashboard during sync

Manual backup now passes its exact published (or deduplicated/requeued) archive to
the uploader instead of draining unrelated pending backups. The dashboard manual
backup path also starts a background upload, and cancellation state is reset for
each job. A cancelled panel hides the live percentage; cancellation requests are
shown as cancelling until the worker actually stops.

Background upload no longer holds the save-operation lock. Library, queue, cloud
versions, PC/console downloads, PC imports and explicitly confirmed restores can
run while an immutable staged archive uploads. Save operations themselves remain
serialized with existing closure/identity/rollback checks. Global TLS lifetime
and shared logging are protected for concurrent jobs. Only competing uploads and
payload stop are blocked. Upload-all captures a fixed filename list, so subsequent
PC imports do not silently enter the running job. Replacing an existing import
waits until upload finishes; importing a separate version remains available.
Backups created during another upload stay queued instead of starting another job.

All 105 host tests and native PS5 builds passed. Fixtures verified exact-game
manual upload leaving another game's queue entry untouched, downloads and
confirmed staged-fixture restore during a blocked upload, competing-upload
rejection, cancellation retention and UI upload-only disabling. Native concurrent
restore/import and corrected cancellation still need console testing. No live
saves were restored for development.

Deployed on firmware 11.40 on 2026-10-09 after the earlier uploads completed and
the old dashboard stopped cleanly. Read-only console checks confirmed v0.11.6,
nonempty game discovery, an empty pending queue, idle upload status and cleared
cancellation state. These checks do not validate concurrent native restore/import
or a new manual backup; those features still require console testing.

## Version 0.11.5 — background queue uploads and corner progress

Upload this / Upload all now start a console background worker and return without
waiting for the transfer. A compact bottom-right panel shows the current game's
name, upload/checksum-verification stage, byte progress and result. Reopening the
dashboard reconnects to the running job; closing the browser does not cancel it.
Cancel upload retains the local archive for retry. Completion still requires
verified remote bytes and the cloud identity commit, not just a completed PUT.

Library, cached queue, connection state and activity remain readable. Save-changing
operations remain serialized and disabled during uploads. Queue views are captured
at job start and refreshed on completion. Only one upload job runs at a time.
PC imports remain manual queue entries; the existing upload preference is preserved.
Manual-backup automatic uploads still use the existing synchronous path in this
milestone. Background processing does not yet detect game closure, run after the
payload stops, or guarantee operation in rest mode. These are not PSN-equivalent
capabilities yet.

All 104 host regression tests and native PS5 builds passed. Fixtures verified
immediate job acceptance, readable library/queue/progress during a blocked upload,
competing-write rejection, verified completion and cancellation retaining the
local archive. Native background and large-backup upload behavior must still be
tested on the console, whose dashboard/loader/FTP ports were unreachable during
the deployment check. No live saves were mounted or restored for this change.

## Version 0.11.4 — bounded large transfers and visible upload stages

- Archive upload/readback budgets scale by byte count (minimum five minutes,
  maximum two hours), with a separate 60-second low-speed guard. Large encrypted
  save containers no longer inherit the same five-minute deadline as tiny saves.
- Explicit bounded `pread` upload streaming with a 256 KiB curl buffer; no full
  archive allocation and no dependency on curl's default FILE reader. Truncated
  reads abort rather than reporting premature EOF. Remote checksum verification
  still precedes the cloud identity commit and local sent marker.
- Authenticated, memory-only transfer status remains readable during uploads.
  The dashboard shows upload and verification byte progress and the current
  stage. Transport/HTTP result codes are logged without credentials or URLs.
- All 102 host tests and native PS5 builds passed. Console validation is pending.
  The observed large queued backup exceeded
  the older five-minute transfer window; that is a size/timeout hypothesis until
  a diagnosed native upload completes. No live save mounts/restores are added.
- Garlic's newer upstream has background jobs, locally staged ZIPs and bounded
  streaming FTP uploads. Its cloud path also packages encrypted containers; the
  mounted/decrypted browser export is a separate feature, not a guaranteed small
  cloud backup. PSCloud retains verified HTTPS WebDAV and its existing ZIP format.

Reference: [Garlic upstream cloud implementation](https://git.etawen.dev/earthonion/garlic-savemgr/src/branch/main/src/main.c).

## Version 0.11.3 — avoid false busy errors from read-only requests

The first console UI test exposed read-only cover/connection requests claiming
the same lock as save operations. Covers, saved cloud state, preference reads and
activity reads now remain available outside that lock, with atomic shared status
scalars and no concurrent access to a helper's shared log FILE. Save/transfer
operations remain serialized. Read-only API calls retry transient busy responses
with bounded backoff. Write calls are never replayed after busy/network failure.
While cloud versions are loading, write buttons are disabled and a progress bar
is shown; navigation remains available. All 100 host tests and native PS5 builds
passed. Fixtures verified metadata/icons remain available during a blocked cloud
request, while a competing backup is refused without changing saves. Browser
fixtures verified safe GET retry and non-replay of busy/failed write calls.

The first deployment attempt was stopped by unreachable console ports before any
payload was sent. After the user confirmed connectivity, v0.11.3 was deployed on
firmware 11.40 on 2026-10-09. Read-only checks passed: while listing Koofr versions,
saved state, preferences and activity returned successfully without busy errors
(about 0.31, 0.54 and 1.65 seconds respectively). Page response was about 1.57
seconds during the listing; the cloud list took about 15.49 seconds. These are
single-run LAN timings, not guarantees. Idle connections did not block health.
The live browser displayed the library, disabled write buttons while versions
loaded, and retained navigation access. No backup, upload, restore or credential
changes were performed by these tests. Remote-upload checksum verification and
broader backup/restore regression checks still need hardware testing.

## Version 0.11.2 — optional startup health check

The embedded browser blocked `/api/health` with `ERR_BLOCKED_BY_CLIENT`, although
the console endpoint responded normally to direct checks. Startup now treats
health as optional: a blocked/failed health request does not prevent the existing
state/preferences/game/queue loading sequence. A working health endpoint still
reports busy and defers loading safely. No save/credential operations are changed.
All 99 host tests and native builds passed. A local JavaScript fixture confirmed
the core loading sequence proceeds when health fails. Deployed v0.11.2 on firmware
11.40 on 2026-10-09; final read-only console checks passed. Page load was about
3.13 seconds; health median 0.72 seconds. During a Koofr listing, health returned
busy in about 1.05 seconds and HTML in about 1.94 seconds; five cloud versions
listed in about 15.19 seconds. Two idle sockets did not block health. These are
single-run timings, not performance guarantees. No save/cloud writes or credential
changes were performed by these checks.

The embedded browser blocked direct top-level API navigation as well; this must
not be confused with all in-page requests failing. After deployment, the patched
in-page startup displayed the real game library. Overlapping read-only requests
can still receive busy responses; refresh after other requests finish. No browser
security settings were changed. Koofr remains configured; Google development is
on hold. Native remote-upload checksum verification still needs a hardware upload
test, and broader interactive backup/restore regression testing is pending.
The live queue displayed one pending backup after refresh; direct queue read took
about 0.40 seconds. No queued archive was uploaded or altered by the smoke checks.

## Version 0.11.1 — verify cloud contents before queue completion

- WebDAV uploads now stream the uploaded archive back through SHA-256 before
  publishing the identity commit or marking the local queue entry sent. Failed
  or corrupt readbacks retain the local backup. Existing-cloud deduplication also
  verifies contents, not just the metadata and byte count. This deliberately
  costs additional bandwidth; no full readback copy is allocated in memory.
- Browser startup can load its page during another operation and waits using the
  lightweight health endpoint. Old game-list responses cannot overwrite a newer
  game selection. No automatic background uploads or save operations are added.
- v0.11.1 passed all 99 host tests and native PS5 builds. Corrupt upload retention,
  no identity commit on corruption and repair of an existing corrupt cloud copy
  passed HTTPS fixtures. Browser preview verified Google settings, game navigation,
  cached versions and explicit refresh.
- Deployed v0.11.1 on firmware 11.40 on 2026-10-09. Read-only checks passed:
  page load about 0.97 seconds; health median about 0.51 seconds; two idle browser
  sockets no longer blocked health. During a Koofr listing, health returned busy
  in about 1.29 seconds and dashboard HTML in about 1.01 seconds. Five cloud
  snapshots listed in about 11.12 seconds. These are single-run LAN timings, not
  guaranteed performance. No backup, upload, restore or credential changes were
  made by the smoke check. New upload checksum readback remains host-tested only;
  Google approval remains real-account unverified. Koofr remains the active
  provider; further Google development is postponed.

## Version 0.11.0 — responsive requests and Google sign-in foundation

- Bounded client threads keep dashboard HTML and authenticated health available
  during slow transfers and idle browser connections. Save/cloud/API operations
  remain serialized: competing operations get an immediate 503 busy response.
  Navigation remains available during an operation. No concurrent save writes.
- Small WebDAV identity reads use a 25-second metadata timeout, not the five-minute
  archive-download timeout. Browser cloud lists are cached for 60 seconds (up to
  16 selections); Refresh bypasses the cache and successful actions invalidate it.
- Native Google device sign-in requests only `drive.file`, respects poll intervals
  and slow-down, handles denial/expiry, privately persists refresh tokens and
  includes a refresh-token helper. It does not change the WebDAV configuration.
  Tokens/device secrets are not returned to the browser or logged.
- **Google Drive transfers are NOT enabled in this milestone.** A registered
  TV/Limited Input OAuth client is needed for real sign-in validation. Resumable
  uploads, provider switching, verified listings/downloads and transfer progress
  and retries still need implementation. This release is a sign-in foundation.
- All 96 host tests and native builds passed: idle sockets, a blocked cloud list,
  Google approval/denial, scope checks, private persistence and unchanged WebDAV.
  Native threading and live Google login are not yet console-tested.
- Previous milestone: all 90 host tests and native v0.10.1 builds passed. Crash's
  three-slot backup/deduplication, cloud archive and original-image hash checks
  passed on console. Astro testing was paused before a backup ran; Tekken was
  inspected only. Generic restores remain hardware-unverified.

See [Google setup and remaining work](ps5/GOOGLE_DRIVE.md).

## Version 0.10.1 — larger complete-game archives

Larger multi-slot games can exceed the initial 256 MiB cap. The bounded archive
and PC-import/download limit is now 512 MiB. Archives above that limit still fail
closed; no slots are silently omitted. The 128-slot count limit is unchanged.
Larger images can consume significant disk space and transfer time; no automatic
retention/deletion is enabled. v0.10.0 passed all 90 host tests and native builds;
Crash's three-slot cloud upload and final integrity/deduplication checks passed.
v0.10.1 native builds passed; larger-game hardware backup checks remain pending.

## Version 0.10.0 — dynamic PS5 save-slot backups

- Encrypted whole-game backup discovers every active `sdimg_` container for the
  selected PPSA game/user, including numbered, profile, replay and custom slots.
  Sony `sdimg_sce_bu_` shadow copies are excluded. Slot names are validated and
  ordered deterministically; regular files, stability and hashes are checked.
- Generic V2 ZIPs include a canonical per-image checksum manifest. Limits are
  128 active slots and 256 MiB per archive: exceeding either aborts the whole
  backup, never silently drops files. Existing two-slot Crash V1 ZIPs still read
  and retain their unchanged-backup fingerprint.
- All discovered PS5 titles can select whole-game backup, cloud upload/download,
  queue and original-PC-ZIP import. This does not include PS4/CUSA saves.
- Experimental same-console restore handles every archived slot as one guarded
  transaction, retaining all originals and a slot-index rollback journal. Exact
  destination slot inventory and existing registered containers are required.
  V2 restores check mounted source/destination SFO game, slot and account identity.
  Generic raw restore requires unchanged sealed keys and sizes; it does not assume
  UE4 payload names. Recreated-container payload migration stays Crash/UE4-only.
- This is general encrypted-container support, NOT a promise that every game,
  firmware, save size or account-specific payload is compatible. Cross-console
  sharing, game-close automation, missing save-database creation and automatic
  power-loss recovery remain unavailable. Hardware support remains FW 11.40.
- All 90 host tests and native builds passed. Crash three-slot backup/upload and
  unchanged-backup/original-integrity checks passed. Generic restore is host-tested
  but remains unverified on hardware.

## Version 0.9.0 — cloud settings, activity and user preferences

- Provider-neutral Koofr/Nextcloud HTTPS WebDAV settings and saved-connection test.
  Saved passwords are never returned to the browser.
- PS5-persisted automatic-upload preference: on preserves immediate upload after
  manual backups; off keeps both whole-game and per-slot backups in the local
  queue. Explicit Upload / Upload all remain available. PC imports stay queued.
- Optional activity auto-refresh while the page is visible, newest-first log
  entries, warnings/errors and event filters, search and recent-log download.
  Activity is the latest 8 KiB of the console log, not a permanent cloud audit.
- Game-close automation and portable cross-console/profile sharing are visibly
  unavailable, not functional switches. No automatic save deletion is added;
  closure, identity, checksum and rollback safeguards cannot be disabled.
- Fixed Settings/Activity navigation: SVG icons no longer duplicate page IDs.
- All 85 host tests and native PS5 builds passed. Deployed on firmware 11.40:
  preferences save/read and original-choice restoration, unavailable-feature
  flags, saved Koofr connection test and activity events passed. Browser checks
  verified both pages, preference controls, and activity search in a safe preview;
  the real PS5 settings page also displayed the saved choices correctly.
  Local-only backup and explicit-upload behavior passed host fixtures, not a new
  live save backup test. No game saves were changed by the settings smoke check.
  Firmware 11.40 Crash restore confirmation belongs to v0.8.0 below.

## Version 0.8.0 — recreated-container recovery

When existing save containers have changed keys or sizes, whole-game restore
decrypts the archived images on staged copies, checks source and destination
SFO game/slot/account identity, and writes only the supported UE4 payload into
staged copies of the current containers. Their new encryption keys and `sce_sys`
metadata are preserved. Both images are prepared before any live replacement;
the existing rollback and interrupted-transaction protections remain in effect.
A `/api/restore-check` staged validation does not replace live save images.
This still requires existing, properly registered destination saves: if both
were deleted, launch the game to create fresh saves, close it, then recover.
Creation of PS5 save-database records from scratch is not implemented.
All 81 host tests and native PS5 builds passed. Firmware 11.40 staged recovery
successfully prepared both recreated Crash Bandicoot 4 saves, preserving their
new keys and metadata; both live image hashes remained unchanged during that check.
The user subsequently confirmed that restoring the game files worked in-game.
This is one same-console Crash test, not validation of every game or firmware.
Power-loss interruption recovery remains manual; retain rollback copies.

## Version 0.7.1 — explicit duplicate choices for PC imports

PC import preflight checks the selected game/user and full ZIP SHA-256 against
verified local snapshots, with a cloud identity lookup if the local archive is
absent. An identical match offers Keep existing (skip), Replace existing copy,
or Upload separate version. Replace retains the existing filename/date and queues
an explicit overwrite; it does not silently create another version. Separate
always creates a new version. Different progress is never automatically replaced.
Direct duplicate imports without a policy require a choice rather than defaulting
to a new file. All 78 host tests and native builds passed for this release;
the duplicate-choice interface still needs a dedicated console interaction test.

Console investigation confirmed that deletion/recreation changes both encrypted
save keys. The raw-image restore intentionally refuses those destinations; it
could not recover into a deleted/recreated save in version 0.7.1.
Version 0.8.0 adds the staged payload migration described above.
The console log also recorded a user-run successful same-key whole-game restore;
changed-progress game-load validation is not confirmed. Rollback copies remain.

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
  At release time, console overwrite/game-load validation was pending.
  See version 0.8.0 above for the subsequent user-confirmed restore result.

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
    User-USER_ID/
      PlayerSaveSlot0Save/
      PlayerSaveProfileSaveData/
```

Existing flat backups are preserved. They have not been automatically classified
or moved. Folder routing and duplicate skipping require the new identity format.
These commits are an initial protocol, not signed or provider-enforced immutable
manifests. The folder/deduplication host tests and PS5 builds have passed;
their live console checks remain pending.

## Version 0.5.0

- Responsive embedded dashboard on PS5 port 8082, (manual pairing was removed; keep it on a trusted LAN).
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
