#pragma once

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace stfc::profiles {

// Lifetime exclusion is rooted outside the active/archive directory. This
// lease validates catalog admission and publishes a verified process identity.
class SessionLease {
public:
  SessionLease(const std::filesystem::path& root, std::string_view id);
  ~SessionLease();
  SessionLease(SessionLease&&) noexcept;
  SessionLease& operator=(SessionLease&&) noexcept;
  SessionLease(const SessionLease&) = delete;
  SessionLease& operator=(const SessionLease&) = delete;

  const std::filesystem::path& Root() const;
  const std::filesystem::path& Directory() const;
  const std::string& Id() const;
  bool Owns() const noexcept;
  bool PreferencesInitialized() const;
  void MarkReady();
  void MarkFailed(std::string_view reason);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Guardians may share an isolated browser. A catalog lifecycle mutation needs
// exclusive browser access, so account data cannot move under a live browser.
class BrowserLease {
public:
  BrowserLease(const std::filesystem::path& root, std::string_view id);
  ~BrowserLease();
  BrowserLease(BrowserLease&&) noexcept;
  BrowserLease& operator=(BrowserLease&&) noexcept;
  BrowserLease(const BrowserLease&) = delete;
  BrowserLease& operator=(const BrowserLease&) = delete;
  const std::filesystem::path& Root() const;
  const std::filesystem::path& Directory() const;
  const std::string& Id() const;
  bool Owns() const noexcept;
  // macOS publishes the stopped browser group's exact identity before resume.
  // Durable observation keeps lifecycle operations excluded if its guardian dies.
  void MarkBrowserStarted(std::uint32_t process_id);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace stfc::profiles
