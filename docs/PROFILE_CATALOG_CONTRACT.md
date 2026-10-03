# Shared profile catalog and launch contract

Status: accepted product decisions, updated 2026-10-02. This is the canonical contract
for the shared catalog, identity, storage, archive lifecycle and CLI direction.
It records the accepted target; the implementation gaps below remain open.
STFC Profiles owns this contract. Bridge and the community mod consume it.

## Terms and ownership

A **profile** is a launch and storage context. Its immutable ID does not fix a
Scopely login, commander or server permanently. A **session** is a running game
process using that context. Names are presentation; the immutable
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

## Typed Default and API versions

The built-in **Default** represents the current Windows user's existing setup.
It has `kind: windows-user`, one generated immutable ID per user catalog and a
token-derived `ownerUserId` SID. Its active ID folder contains `metadata.json`
only. Creating the descriptor never imports, decrypts, copies, initializes or
redirects PlayerPrefs, browser data or game settings. The descriptor's identity
is fixed; the ordinary Windows user's live account preferences remain mutable.
Default configuration belongs to the selected installation's existing TOML.

`ensure-default` creates this descriptor once under the catalog lock;
`resolve-default` requires an existing descriptor. Conflicting IDs, wrong owners,
unexpected owned files and interrupted metadata fail explicitly. Default's name
is fixed; revision-bound `edit` may change its preferred installation. Rename,
archive, restore, deletion and isolated writer/browser/data leases are rejected.
Copying a Windows setup creates a new isolated profile and leaves Default alone.

The stable `stfc_profiles_catalog_request_v1` allocation ABI accepts JSON
`apiVersion: 2` for typed projections. Default uses metadata schema 2, requiring
`kind`, `ownerUserId` and `name: Default`; it has no protected-store lifecycle.
Existing isolated metadata stays schema 1 and projects `kind: isolated`. Existing
IDs, protected files, archives and import provenance are preserved without a
whole-catalog rewrite. API 1 lists omit Default and direct API 1 access rejects it
with `api_version`. Older game hosts likewise reject unsupported schema 2 before
opening isolated storage. API 2 consumers must dispatch by kind rather than infer
kind from a name, null selection or absent preferences.

Typed profile projections include `id`, `kind`, `builtIn`, `name`, `directory`,
`revision`, `state`, `gameDirectory`, `preferredInstallationId`, `preferenceScope`
and `configurationScope`. Default adds `ownerUserId` and omits `configPath`,
`logPath` and `preferencesInitialized`. Isolated profiles retain those owned paths
and lifecycle fields. A saved registration also projects `installationState`.

`launch-ordinary` takes a Default ID and an explicit or preferred installation.
It starts only `prime.exe`, omitting `-stfc-profile`, isolated log arguments and
the runtime capability probe. It returns PID, process-start identity, executable
and `readiness: ordinary`; this proves coordinated startup, not isolation or a
particular logged-in account. It creates no profile session receipt or lifetime
writer/browser lease. Shared installation access covers preflight and spawn;
afterward ordinary processes are observed by installation inspection. Direct
ordinary game launches remain external behavior. `sessions` retains its isolated
receipt semantics and does not claim ordinary lifetime ownership.

The Windows setup descriptor and native physical installation registration are
Windows source features. macOS reports explicit unavailability for them pending
its native design and qualification; existing isolated macOS operations remain.

## Installation registrations

The shared catalog stores installation metadata under
`installations/<immutable-registration-id>/metadata.json`. This directory catalog
has no central index. Registration schema 1 contains a friendly name, canonical
game directory and a SHA-256 representation of its Windows volume/file ID.
`installations` lists registrations and explicit issues; `register-installation`
accepts `name` and `gameDirectory`; `installation-paths` resolves `installationId`.
These operations require JSON API 2 and never write to the game installation.

A valid registration requires the game's executable, Unity/IL2CPP files,
`prime_Data` and the official `&game=<version>` marker. Labels and registration
IDs do not establish physical identity. Alias/case spellings reuse the same
registration and preserve its saved label/path. Missing or replaced physical
directories remain `state: unknown`; a moved original does not silently repair
the path, retarget recovery journals or rewrite profile preferences. Explicitly
registering a different physical installation gives it a different ID.
Relocation/removal/editing of registrations is not implemented in this slice.

`create`, `prepare-user-import`, `import-user` and profile `edit` may accept
`preferredInstallationId`; an empty value removes the preference when editing.
Bound creation/import commits the reference with profile publication. The import
plan includes `preferredInstallationId` and `installationRevision`; bound import
requires `expectedInstallationRevision` and revalidates before capture and before
publication. No partial published profile appears if that binding changes.
`gameDirectory` remains projected for current consumers. An explicit path edit
without a registration clears the old reference. A request supplying both a
registration and a path must identify the same physical directory.

Launch and game maintenance accept explicit `installationId`; otherwise launch
uses the profile's preferred registration. A stale registration fails before use.
An explicit path override is a deliberate separate selection, not an automatic
fallback from an unavailable registration. Canonical installation update locks,
stopped-process checks and recovery boundaries remain in force.

## Per-user storage

Windows root: `%LOCALAPPDATA%\STFC Profiles`.
macOS root: `~/Library/Application Support/STFC Profiles/`.
These are OS-user data locations, independent of the game installation and of
which distribution supplies the runtime. macOS protection has native source;
compilation, consent and game loading still require qualification.

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

Each isolated profile has versioned, plaintext `metadata.json` containing its editable
display name and optional preferred game installation. The directory name is
the ID authority. Login tokens and account preferences remain outside metadata
in the protected preference store. Profile-specific logs and other owned data
travel with the directory. Named sessions and Bridge Settings use profile-owned
`config.toml`, `runtime.toml` and logs; configuration data leases block directory
moves while reading or saving.

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

For isolated profiles, the runtime holds per-profile writer exclusion for the full session lifetime,
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

Windows resolves this shared root through the OS known-folder API with package
redirection disabled. A packaged consumer must also exclude the shared directory
from filesystem virtualization; a matching displayed path alone does not prove
cross-process storage or lock visibility. `stfc-profiles location` reports the
OS-user root without creating or opening catalog state.
Catalog admission and the shared installation-lock directory compare their
physical Windows directory with that OS-user root. Private app-storage
redirection is an explicit `root_redirected` failure, even when the process has
no package identity. It never creates a second active catalog as a fallback.
Desktop shortcuts and ordinary terminals supply the usual desktop launch
context; packaged hosts must qualify their filesystem declaration separately.

## Launch and CLI direction

Installing capability does not activate a named profile. Ordinary `prime.exe`
and official-launcher launches retain ordinary OS-user preferences. GUI selection
and other profile activity never redirect a bare launch. An explicit isolated profile
request must isolate the exact requested ID or stop with guidance. A typed Default
coordinator request intentionally uses ordinary Windows-user behavior.

On Windows the game-host profile selector is `prime.exe -stfc-profile <id>` using
separate argument tokens. The coordinator supplies Unity's `-logFile` argument
with that profile's `logs/Player.log` path before game startup. CLI launches,
native shortcuts and Bridge use this coordinator. A manual direct game command
must also supply `-logFile <profile-directory>/logs/Player.log` to isolate Unity
logs; the selector alone routes account preferences. Store lifecycle modes
`New`, `Resume` and `Existing` remain
explicit host inputs; the host must not silently create a replacement for a
missing established store. Profile selection must occur before login state is
read. Isolation readiness and a correct logged-in account are separate observations.

The initial CLI forms are implemented in local development source:

```text
stfc-profiles list
stfc-profiles list --archived
stfc-profiles create "Science"
stfc-profiles launch --profile <id> --game <path>
stfc-profiles sessions
```

Archive, restore and explicit permanent deletion use the same catalog operations.
Shortcuts invoke the
coordinator with an immutable ID and explicit or saved installation selection.
Renaming a profile does not invalidate its shortcut. Native macOS launch input
and protection follow the same identity/lifecycle contract and need qualification.

## User import and elevation explanation

Accepted UX direction, 2026-10-02. Import is selected by source OS user, not by
commander. Two Windows users may hold the same game account and still be separate
import sources with separate destination profile IDs. Windows development source
implements this operation in the shared owner. Live account portability and
standard-user credential/UAC qualification remain distinct checks.

`import-sources` returns only Windows users with a nonempty STFC preference key,
plus the original destination user, `requiresElevation` and `unavailableUsers`.
Readable matches remain available when another user's setup is protected or
unavailable. Discovery resolves OS profile metadata without inspecting every
private directory; source resolution checks only the selected SID. Key presence
is checked without decoding login values. Offline discovery parses the selected
key path in a read-only in-memory hive, never mounting or copying it.

**Find other Windows users…** presents a friendly discovery explanation before
permission is requested. Its `import-sources` request supplies `allowElevation`
and the expected original destination SID. The short-lived helper checks STFC
key availability and returns names/IDs/status only, with no credential values,
profile creation or launch-selection change. Discovery approval does not retain
an elevated launcher or authorize a later account copy. A protected selected
source may require another native Windows approval after its import review.

 `prepare-user-import` validates the source SID, new display name and preferred
installation, then reports the actual read-access requirement. `import-user`
requires the reviewed destination SID and explicit elevation consent. Neither
planning nor discovery reads account values into the public JSON response.

The capture reads only the selected user's STFC preference subtree. Loaded hives
use two matching, timestamp-checked raw registry observations; close that user's
game first to finish saving. Unloaded hives are opened read-only, parsed in memory
and rejected if dirty, corrupt, changing or unsupported. They are never mounted,
recovered or copied to disk. This observes a settled preference store, not a game
transaction or proof of a usable logged-in account.

Unity hashed names are validated against signed UTF-8 bytes, with Windows ANSI
name conversion reproduced before the hash is removed. Only hashed Unity entries
belong to this import; unrelated registry metadata is not interpreted. Numeric,
binary and otherwise unrecognized value types are retained. In particular, Unity
stores floats as eight-byte doubles even under REG_DWORD; typed registry readers
that truncate these values must not be used. The protected schema 2 stores native
representations alongside normal typed writes. Existing schema 1 stores remain
readable. Native getter behavior follows current client 270 UnityPlayer; an invalid
string that would make Unity read beyond its allocation returns the default safely.

Administrator approval starts the exact Profiles CLI or native module's capture
helper through Windows. A private local pipe binds both process IDs and transfers
only discovery metadata or the selected capture in memory, according to the explicit private operation. The helper never receives a destination path
and never writes profile data. The original process encrypts under the original
Windows user and fresh immutable ID, stages complete preferences and initialized
metadata, then publishes the new catalog directory. Capture or write failures
before publication and declined approval publish no profile. If the process is
interrupted after publication, the committed catalog directory remains the
record of the completed import and must be inspected before repeating it. Source state, other profiles and the launch selection remain unchanged.
Personal browser directories are not copied; named sign-in remains isolated.

Windows user import is explicitly unavailable on macOS until its native source
and permission design is implemented and qualified; the existing Mac storage and
runtime contracts remain first-class work.

Determine the selected source's access requirements before requesting elevation.
Current-user import should use ordinary read access when available. Another
user's protected or unloaded store may require administrator access, depending
on the chosen implementation. Do not elevate merely because the operation is
named import, or treat corrupt data, unsupported formats or redirected storage
as permission failures that administrator access can solve.

When administrator access is required, show an application explanation BEFORE
opening the Windows UAC prompt. It must identify:

- The selected source Windows user and STFC data being read, including saved
  login state and game preferences.
- Why this particular source/access method needs administrator permission.
- The destination profile name and destination Windows user.
- The actual action and scope: copying selected STFC state into a new profile
  while preserving the source state.
- That continuing will open the Windows permission prompt.

Example for an other-user import:

> Import josep's STFC setup
>
> We'll copy josep's saved STFC login and game settings into **Main**, a new
> profile for Windows user **Guff**. josep's original setup will stay as it is.
>
> Windows needs administrator approval to read another Windows user's saved
> game data. Choose **Continue** to open the Windows permission prompt. If
> needed, Windows will ask for an administrator's username and password.

Buttons: **Continue** and **Not now**. Source, destination and reason come from the
actual import plan; the example names are not defaults. Declining either this
explanation or UAC cancels the request without publishing a profile or changing
source state. Current-user sources requiring elevation receive an accurate
reason for their own protected location, not the other-user explanation.

Accepted presentation direction, 2026-10-02: keep this friendly and informative.
Use a neutral information presentation, plain language and short paragraphs;
avoid alarming warning icons or technical permission jargon. Keep source,
destination, copied data and the next Windows prompt visible in the main text.
Additional explanation can live under an accessible **Why is this needed?**
expander. Do not hide required facts in a tooltip or the expander. Support keyboard
navigation and screen-reader labels, readable contrast and OS text scaling.
**Not now** and Escape dismiss the dialog with the user's selection retained.

For a standard Windows user, use the native UAC credential prompt to authorize
an administrator account; Profiles does not collect or save that account's
password. The explanation says that Windows may ask for an administrator's
username and password. The administrator authorizes the required helper access,
not a change of import source or destination owner. The original requesting
user remains the destination catalog owner; do not resolve the destination from
the administrator helper's own LOCALAPPDATA or encrypt the destination store
under that administrator. Windows policy may deny elevation, which must remain
an explicit canceled/blocked import rather than prompting through another path.

Bridge and the standalone product must present the same operation facts. CLI
imports must explain those facts before requesting elevation as well; final
interactive/noninteractive presentation is still to be specified. Elevation
execution and transfer design remain separate from this accepted UI requirement.

## Installation updates in the MVP

On 2026-09-29 Guff added installation selection and game updating to the Profiles
MVP. Direct download and application is the preferred implementation; a managed
official-updater handoff is authorized if the direct route cannot be qualified.
The player should choose the intended installation in Profiles, rather than
manually change the official launcher's settings as a prerequisite to testing.

The shared component owns these operations for both the standalone CLI and
Bridge. An update targets an installation, not an account: update the shared
game files once while every session using that installation is stopped. Profile
identity, account data and archive state remain separate. See the accepted
[game installation contract](GAME_INSTALLATION_CONTRACT.md) for locking, staging,
verification, recovery and the initial full-image implementation boundary.

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
remain superseded. No installation selector, installation-marker adoption, compatibility shim or
legacy fallback is part of this design. The feature has no player deployments;
implementation replaces the unshipped design directly.

Historical source provenance, verification receipts and continuity frames remain
evidence of their exact checkpoints. They are not current design instructions.

## Current implementation and return point

Local source implements the shared native catalog and versioned API, metadata
mutations, archive/restore and independent writer/data/installation locks. The CLI,
Windows shortcuts and standalone bootstrap build, and native catalog/preference
tests pass on synthetic data. Bridge consumes the native component and preserves
its selected ID as private UI state. Named configuration/log paths use the shared
profile directory; the earlier storage root/private account index are removed.

Mod and standalone host source share the adapter. The runtime commits monotonic
preference-initialization metadata before publishing readiness; missing established
data cannot become a fresh empty account. Native game-update operations and
Bridge presentation are implemented locally, with synthetic integrity/recovery
fixtures. No new signed implementation checkpoint or player release is claimed
by this in-progress documentation. Final pins, package/review checks, macOS native
compilation/loading, and live dev update/account qualification remain open.

Qualify ordinary launch, two distinct accounts from one executable,
reverse restart persistence, duplicate-profile refusal, sign-in callbacks and
stopped-session archive/restore before claiming player support. See
[integration](INTEGRATION.md), [roadmap](ROADMAP.md) and the
[Bridge launch obligations](https://github.com/Guffawaffle/stfc-mod-bridge/blob/feature/named-launch-profiles-225/docs/windows-launcher/SHARED_INSTALL_PROFILE_CONTRACT.md).
