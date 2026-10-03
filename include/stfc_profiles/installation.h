#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace stfc::profiles {

// A game session holds shared installation access for its lifetime. Updating or
// recovering an installation requires exclusive access to that exact directory.
// Windows retains directory/ancestor handles without delete sharing. Observation
// mode retains that namespace for status only, without owning a coordination lock.
class InstallationLease {
public:
  InstallationLease(const std::filesystem::path& root,
                    const std::filesystem::path& gameDirectory, bool exclusive,
                    bool observation_only = false);
  ~InstallationLease();
  InstallationLease(InstallationLease&&) noexcept;
  InstallationLease& operator=(InstallationLease&&) noexcept;
  InstallationLease(const InstallationLease&) = delete;
  InstallationLease& operator=(const InstallationLease&) = delete;
  const std::filesystem::path& Root() const;
  const std::filesystem::path& Directory() const;
  const std::string& Key() const;
  const std::string& PhysicalIdentity() const;
  bool Owns() const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

void CheckInstallationReady(const std::filesystem::path& root,
                            const std::filesystem::path& gameDirectory);

// Version 1 UTF-8 JSON requests: apiVersion, operation, root?, gameDirectory.
// Operations: installation-status, check-game-update, update-game,
// recover-game-update. Recovery rolls an interrupted transaction back; it never
// silently accepts a partly installed image. The selected directory is explicit.
std::string ExecuteInstallationRequest(std::string_view request_utf8);

} // namespace stfc::profiles
