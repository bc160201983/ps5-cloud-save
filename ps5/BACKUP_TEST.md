# Console-managed staged backup: firmware 11.40

This adapter selects a specific user, title and save name. It opens the original
encrypted image read-only, copies it to a unique staging directory under
`/data/pscloud/staging`, checks that source size and timestamps stayed stable,
mounts that copy, exports game payload (excluding root sce_sys), and checks
unmount success before publishing `.zip.ready`. It never copies the image back
to `/user/home` and never edits or deletes the original. Source stability checks
are not proof that a running game has finished writing: close the game first.

The initial selection is limited to the tested PPSA02433 slot/profile names.
Each queue archive gets a local `.identity` sidecar recording the exact user,
title and save name. Cloud manifests and account/slot-aware cloud discovery are
still pending. The existing uploader processes ready ZIPs, not identity files.

## Hardware test

1. Close the game, unmount saves in Save Mounter and leave no other save mounted.
2. Keep independent backups. Download the `pscloud-backup` Actions artifact.
3. Fill in the configuration example, confirm the exact encrypted save name,
   set CONFIRM_GAME_CLOSED=yes, and upload as `/data/pscloud-backup.conf`.
   `/data/pscloud` must already exist from earlier export tests.
4. Send `pscloud-backup.elf` to the PS5 ELF loader. No Windows Save Mounter is
   involved in the mount lifecycle. Inspect the loader output and pscloud.log.
5. Check the produced ready archive's CRC and compare its payload with your
   independent original. Run the existing uploader to transfer it to WebDAV.

No automatic game-close detector, automatic payload startup or rest-mode support
is included yet. This is a manual trigger for a console-owned backup lifecycle.
The platform ABI, privilege calls and pfsmgr operation require hardware testing.
Privilege changes are restricted to the payload process and restored on exit.
Unmount failure prevents queue publication and leaves the staged image intact;
do not remove it or start another mounting tool until that mount is resolved.
Failed staging directories are retained for diagnosis, not automatically pruned.
An active-mount marker prevents another backup after a crash or failed mount/
unmount. It is cleared only after checked unmount or before a mount was attempted.
Do not clear `/data/pscloud/.mount-active` without inspecting the recorded mount.

The adapter was independently written using SDK interfaces and inspecting
Garlic SaveMgr's API usage. No Garlic implementation or UI was copied.
References:
https://github.com/ps5-payload-dev/sdk/blob/v0.43/include/ps5/kernel.h
https://github.com/earthonion/garlic-savemgr/blob/main/src/main.c
