# STFC Profiles

Shared STFC profile identity, encrypted Windows preference storage and game hook
adapter code. This is development source; a standalone player DLL and explicit
launch runtime are still being implemented.

## Components

- `stfc-profiles-core`: static library containing ID validation and Windows
  DPAPI preference storage, with per-profile writer exclusion.
- Community-mod adapter: takes a profile ID and store mode explicitly from its
  host, then installs preference and browser hooks through IL2CPP/SPUD.
- Shared profile catalog and CLI: owned by this repository; the accepted directory
  layout, metadata and archive lifecycle are recorded in the
  [canonical contract](docs/PROFILE_CATALOG_CONTRACT.md). Implementation is open.
- Bridge: consumes the shared catalog operations and supplies the requested ID
  at launch; its existing private JSON tracking remains an implementation gap.

The profile-only tool and community mod are mutually exclusive distributions in
one game installation. Both will compile the same pinned library into their
single bootstrap DLL. Installing capability does not activate a profile;
ordinary launches retain ordinary state and named launches isolate or fail.

The installation-marker selector, path-bound enrollment receipts and compatibility
wrapper have been deleted. No migration or fallback implementation is retained.
The host must supply the requested profile ID and an explicit `New`, `Resume` or
`Existing` store mode. An existing store is not silently replaced by a new one.

## Build

Use native Windows XMake, Visual Studio C++ and PowerShell 7:

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\Test.ps1
```

The existing identity/store checks and separate consumer project use synthetic
state. Optional adapter compilation uses an explicit host root:

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\Test.ps1 -CommunityModRoot D:/dev/stfc-mod
```

Adapter archive compilation does not qualify a game runtime. See
[integration](docs/INTEGRATION.md) and [remaining work](docs/ROADMAP.md).
macOS storage/loading qualification remains open.

## License and source

Derived from [Guffawaffle/stfc-mod](https://github.com/Guffawaffle/stfc-mod) at
`323fb857f51f4cb08231d4b150ea8b5bb59340d1`. License text and attribution are retained.
[Provenance](docs/PROVENANCE.json) records original and current file identities;
[LICENSE](LICENSE) contains GPL version 3 terms.
