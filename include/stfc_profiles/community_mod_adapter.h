#pragma once

#if _WIN32
namespace stfc::profiles::community_mod {

// Host owns the early il2cpp_init hook. Prepare runs before its original;
// Install runs after the original returns and before account startup resumes.
// This legacy adapter requires the host's validated IL2CPP and SPUD interfaces.
// It preserves marker/V2 selection, including process termination on failures.
void PrepareLegacyProfile();
void InstallLegacyProfileHooks();

} // namespace stfc::profiles::community_mod
#endif
