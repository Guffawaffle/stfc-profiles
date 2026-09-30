# STFC Profiles

Shared profile code for Star Trek Fleet Command, extracted from the Windows
community-mod profile isolation candidate. This repository owns the reusable
library, compatibility adapters and future profile-only distribution.

**Status: initial source extraction, not a player download.** The Windows core
builds and its inherited tests run independently of the community mod. The
current adapter still needs the mod's IL2CPP/SPUD host. There is no standalone
`version.dll`, launcher CLI or shared-install runtime implementation here yet.

## Composition

| Component | Responsibility | Current state |
| --- | --- | --- |
| `stfc-profiles-core` | ID/receipt contract, Windows selection/enrollment and encrypted preferences | Independent static library |
| Community-mod adapter | Validates and installs preference/browser hooks through the host | Extracted compatibility source |
| Profile-only bootstrap | Loads the game interfaces and invokes the same profile code | Planned |
| Bridge | Profile selection, shortcuts, lifecycle and readiness | Separate repository |

The intended distributions are mutually exclusive in each game installation:
install the profile-only tool **or** the community mod. Both will compile the
same pinned library into their own single bootstrap DLL. Users will not install
both DLLs together. The community mod will consume an immutable source revision
from this repository, with an explicit local source override for development.

Installing the capability will not activate a profile. In the intended
shared-install contract, ordinary game launches retain ordinary OS-user state;
an explicit named-profile launch must isolate that exact profile or fail. CLI
arguments and shortcuts are still proposed contracts, not working commands.

## Build and check on Windows

Use native Windows XMake, Visual Studio C++ and PowerShell 7:

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\Test.ps1
```

The script builds the library, runs the legacy contract and encrypted-store
tests, and builds/runs the separate consumer project. Tests use synthetic data
in disposable temporary directories. They do not touch game installations.

Optional adapter compilation uses an explicit community-mod source root:

```powershell
xmake f -p windows -a x64 -m release --community_mod_root=D:/dev/stfc-mod -y
xmake build -y stfc-profiles-community-mod-adapter
```

That checks compilation against host headers; it does not link a full mod or
qualify game runtime behavior. See [integration guidance](docs/INTEGRATION.md).

## Compatibility boundary

The initial extraction preserves `v1:<id>` installation markers, V2 enrollment
receipts, preference-store schema 1, DPAPI identity binding and existing lock
behavior. The legacy selector binds an installation to a profile; it is not the
new shared-install selector. No migration or format change is introduced here.

Windows tests do not establish macOS storage/loading parity. The initial
header-only ID/receipt contract is portable; platform implementations still
require separate qualification. See [the work sequence](docs/ROADMAP.md).

## Source and license

Extracted from [Guffawaffle/stfc-mod](https://github.com/Guffawaffle/stfc-mod)
at `323fb857f51f4cb08231d4b150ea8b5bb59340d1`. Existing license text and attribution
are retained. [Provenance](docs/PROVENANCE.json) records original blob identities
and source/extracted file hashes. See [LICENSE](LICENSE) for GPL version 3 terms.
