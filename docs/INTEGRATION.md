# Source integration

Consume an immutable reviewed revision of `Guffawaffle/stfc-profiles`:

```lua
includes("<pinned-source>/xmake/library.lua")
target("your-product")
    add_deps("stfc-profiles-core")
target_end()
```

This declares only the static library and its public include/system-link
requirements. Consumers choose compatible compiler/runtime settings. The separate
`examples/consumer` project uses C++23 and the Windows static C runtime; invoke
XMake with `-P .` in that directory to select its own project.

## Shared catalog ownership

The [canonical catalog and launch contract](PROFILE_CATALOG_CONTRACT.md) owns
per-user paths, immutable IDs, metadata, archive/restore and CLI direction.
Bridge and the CLI use the shared catalog operations; consumers must not create
independent authoritative indexes or derive account identity from installation
paths or display names. Launch and archive/restore share lifecycle exclusion
whose identity survives directory moves. These operations and the neutral
storage layout remain implementation work; the host interface below describes
the existing extracted adapter.

## Host interface

Compile `adapters/community_mod/profile_isolation.cc` exactly once, link the core
and provide the host's validated IL2CPP helpers, SPUD, eastl and spdlog.
The host owns the single early `il2cpp_init` hook:

1. For a named request, call `stfc::profiles::community_mod::PrepareProfile(id, mode)`
   before the original initializer. The ID and `windows::ProfileOpenMode` are
   explicit inputs. Caller-owned lifecycle state determines the mode.
2. Call `InstallProfileHooks()` after the original returns and before account
   startup continues. Preparation and hook failures stop the process.
3. For ordinary launch, invoke neither profile adapter function.

The old selector, enrollment API and global forwarding entry points are removed.
Replace their callers rather than adding shims. There is no implicit selection,
installation binding, fallback or migration API. The DPAPI store's schema and
per-ID protection remain; missing established stores still reject `Existing`.

The optional adapter archive checks compilation against an explicit host source
root. Full mod integration and a minimal standalone bootstrap remain open.
Record the exact dependency revision, toolchain and any development override;
a moving branch or personal path must not become a release pin.
