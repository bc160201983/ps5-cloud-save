# PSCloud dashboard 0.5.0

Download the `pscloud-dashboard` artifact from a successful build on `main`.
Deploy its CA bundle to `/data/pscloud-ca.pem` if not already present. Send
`pscloud-dashboard.elf` to the PS5 ELF loader, then open `http://PS5-IP:8082` from
a phone or browser. Enter the per-run pairing code shown in the loader output or
console notification. A URL fragment `#token=CODE` also pairs the page; it is
removed from the browser URL after loading. No token is placed in query strings.

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
