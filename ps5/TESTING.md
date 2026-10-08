# Console diagnostic — firmware 7.00 and 11.40

This source targets ps5-payload-dev/elfldr on TCP 9021. The diagnostic does not
patch privileges, mount saves, read save bytes, contact cloud services, or modify
save files. It writes one report: /data/pscloud-probe.txt (overwritten each run).
It enumerates visible user and game directories, anonymizing user IDs. The report
contains title IDs but no account names, save contents or encryption keys.

If the loader's existing permissions cannot access /user/home, the report will
say unavailable. This is useful evidence, not permission to add guessed offsets.

## Build on Ubuntu / WSL

Install the official ps5-payload SDK per its README:
https://github.com/ps5-payload-dev/sdk

With clang-18 and lld-18 installed and the SDK unpacked:

```sh
export PS5_PAYLOAD_SDK=/absolute/path/to/ps5-payload-sdk
make -C ps5
```

Alternatively place this project's contents at the root of your GitHub repository
and run the included “Build PSCloud diagnostic” workflow. Its artifact should be
pscloud-probe.elf. The workflow is supplied but has not been run in this session.
No repository was created or modified remotely.

## Run on each console

1. Load your jailbreak and ps5-payload-dev/elfldr as usual.
2. Close games for this initial diagnostic.
3. Send the SDK-built ELF from Windows PowerShell (Python installed):

```powershell
py tools/send_payload.py YOUR_PS5_IP ps5/pscloud-probe.elf
```

Use the real console IP. Do NOT send cloud-worker or probe-host: those are Linux
host executables, not PS5 payloads. ELF header checks alone do not prove the right
platform. Sender success means only that bytes were transmitted.

4. Using your existing PS5 FTP tool, retrieve /data/pscloud-probe.txt. Download it
   separately for each console and label the copies probe-7.00.txt and probe-11.40.txt.
5. Confirm firmware fields match. If the report is absent, report the loader's
   error/log instead; do not assume the ELF ran.

The expected firmware field is 7.00 or 11.40. Host tests deliberately report 0.00.
Successful enumeration does not prove save-mount or restore compatibility.

## Next integration gate

Use one disposable game save to validate a compatible mounter on each firmware.
The currently inspected Save Mounter exposes TCP commands on port 9090 and can
mount/write saves; the diagnostic intentionally does not call those commands.
Disconnecting its client can trigger auto-unmount, so future integration needs
exclusive ownership and careful session lifetime rather than ad-hoc polling.
After export/import is proven, connect consistent archives to the cloud queue.
