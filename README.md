# STFC Profiles

Shared STFC profiles, protected account preferences, runtime isolation and game
installation coordination. This is development source. Windows builds and
synthetic tests are available; live account/update and macOS qualification remain
in progress.

## Components

- `stfc-profiles-core`: shared directory catalog, immutable IDs, encrypted
  preference storage and lifetime profile/data/installation exclusion.
- `stfc-profiles`: CLI for catalog operations, launch, shortcuts and game updates.
- `stfc-profiles-native`: versioned UTF-8 JSON API used by Bridge.
- Community-mod adapter: takes a profile ID and store mode explicitly from its
  host, then installs preference and browser hooks through IL2CPP/SPUD.
- Shared profile catalog and CLI: owned by this repository; the accepted directory
  layout, metadata and archive lifecycle are recorded in the
  [canonical contract](docs/PROFILE_CATALOG_CONTRACT.md).
- Bridge: consumes the shared catalog operations and supplies the requested ID
  at launch. Selection is private UI state, separate from the shared catalog.
- Game installation/update operations: part of the Profiles MVP, shared with
  Bridge under the [installation contract](docs/GAME_INSTALLATION_CONTRACT.md).
- `stfc-profiles-runtime`: standalone bootstrap supplying profile isolation.

The profile-only tool and community mod are mutually exclusive distributions in
one game installation. Both compile the same pinned library into their
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

Identity/store/catalog/CLI checks and a separate consumer project use synthetic
account data. Installation coordination may create only neutral per-user lock
files. Adapter and standalone builds use repository-owned dependencies and do
not require mod headers. An explicit consumer root can be recorded for evidence:

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\Test.ps1 -CommunityModRoot D:/dev/stfc-mod
```

Adapter archive compilation does not qualify a game runtime. See
[integration](docs/INTEGRATION.md) and [remaining work](docs/ROADMAP.md).
macOS storage/loading qualification remains open.

## License and source

Derived from [Guffawaffle/stfc-mod](https://github.com/Guffawaffle/stfc-mod) at
`323fb857f51f4cb08231d4b150ea8b5bb59340d1`. License text and attribution are retained.
[Provenance](docs/PROVENANCE.json) records the historical extraction's file identities;
[source provenance](docs/SOURCE_PROVENANCE.md) records later derivations.
[LICENSE](LICENSE) contains GPL version 3 terms.
