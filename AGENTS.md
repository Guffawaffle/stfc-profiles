# STFC Profiles working agreement

This repository owns the shared STFC profile library, platform adapters,
profile-only product and their contracts. The community mod consumes a pinned
source revision; Bridge consumes release and launch contracts. Use the existing
checkout. Do not edit another repository or an installed game through this repo.

Use native PowerShell and Windows Git here. Preserve concurrent changes. Keep
commits signed. Runtime tests use disposable state; never use or copy real
account preferences, browser profiles or enrollment receipts into fixtures.

Profile-only and full-mod deployments are mutually exclusive within one game
installation. Installing capability does not activate isolation for ordinary
launches. Explicit profile requests must isolate or fail. Selection is supplied explicitly by the host. Do not add installation-marker
selection, compatibility shims or migration fallbacks.

Core code must not depend on community-mod feature globals, Config, logging
singletons or bootstrap entry points. Keep adapters explicit. One bootstrap and
one owner of each hook per product. Preserve source license and attribution.
No DLL deployment, signing, runtime migration or game cycle is implied by a build.

For source changes run the relevant XMake targets and tests, the standalone
consumer check and git diff --check. Record revision, command, cwd, duration,
exit code and bounded output. Mac storage/loading and runtime qualification
remain first-class open work; do not claim parity from Windows tests.

Development source overrides must be explicit and recorded. Release dependency
pins must be immutable. Do not publish binary releases before qualification.
