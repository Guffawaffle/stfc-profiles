#pragma once
#include "stfc_profiles/prefs_store.h"
#include <filesystem>
#include <string_view>

namespace stfc::profiles::community_mod {
// A named host owns one early initializer. Its lease stays live for the entire
// game process. Ordinary launches invoke none of these functions.
void InstallProcessAdmission(std::string_view id);
void PrepareProfile(const std::filesystem::path& root, std::string_view id,
                    ProfileOpenMode mode, SessionLease& lease);
// Installs every required preference/browser hook, persists first-use data,
// then marks this exact session ready. Any failure terminates before login.
void InstallProfileHooks();
// Shared by both distributions; the platform guardian retains BrowserLease
// until the isolated browser has finished using the profile directory.
bool LaunchIsolatedBrowser(const std::filesystem::path& root, std::string_view id,
                           std::u16string_view url);
} // namespace stfc::profiles::community_mod
