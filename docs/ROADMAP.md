# Remaining work

The [canonical catalog and launch contract](PROFILE_CATALOG_CONTRACT.md) records
Guff's accepted 2026-09-29 direction and supersedes the earlier Bridge-owned
registry. Local source now implements the shared catalog, lifetime coordination,
protected preference storage, native API, CLI/Windows shortcuts and both Windows
host compositions. Synthetic catalog/preference/CLI tests pass. The remaining
work is:

1. Complete independent review and final consumer/package checks, commit the
   shared source, and pin the same immutable revision in the mod and Bridge.
2. Qualify the MVP direct game updater, including corrupted payloads, unsafe
   archives, process exclusion, interrupted commits, retained backup evidence and
   recovery. The earlier dev 221-to-267 technical update completed; current
   shared-main profile testing uses client 270. The separate current dev folder
   must be selected explicitly rather than reusing the old loader-enabled folder.
   See [installation contract](GAME_INSTALLATION_CONTRACT.md).
3. Qualify Windows user import for both ordinary and protected sources, native
   UAC cancellation, administrator credentials from a standard user, and saved
   login retention without changing ordinary source preferences.
4. Prove readiness and startup/admission coordination with two named
   processes from the same canonical executable with per-profile writer exclusion,
   browser callbacks and correct account state. Qualify archive/restore exclusion.
5. Qualify the minimal profile-only bootstrap and full mod in the dev installation
   using the same shared component, including ordinary launch and distribution
   switching while stopped.
6. Compile and qualify native macOS protected storage, game launch/loading and
   update integration as
   first-class work under the same catalog/identity contract.

Windows builds and synthetic tests are development evidence; live account, update,
browser lifetime/crash and native macOS qualification remain open. Ordinary
launch retains ordinary state; explicit requests isolate or fail. The retired
installation-marker selector, installation-marker adoption and their fallbacks are not work items.
