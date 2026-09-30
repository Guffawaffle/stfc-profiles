// Extracted preference semantics: see docs/PROVENANCE.json and LICENSE (GPL-3.0).
#pragma once

#include "stfc_profiles/session.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace stfc::profiles {
enum class ProfileOpenMode { New, Resume, Existing };

// The caller retains its live SessionLease until after this store is destroyed.
// The lease excludes catalog moves and all other writers under the stable ID.
class ProfilePrefsStore {
public:
  ProfilePrefsStore(const std::filesystem::path& root, std::string_view id,
                    ProfileOpenMode mode, SessionLease& lease);
  ~ProfilePrefsStore() = default;
  ProfilePrefsStore(const ProfilePrefsStore&) = delete;
  ProfilePrefsStore& operator=(const ProfilePrefsStore&) = delete;
  void SetInt(std::u16string_view key, std::int32_t value);
  void SetFloat(std::u16string_view key, float value);
  void SetString(std::u16string_view key, std::u16string_view value);
  std::int32_t GetInt(std::u16string_view key, std::int32_t fallback) const;
  float GetFloat(std::u16string_view key, float fallback) const;
  std::optional<std::u16string> GetString(std::u16string_view key) const;
  bool HasKey(std::u16string_view key) const;
  void DeleteKey(std::u16string_view key);
  void DeleteAll();
  void Save();
  void FinishNewProfile();
private:
  using Value = std::variant<std::int32_t, float, std::u16string>;
  using Values = std::map<std::u16string, Value, std::less<>>;
  void Serialize(const Values&, std::vector<std::uint8_t>&) const;
  Values Deserialize(const std::uint8_t*, std::size_t) const;
  Values LoadStore(const std::filesystem::path&) const;
  void Persist(const Values&);
  void MarkInitialized();
  void RequireLease() const;
  void Set(std::u16string_view, Value);
  SessionLease* lease_;
  std::filesystem::path file_path_, initialized_path_;
  std::string id_;
  std::u16string profile_id_;
  bool file_exists_ = false, initialized_ = false;
  mutable std::mutex mutex_;
  Values values_;
};
} // namespace stfc::profiles
