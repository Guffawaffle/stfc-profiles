# Shared profile catalog and launch contract

Status: accepted product decisions, 2026-09-29. This is the canonical contract
for the shared catalog, identity, storage, archive lifecycle and CLI direction.
It records the accepted target; the implementation gaps below remain open.
STFC Profiles owns this contract. Bridge and the community mod consume it.

## Terms and ownership

A **profile** is a saved account identity and its preferences. A **session** is
a running game process using a profile. Names are presentation; the immutable
profile ID connects the catalog, account preferences, runtime exclusion and
shortcuts. It is generated once at creation and preserved through rename,
installation changes, archive, restore and backup. It is not an encryption key.
New IDs use the existing generated form: 32 lowercase hexadecimal characters.

The `stfc-profiles` component owns catalog discovery and mutations. The CLI and
Bridge use the same shared operations and metadata contract. Bridge owns its
UI preferences, including its last selected profile; those preferences cannot
select an account for another launch path. Installation choice is launch
metadata, separate from account identity. Several profiles may launch the same
canonical game executable; a preferred installation may be overridden explicitly.

## Per-user storage

Windows root: `%LOCALAPPDATA%\STFC Profiles`.
macOS root: `~/Library/Application Support/STFC Profiles/`.
These are OS-user data locations, independent of the game installation and of
which distribution supplies the runtime. macOS protection and loading still
require implementation and qualification.

```text
STFC Profiles/
  profiles/
    <immutable-profile-id>/
      metadata.json
      player_prefs.bin
      logs/
  archives/
    <immutable-profile-id>/
      metadata.json
      player_prefs.bin
      logs/
```

The directories themselves are the catalog. There is no central `registry.json`
or Bridge-owned account-profile index. `profiles/<id>` is active;
`archives/<id>` is archived. Discovery enumerates profile directories and
validates their metadata; invalid or incomplete entries are reported explicitly.
An ID present in both locations is a conflict, never permission to overwrite.

Each profile has versioned, plaintext `metadata.json` containing its editable
display name and optional preferred game installation. The directory name is
the ID authority. Login tokens and account preferences remain outside metadata
in the protected preference store. Profile-specific logs and other owned data
travel with the directory; exact configuration integration remains open.

Windows preference encryption remains bound to the OS user through DPAPI.
Preserving an ID keeps identity intact; it does not make encrypted account data
portable to arbitrary OS users. Backup and restore preserve the ID and complete
directory, including the preference store's lifecycle and recovery data.

## Catalog and session lifecycle

- Create generates an ID and initializes a new profile directory. Publication
  of a valid catalog entry must distinguish completed creation from interruption.
- List discovers active profiles. `list --archived` discovers archived profiles.
- Rename updates metadata while preserving the directory ID and account data.
- Launch opens an active ID explicitly. An archived profile must be restored first.
- Archive moves the complete directory from `profiles` to `archives`.
- Restore moves the complete directory back, preserving the same ID and contents.
- Permanent deletion is a separate, explicit operation. Archive never deletes
  account data. No operation overwrites an existing destination profile.

Archive, restore and permanent deletion require the profile's session to be
stopped. Checking inactivity and changing directory state form one excluded
operation, coordinated with launch admission. The exclusion namespace must stay
stable across directory moves and be keyed by immutable identity; placing the
only coordination lock inside a movable directory is insufficient.

The runtime holds per-profile writer exclusion for the full session lifetime,
independent of Bridge or the CLI remaining open. Two different profiles may run
concurrently; two sessions must never write the same profile. The catalog records
metadata, not a persistent `running` flag. Sessions are discovered from live
runtime evidence with process identity and readiness, rather than a stored PID
being accepted as ownership by itself.

Shared operations serialize conflicting metadata edits, detect stale updates
and commit writes through durable temporary files and atomic replacement.
Unresolved interrupted operations, unsupported metadata and corrupt or missing established
preferences report explicit failures. They never silently recreate an empty
account, select another ID or use a second catalog as fallback. Filesystem and
crash behavior must be verified on supported platforms during implementation.

## Launch and CLI direction

Installing capability does not activate a named profile. Ordinary `prime.exe`
and official-launcher launches retain ordinary OS-user preferences. GUI selection
and other profile activity never redirect a bare launch. An explicit profile
request must isolate the exact requested ID or stop with guidance.

On Windows the game-host request is `prime.exe -stfc-profile <id>` using separate
argument tokens. Store lifecycle modes `New`, `Resume` and `Existing` remain
explicit host inputs; the host must not silently create a replacement for a
missing established store. Profile selection must occur before login state is
read. Isolation readiness and a correct logged-in account are separate observations.

The executable name and initial CLI forms are agreed direction, not commands
implemented in the current source:

```text
stfc-profiles list
stfc-profiles list --archived
stfc-profiles create "Science"
stfc-profiles launch --profile <id> --game <path>
stfc-profiles sessions
```

Archive, restore and permanent deletion will use the same catalog operations;
their detailed CLI syntax remains implementation work. Shortcuts invoke the
coordinator with an immutable ID and explicit or saved installation selection.
Renaming a profile does not invalidate its shortcut. Native macOS launch input
and protection follow the same identity/lifecycle contract and need qualification.

## Distribution and supersession

The profile-only product and full community mod are mutually exclusive within
one installation. Both compile the same pinned library into their one bootstrap;
Windows uses `version.dll`. Users install one distribution. Switching distribution
is a stopped-install operation that preserves the shared per-user profile data.

This decision supersedes Bridge ownership of `launch-profiles.json`, the earlier
central `registry.json` proposal, and the old `STFC Community Mod\Profiles` target
path. It also supersedes deleting only a registry entry to unregister a profile:
archiving now performs that catalog removal while retaining the whole directory.
The old mandatory-profile bare-launch choice and installation-selector experiment
remain superseded. No installation selector, adoption, compatibility shim or
legacy fallback is part of this design. The feature has no player deployments;
implementation replaces the unshipped design directly.

Historical source provenance, verification receipts and continuity frames remain
evidence of their exact checkpoints. They are not current design instructions.

## Current implementation and return point

The shared source currently implements ID validation, Windows DPAPI preference
storage and the explicit-input game adapter. Its Windows store still uses
`STFC Community Mod\Profiles` and a writer lock inside the profile directory.
Bridge currently persists its private `launch-profiles.json`, passes the profile
argument, and derives configuration/log paths using the earlier layout. These
are implementation gaps against this contract, not alternate supported designs.

The shared catalog, metadata operations, archive/restore coordination, CLI and
shortcuts are not implemented. Active mod host integration, standalone bootstrap,
readiness/admission, macOS protected storage/loading and live shared-install
qualification remain open. SDK and Go validation repairs do not qualify these.

Implementation starts with the shared catalog and lifecycle operations, then
the CLI and Bridge consumption of them, then both runtime hosts and platform
adapters. Qualify ordinary launch, two distinct accounts from one executable,
reverse restart persistence, duplicate-profile refusal, sign-in callbacks and
stopped-session archive/restore before claiming player support. See
[integration](INTEGRATION.md), [roadmap](ROADMAP.md) and the
[Bridge launch obligations](https://github.com/Guffawaffle/stfc-mod-bridge/blob/feature/named-launch-profiles-225/docs/windows-launcher/SHARED_INSTALL_PROFILE_CONTRACT.md).
