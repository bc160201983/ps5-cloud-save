# Local profile candidate — 0.34.0

Date: 2026-10-12. These are the local checks run before the candidate was pushed
to GitHub for its Actions build. Neither PS5 has been updated by this document.

## Implemented

- Save discovery by exact values or increased/decreased/changed/unchanged fields.
- Named save profiles with masked context, game/version checks, JSON import/export,
  later-save relocation, and Set or signed adjustment queued for review.
- Known game checksum repair plus ZIP CRCs and package hashes; encrypted `DSSS`
  saves are refused. Unknown checksums require an explicit warning acknowledgement.
- Live **Retain profile** and **Relocate & bind**. Retained rules survive PSCloud
  and game restarts; session addresses and freeze state do not. Every rule must
  match once before binding. Binding reads memory; changing/freezing stays explicit.
- Console profile storage with revision checks; stale browser writes are refused.
- Save editing refuses to proceed to its staged check/replacement when game
  closure cannot be verified. Profiles contain data, not executable scripts.

## Passing focused local checks

| Test group | Result |
|---|---|
| Live C engine, batches, freeze, process restarts and signature relocation | 10 passed |
| Save-profile relocation/discovery/bounds and real package checksum repair | 2 passed |
| Browser syntax, request behavior and saved-value controls | 3 passed |
| Profile storage and full Live export/restart/relocate/version/generation HTTP flow | 3 passed |

The C dashboard compiled with `-Wall -Wextra -Werror`. Test saves and memory are
synthetic fixtures, not user saves. A portable MSYS2 toolchain lives under the
ignored `build/host-tools` folder; the editor JavaScript was tested with local Node.

## Broader test limitations

The full host suite is **not green on MSYS2**: 139 tests ran, with 43 failures,
20 errors and one skipped test. Some test classes cannot run in this environment.
This result is not a passing full regression check.

Three representative backup/upload/save-creation failures were reproduced on an
isolated copy of unchanged commit `805aca7`, using the same TLS fixture adjustment.
This demonstrates those three failures are not caused by the profile changes; it
does not establish the cause of every other failure.

Logs remain locally in `build/local-profile-suite.log`,
`build/baseline-profile-tests.log` and `build/final-profile-http-tests.log`.
Full Linux regression testing and PS5 payload build/in-game verification are pending.

## What retention means

The library keeps locator rules in `/data/pscloud/editor-profiles.json`, not old
addresses. After a game restart, choose **Relocate & bind**, inspect current values,
then explicitly Set, adjust or Freeze. Missing/ambiguous/weak signatures stop.
Profiles can be refined with another verified capture under the same name.

This is a general profile engine, not guaranteed support for every save layout or
game. Memory context can change and signatures can stop working. Unique matching
does not prove a field's meaning. Verify profiles in-game across restarts before
sharing them as tested. A bigger saved money balance is not infinite money.
