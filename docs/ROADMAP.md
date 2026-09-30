# Remaining work

The [canonical catalog and launch contract](PROFILE_CATALOG_CONTRACT.md) records
Guff's accepted 2026-09-29 direction and supersedes the earlier Bridge-owned
registry. Shared identity, Windows preference storage and the explicit-input
game adapter exist. The remaining work is:

1. Implement the shared per-user directory catalog with `profiles` and `archives`,
   per-profile metadata, neutral storage roots and lifecycle exclusion that stays
   stable while directories move. Replace the unshipped storage layout directly.
2. Implement the `stfc-profiles` CLI and shortcut coordinator. Bridge consumes
   the same catalog operations and retains only its private UI selection.
3. Integrate an immutable library revision into the mod and provide explicit
   per-launch ID/lifecycle selection through its single bootstrap.
4. Complete readiness and startup/admission coordination, then prove two named
   processes from the same canonical executable with per-profile writer exclusion,
   browser callbacks and correct account state. Qualify archive/restore exclusion.
5. Build and qualify the minimal profile-only bootstrap using the same library.
6. Implement and qualify macOS protected storage, native launch and loading as
   first-class work under the same catalog/identity contract.

The CLI, shared catalog and host runtime are not yet implemented. Ordinary
launch retains ordinary state; explicit requests isolate or fail. The retired
installation selector, adoption, fallback and migration are not work items.
