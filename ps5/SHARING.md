# Manual Crash portable sharing

## 0.12.0: generic dashboard sharing

Open either firmware 7.00 or 11.40 dashboard, choose a game, and expand
**Share saves · another PS5 or profile**. On the source, close the game and
choose **Export for sharing**. Send the downloaded portable ZIP to the receiver.
On its dashboard, choose the same game and desired profile, **Import package**,
confirm game closure and **Check compatibility safely**. Only after a successful
staged check and a separate replacement confirmation can **Restore shared save**
replace included slots. This requires no WebDAV account and does not upload
personal progress publicly. Export/check never replace live saves.

The generic format packages all discovered slots as stored ZIPs containing
regular game-data files and nested folders. It excludes root sce_sys, preserves
recipient account metadata and keys, verifies CRC and slot hashes, refuses
traversal/duplicate/conflicting paths and requires identical installed
contentVersion/PPSA ID. Slots must already exist with at least the source image
capacity; extra receiver slots stay unchanged. 128 slots, 4096 files per slot,
512 MiB package/combined receiver-image limit. Empty directories are not encoded.
System-memory slots (sce_sdmemory) require a valid SFO with no normal identity
fields, and are identified by the selected existing local game/user directory,
installed version and original key; their metadata stays untouched. Normal slots
still require title/slot/account SFO fields. There is no per-game allowlist.
Cloud version history is loaded on demand so local sharing does not wait for
WebDAV metadata calls.
Automatic save creation, cross-region conversion, version upgrade/downgrade and
game-internal account-binding conversion are not implemented. Passing staged
checks is not a universal guarantee of in-game loading.

Before live replacement, every original is rechecked and rollback is retained
under /data/pscloud/portable-stage-<id>/before-N.img. .restore-active records
user/title/package digest, slot names, original hashes and stage location.
Partial failures attempt verified rollback; uncertain recovery or unmount keeps
its safety marker and blocks new operations. Multiple renames are not atomic
across power loss. Never blindly clear safety markers. Automatic recovery is
not implemented. Dashboard sharing on both firmwares is new and initially
unverified on hardware; representative staging tests do not certify every game.

The old three-file Crash format is separate and is not accepted by the new
generic dashboard importer. Re-export using the new dashboard for sharing.
Ordinary cloud backup/restore on firmware 7.00 remains disabled; the new sharing
export/import/restore path accepts both supported firmwares.

Advanced staged-only tool pscloud-share-game.elf reads /data/pscloud-share-game.conf:
MODE=export or check, USER_ID=<local hex>, TITLE=<PPSA ID>,
CONFIRM_GAME_CLOSED=yes, and PACKAGE=<generic ZIP filename> for check.
It has no live-restore mode. It logs to /data/pscloud/share-game.log.

## Historical Crash-specific tool

## 0.11.9: requested direction, 7.00 to 11.40

Export and check modes accept both 7.00 and 11.40. Restore is deliberately gated
to 11.40. This release adds a manual live restore path, not dashboard sharing.
The new live path has not been console/game-load verified. Do not treat the
older staged reverse-direction test below as proof that it works in-game.
All 114 host tests and native builds passed. Native 7.00 export and 11.40 staged
import passed on 2026-10-09, including both clean unmounts, payload checksum,
unchanged recipient metadata/key and unchanged live-image checks. No live
replacement was run. Receiver configuration was left in check-only mode.

1. Close Crash on both consoles. On the 7.00 source, use MODE=export with its
   own USER_ID. Send the sharing ELF and require success in share.log.
2. Copy the resulting portable ZIP (not encrypted containers) into the 11.40
   receiver's /data/pscloud/share/. Calculate the entire ZIP's SHA-256 and verify
   the transferred file matches. Keep an independent backup of both consoles.
3. On 11.40, use MODE=check with its own USER_ID and PACKAGE filename first.
   Check must succeed, unmount cleanly and report LIVE_SAVES_UNCHANGED=yes.
4. Only when YOU intend to replace the live primary progress/profile, set
   MODE=restore, the same recipient USER_ID and PACKAGE, PACKAGE_SHA256=<exact
   lowercase 64-character digest>, CONFIRM_GAME_CLOSED=yes and CONFIRM_RESTORE=yes.
   Sending the sharing ELF with this configuration performs live replacement.
   Never deploy that configuration casually or leave it enabled for later runs.

The receiver must already have both primary containers for PPSA02433. Extra
slots remain untouched. This does not register deleted saves or migrate other
games. Confirm installed game versions match before transfer. Account metadata
and sealed keys stay with the receiving console/profile.

Both replacements and verified rollback images are prepared before commit.
The tool checks original file identity/checksums again, writes .restore-active,
then atomically replaces each container individually and checks its checksum.
The pair is not power-loss atomic: interruption between renames requires recovery.
On a detected partial failure, it attempts rollback and verifies restored hashes.
An uncertain recovery leaves .restore-active in place and blocks further work.
Do not clear safety markers or retry blindly. Rollback files are retained in the
recipient title directory as .pscloud-<id>-<slot>.rollback; the journal records
their names and original hashes. Automatic crash recovery and a rollback UI
are not implemented. Never delete these copies until the game-load check passes.

## Historical 0.11.8 staged pilot

This is a staged compatibility pilot, NOT live cross-console restore yet. It
supports Crash Bandicoot 4 (PPSA02433), primary progress slot PlayerSaveSlot0Save
and PlayerSaveProfileSaveData. Other source slots are not transferred. Existing
recipient containers are required; they are not created, registered or replaced.

Export on firmware 11.40 creates a stored ZIP containing only progress.dat,
profile.dat and a canonical manifest with game, source firmware, creation date
and SHA-256 digests. No console keys, encrypted images, sce_sys, profile/account
metadata or cloud credentials are included. Both source SFO identities must
match the expected game/slots and the same source account. Maximum payload size
is 32 MiB per slot. Unknown entries, path traversal, compression, extra metadata,
corruption and noncanonical manifests are rejected.

Check on firmware 7.00 or 11.40 writes payloads into staged copies of the
receiver's own containers. Its existing SFO is checked before/after; ownership,
permissions and sealed keys are preserved. Original image hashes are checked
afterwards. There is no live-container commit or database update in this pilot.
Game closure is mandatory. A failed/stalled unmount must retain its safety marker
and stage for inspection; never blindly clear a marker or power off.

Configuration goes in /data/pscloud-share.conf using the example. For export,
MODE=export, USER_ID=<source hex user>, CONFIRM_GAME_CLOSED=yes. For check,
MODE=check, USER_ID=<recipient hex user>, PACKAGE=<portable ZIP filename>, and
CONFIRM_GAME_CLOSED=yes. Put the complete verified package under
/data/pscloud/share/. Send pscloud-share.elf through the existing loader. Logs are
in /data/pscloud/share.log. Success reports LIVE_SAVES_UNCHANGED=yes. Keep the
staging data until compatibility is reviewed. No public link or cloud upload is
performed automatically. Sharing packages contain personal game data: share
only deliberately and only with trusted recipients.

The firmware 7.00 read-only diagnostic successfully enumerated save folders.
All 112 host tests and native builds passed. Native source export on 11.40 and
recipient staged import on 7.00 passed on 2026-10-09 with the same installed
Crash version on both consoles. Live images stayed unchanged; recipient SFO and
sealed keys were preserved and both mounts unmounted successfully. No live
recipient restore or game-load test was performed. Dashboard sharing stays
unavailable until its integration and an explicitly authorized recipient restore
are implemented and tested.
