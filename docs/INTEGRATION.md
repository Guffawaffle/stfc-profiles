# Source integration

Consume an immutable reviewed revision of `Guffawaffle/stfc-profiles`:

```lua
includes("<pinned-source>/xmake/library.lua")
target("your-product")
    add_deps("stfc-profiles-core")
target_end()
```

The reusable static core supplies the directory catalog, metadata and lifecycle
operations, process/session evidence, installation exclusion and protected
preferences. The JSON dependency and platform system links are transitive.
Consumers select compatible C++23/compiler/runtime settings. The separate
`examples/consumer` project verifies that the source library can be consumed
without importing the profile product's bootstrap or tests.

## Shared ownership and native API

The [canonical catalog contract](PROFILE_CATALOG_CONTRACT.md) owns per-user
paths, immutable IDs, metadata and archive/restore. Bridge and the CLI invoke
`ExecuteCatalogRequest` through the same source implementation. Native clients
may use the versioned C ABI in `include/stfc_profiles/c_api.h` and must release
responses through the matching module allocator. They must not persist a second
authoritative profile index or derive account identity from display names or
installation folders.

Game inspection and update requests follow the
[installation contract](GAME_INSTALLATION_CONTRACT.md). Clients select the
installation; the shared component owns admission, download/application and
recovery. CLI integration is documented in [runtime and CLI](RUNTIME_AND_CLI.md).

## Game host interface

Compile the sources in `adapters/community_mod/` exactly once and link the
shared core, pinned SPUD and spdlog. The adapter resolves its minimal required
IL2CPP API dynamically and validates complete managed method signatures. It
imports no full-mod helper, EASTL, feature configuration, GameAssembly import
library or logging singleton owned by the host.

A named host owns one early `il2cpp_init` hook:

1. Parse exactly one explicit `-stfc-profile <immutable-id>` before account
   startup. Install the platform process-admission hook for this named request.
2. Before the original initializer, acquire `SessionLease(root, id)` and retain
   it until process exit. Select the store mode under that lease. Initialized
   metadata or the initialization flag requires `Existing`; an uninitialized
   profile with a committed bin uses `Resume`; only an untouched catalog profile
   uses `New`.
3. Call `PrepareProfile(root, id, mode, lease)` before the original initializer.
   A corrupt/missing established store, conflicting writer or mismatched lease
   stops the process.
4. Check the original initializer's integer return value. After success, call
   `InstallProfileHooks()` before account startup resumes. It installs every
   required preference/browser hook, commits first-use preferences, then marks
   the exact session ready. The host must not publish readiness separately.
5. An ordinary launch invokes none of these profile functions and retains
   ordinary OS-user state.

Readiness confirms that isolation was installed for the requested process and
ID. It does not prove that the user signed into the intended account. Missing
established data never selects another identity or creates a replacement account.

## Browser guardian and product bootstrap

Both distributions use the same adapter. On Windows the isolated Edge directory
is inside the profile's `browser/` folder. The current runtime exports
`STFCProfilesBrowserLaunchW`; its adapter starts the same DLL through native
System32 `rundll32.exe`. That guardian acquires `BrowserLease` before starting
Edge, owns the entire child tree in a kill-on-close Job, and retains exclusion
until the tree is empty. Catalog moves and deletion require both the stopped
writer and exclusive browser-data admission.

The Mac adapter requires the `stfc-profiles` CLI beside the runtime library for
its internal browser guardian. The source uses a dedicated Chromium data folder
and process group. Native build, Keychain sharing/consent, browser containment,
callback behavior and crash/lifecycle qualification remain open; Windows build
success is not Mac qualification.

`stfc-profiles-runtime` is the minimal profile-only bootstrap; on Windows its
output is `version.dll`. It forwards the OS version API and installs its one early
initializer only for explicit named launches. The full mod supplies its existing
bootstrap and invokes the same adapter. Users install one runtime distribution
per installation.

## Verification boundary

Native Windows core, CLI, adapter and standalone bootstrap compilation and
synthetic preference/catalog/CLI tests are available. They do not qualify live
accounts, game login callbacks, browser crash ordering or updated-client hook
compatibility. Record the exact source revision, dirty state, toolchain and any
explicit development source override. Release dependencies must remain immutable;
no binary release is qualified solely by these source/build checks.
