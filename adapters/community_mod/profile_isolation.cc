// Extracted from Guffawaffle/stfc-mod at 323fb857f51f4cb08231d4b150ea8b5bb59340d1.
// See docs/PROVENANCE.json and LICENSE (GPL-3.0).
#include "stfc_profiles/community_mod_adapter.h"
#include "runtime_api.h"
#include <spdlog/spdlog.h>
#include <spud/detour.h>
#if _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(__aarch64__) || defined(_M_ARM64)
namespace spud::detail::arm64 { uintptr_t maybe_resolve_jump(uintptr_t); }
#else
namespace spud::detail::x64 { uintptr_t maybe_resolve_jump(uintptr_t); }
#endif

namespace stfc::profiles::community_mod {

namespace {

std::string profile_id;
std::filesystem::path profile_root;
SessionLease* profile_lease = nullptr;
std::unique_ptr<detail::RuntimeApi> runtime;
std::unique_ptr<ProfilePrefsStore> profile_store;

[[noreturn]] void FailClosed(const char* reason)
{
  spdlog::critical("[ProfileIsolationProbe] {}", reason);
  spdlog::default_logger()->flush();
  if (profile_lease) { try { profile_lease->MarkFailed(reason); } catch (...) {} }
#if _WIN32
  ExitProcess(190);
#else
  _exit(190);
#endif
  std::abort();
}

std::u16string_view RequiredString(void* value)
{
  if (!runtime || !value) FailClosed("The client supplied an invalid profile preference string");
  const auto size = runtime->string_length(value);
  auto data = runtime->string_chars(value);
  if (size < 0 || size > 8*1024*1024 || !data)
    FailClosed("The client supplied an invalid profile preference string");
  return {data,static_cast<std::size_t>(size)};
}

template <typename Action>
decltype(auto) WithProfileStore(const char* failure, Action&& action)
{
  try {
    if (!profile_store)
      FailClosed("Profile preference store is unavailable");
    return std::forward<Action>(action)(*profile_store);
  } catch (...) {
    FailClosed(failure);
  }
}

bool TrySetInt_Hook(auto, void* key, int value)
{
  return WithProfileStore("Could not persist an integer preference", [&](ProfilePrefsStore& store) {
    store.SetInt(RequiredString(key), value);
    return true;
  });
}

bool TrySetFloat_Hook(auto, void* key, float value)
{
  return WithProfileStore("Could not persist a float preference", [&](ProfilePrefsStore& store) {
    store.SetFloat(RequiredString(key), value);
    return true;
  });
}

bool TrySetString_Hook(auto, void* key, void* value)
{
  return WithProfileStore("Could not persist a string preference", [&](ProfilePrefsStore& store) {
    store.SetString(RequiredString(key), RequiredString(value));
    return true;
  });
}

int GetInt_Hook(auto, void* key, int fallback)
{
  return WithProfileStore("Could not read an integer preference", [&](ProfilePrefsStore& store) {
    return store.GetInt(RequiredString(key), fallback);
  });
}

float GetFloat_Hook(auto, void* key, float fallback)
{
  return WithProfileStore("Could not read a float preference", [&](ProfilePrefsStore& store) {
    return store.GetFloat(RequiredString(key), fallback);
  });
}

void* GetString_Hook(auto, void* key, void* fallback)
{
  return WithProfileStore("Could not read a string preference", [&](ProfilePrefsStore& store) -> void* {
    const auto value = store.GetString(RequiredString(key));
    if (!value) {
      if (fallback)
        return fallback;
      // Unity's Windows PlayerPrefs.GetString(missing, nullptr) returns an empty string.
      constexpr char16_t empty[] = u"";
      auto* result = runtime->string_new_utf16(empty, 0);
      if (!result)
        throw std::runtime_error("could not allocate empty preference string");
      return result;
    }
    auto* result = runtime->string_new_utf16(value->data(),
                                           static_cast<std::int32_t>(value->size()));
    if (!result)
      throw std::runtime_error("could not allocate preference string");
    return result;
  });
}

bool HasKey_Hook(auto, void* key)
{
  return WithProfileStore("Could not inspect a preference", [&](ProfilePrefsStore& store) {
    return store.HasKey(RequiredString(key));
  });
}

void DeleteKey_Hook(auto, void* key)
{
  WithProfileStore("Could not delete a preference", [&](ProfilePrefsStore& store) {
    store.DeleteKey(RequiredString(key));
  });
}

void DeleteAll_Hook(auto)
{
  WithProfileStore("Could not clear profile preferences", [](ProfilePrefsStore& store) { store.DeleteAll(); });
}

void Save_Hook(auto)
{
  WithProfileStore("Could not save profile preferences", [](ProfilePrefsStore& store) { store.Save(); });
}

bool LaunchProfileBrowser(void* url)
{
  if (!url) return false;
  return LaunchIsolatedBrowser(profile_root,profile_id,RequiredString(url));
}

bool PresentUrl_Hook(auto, void*, void* url)
{
  try {
    if (LaunchProfileBrowser(url))
      return true;
  } catch (...) {
  }
  FailClosed("Could not open the isolated sign-in browser");
}

void* Resolve(void* cls, const char* name, const char* result, std::initializer_list<const char*> args)
{
  return runtime->Resolve(cls,name,true,result,args);
}

} // namespace

void PrepareProfile(const std::filesystem::path& root, std::string_view id,
                    ProfileOpenMode mode, SessionLease& lease)
{
  if (profile_store) FailClosed("Profile preference store was already prepared");
  profile_root = root;
  profile_id = id;
  profile_lease = &lease;
  try {
    profile_store = std::make_unique<ProfilePrefsStore>(root,id,mode,lease);
  } catch (const std::exception& error) { FailClosed(error.what()); }
  catch (...) { FailClosed("Could not open the isolated preference store"); }
}

void InstallProfileHooks()
{
  if (!profile_store)
    FailClosed("Profile preference store was not prepared");

  if (runtime) FailClosed("Profile hooks were already installed");
  try { runtime=std::make_unique<detail::RuntimeApi>(); }
  catch (const std::exception& error) { FailClosed(error.what()); }
  void* prefs=nullptr;
  void* oidc=nullptr;
  try {
    prefs=runtime->Class("UnityEngine.CoreModule","UnityEngine","PlayerPrefs");
    oidc=runtime->Class("Playgami.Sdk.Identity.Runtime","Playgami.Identity.Api.Internal","OidcAuthorizer");
  } catch (const std::exception& error) { FailClosed(error.what()); }
  if (!prefs || !oidc) FailClosed("Required profile classes are unavailable");

  const std::array<void*, 10> pref_methods = {
      Resolve(prefs, "TrySetInt", "System.Boolean", {"System.String", "System.Int32"}),
      Resolve(prefs, "TrySetFloat", "System.Boolean", {"System.String", "System.Single"}),
      Resolve(prefs, "TrySetSetString", "System.Boolean", {"System.String", "System.String"}),
      Resolve(prefs, "GetInt", "System.Int32", {"System.String", "System.Int32"}),
      Resolve(prefs, "GetFloat", "System.Single", {"System.String", "System.Single"}),
      Resolve(prefs, "GetString", "System.String", {"System.String", "System.String"}),
      Resolve(prefs, "HasKey", "System.Boolean", {"System.String"}),
      Resolve(prefs, "DeleteKey", "System.Void", {"System.String"}),
      Resolve(prefs, "DeleteAll", "System.Void", {}),
      Resolve(prefs, "Save", "System.Void", {}),
  };
  const std::array<void*, 3> browser_methods = {
      runtime->Resolve(oidc, "PresentLoginUrlToUser", false,
                                                        "System.Boolean", {"System.String"}),
      runtime->Resolve(oidc, "PresentLogoutUrlToUser", false,
                                                        "System.Boolean", {"System.String"}),
      runtime->Resolve(oidc, "PresentLinkUrlToUser", false,
                                                        "System.Boolean", {"System.String"}),
  };
  for (auto* method : pref_methods)
    if (!method)
      FailClosed("A preference method is unavailable");
  for (auto* method : browser_methods)
    if (!method)
      FailClosed("A browser handoff method is unavailable");

  // The pinned SPUD installer resolves x64 jump thunks before patching.
  static_assert(sizeof(void*) == 8);
  const auto canonical_target = [](void* method) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return spud::detail::arm64::maybe_resolve_jump(reinterpret_cast<uintptr_t>(method));
#else
    return spud::detail::x64::maybe_resolve_jump(reinterpret_cast<uintptr_t>(method));
#endif
  };
  std::array<uintptr_t, pref_methods.size() + browser_methods.size()> targets{};
  std::transform(pref_methods.begin(), pref_methods.end(), targets.begin(), canonical_target);
  std::transform(browser_methods.begin(), browser_methods.end(), targets.begin() + pref_methods.size(),
                 canonical_target);
  for (size_t i = 0; i < targets.size(); ++i)
    for (size_t j = i + 1; j < targets.size(); ++j)
      if (targets[i] == targets[j])
        FailClosed("Profile hook methods share a native target");

  try {
    if (!SPUD_STATIC_DETOUR(pref_methods[0], TrySetInt_Hook)
        || !SPUD_STATIC_DETOUR(pref_methods[1], TrySetFloat_Hook)
        || !SPUD_STATIC_DETOUR(pref_methods[2], TrySetString_Hook)
        || !SPUD_STATIC_DETOUR(pref_methods[3], GetInt_Hook)
        || !SPUD_STATIC_DETOUR(pref_methods[4], GetFloat_Hook)
        || !SPUD_STATIC_DETOUR(pref_methods[5], GetString_Hook)
        || !SPUD_STATIC_DETOUR(pref_methods[6], HasKey_Hook)
        || !SPUD_STATIC_DETOUR(pref_methods[7], DeleteKey_Hook)
        || !SPUD_STATIC_DETOUR(pref_methods[8], DeleteAll_Hook)
        || !SPUD_STATIC_DETOUR(pref_methods[9], Save_Hook)
        || !SPUD_STATIC_DETOUR(browser_methods[0], PresentUrl_Hook)
        || !SPUD_STATIC_DETOUR(browser_methods[1], PresentUrl_Hook)
        || !SPUD_STATIC_DETOUR(browser_methods[2], PresentUrl_Hook))
      FailClosed("A profile hook could not be installed");
  } catch (...) {
    FailClosed("A profile hook failed to install");
  }
  try {
    profile_store->FinishNewProfile();
    profile_lease->MarkReady();
  } catch (...) {
    FailClosed("Could not finish profile enrollment");
  }
  spdlog::info("[ProfileIsolationProbe] Active for a dedicated launch profile");
}

} // namespace stfc::profiles::community_mod
