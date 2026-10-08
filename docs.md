# Integration design (planned, not implemented)

## Console lifecycle

A platform adapter owns title/user discovery, game-exit notifications, save
mounting, and unmounting. Polling stable file sizes is not proof a save has finished.
The first implementation should wait for the game to exit and use a verified
mount/export path. Every failure path must unmount and release resources.

After obtaining an exclusive consistent snapshot:
1. Export game payload into private staging without altering the original save.
2. Build an archive and manifest containing schema version, console UUID, local
   user identity, title ID, game version, save slot, snapshot UUID, parent snapshot,
   payload paths, sizes, and SHA-256 checksums. Exclude credentials.
3. Flush archive bytes and fsync the file, then rename .part to .zip.ready on the
   same filesystem and fsync the spool directory. Never change it after publication.
4. The worker uploads the object and retains the local .sent archive.

A later provider implementation must upload a commit manifest last. Uncommitted
objects must not be shown as restorable backups. UUIDs and parent relationships,
not timestamps alone, identify progress/conflicts across consoles.

## Provider interfaces to add

put_immutable(snapshot), commit(manifest), list_committed(title, slot),
download(snapshot), verify(snapshot). Never implement automatic remote deletions
or last-write-wins restoration. WebDAV is the initial transport implementation;
Google Drive needs its own OAuth refresh-token lifecycle and Drive file IDs.
Use a phone/PC browser only for initial account authorization if necessary; routine
uploads should originate from the console. Check provider OAuth policies before
choosing redirect/device authorization flow.

## Restore pipeline

User chooses a cloud version; validate archive size limits, safe paths, manifest
and checksums; confirm title/slot compatibility; back up the destination first;
mount destination through the verified platform adapter; import supported payload
files while preserving destination-specific metadata; unmount; test game loading.
Do not infer that common cloud credentials mean PS5 users have matching identities.
Cross-console import remains an unverified capability until tested on two consoles.

## Firmware gate

Record exact firmware, jailbreak, ELF loader, user setup and working save mounter.
Build a small read-only enumeration probe with the actual SDK before implementing
platform calls. No offsets, lifecycle hooks, or mount APIs are invented in this
project. libcurl, TLS trust-store availability, flock/fsync support and background
process lifetime must be established on the PS5 target. Host tests do not establish
PS5 binary or ABI compatibility.

## Sources checked on 2026-10-08

- https://github.com/n0llptr/Playstation-5-Save-Mounter
- https://github.com/ps5-payload-dev/sdk
- https://curl.se/libcurl/c/CURLOPT_UPLOAD.html
- https://docs.nextcloud.com/server/latest/user_manual/en/files/access_webdav.html

No upstream source code is bundled or copied. Review upstream licensing before
incorporating an existing save mounter or SDK implementation.
