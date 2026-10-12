# PSCloud changelog

Every release, newest first, with what was verified on a console and what was not.
The current overview is in [README.md](README.md).

## Version 0.31.0 — change values in a save (beta)

A new **Edit (beta)** tab on each game:
- **Load current save** reads the game's save into the page, the same way Share exports it.
- **Find a value**: type a number the game shows, such as money or skill points, and see every
  place it is stored. After playing and saving, load again and **Find again** to keep only the
  places that changed to the new number.
- **Compare two loads** lists what changed, with the nearest name stored in the file (for
  example `CASE_MAIN_FIND_TOR`).
- **Make the changed save and check it**: PSCloud writes the new values and fixes every
  checksum: the game's own where known, the archive checksums and the package SHA-256. It then
  uses the normal check and **Replace my save** path, with a rollback copy.

Research behind it: decrypted PS5 saves from 7 games on 11.40 were surveyed.
- Ghost of Tsushima, Alan Wake 2 and Marvel's Wolverine store progress in plain data with
  readable field names.
- One game (`DSSS` header) encrypts its own saves; those cannot be edited.
- Alan Wake 2's files are `4 | 4 | CRC32 of everything after byte 12 | name | data`, confirmed
  on every file. PSCloud recomputes that CRC32 after a change.

Verified by tests:
- The editor runs against the real Alan Wake 2 backup: all checksums are correct and Python's
  ZIP reader agrees.
- The page's own code (run with node) changes a value; the dashboard accepts it through check
  and replace, and the game checksum is right.

Not yet verified in a game.

## Version 0.30.2 — compact list of save slots

**Saves on this PS5** can show a compact list: one thin row per slot with its picture, name
and ID. A **Cards / List** switch beside the heading shows the slot count. PSCloud remembers
your choice, and games with more than 8 slots open as a list. Long lists show the first 24
rows (12 cards) and a **Show all N slots** button.

## Version 0.30.1 — getting the newer save finishes where you clicked

Fixes from the first console tests of 0.30.0:
- **Get the newer save** and **Send to my other PS5** stopped with "Install this game on
  this console" when the receiving PS5 did not have the game. The check now lets those saves
  through, and the replace creates the slots.
- **Get the newer save** no longer jumps to the Share tab. The banner shows **Newer save
  ready** with **Replace my save now** (asks once, keeps a rollback copy) and **Not now**.
- PSCloud checks itself whether a game is running. The "Game closed" switch is only needed
  when it cannot tell.
- On the game page, **Create the save slots this profile does not have yet** is on by
  default. **Write the save anyway** is on when the game is not installed. A slot that cannot
  be created (for example from an older ZIP) is skipped as before, instead of stopping the
  replace.

Verified by host tests. On the consoles, only the two refusals above were reproduced; the
full flow is not yet verified there.

## Version 0.30.0 — real save times, saves the other PS5 does not have, lighter uploads

**Sync compares when the game saved each slot.** The PS5 keeps, per save slot, the time the
game last saved it in its save list (`savedata.db`, read only). Sync now uses that instead
of file dates. Replacing a save changes the file date but not the game's time, which is
why 7.00 looked newest while 11.40 had the newer Wolverine save. Slots received from the
other PS5 keep the time they were saved there until the game saves them again. Before
sending, PSCloud asks the other PS5 to back up its current save first.

**Write saves this PS5 does not have yet (experimental).** When a shared save has slots the
profile lacks, PSCloud can create them, the way the console creates saves:
- a new save container from the console's own create call;
- the game's `sce_sys` (keystone and save details);
- one row in the PS5's save list.

This works when:
- the game is not installed here (**The game is not installed here: write the save
  anyway**);
- the profile never had the game. The source slot's keystone and details then come with the
  package, and the account and profile number come from another save on this profile.

On the receive screen, **Write it now** does this without installing or playing first. A
slot that fails is removed again. The save list is copied to
`rollback/savedata-<profile>.db.before-create` before each change.

**Shared packages are format V2:** each slot also carries its `sce_sys`. For existing slots
the receiving console still keeps its own. Older PSCloud versions cannot read V2 packages,
so update both consoles.

**Uploads no longer download everything again.** By default an upload counts when:
- it went over HTTPS, which detects damage in transit;
- the server accepted it;
- a size check shows exactly the bytes sent.

Only then is the backup's SHA-256 recorded. "Already in the cloud" is decided from that
record and the size, also without downloading. **Cloud settings → After each upload** can
switch the full download-and-compare back on.

**Cancel stops the upload completely.** Before, cancel did not stop the check after an
upload, and the upload could still be recorded.

Verified on the consoles:
- 11.40 and 7.00: the create call makes a container PSCloud can open again.
- Both consoles: the layout of `param.sfo`, `PARAMS` and the save list database.
- Both consoles: a game's keystone is the same on both consoles, across 10 games.
- 11.40: two test slots were added to TEKKEN 8 with database rows, and the database stayed
  intact.

Not yet verified on a console:
- whether the system menu and the game accept created slots;
- the full write flow from the dashboard;
- the per-slot time comparison;
- the size-only upload check.

## Version 0.29.2 — "in sync" means both hold the same save

**Bug found on the consoles:** Marvel's Wolverine showed "In sync" on 11.40 while 7.00 held
an older save. 11.40 only checked that its own save had not changed since its own last
backup. Now each console reports over the network which save it holds right now: the save
it last received from the other PS5 if it has not been played since (even if backed up
again, because a backup of the same data made on another console is arranged differently),
otherwise its last backup if not played since. **In sync** requires both to hold the same
save. Otherwise the more recently played side is newer, or "both changed" if both were
played after the last sync. A linked profile whose other PS5 has no copy shows **Only on
this PS5**. Receiving a save records which save it was (`sync/<profile>-<ID>`: WHEN, SHA).

**Clearer names:** the game page shows "Profile: M-Bilal" (the ID is in the tooltip), and
the sync banner names the other console, e.g. "Profile M-Bilal on your PS5 11.40
(192.168.0.193) has newer progress". The page now retries profile and status requests while
the dashboard is briefly busy, so names no longer fall back to "Profile 179a0cd8".

When the other PS5 does not answer, the banner says **Other PS5 not reachable** instead of
guessing.

Verified by host tests with two PSCloud instances (in sync after receiving, then "This PS5
is newer" after playing here, and "not reachable" once the other instance stops).

## Version 0.29.1 — per-game sync, send to the other PS5, honest sync status

- **Sync this game with my other PS5** (per game): backs the game up when it is closed,
  even with the main "Back up when I close a game" setting off, and brings newer progress
  from the linked profile automatically (checked first; optional automatic replace). Each
  console lists its synced games in `/api/peer/info`, so turning it on once on either PS5
  turns it on for both. Synced games are checked every 5 minutes.
- **Send to my other PS5** (manual button on the game's sync banner, `/api/sync-push`):
  backs the game up here, then asks the other PS5 (`POST /api/peer/push`) to fetch it over
  the read-only peer API, verify it and run the staged check. It replaces the save there
  only if that PS5 has **Allow my other PS5 to update saves on this PS5** on (`remote.conf`,
  off by default) and the game is closed there; otherwise the save waits there as ready to
  replace and that PS5 shows a notification. A rollback copy is always kept. The receiving
  PS5 accepts only from consoles in its own network list, for profiles linked on it.
- **Sync status is honest about what it compared:** found on the 7.00 console, the banner
  said "This PS5 is newer" for a profile that was not linked and had nothing to compare
  with. It now says "Not linked yet", "Only on this PS5" (the other PS5 has no copy), or
  "This PS5 is newer" only when the other copy really is older, and names where the other
  copy is (on your network or in the cloud).

Verified by host tests with two PSCloud instances (send with and without permission to
replace, refusal from a console not in the list). Not yet tried between the two consoles.

## Version 0.29.0 — sync over your home network

Two PS5s running PSCloud can now sync directly, with no cloud account.
- **Finding each other:** every PSCloud answers a UDP "PSCLOUD-DISCOVER" on port 8083 with
  its console ID, dashboard port and firmware. Cloud settings → **Other PS5s on your
  network** → **Find on my network** (`/api/peers/discover`), or type the other PS5's IP.
  The list is kept in `peers.conf` (`/api/peers`).
- **What a console shares:** read-only, under `/api/peer/` and without a browser session:
  its profiles (`info`), the newest portable whole-game backup per profile and game
  (`latest`), and that backup file by its exact name (`file`). Nothing else, and nothing
  can be written. Anyone on the home network can already open the dashboard, so this adds
  no new exposure; keep port 8082/8083 off the internet.
- **Sync:** profile links (Cloud settings → Link with your other PS5) now list profiles
  from PS5s on the network too. Sync status looks at the cloud (if connected) and at the
  linked profile's newest backup on each network PS5; a backup this PS5 made counts as a
  sync point. "Get the newer save" downloads from the other PS5 over the network, verifies
  the checksum and runs the usual staged check; replacing keeps a rollback copy. Automatic
  download/replace per game works the same way. The other PS5 shares its backups, so keep
  "Back up when I close a game" on there to always share the latest progress.

Verified by a host test with two PSCloud instances (discovery, profile names, newer save
found on the network, fetch and check without changing the save, replace, in sync, and
refusal of paths outside the backups). Not yet tried between the two consoles.

## Version 0.28.0 — start with the console, rest mode

- **Start PSCloud with your console** (Cloud settings guide): add `pscloud-dashboard.elf`
  to an autoloader. With ProsperoMgr: Payloads → upload the ELF, then Settings → Autoloader
  → add it to the sequence and enable the autoloader. PSCloud does not edit other apps'
  configuration files. Everything PSCloud needs is in `/data/pscloud`, so a fresh start
  picks up all settings.
- **Rest mode:** the background loop notices when the process was suspended for more than
  two minutes and logs "PSCloud was paused for N minutes" to Activity, so the effect of
  rest mode on a given console can be seen. A nightly backup missed while the console was
  in rest mode now runs once the console is back on that day (at or after the chosen hour,
  once per day). What happens to uploads in progress during rest mode has not been tested.
- **Cloud space:** a provider that answers with zero for both used and free space is shown
  as "does not report how much space is used" instead of 0 MB.

**Storage providers:** PSCloud speaks WebDAV over HTTPS, which covers Nextcloud, ownCloud,
Koofr, pCloud, Box, Synology/QNAP/TrueNAS NAS WebDAV servers and similar. Dropbox needs an
app key registered with Dropbox, and SMB needs a client library not in the PS5 SDK; neither
is included. The Google Drive sign-in code is present but transfers are not enabled.

## Version 0.27.0 — encrypted cloud backups

- **Encrypt backups in the cloud** (Cloud settings; `/api/encryption`, `encrypt.conf`,
  owner-only, never sent back to the browser): new uploads are encrypted before they leave
  the PS5. Format: `PSCENC1\0` | 16-byte salt | PBKDF2 iterations | 12-byte nonce |
  AES-256-GCM ciphertext | 16-byte tag; key = PBKDF2-HMAC-SHA256(password, salt, 100000);
  the header is authenticated. Each upload gets a fresh salt and nonce.
- The worker encrypts into an already-unlinked temporary file (nothing is left behind),
  uploads it and verifies the cloud copy against the encrypted file's checksum. The
  identity file keeps the plain checksum, so every download is decrypted and then checked
  against it as before.
- Every download path decrypts: restore and Download to PS5, Download to PC, sync (Get the
  newer save) and the standalone download tool. A missing or wrong password stops with a
  clear message and never touches a save.
- Backups on the PS5 and on USB are not encrypted. To sync between consoles, set the same
  password on both. Turning encryption off affects only new uploads.

Verified by host tests (encrypted upload is not readable and is 56 bytes larger, no
temporary file left, round trip back to the exact original, wrong or missing password
refused).

## Version 0.26.0 — cloud housekeeping

- **Keep in the cloud** (Cloud settings; `cloud-keep.conf`, `/api/cloud-keep`): all
  versions, or the newest 3/5/10/20 per save. After each successful upload, older versions
  of that save are removed from the cloud (archive first, then its identity). Versions whose
  identity cannot be read are never removed. Backups on the PS5 are not affected.
- **Clean up older versions** on a game's Cloud versions tab (`/api/cloud-prune`).
- **Cloud space** (`/api/cloud-housekeeping`): how much the PSCloud folder uses and how much
  is free in the cloud account, from a WebDAV quota request; shown in Cloud settings and on
  the Storage page.

Verified by host tests (keeps the newest N, never deletes unreadable versions, setting
persists). Not yet run against your Nextcloud.

## Version 0.25.0 — sync between consoles

Cloud backups are stored per console profile (`<game>/User-<profile>/WholeGame`), and the
same person has a different profile ID on each PS5. So syncing starts with **linking**:
- Each console publishes its profiles to `<cloud>/.pscloud-profiles/<profile>.txt` (name,
  a random console ID kept in `console.id`, firmware) when it starts and hourly.
- Cloud settings → **Link with your other PS5**: for each local profile, choose which
  profile on the other console is the same person (`links.conf`, `/api/links`).

**Sync status** (`/api/sync-status`, banner on each game page) compares the newest save
time on this PS5 with the newest cloud version of this profile and its linked profiles, and
the last sync point (`sync/<profile>-<ID>`, written after a replace from a synced save):
in sync · this PS5 is newer · cloud is newer · **both changed (conflict)** · not in the
cloud yet. The banner offers Back up now, **Get the newer save**, or both for a conflict.

**Get the newer save** (`/api/sync-get`) downloads the newest linked version, verifies its
checksum, stores it in `share/` and runs the usual staged check for this profile (version
checks stay on; slots this profile lacks are skipped). The normal Replace my save step then
uses it, with a rollback copy.

**Per game, optional:** "Download newer saves automatically" checks every 15 minutes while
the game is closed and notifies you when a checked newer save is ready; "…and replace my
save automatically" also replaces it when there is no conflict (rollback copy kept).
Background checks do not write every cloud request to Activity.

Both consoles need PSCloud connected to the same cloud account. Verified by host tests
(not linked, cloud newer, fetch and check without changing the save, replace, in sync, and
a conflict when both change) and a local preview. Not yet tried between two consoles.

## Version 0.24.0 — readable save names, per-game settings, back up all, nightly

First part of the "closer to PS Plus cloud saves" plan (sync status, auto-download, cloud
housekeeping, encryption and start-with-console follow in 0.25-0.28; PS4 saves later).
- **Readable save names:** each slot shows the PS5's own labels from the save's
  `param.sfo` copy in `savedata_prospero_meta` (MAINTITLE, SUBTITLE, DETAIL), for example
  "Slot 1 [Auto] A Debt Repaid", everywhere a slot is named. The game's Back up tab lists
  "Saves on this PS5" with each slot's own picture (`/api/slot-icon`).
- **Per-game settings** (`/api/game-settings`, stored in `games.conf`): "Back up when I
  close this game" and "Include in Back up all games", both on by default.
- **Back up all games** (`/api/backup-all-start`, status `/api/backup-all`): runs in the
  background one game at a time, skips running games and games switched off, counts
  unchanged saves without queueing them again, and uploads the new backups together when
  automatic upload is on. Progress shows on the home banner; a PS5 notification reports
  the result.
- **Nightly backup** (`/api/schedule`, `schedule.conf`): back up all games of every
  profile once a day at a chosen hour (console clock), when no game is running.

Verified by host tests (labels and pictures, settings, back up all counting new /
unchanged / skipped, nightly runs once a day) and a local preview.

## Version 0.23.0 — less storage, everything in one folder

On the 11.40 console PSCloud used 2.5 GB. A read-only inventory showed why:
- Every replace kept its whole working folder: the original save (`before-N.img`, the
  safety copy) **and** the imported copy (`image-N.img`) plus empty mount points. Two
  replaces alone held 684 MB and 619 MB.
- Eight working folders from earlier versions' checks were never removed (the live
  saves were never changed by them).
- `share/` kept every export and received ZIP (53 files, 767 MB).

Changes:
- After a replace, only the original save is kept, labelled with `info.txt` and moved to
  `rollback/restore-<ID>-<profile>-<id>/`; the 3 newest per game and profile are kept.
- `share/` keeps the 2 newest files per game after each export or import.
- **Free up space** on the Storage page (`GET/POST /api/cleanup`) shows, before anything
  is removed: leftover working files (safe; originals are kept), older shared saves,
  older uploaded backups (newest per game kept; the rest are in the cloud), and restore
  safety copies (off by default, with a warning). It refuses while an interrupted restore
  or mount marker exists or another save operation holds the lock, never touches a folder
  with something mounted inside, and only removes known file types.
- The Storage page now counts leftovers and safety copies file by file, so originals kept
  inside old working folders show as safety copies, not temporary files.
- Everything lives in `/data/pscloud/` (log, cloud sign-in, CA bundle, tool settings),
  moved there automatically on first start; standalone tools use the new paths.

Verified by host tests (only chosen categories removed, originals kept, newest shared save
per game kept, refusal while a restore marker exists, only the 3 newest safety copies kept,
replace keeps just the originals). Not yet run on a console.

## Version 0.22.1 — USB without duplicates, plain wording

Found while testing on the 11.40 console with a USB drive: every press of "Save current
save to USB" wrote another identical copy (six 5.2 KB copies of the same save), because
each export gets a new name and time. Now:
- **Save to USB now** (`/api/usb-save-game`) exports the game, copies it, and removes the
  temporary export from the PS5. Before copying, PSCloud compares the save's contents
  (the checksums of its save slots, ignoring when it was exported) with the files already
  on the drive; an unchanged save is reported as "Already on USB, unchanged" and nothing
  is written. "Copy older backups" and "Copy all saves to USB" use the same check.
- New copies go to one folder per game: `PSCloud/<Game name> (<ID>)/`. Files in the
  earlier `Backups/` and `Shared saves/` folders are still listed and recognised.
- Each file shows as "Save from <date>", the newest marked **Newest**, with
  **Restore…** (opens this game's Share tab with the file loaded, ready for the usual
  check, then replace with a rollback copy) and **Delete** (removes only the copy on the
  drive, after confirmation; `/api/usb-delete`, limited to the PSCloud folder).
- The buttons explain themselves: "Save to USB now" (this game's progress as it is right
  now) and "Also copy older backups".

Verified by host tests (unchanged save not copied twice, no export left behind, game must
be confirmed closed, delete limited to PSCloud) and a local preview.


## Version 0.22.0 — friendlier Manage saves and Storage pages

**Manage saves** is reorganised:
- A header with the game's cover, name, profile and status (Installed / Saves only,
  number of save slots, Playing), and a **Game closed** switch that is always visible
  (it replaces the "I have fully closed this game" checkbox and works the same way).
- Four tabs: **Back up** (back up now, add a backup from your PC), **Share**, **Cloud
  versions** and **USB**.
- **Share** shows two cards, "Send to another PS5" and "Receive from another PS5", each
  with numbered steps; save slots are tappable chips; replacing your save sits in its
  own clearly marked box.
- **USB, per game:** "Save current save to USB" (a fresh export of this game, copied
  and checked), "Copy all backups of this game" (/api/usb-copy-all now accepts `title`),
  and this game's files on the drive with "Receive this save".

**Storage** shows how much PSCloud uses with a coloured bar and legend, one card per
location (icon, size, file count, plain description, path behind "Show location";
leftover temporary copies are highlighted), and a USB card with a capacity bar,
"Copy all saves to USB" and the drive's files grouped by game with cover art.

Verified by host tests (including the per-game USB filter) and a local preview of both
pages.

## Version 0.21.0 — refreshed design

A new look, following the design notes in docs/DESIGN_NOTES.md:
- Deep navy background with soft colour glows, frosted-glass panels and sidebar, a
  gradient primary button, rounder controls and consistent status pills.
- Home banner greets the chosen profile ("Good evening · Welcome back, M-Bilal") and
  shows live counts: games with saves, backups waiting to upload, and cloud status, next
  to the Now playing card.
- Game cards use the game's cover art full-bleed with an Installed / Saves only label,
  lift on hover, fade in one after another, and the running game gets a green ring and
  a "Playing" label.
- The library comes first on the home page; Receive a shared save moved below it.
- Messages appear in the top-right corner. Motion is turned off for people who ask
  their system for reduced motion.

Behaviour is unchanged. Verified with a local preview and the page script check.

## Version 0.20.0 — Storage page and USB drives

- **Storage page** (sidebar): every location above with its path, purpose, size and
  file count, plus free space on the PS5. New read-only endpoint /api/storage.
- **USB:** pick a drive, **Copy all backups and shared saves** (skips files already
  there with the same checksum), **Copy to USB** on each queued backup, and **Receive
  this save** for any PSCloud ZIP on the drive, which opens the normal receive steps.
  Endpoints /api/usb, /api/usb-files, /api/usb-file (read-only), /api/usb-copy and
  /api/usb-copy-all. Paths are restricted to `<drive>/PSCloud/`.

Verified by host tests (locations and sizes, verified copies, "already there", folder
names, rejected paths outside PSCloud, nothing written elsewhere on the drive). Not yet
tried with a real USB drive on a console.


## Version 0.19.5 — backups no longer fail while an upload runs

**Bug found on the 11.40 console:** three backups at 10:39 failed with "Compact backup
failed" while the previous backup (Resident Evil Requiem, 15 MB) was still uploading.
The export itself succeeded, but adding it to the queue needs the queue lock, which the
running upload holds, and the backup gave up immediately. Live saves were never
touched. The backup now waits for the upload to finish (up to 10 minutes, logged as
"Waiting for the current upload to finish before adding this backup"); this also
covers game-close backups. The three failed attempts left their temporary export ZIPs
in /data/pscloud/share (copies of the live save; safe to remove).

Saves named only with a generic label such as "Game Data", "Save Data" or "Saved Data"
now keep their ID instead of showing that label as the game name.

Verified by host tests, including a backup requested while the queue lock is held.


## Version 0.19.4 — names for saves of games that are not installed

Saves of games that are not installed showed only their ID. Following ProsperoMgr,
which names saves from the console's save records, PSCloud now reads each save's
unencrypted `param.sfo` copy in `/user/home/<user>/savedata_prospero_meta/user/<ID>/`
(read-only) and uses its MAINTITLE when the game's saves agree on one label that is not
just the slot folder name. For example "Hades" is used; a game whose saves are labelled
"Settings" and "Slot 1" keeps its ID. The game list now also reports `installed`
explicitly instead of the page guessing from the name. On 11.40, Ghost of Tsushima
(v0.19.3) now appears by name. Verified by host tests.


## Version 0.19.3 — games that keep saves under another ID

Ghost of Tsushima (PPSA03210) was installed on the 11.40 console but missing from
the library. Its param.json sets `savedata.titleIdForSharing` to PPSA02225, so its
saves live under PPSA02225, which the library showed as an unnamed "PPSA02225 · Game
not installed" card. The game-info lookup (name, icon, version, installed check, and
the version used by sharing) now resolves a save ID to:
1. `<appmeta>/<ID>` when it exists (unchanged);
2. otherwise a folder in appmeta or appmeta/external whose param.json `titleId` is
   that ID, including folders named by content ID (an approach taken from
   ProsperoMgr);
3. otherwise an installed game whose `savedata.titleIdForSharing` is that ID.

Verified by host tests that recreate this layout. Not yet checked on the console.


## Version 0.19.2 — live Activity page

The Activity page now updates when the console log changes instead of every 10
seconds. The page asks /api/log?wait=1&since=<size>; the console holds the request
and answers as soon as the log grows (checking the file size once a second), or
after 25 seconds with no change, and the page asks again. At most 4 such waits run
at once, so other requests are never starved. Nothing is requested while the page
is hidden or another page is open. The "Refresh the Activity page automatically"
setting is removed because it is no longer needed (ACTIVITY_REFRESH stays in
preferences.conf for compatibility). Verified by host tests.


## Version 0.19.1 — clearer backup settings

Cloud settings → Backup settings is grouped into Automatic, Storage and Other, with
plain labels and one short explanation each: "Back up when I close a game", "Upload
new backups to the cloud" (it also applies to game-close backups, which the old label
did not say), "Smaller backups (recommended)", "Keep only the newest backup of each
game on the PS5" and "Refresh the Activity page automatically". The stored settings
and their behaviour are unchanged. Verified in a local preview.


## Version 0.19.0 — cleaner home page, now playing

- **Now playing:** the right side of the home banner shows the game that is running
  (its icon and name, and whether it will be backed up when closed); with no game
  running, the cloud picture returns. New read-only endpoint /api/running uses the
  same detection as the game-close watcher. It refreshes every 10 seconds while the
  home page is visible.
- **Uploads page:** the upload queue moved from the home page to its own Uploads
  page in the sidebar, with its own Upload all button.
- **Profile switcher:** moved from the home page to a compact drop-down in the top
  bar, visible on every page.
- **Upload notification:** has a close (×) button, closes itself 8 seconds after an
  upload finishes, and is not shown again for an upload already seen in that
  browser. Other messages close when tapped.

Verified: host tests and a local preview (home page, Uploads page, close button
staying closed after a reload). The now-playing card has not been seen on a console
yet.


## Version 0.18.2 (0.18.1 corrected) — game-close backup waits for the game to really close

**Bug found on the 11.40 console:** with "Back up automatically when a game closes"
on, Marvel's Wolverine was backed up 7 times in 15 minutes while it was still being
played, and automatic upload sent every copy to the cloud. Wolverine mounts its save
only while writing it, so "no save mounted" was read as "game closed" after each
autosave. One upload attempt also failed with transport 35 (a TLS connection error);
that backup stayed queued and uploaded on the next attempt.

**Fix:** before a game-close backup, the watcher checks whether the game is running
and waits while it is. It asks the system service for the foreground game
(sceSystemServiceGetAppIdOfRunningBigApp and sceSystemServiceGetAppTitleId, the
approach ProsperoMgr uses) and also scans the process list (sysctl kern.proc plus
sceKernelGetAppInfo). A read-only diagnostic payload (pscloud-ps.elf) showed on
11.40 that the title ID sits at byte 0x10 of the app info (eboot.bin = PPSA03671),
not where the SDK ps sample's struct puts it; the first 0.18.1 build read the wrong
bytes and never saw a running game, which this build corrects. It logs once per wait ("still running;
backup waits until it is closed"). If the process list cannot be read, it only backs
up after 5 minutes with no save changes. /api/save-status also reports `running`
(true, false, or null when unknown), and Receive a shared save now waits on the
"close the game" step while the game is running.

Verified: host tests, including a simulated game that autosaves while running with
no mounted save (no backup until it closes). On 2026-10-11, with v0.18.2 on the 11.40
console and Marvel's Wolverine open, /api/save-status reported running=true for
Wolverine and running=false for a game that was not open. Deployed to both consoles.
A full play-then-close cycle with the fixed build has not been observed yet.


## Version 0.18.0 — a separate view for each PS5 profile

When the console has more than one profile, My games starts with "Who is using
PSCloud?" and one button per profile, using the names shown on the PS5. The game
list, the upload queue and the default profile for Receive a shared save show only
the chosen profile. The queue notes how many items belong to other profiles, and
"Upload all queued backups" still uploads every profile's queue. Each browser
remembers its own choice. With one profile the switcher is hidden.

This is a view, not access control: anyone on the network can open the dashboard
and switch profiles, and Cloud settings and Activity stay shared for the console.

Verified: page script syntax check and a local preview with two sample profiles
(switching filters games and queue; the choice survives a reload).
CI passed. Deployed on 2026-10-11 to both consoles. On 11.40 (profiles M-Bilal,
User2, fat) switching showed 24 games for M-Bilal and 2 for User2, with the one
queued backup listed only under its own profile; no script errors. 7.00 lists
profiles User1 and bilal.


## Version 0.17.0 — every jailbroken PS5 firmware

**Supported firmware.** The dashboard now starts on any firmware the PS5 payload
SDK runs on (the pinned SDK v0.43 lists 1.00 through 13.60; CI prints the coverage
of the SDK copy used for the dashboard build). Before, it refused everything except
7.00 and 11.40. Shared save ZIPs record the sender's firmware and are accepted from
any valid firmware, so saves move between all of them.

| Firmware | Status |
|---|---|
| 11.40 | Tested on a console: compact and encrypted backups, sharing both ways |
| 7.00 | Tested on a console: compact backups, sharing both ways |
| Every other firmware the SDK supports | Implemented, **not tested on a console** |

On untested firmware the dashboard shows an "untested" notice and writes a warning
to Activity. The same safety rules apply on every firmware: a replace only runs
after the shared save passes the check on a temporary copy of this console's own
save in the same operation (using the console's real save mounting), the game must
be closed, and rollback copies of the original are kept. If a firmware handled
saves differently, the check on the temporary copy is expected to fail before any
live save is touched, but this has not been proven on other firmware.

Encrypted (legacy) whole-game backup and restore stays 11.40-only. On every other
firmware, backups always use the compact format and the setting is locked on. The
standalone legacy tools (pscloud-backup, -restore, -export, -upload, -download,
-share) keep their 11.40 checks; the dashboard is the supported path.

Verified: host tests, including packages from 1.00 to 13.60, rejection of malformed
firmware values, export and staged check while simulating firmware 9.60, and the
dashboard forcing compact backups and reporting "untested" on 9.60.
All 143 host tests and native builds passed in CI, and CI confirmed the dashboard
build's SDK copy contains values for firmware 1.00 through 13.60. Deployed on
2026-10-11 to both consoles: 11.40 and 7.00 each report v0.17.0 as tested firmware,
existing preferences were kept, and 7.00 reports compact backups as required. No
console on other firmware has run it yet.


## Version 0.16.0 — back up on game close, clearer receive

**Back up automatically when a game closes** (Cloud settings, off by default). While
the PSCloud dashboard runs on the PS5, a watcher checks the profiles' save
containers every 10 seconds. When a game's save files change, that game is marked;
once no save is mounted (the game has closed) and its files have been unchanged for
two checks (about 20-30 seconds), the normal backup runs for that game: compact
when Compact cloud backups is on, otherwise the encrypted whole-game backup (11.40
only). A PS5 notification confirms it, and it uploads in the background when
automatic upload is on. Only changes made while the option is on are backed up; it
waits while another operation runs and retries a failed backup up to three times.
Backups read staged copies and refuse a save that changes while being copied. The
setting is stored as GAME_CLOSE_BACKUP in preferences.conf (older files still load).
Detection relies on file timestamps and the mounted-save check; it has not been
verified with a running game yet.

**Receive a shared save** now accepts drag and drop, shows the package's game,
slot count, game version, sender firmware and size, marks steps as done/current/
upcoming with a "waiting N min" timer, and flags the browser tab when the save is
checked and ready.

Verified: host tests (including an automatic game-close backup on a changed save,
and no backup while the option is off) and a local preview of the new receive panel.
Deployed to both consoles: pages load without script errors, existing preferences
were kept, and game-close backup is available but off until enabled in Cloud
settings. A real game-close backup has not been observed on a console yet.

## Version 0.15.0 — guided receive for profiles without a save

"Receive a shared save" on the My games page accepts a sharing ZIP even when the
chosen profile has never played that game. The page reads the package's game from
the ZIP, then guides and waits:

1. Install the game (re-checked every 5 s).
2. Start it on that profile and play until it saves once (re-checked every 4 s).
   A per-game hint is shown when one is known; otherwise a general hint.
3. Close the game (re-checked every 3 s; a mounted save means it is still running).
4. The shared save is received and staged-checked automatically. Slots the game has
   not created yet are skipped automatically; import the same ZIP again later to
   add them. A version mismatch offers "Allow this game version and check again".

It then opens the game page with the checked save ready. Replacing the live save
still requires ticking "I have fully closed this game" and the replace
confirmation. The automatic check sends the game-closed flag only after the
console reports no mounted save; checks never change saves.

New read-only endpoints: `/api/users` (profiles and names) and `/api/save-status`
(installed, mounted, existing slots for one profile and game).

Per-game first-save hints live in `FIRST_SAVE_HINTS` in `ps5/ui.html`
(`'PPSAxxxxx':'when the game first saves'`). It is empty until games are tested;
report when a game first saves and it will be added.

Verified: host tests and a local preview of the full flow with simulated console
responses. Deployed to both consoles (v0.15.0, pages load without script errors);
`/api/users` lists every profile, including those without saves (11.40: two
profiles without a Crash save; 7.00: one), and `/api/save-status` reports their
empty slot lists. A real first-save receive has not been run yet.

## Receiver saves: findings (2026-10-10)

The receiving profile must already have a save for the game: start the game once
so the game creates its own save, close it, then import. PSCloud does not create
save containers, because that would mean handling the console's save encryption
outside the system's own save process. This is a deliberate decision, not a
pending feature.

Read-only findings on the 11.40 console (details in
[ps5/SAVE_CREATION.md](ps5/SAVE_CREATION.md)):

- Each PS5 save slot is an encrypted container
  (`/user/home/<user>/savedata_prospero/<title>/sdimg_<slot>`), one row in the
  per-user SQLite registry
  `/system_data/savedata_prospero/<user>/db/user/savedata.db`, and a menu icon in
  `savedata_prospero_meta`. The registry row mirrors the container's titles, size,
  account ID and user ID.
- `/system_data/savedata/<user>/db/user/savedata.db` is the separate PS4/legacy
  registry and is not used for PS5 saves.
- Sharing replaces container contents only and keeps the receiver's registry row,
  so the system menu may show the old slot title until the game saves again; the
  game loads the shared progress (Wolverine, 11.40 to 7.00, confirmed in-game).
- Save keys: every container is encrypted with a key that is sealed to the console
  that owns it. PSCloud only uses the system's own unsealing (see
  `ps5/common/mount.c`) to open an existing container on the same console, works
  on a staged copy, and never exports, imports or converts keys. Portable packages
  contain only game files; the receiver's container, sealed key and `sce_sys`
  metadata stay its own. This is why sharing works across consoles and accounts,
  and also why a receiver without its own save cannot be supplied one.
- When the game has created only some slots, "Skip save slots this profile does
  not have" imports the rest; missing slots can be imported later.

## Version 0.14.0 — simpler dashboard for sharing

- Library: search box, installed games listed first, plain wording ("2 save
  slots"), and games without installed app data are labelled "Game not installed ·
  saves only" instead of only showing a title ID.
- Game page: one "I have fully closed this game" confirmation covers backup,
  export, check and replace. The sharing section is open by default and split into
  "Send this save" and "Receive a save" with numbered steps.
- Receiving: "Check this save" uploads and runs the staged check in one step. When a
  check is refused for a fixable reason (missing slots, different game version) a
  "Fix and check again" button applies the matching option. Advanced options are
  folded under "Options".
- The finished-upload notice hides itself after 15 seconds.

Verified in a local preview with the 11.40 console's real game list, then deployed
to both consoles: both dashboards report v0.14.0 and load without script errors;
11.40 lists 25 game cards (14 labelled not installed), 7.00 lists Crash. The new
receive flow has not yet been run end to end on a console. Save-container creation research:
inspection of /system_data/savedata/<user>/db/user/savedata.db was not performed.

## Version 0.13.0 — universal game-data sharing options (host-tested only)

Sharing moves only game data; the receiver keeps its own save keys and metadata,
so the same path is used for every game with no per-game code. This release
removes the remaining blanket refusals, each behind an explicit choice:

- Slot selection in the dashboard export (untick large replay/ghost slots).
- "Skip package slots this profile does not have": imports the slots the receiver
  has and leaves the rest; refused when none match.
- "Allow a package from an older or unknown game version": a package from a newer
  game version is always refused ("update the game first").
- A smaller receiver container no longer refuses up front; the data is written into
  the disposable copy and the import is refused only if it does not fit.
- Saves of games that are not installed on the source can be exported; the package
  records GAME_VERSION=unknown and the receiver must allow it explicitly. The
  receiver must have the game installed.
- Refusals now return the specific reason to the dashboard instead of a generic
  message.

Still required: the receiver profile must already have a save for the game (create
one by playing once); creating new save containers is not implemented. Game-level
account binding can still stop a save from loading. Verified so far: host tests
for these options plus these 2026-10-10 console checks: v0.13.0 deployed on 11.40
and 7.00 and both dashboards render. A three-slot Crash package from 11.40 imported
on 7.00 (two slots) was refused by the strict check with the specific reason
"no save slot PlayerSaveSlot3Save", and passed the staged check with "skip missing
slots" (live saves unchanged). Hades, whose game is not installed on 11.40, now
exports. No live restore was performed by the developer tooling; live restore and
in-game loading are left to the user.

User-confirmed on 2026-10-10: a Marvel's Wolverine save shared from the 11.40 console
to the 7.00 console with v0.13.0 restored live and works in-game. This is the first
in-game confirmation of the generic live restore path; it is evidence for this game
and this pair of consoles, not a guarantee for every game.

## Version 0.12.5 — fix blank dashboard in 0.12.4

Version 0.12.4 shipped a JavaScript syntax error in the busy-banner code, which
left the whole dashboard page blank on both consoles. Fixed, and a host test now
syntax-checks the complete embedded script. Page load verified in a browser after
deployment (see below if recorded); live restore remains unverified.

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
