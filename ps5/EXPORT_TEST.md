# FW 11.40: manual mounted-save export with PS5 notifications

## What this payload does

pscloud-export.elf runs on the console, shows PS5 notifications, writes timestamped
logs to /data/pscloud.log, and copies game payload files from an explicitly mounted
save into a uniquely named ZIP under /data/pscloud/spool/. Successful archives end
in .zip.ready; failed archives are removed, or left .part after an interruption.
The .ready suffix is for our future queue consumer, not a claim of cloud upload.

This payload does not mount or unmount saves itself, restore files, auto-detect
game closure, upload anything, or provide cross-console save compatibility. It
runs only on firmware 11.40 for this development test. Do not use an encrypted
/user/home/... save image as SOURCE: it requires the decrypted mounted directory.

## Download and prepare

1. Run `git pull --ff-only` in your clone.
2. Open GitHub Actions -> Build PSCloud payloads -> latest successful build.
3. Download and extract the pscloud-export artifact.
4. Place pscloud-export.elf in your local tools folder.
5. Choose one game's save in a working PS5 Save Mounter session. Close the game
   BEFORE mounting. Mount that save and note the exact /mnt/pfs/... directory.
   Keep the mounter connected and the save mounted throughout export. Its client
   disconnection can auto-unmount; the exporter does not own that session.
6. Copy ps5/pscloud-export.conf.example to a local file named pscloud-export.conf.
   Set SOURCE to the actual mount directory and TITLE to that game's PPSA ID.
   CONFIRM_GAME_CLOSED=yes is your confirmation, not an automatic safety check.
7. Upload that configuration via your usual FTP tool to /data/pscloud-export.conf.

Save Mounter reference and instructions:
https://github.com/n0llptr/Playstation-5-Save-Mounter
Compatibility of its mount operation on your own 11.40 setup is still a hardware
validation gate. This exporter assumes you have obtained a valid mount. If you
have not mounted a save yet, stop at step 5 and report the mounter result.

## Send from Windows CMD

Run from the cloned repository folder, substituting your exact console IP:

```cmd
py tools\send_payload.py YOUR_PS5_IP tools\pscloud-export.elf --port 9021 --connect-timeout 30 --timeout 60 --reply-timeout 30
```

PS5 notification sequence:
- Save export payload started
- Exporting PPSAxxxxx - please keep the save mounted
- Export progress (every 16 MiB or 25 completed files on larger exports)
- Export complete: N files - backup saved locally

Missing config, unavailable mount, copy failure or queue commit failure produces
an error notification instead. Notifications require the PS5 notification API
and your console's notification settings; the same events also go to stdout and
the log. If no start notification appears, successful transmission alone does
not prove execution. Inspect the sender's loader reply.

## Verify and report

Download /data/pscloud.log and the new .zip.ready file through FTP. On a computer,
Python can verify the ZIP CRCs without extracting it:

```cmd
py -m zipfile -t "FULL_PATH_TO_THE_DOWNLOADED.zip.ready"
```

Keep an independent original-save backup before later restore testing. The ZIP
contains decrypted save payload and should remain private; send only the log
here unless we specifically need a save sample. Never commit save files or logs
to the public GitHub repository.

Unmount in Save Mounter after export finishes. Original game payload files are
opened for reading only by this exporter. Copying is not a transaction with the
console's save system: per-file stat and directory-change checks detect many
concurrent changes but do not establish an atomic snapshot. A closed game and
stable mount are required. Empty directories are omitted. Root sce_sys is
excluded to avoid copying destination-specific metadata into future restores.

Limits: ZIP32, no compression, at most 4096 regular files, 32 nesting levels,
2 GiB archive, 1023-byte relative paths. Symlinks, special files, backslashes and
control characters in names are rejected. TLS/cloud integrity checks and restore
validation are separate work; ZIP CRC32 detects accidental corruption and is
not authentication. The host test suite exercises export, metadata exclusion,
version preservation, invalid configuration, path handling and failure cleanup.
