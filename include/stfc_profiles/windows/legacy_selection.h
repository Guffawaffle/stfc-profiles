// Extracted from Guffawaffle/stfc-mod at 323fb857f51f4cb08231d4b150ea8b5bb59340d1.
// See docs/PROVENANCE.json and LICENSE (GPL-3.0).
#pragma once

#if _WIN32

#include <filesystem>
#include <string>

namespace stfc::profiles::windows {

struct ProfileSelection {
  bool                  marked = false;
  bool                  enroll = false;
  bool                  resume = false;
  std::wstring          id;
  std::filesystem::path config_path;
};

const ProfileSelection& ResolveProfileSelection();
void                    StartProfileEnrollment();
void                    CompleteProfileEnrollment();
[[noreturn]] void       AbortProfileLaunch(const char* reason);

} // namespace stfc::profiles::windows

#endif
