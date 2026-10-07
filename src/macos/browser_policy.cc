#include "stfc_profiles/macos_browser_policy.h"
#include <CoreFoundation/CoreFoundation.h>
namespace stfc::profiles {
bool MacBrowserAllowsPrivateStore(std::string_view preference_domain)
{
  const auto domain = CFStringCreateWithBytes(nullptr,
      reinterpret_cast<const UInt8*>(preference_domain.data()), preference_domain.size(), kCFStringEncodingUTF8, false);
  if (!domain) return false;
  const auto synced = CFPreferencesAppSynchronize(domain);
  const auto override = CFPreferencesCopyAppValue(CFSTR("UserDataDir"), domain);
  const auto forced = CFPreferencesAppValueIsForced(CFSTR("UserDataDir"), domain);
  if (override) CFRelease(override);
  CFRelease(domain);
  return synced && !override && !forced;
}
}
