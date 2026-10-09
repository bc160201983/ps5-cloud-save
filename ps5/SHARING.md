# Crash portable sharing pilot

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
