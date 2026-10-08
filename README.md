# PS5 Cloud Save — development milestone 0.3

This is the first tested component of a proposed standalone PS5 cloud-save app.
It is NOT an installable PS5 application or a complete save-sync implementation.
PS5 ELF binaries are available as GitHub Actions artifacts. The top-level upload-worker executable is for the host OS; it is not yet a PS5 cloud service.

## Added in 0.3

- Controlled recovery source for the verified PPSA02433 single-file save format:
  HTTPS download with a pinned SHA-256 and restore after destination backup.
  See [ps5/RESTORE_TEST.md](ps5/RESTORE_TEST.md). Game-load validation is pending.

- Next transport work: optional `pscloud-upload.elf` target for direct PS5
  HTTPS/WebDAV uploads, local configuration and queue watching. Requires separate
  PS5 curl/OpenSSL libraries; console build/runtime remain unverified.
  See [ps5/UPLOAD_TEST.md](ps5/UPLOAD_TEST.md). Google Drive is still pending.

- PS5 notifications for start, progress, completion and failure in our diagnostic
  and new mounted-save export payload; timestamped logs in /data/pscloud.log.
- Firmware 11.40 manual export of an already-mounted save to an uncompressed ZIP.
- Root sce_sys exclusion, archive CRCs, unique backup names and failed-copy cleanup.
- The GitHub build produces separate pscloud-probe and pscloud-export artifacts.
- See [ps5/EXPORT_TEST.md](ps5/EXPORT_TEST.md) for the next console test.
- Firmware 11.40 diagnostic enumeration has been confirmed on the user's console.
  The export payload still needs a real mounted-save test. Firmware 7.00 is deferred.

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

Automatic console save mounting/export; game-exit detection; cloud commit
manifests and authenticated integrity verification; cloud listing/download;
Google Drive OAuth and API; cross-console import; conflict selection; UI;
rest-mode operation; automatic startup. Cloud transport tests use a local mock
server; no live cloud-provider upload has been tested. Google Drive does not expose this WebDAV API and cannot be used by
setting its URL in this worker.

## Build and test on Linux / Windows WSL

Install a C compiler, make, libcurl development headers (libcurl >= 7.85), Python 3,
and OpenSSL CLI. On Ubuntu these are build-essential libcurl4-openssl-dev python3
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
