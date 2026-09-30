#include "stfc_profiles/community_mod_adapter.h"

#if _WIN32
// Optional migration shim for the source candidate's existing bootstrap calls.
// Compile this only once; remove the old implementation from the consumer build.
void PrepareProfileIsolationProbe()
{ stfc::profiles::community_mod::PrepareLegacyProfile(); }

void InstallProfileIsolationProbe()
{ stfc::profiles::community_mod::InstallLegacyProfileHooks(); }
#endif
