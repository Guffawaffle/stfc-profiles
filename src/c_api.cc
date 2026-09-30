#include "stfc_profiles/c_api.h"
#include "stfc_profiles/catalog.h"
#include "stfc_profiles/session.h"
#include "stfc_profiles/installation.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <memory>
#include <string>

namespace {
char* Copy(const std::string& value) {
  auto* output = static_cast<char*>(std::malloc(value.size() + 1));
  if (!output) throw std::bad_alloc();
  std::memcpy(output, value.c_str(), value.size() + 1);
  return output;
}
}

extern "C" int STFC_PROFILES_CALL
stfc_profiles_catalog_request_v1(const char* request, char** response) {
  if (!response) return 1;
  *response = nullptr;
  if (!request) return 1;
  try {
    const auto length = strnlen(request, 65537);
    if (length > 65536) return 1;
    *response = Copy(stfc::profiles::ExecuteCatalogRequest(std::string_view(request, length)));
    return 0;
  } catch (...) { return 2; }
}

extern "C" void STFC_PROFILES_CALL stfc_profiles_free_v1(void* response) {
  std::free(response);
}

extern "C" int STFC_PROFILES_CALL
stfc_profiles_acquire_data_lease_v1(const char* root, const char* id, void** lease, char** error) {
  if (!lease || !error) return 1;
  *lease = nullptr;
  *error = nullptr;
  if (!id || strnlen(id, 33) > 32 || (root && strnlen(root, 32769) > 32768)) return 1;
  try {
    const auto path = root && *root ? std::filesystem::u8path(root) : stfc::profiles::DefaultCatalogRoot();
    *lease = new stfc::profiles::BrowserLease(path, id);
    return 0;
  } catch (const std::exception& failure) {
    try { *error = Copy(failure.what()); } catch (...) {}
    return 2;
  } catch (...) { return 2; }
}

extern "C" void STFC_PROFILES_CALL stfc_profiles_release_data_lease_v1(void* lease) {
  delete static_cast<stfc::profiles::BrowserLease*>(lease);
}

extern "C" int STFC_PROFILES_CALL
stfc_profiles_acquire_installation_lease_v1(const char* root, const char* game, void** lease, char** error) {
  if (!lease || !error) return 1;
  *lease = nullptr; *error = nullptr;
  if (!game || !*game || strnlen(game, 32769) > 32768
      || (root && strnlen(root, 32769) > 32768)) return 1;
  try {
    const auto data_root = root && *root ? std::filesystem::u8path(root) : stfc::profiles::DefaultCatalogRoot();
    const auto directory = std::filesystem::u8path(game);
    auto access = std::make_unique<stfc::profiles::InstallationLease>(data_root, directory, false);
    stfc::profiles::CheckInstallationReady(data_root, access->Directory());
    *lease = access.release(); return 0;
  } catch (const std::exception& failure) {
    try { *error = Copy(failure.what()); } catch (...) {}
    return 2;
  } catch (...) { return 2; }
}

extern "C" void STFC_PROFILES_CALL stfc_profiles_release_installation_lease_v1(void* lease) {
  delete static_cast<stfc::profiles::InstallationLease*>(lease);
}
