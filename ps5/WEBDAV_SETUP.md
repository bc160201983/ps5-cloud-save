# Set up WebDAV storage for the first PS5 test

Use a hosted Nextcloud account with HTTPS and WebDAV access. For a small trial,
Nextcloud lists hosted providers at https://nextcloud.com/sign-up/ . Check your
chosen provider's quota and WebDAV access before signing up. No self-hosted server
is required, and this setup does not route PS5 uploads through your PC.

1. Create an account and sign in to its Nextcloud web interface.
2. In Files, create a folder named `PS5Backups` in your account root.
3. Open Files settings and copy your personal WebDAV URL. Use the URL shown by
   your provider; the WebDAV username can differ from your email address.
4. In Personal settings -> Security, create an app password named `PSCloud`.
   Keep it private and copy it directly into your local upload configuration.
5. Download and extract the `pscloud-upload` artifact from a successful run on
   the `codex/direct-cloud-upload` branch. Copy the included example to a private
   file named `pscloud-upload.conf`, with plain UTF-8 encoding and no BOM.
6. Set URL to your personal WebDAV URL with `/PS5Backups` appended. Avoid a
   trailing slash. For example:

   `https://cloud.example.com/remote.php/dav/files/YOUR-USER/PS5Backups`

7. Set USER to your Nextcloud username, PASSWORD to the app password,
   CA_BUNDLE to `/data/pscloud-ca.pem`, and MODE to `once`.
8. Upload the included `pscloud-ca.pem` and your config to `/data/` on the PS5.
   Follow UPLOAD_TEST.md to run the ELF and verify a round-trip download.

Do not paste your password or upload the completed configuration to GitHub.
Share only your provider name and non-secret server URL when requesting help.
Keep local backups while validating the service; free accounts may have limited
quotas and retention policies. Cloud acceptance does not verify game restoration.

Official instructions:
https://docs.nextcloud.com/server/latest/user_manual/en/files/access_webdav.html
