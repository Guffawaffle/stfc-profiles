#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <nlohmann/json.hpp>

namespace stfc::profiles {
// Lossless Windows Unity representation. Values never cross the public JSON API.
struct NativePreference {
  std::u16string key;
  std::uint32_t type;
  std::vector<std::uint8_t> bytes;
  NativePreference(std::u16string name, std::uint32_t kind, std::vector<std::uint8_t> data)
      : key(std::move(name)), type(kind), bytes(std::move(data)) {}
  NativePreference(const NativePreference&) = default;
  NativePreference& operator=(const NativePreference&) = default;
  NativePreference(NativePreference&&) noexcept = default;
  NativePreference& operator=(NativePreference&&) noexcept = default;
  ~NativePreference() {
    volatile std::uint8_t* data = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) data[i] = 0;
  }
  bool operator==(const NativePreference&) const = default;
};
struct ImportUser {
  std::string sid, name;
  std::filesystem::path directory;
  bool current_user = false;
};
std::string CurrentUserSid();
struct ImportUserDiscovery {
  std::vector<ImportUser> users;
  bool requires_elevation = false;
  std::size_t unavailable_users = 0;
};
void ValidateImportUserSid(std::string_view sid);
ImportUser CurrentImportUser();
std::vector<ImportUser> ImportUsers(bool* requires_elevation = nullptr, std::size_t* unavailable_users = nullptr);
ImportUserDiscovery DiscoverImportUsers(std::string_view destination_sid);
ImportUserDiscovery DiscoverImportSources(bool allow_elevation);
ImportUser ResolveImportUser(std::string_view sid);
// Throws CatalogError(elevation_required) only for an actual access denial.
void CheckImportAccess(const ImportUser& user);
std::vector<NativePreference> CaptureUserPreferences(const ImportUser& user);
// Reads an already captured, clean hive in memory; never loads or alters a hive.
std::vector<NativePreference> ReadRegistryHivePreferences(const std::vector<std::uint8_t>& hive);
bool RegistryHiveHasPreferences(const std::vector<std::uint8_t>& hive);
std::u16string UnityPreferenceKey(std::u16string_view registry_name);
std::vector<NativePreference> CaptureImport(const ImportUser& user, bool allow_elevation);
void RunUserImportHelper(std::wstring_view command_line);
}
