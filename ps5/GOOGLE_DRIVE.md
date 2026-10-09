# Google Drive integration status (0.11.0)

Native **device sign-in only** is implemented, not Google backup transfers.
WebDAV remains active. Do not migrate/delete existing archives for this preview.

## Developer registration

1. Create a Google Cloud project owned by the PSCloud maintainer.
2. Enable the Google Drive API and configure the consent screen, support contact,
   privacy policy and applicable public-release/verification settings.
3. Create OAuth credentials of type **TVs and Limited Input devices**. The device
   flow supports `https://www.googleapis.com/auth/drive.file`.
4. In Cloud Settings, open the Google preview developer setup and enter the client
   ID and optional client secret. Never enter a Google account password. During
   development, test users must match the project's publishing settings.
5. Choose Connect Google Drive, open Google's approval page in a normal phone/PC
   browser, enter the temporary approval code and approve. Choose Check approval
   after Google's poll interval. This is Google authorization, not dashboard
   pairing. No central PSCloud token relay/server is used.

Normal users should not create developer projects. A maintainer-registered public
application and end-to-end tests are needed before release as a supported provider.
Provider switching is unavailable in this milestone.

## Storage and safety

Refresh credentials are stored only in `/data/pscloud/google.conf`, with 0600
permissions, no-follow regular-file checks and atomic replacement. Access tokens
are memory-only. Missing/unsafe files fail closed. A new login replaces saved
authorization only after approval and successful persistence. Denial/expiry/error
does not change WebDAV credentials or saves.

The dashboard is HTTP: use a trusted private network, never expose it to the
internet. Refresh tokens are sensitive. Permissions do not protect them against
other privileged console payloads. Keep credentials out of Git and public logs.

## Remaining work

- Connect provider selection to queue, backup/import/download/restore routes.
- Paginated folder/file lookup with game/user/snapshot metadata.
- Bounded resumable uploads, restart recovery, retry/backoff and progress.
- Verify complete remote archive checksum before marking a queued copy sent.
- Verified downloads and unchanged destination/rollback restore protections.
- Disconnect/revoke, public application registration and real login tests.
- Console tests of threading, large transfers and authentication.

The WebDAV worker already retains failed uploads for a later explicit retry.
Automatic background retries/resumable WebDAV PUT are not added. Game-close
watching and portable cross-console sharing remain unavailable.

Official references:

- [Device authorization](https://developers.google.com/identity/protocols/oauth2/limited-input-device)
- [Drive scopes](https://developers.google.com/workspace/drive/api/guides/api-specific-auth)
- [Resumable uploads](https://developers.google.com/workspace/drive/api/guides/manage-uploads)
