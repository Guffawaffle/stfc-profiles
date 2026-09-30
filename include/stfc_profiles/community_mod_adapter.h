#pragma once

#if _WIN32
#include "stfc_profiles/windows/prefs_store.h"

#include <string_view>

namespace stfc::profiles::community_mod {

// Host owns the early il2cpp_init hook. Prepare runs before its original;
// Install runs after the original returns and before account startup resumes.
// The host supplies the requested ID and store lifecycle mode explicitly.
// For an ordinary launch, the host invokes neither function. There is no
// installation-bound selector or implicit enrollment inside the adapter.
// Preparation failure terminates the process before account startup.
void PrepareProfile(std::wstring_view id, windows::ProfileOpenMode mode);
void InstallProfileHooks();

} // namespace stfc::profiles::community_mod
#endif
