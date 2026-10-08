# Direct PS5 cloud upload: first transport test

This is an initial FW 11.40 WebDAV upload payload. Google Drive authentication,
automatic game-exit export, downloads and restore remain separate milestones.
No PC relay is used by the payload. A PC is only used to configure and send it.

## Build prerequisites

Install the PS5 payload SDK and its matching PS5 curl/OpenSSL libraries from
https://github.com/ps5-payload-dev/pacbrew-repo . The base SDK alone is insufficient.
The libraries must provide `bin/prospero-curl-config` under PS5_PAYLOAD_SDK.
Build on Linux with `make -C ps5 upload`. This optional target is not included
in the base-SDK GitHub artifact build yet. Run `make test` for host verification.
The console build and runtime still need validation with those libraries.

## Console test

1. Use an existing HTTPS WebDAV folder and a dedicated app password. Google
   Drive does not provide this interface; its URLs will not work here.
2. Copy `pscloud-upload.conf.example` to a private local `pscloud-upload.conf`.
   Fill in URL, USER and PASSWORD. Keep MODE=once for the first test.
3. Obtain the trusted CA bundle distributed with the curl/OpenSSL libraries
   (or your provider's trusted private CA), and upload it to
   `/data/pscloud-ca.pem`. Certificate verification stays enabled.
4. Upload the config to `/data/pscloud-upload.conf`, restricting access to your
   console and FTP session. Never send passwords in logs or chat.
5. Confirm the exporter-created `.zip.ready` archives are present in
   `/data/pscloud/spool`. Keep independent copies of your tested saves.
6. Send `pscloud-upload.elf` using `tools/send_payload.py`. Inspect the loader
   output and `/data/pscloud.log` for execution; transmission is not execution.
7. Check that accepted jobs become `.zip.sent` locally and appear as `.zip`
   objects in your cloud folder. Download an object and compare SHA-256 with
   its original archive. A server acceptance notification alone does not
   establish byte integrity or game restoreability.
8. Later, MODE=watch polls the ready queue with retries while the process lives.
   Background lifetime, restart behavior, and rest mode are unverified. It does
   not automatically mount or export saves after closing a game.

On failure ready archives remain available for retry. Local sent archives are
retained. This transport does not implement remote immutable-object enforcement
or committed manifests. Do not treat the cloud folder as a verified restore list.
