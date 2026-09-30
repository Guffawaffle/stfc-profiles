# Consuming the extraction

## Core

Checkout or vendor an immutable reviewed commit of `Guffawaffle/stfc-profiles`.
Verify its full commit ID before configuring a release build. Include the narrow
library declaration rather than the repository's root project:

```lua
includes("<pinned-source>/xmake/library.lua")

target("your-product")
    add_deps("stfc-profiles-core")
target_end()
```

This imports only the static library and public include/system-link requirements.
It does not import tests, optional package repositories or a bootstrap. The
consumer chooses compatible compiler/runtime settings. The checked example uses
C++23 and the Windows static C runtime. Public entry points live under
`include/stfc_profiles`; the example calls library identity and legacy ID logic.

`examples/consumer` is an independent XMake project. Run XMake with `-P .` in that
directory so parent-project discovery does not select this repository's root.

## Legacy community-mod adapter

The adapter preserves the existing candidate's host seam. It depends on the
host's validated IL2CPP method helpers, SPUD detours and spdlog, and owns the
preference/browser hooks. The product host remains responsible for its single
early `il2cpp_init` hook:

1. Invoke `stfc::profiles::community_mod::PrepareLegacyProfile()` before the
   original `il2cpp_init` call.
2. Invoke `InstallLegacyProfileHooks()` after that call returns and before game
   account startup continues.

Failures retain the source candidate's process-termination behavior. This is
compatibility code, not a general runtime interface for the future bootstrap.

For a mod consumer, compile `adapters/community_mod/profile_isolation.cc` exactly
once, link `stfc-profiles-core`, and provide the host include directories plus
its existing eastl/spdlog/SPUD packages. Replace the old definitions of selection,
store and hook implementation; do not compile both copies. For an initial
integration, `integration/community_mod/legacy_compat.cc` forwards the old global
hook entry points to the extracted namespace. Replace old selection includes and
calls with `stfc_profiles/windows/legacy_selection.h` and its namespace. The mod's
Config/patch bootstrap must then reference that shared selection instance.

The optional `stfc-profiles-community-mod-adapter` archive is a compile fixture
against an explicit local host root. It does not produce a DLL or prove hook
ordering in a running game. This extraction has not modified or integrated the
active community-mod checkout.

## Pinning and product boundaries

Release consumers record the source URL, full Git revision and build/toolchain
inputs in their own dependency lock and release evidence. A local source override
must be explicit and identified in build evidence; a moving branch is not a
release pin. The library's embedded extraction-source revision describes its
historical origin, not the consuming repository's dependency lock.

The future profile-only product supplies its own minimal Windows loader and
validated game-interface adapter. It must not import unrelated mod features or
reuse the full mod bootstrap as a hidden dependency. Both products own one
bootstrap and one copy of each hook. Build success is not permission to deploy,
enroll an existing account, migrate receipts or change stored profile identity.
