#include <cstdio>
#include <cstdlib>
extern "C" __attribute__((visibility("default"), used, section("__DATA,__stfc_profile")))
const unsigned int STFCProfilesExplicitLaunchContractV1 = 1;
__attribute__((constructor)) static void Injected()
{
  const auto marker = std::getenv("STFC_PROFILES_TEST_INJECTION");
  if (!marker) return;
  if (auto file = std::fopen(marker, "w")) { std::fputs("injected", file); std::fclose(file); }
}
