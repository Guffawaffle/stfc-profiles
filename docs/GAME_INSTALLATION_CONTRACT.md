# Game installation and update contract

Status: accepted MVP scope, 2026-09-29; implementation and qualification in progress.

Guff requested installation selection and game updating as a Profiles MVP feature.
Direct downloading and application is preferred. A managed official-updater
route is acceptable if direct updating cannot be qualified. The request to update
the existing dev installation to client 267 remains the live qualification target;
manual changes to the Scopely launcher's installation setting are no longer the
planned prerequisite.

## Ownership and selection

The shared Profiles library owns installation inspection, update checks, update
execution and recovery. The standalone CLI and Bridge invoke the same operations.
Bridge presents progress and its selected installation; it does not own a separate
patch engine. The community mod and profile-only runtime consume the same
installation exclusion contract when launching named sessions.

An installation is a canonical game directory. A profile may save that directory
as its preferred launch target; an explicit choice overrides the saved choice.
Account IDs are independent of installation paths. Several profiles can use one
installation. Updating it changes the game files for all of those profiles once.
The selected directory and installed/target game versions must be visible before
the operation starts. A default or main installation must never substitute for
the requested target.

Initial CLI forms:

```text
stfc-profiles game status --game <path>
stfc-profiles game check --game <path>
stfc-profiles game update --game <path>
stfc-profiles game recover --game <path>
```

`--profile <immutable-id>` may resolve the saved installation when `--game` is
absent. It chooses the installation for this action; it does not select account
preferences for the updater. Profiles never imports ordinary-account credentials
or edits the official launcher's settings for a direct update.

`game update --expected-version <number>` binds the operation to a previously
checked available version. If the official target changes, the operation stops
before staging rather than silently accepting a different target. Bridge passes
the same expected version from its check result.

## Admission and lifetime

The coordinator identifies the exact directory, validates the game layout and
uses a stable installation lock outside the mutable game files. Named runtime
sessions hold shared access for their entire lifetime. Updating and recovery
require exclusive access. An ordinary game or an official updater may not hold
our lock, so exact executable-path process inspection is also required. An
uninspectable relevant process blocks mutation rather than being guessed absent.

All sessions using the target installation must stop before update or recovery.
A different installation can remain open when its path and process identity are
observable. The updater never terminates an unrelated process or a game merely
to obtain admission. Launch checks unresolved update journals before starting
an installation. File replacement rechecks admission immediately before commit.

## Initial direct updater

The Windows MVP queries the current official Xsolla update service and downloads
a complete official game image over HTTPS. It does not initially execute the
delta patch protocol. The current Windows service supplies a full client 267
image and torrent metadata with per-piece SHA-1 hashes; the torrent metadata is
used to verify an HTTPS download without peer-to-peer transfer.

Supported manifest fields, HTTPS destinations, archive sizes and file inventory
are bounded and validated. The download must match the advertised piece hashes,
length and archive integrity checks before applying anything. This establishes
consistency with metadata obtained from the official HTTPS service. It must not
be described as an independently signed publisher manifest; no such signature
has been observed for this route.

The updater inventories and extracts into a separate staging directory. Reject
absolute/traversing paths, alternate streams, links/reparse points, duplicate or
conflicting paths and unsupported file types. Validate required game files and
the platform-specific target version before replacement. Compare version numbers
only within the selected platform; reject a downgrade and unsupported rule plans.

Only files owned by the official image are eligible for replacement. Preserve
the installed Profiles/community-mod runtime, configuration, logs and all
per-user profile data. Unknown extra files remain untouched. Do not treat
preserving a runtime DLL as proof it is compatible with the new game client.

## Commit and recovery

Before replacement, durably record a journal bound to the canonical installation,
source version, target version, reviewed file plan and verified staged bytes.
Back up each existing file before replacing it. Record progress so an interrupted
commit can be rolled back explicitly. Write the authoritative game version marker
last, after the complete image has been installed and checked.

An unresolved transaction blocks launch and another update. Recovery must verify
the journal's target and file plan, acquire the same exclusive installation lease
and restore the prior image or report the exact unresolved failure. It must not
accept a partially installed image or overwrite unexpected external changes.
Keep backups, journals and failure evidence until explicit cleanup or an accepted
retention policy applies. No cleanup policy is implied by this MVP.

After success, reread the installed version and required game files, report the
result and reassess runtime compatibility before named launch. Download completion,
successful extraction and process exit are separate from a successful update.

## Qualification

Exercise malformed manifests/torrent metadata, bad hashes, unsafe archives,
insufficient space, active/unknown processes, installation aliases and contention,
interrupted downloads/extraction/replacement, unexpected external edits and
recovery on synthetic fixtures before updating the dev installation. Then qualify
the actual client 221 to 267 update there and retain concrete version/hash evidence.

macOS remains a first-class product target. Its existing updater is research
evidence, not permission to port unvalidated rules or reuse Windows version
numbers. Native macOS installation/loading, archive layouts, encryption and
updater behavior require separate compilation and runtime evidence before support
is claimed.
