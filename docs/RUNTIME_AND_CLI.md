# Runtime and CLI

The source now provides the shared directory catalog, CLI, protected preference
store, game adapter and minimal profile-only bootstrap. Windows source/build and
synthetic lifecycle evidence are available. Live account/login qualification and
native Mac qualification remain required before player support is claimed.

## CLI

```text
stfc-profiles default [--json]
stfc-profiles resolve-default [--json]
stfc-profiles installations [--json]
stfc-profiles register-installation "Primary" --game <directory>
stfc-profiles installation-paths --installation <id>
stfc-profiles list
stfc-profiles list --archived
stfc-profiles create "Science" [--game <directory> | --installation <id>]
stfc-profiles users [--json] [--approve-elevation]
stfc-profiles import "Main" --user <Windows-SID> [--game <directory>]
stfc-profiles import "Main" --user <Windows-SID> [--game <directory>] --approve-elevation
stfc-profiles rename --profile <id> "Engineering"
stfc-profiles edit --profile <id> --game <directory>
stfc-profiles launch --profile <id> [--game <directory>]
stfc-profiles sessions
stfc-profiles location
stfc-profiles archive --profile <id>
stfc-profiles restore --profile <id>
stfc-profiles delete --profile <id> --archived --permanent
stfc-profiles shortcut --profile <id> --output <path.lnk> [--game <directory>]
stfc-profiles game status [--game <directory> | --profile <id>]
stfc-profiles game check [--game <directory> | --profile <id>]
stfc-profiles game update [--game <directory> | --profile <id>] [--expected-version <number>]
stfc-profiles game recover [--game <directory> | --profile <id>]
```

`default` ensures the catalog-owned Windows setup descriptor without copying
account data. `resolve-default` only resolves its existing immutable ID. The
human CLI alias `--profile default` resolves that ID; shortcuts embed the resolved
ID. `launch` dispatches by kind: Default starts ordinary `prime.exe`, while
isolated profiles keep their explicit runtime readiness checks. The CLI uses
JSON API 2; the C allocation ABI remains unchanged. Default has no owned config,
browser, log or protected preference files and cannot be archived or deleted.

`--installation <id>` selects a physical registration for launch, shortcut and
game commands, or saves it during create/edit/import. Alias registration does
not overwrite a friendly label. Missing, moved or replaced folders stay unknown;
selection does not repair them. Registration/Default Windows behavior is native
source work; macOS equivalents remain explicit qualification work.

`--json` returns the versioned shared response; failures return a nonzero exit
code and structured error. `--root <directory>` explicitly selects disposable or
otherwise deliberate catalog state. A game host currently uses its OS-user
catalog root, so launching a non-default catalog is rejected rather than silently
opening a different profile. `--expected-revision <revision>` binds a metadata
mutation to a prior observation. Without that option, the CLI reads the selected
profile immediately before submitting its revision-bound mutation.

Windows `users` lists source Windows accounts; import selects the exact SID,
not a commander or display-name match. Import makes a one-time copy of saved STFC
login/preferences into a fresh immutable profile ID for the current Windows user.
Close the source user's game first so its preferences have finished saving. The
source setup stays intact and profiles may share the same preferred game folder.
The CLI displays the source/destination explanation before copying. Ordinary
access is used when available. If access is denied, rerun with
`--approve-elevation`; a native **Continue / Not now** information dialog precedes
Windows UAC. Native Windows credentials, when required, are entered only into
Windows. `--json` keeps response JSON on stdout and explanation text on stderr.
No account values enter the public API or its error output. Cancellation creates
no published profile. Import does not select the new profile for another host.
See the [user import contract](PROFILE_CATALOG_CONTRACT.md#user-import-and-elevation-explanation).

Native Windows shortcuts point to the CLI with the immutable ID. Names can
change without invalidating the shortcut. Publication stages and flushes the
link before a non-overwriting final move. Native Mac shortcut authoring remains
unqualified and reports an explicit unavailable operation.

`location` reports the neutral OS-user catalog root without reading or changing
profile metadata. It does not accept `--root`. Windows uses the known-folder API
with package redirection disabled so the CLI, game and packaged consumers can
select the same directory. Packaged consumers must separately establish that
filesystem virtualization does not redirect writes or lock files.
Admission rejects a physically redirected default root with `root_redirected`;
launch from a desktop shortcut or ordinary terminal to use the shared catalog.
The shared installation lock applies the same physical-root check even when a
consumer supplies a separate synthetic catalog root for tests.

Game commands use the same installation operations as Bridge. An explicit
`--game` overrides a saved preferred installation. `--profile` only resolves that
installation choice for game maintenance; it does not select account preferences
for the updater. `--expected-version` binds an update to the version returned by
the preceding check; a changed target stops before the game payload is downloaded.
See [installation/update contract](GAME_INSTALLATION_CONTRACT.md)
for exact source, transaction and recovery boundaries.

## Protected preferences

The catalog root is independent of the game installation. Isolated account preferences,
initialization and recovery files live in the active/archived immutable-ID
folder. The stable writer and browser lock namespaces stay outside that movable
folder. Plaintext metadata contains display/launch information, never account
secrets. `preferencesInitialized` is monotonic lifecycle evidence: deleting both
the encrypted bin and its initialization flag cannot turn an established profile
into a new empty account.

Windows uses user-bound DPAPI with profile-ID-specific entropy. The decrypted
payload also embeds the ID, typed preferences and imported native values; copied ciphertext cannot open
under a different ID. macOS source stores a per-profile P-256 private key in the
user Keychain and protects files using Security-framework ECIES X9.63 SHA-256
AES-GCM. The private key is accessed through Security APIs without exporting it.
Missing keys and failed ciphertext authentication stop established-store access.
Permanent deletion removes the exact Mac Keychain key; archives retain it.

Apple documents the [key creation and permanent Keychain attributes](https://developer.apple.com/documentation/security/generating-new-cryptographic-keys)
and [public-key encryption/decryption APIs](https://developer.apple.com/documentation/security/seckeycreateencrypteddata(_:_:_:_:)).
Mac key access, consent across installations/distributions, disk durability,
loading, browser lifetime and live game behavior still require native evidence.
Preserving a directory and ID does not make either platform's protected data
portable to arbitrary OS users.

## Verification

`prefs-store-tests` uses only synthetic secrets and temporary catalog state. It
covers native import byte preservation and schema 1 reopen, typed preference persistence, immutable-ID encryption binding, matching
lease requirements, loss of established files, duplicate writers, stopped
archive/restore, interrupted first use and validated backup recovery.

`user-import-source-tests` uses generated hives and synthetic values only. It
covers inline and segmented data, all supported registry list layouts, exact
Unity hash/Unicode normalization, eight-byte DWORD floats and rejection of dirty,
malformed, cyclic, redirected or out-of-bounds cells.

`catalog-tests` covers stale revisions, duplicate JSON properties/active-archive
identity, incomplete metadata, browser/writer exclusion, archive/restore and
permanent deletion. Its synthetic child process verifies cross-process writer
exclusion and sessions bound to PID, process start and executable identity. A
receipt with the same PID but a different start is rejected.

`tests/cli_test.ps1` exercises Unicode names, revision failures, native shortcut
identity and non-overwrite behavior, missing installation/launch failures, archive,
restore and explicit permanent deletion. All state is disposable; no account or
browser profile is copied into fixtures.

Live qualification must still prove ordinary launch, two distinct accounts from
one installation, reverse restart persistence, duplicate-profile refusal, isolated
sign-in callbacks and stopped-session archive/restore. Browser/helper crash
ordering and updated-client admission/hook compatibility need runtime evidence.

Windows user discovery preserves readable STFC matches and reports partial access
with `requiresElevation`. `users --approve-elevation` shows the discovery
explanation, then asks Windows to check protected users if needed. The result
contains only users with STFC data, names/IDs and availability status; it creates
no profile and contains no login values. Account copying retains its separate
import review and conditional approval.
## macOS profiles preview

Named Mac launches are explicit:

```sh
stfc-profiles launch --profile ID \
  --game "/path/Star Trek Fleet Command.app/Contents/MacOS" \
  --runtime "/path/libstfc-community-mod.dylib" --json
```

The coordinator inspects the runtime's
native-architecture Mach-O contract without loading it, checks the game's loader
entitlements, starts a separate suspended executable, publishes its exact process
identity, then resumes it. Success requires the requested profile's runtime-ready
receipt and live writer lease. Missing capability fails before game startup.

The complete community-mod preview app bundles `stfc-profiles` beside its dylib;
the same helper owns isolated Chrome/Edge sign-in. Ordinary launcher Engage remains
ordinary. A named profile has its own preferences, browser, config and logs under
`~/Library/Application Support/STFC Profiles/profiles/ID`. It starts fresh and does
not import the ordinary Mac login. Native launch fixtures exercise injection,
admission and concurrent distinct sessions; actual STFC/Scopely sign-in still needs
tester verification. Mac user import, game updating through the profiles CLI and
configuration writer reservations are separate work.
