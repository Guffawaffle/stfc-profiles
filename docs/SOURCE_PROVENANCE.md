# Source provenance

The [original extraction record](PROVENANCE.json) preserves exact source blobs,
hashes and destination paths at that historical checkpoint. It is not a current
file inventory. Subsequent source changes use the catalog/runtime contracts and
Git history; the original hashes are not regenerated to resemble later code.

The original component derived profile identity, Windows preference persistence
and the game adapter from Guffawaffle/stfc-mod commit
`323fb857f51f4cb08231d4b150ea8b5bb59340d1`. The generic preference API, portable
serialization, OS-user catalog, native API, CLI, installation coordinator and
macOS Keychain implementation are subsequent development in this repository.
The Windows-only public preference header has been removed; consumers use
`include/stfc_profiles/prefs_store.h` directly.

The standalone `bootstrap/version.cc` derives Windows version-DLL forwarding
from `win-proxy-dll/src/version.cc` in Guffawaffle/stfc-mod commit
`0c34f249b1ea7bf00dd5221e9834983af7c5883a`. It retains System32 forwarding while
omitting community-mod feature initialization.

The repository-owned SPUD package/source under
`xmake-packages/packages/s/spud` was copied from the same mod commit's
`xmake-packages/packages/s/spud`. This includes the reviewed x64 relocation fix.
It avoids requiring a mod checkout to build the standalone bootstrap. The package
build retains its declared instruction-decoder/assembler dependency pins;
comparison-library tests remain disabled. Do not replace this source with an
unreviewed upstream archive or restore the retired target registry.

The libarchive package recipe derives from the official XMake package recipe,
with version 3.8.9 pinned to its official release SHA-256:
`f5a6539059cf5e597dbeda37bfa4874b1e8dea063c8d93bf85a2b44af90a5bd4`.
libarchive is BSD-2-Clause; nlohmann_json 3.12.0 is MIT. Build/package notices
must also cover transitive compression and runtime dependencies. These libraries
do not qualify the game-update protocol or replace publisher artifact evidence.

The original repository GPL version 3 license and attribution remain in force.
No source-provenance record is a game-runtime, account-isolation or release
qualification claim.
