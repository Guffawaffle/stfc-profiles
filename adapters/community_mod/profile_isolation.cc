// Extracted from Guffawaffle/stfc-mod at 323fb857f51f4cb08231d4b150ea8b5bb59340d1.
// See docs/PROVENANCE.json and LICENSE (GPL-3.0).
#if _WIN32

#include "il2cpp/method_contract.h"
#include "stfc_profiles/windows/prefs_store.h"
#include "stfc_profiles/community_mod_adapter.h"

#include <il2cpp/il2cpp-functions.h>
#include <il2cpp/il2cpp_helper.h>

#include <spdlog/spdlog.h>
#include <spud/detour.h>

#include <Windows.h>
#include <ShlObj.h>

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

using namespace stfc::profiles::windows;

namespace spud::detail::x64 {
uintptr_t maybe_resolve_jump(uintptr_t);
}

namespace stfc::profiles::community_mod {

namespace {

std::wstring                       profile_id;
std::unique_ptr<ProfilePrefsStore> profile_store;

[[noreturn]] void FailClosed(const char* reason)
{
  spdlog::critical("[ProfileIsolationProbe] {}", reason);
  spdlog::default_logger()->flush();
  ExitProcess(190);
  std::abort();
}

std::wstring KnownFolder(REFKNOWNFOLDERID folder_id)
{
  PWSTR path = nullptr;
  if (FAILED(SHGetKnownFolderPath(folder_id, 0, nullptr, &path)) || !path) {
    CoTaskMemFree(path);
    return {};
  }
  std::wstring result(path);
  CoTaskMemFree(path);
  return result;
}

bool HasForcedEdgeUserDataDir()
{
  for (const auto root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    DWORD size = 0;
    const auto status = RegGetValueW(root, L"SOFTWARE\\Policies\\Microsoft\\Edge", L"UserDataDir", RRF_RT_ANY,
                                     nullptr, nullptr, &size);
    if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)
      return true;
  }
  return false;
}

std::u16string_view RequiredString(Il2CppString* value)
{
  if (!value || value->length < 0)
    FailClosed("The client supplied an invalid profile preference string");
  return {reinterpret_cast<const char16_t*>(value->chars), static_cast<std::size_t>(value->length)};
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

bool TrySetInt_Hook(auto, Il2CppString* key, int value)
{
  return WithProfileStore("Could not persist an integer preference", [&](ProfilePrefsStore& store) {
    store.SetInt(RequiredString(key), value);
    return true;
  });
}

bool TrySetFloat_Hook(auto, Il2CppString* key, float value)
{
  return WithProfileStore("Could not persist a float preference", [&](ProfilePrefsStore& store) {
    store.SetFloat(RequiredString(key), value);
    return true;
  });
}

bool TrySetString_Hook(auto, Il2CppString* key, Il2CppString* value)
{
  return WithProfileStore("Could not persist a string preference", [&](ProfilePrefsStore& store) {
    store.SetString(RequiredString(key), RequiredString(value));
    return true;
  });
}

int GetInt_Hook(auto, Il2CppString* key, int fallback)
{
  return WithProfileStore("Could not read an integer preference", [&](ProfilePrefsStore& store) {
    return store.GetInt(RequiredString(key), fallback);
  });
}

float GetFloat_Hook(auto, Il2CppString* key, float fallback)
{
  return WithProfileStore("Could not read a float preference", [&](ProfilePrefsStore& store) {
    return store.GetFloat(RequiredString(key), fallback);
  });
}

Il2CppString* GetString_Hook(auto, Il2CppString* key, Il2CppString* fallback)
{
  return WithProfileStore("Could not read a string preference", [&](ProfilePrefsStore& store) -> Il2CppString* {
    const auto value = store.GetString(RequiredString(key));
    if (!value) {
      if (fallback)
        return fallback;
      // Unity's Windows PlayerPrefs.GetString(missing, nullptr) returns an empty string.
      constexpr char16_t empty[] = u"";
      auto* result = il2cpp_string_new_utf16(reinterpret_cast<const Il2CppChar*>(empty), 0);
      if (!result)
        throw std::runtime_error("could not allocate empty preference string");
      return result;
    }
    auto* result = il2cpp_string_new_utf16(reinterpret_cast<const Il2CppChar*>(value->data()),
                                           static_cast<std::int32_t>(value->size()));
    if (!result)
      throw std::runtime_error("could not allocate preference string");
    return result;
  });
}

bool HasKey_Hook(auto, Il2CppString* key)
{
  return WithProfileStore("Could not inspect a preference", [&](ProfilePrefsStore& store) {
    return store.HasKey(RequiredString(key));
  });
}

void DeleteKey_Hook(auto, Il2CppString* key)
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

bool LaunchProfileBrowser(Il2CppString* url)
{
  if (!url)
    return false;
  std::wstring address(reinterpret_cast<const wchar_t*>(url->chars), url->length);
  if (!address.starts_with(L"https://") || address.find_first_of(L"\"\r\n\t ") != std::wstring::npos)
    return false;

  if (HasForcedEdgeUserDataDir())
    return false;
  const auto local_app_data = KnownFolder(FOLDERID_LocalAppData);
  if (local_app_data.empty())
    return false;
  std::filesystem::path browser;
  for (const auto* folder_id : {&FOLDERID_ProgramFilesX86, &FOLDERID_ProgramFiles, &FOLDERID_LocalAppData}) {
    const auto base = KnownFolder(*folder_id);
    if (base.empty())
      continue;
    const auto candidate = std::filesystem::path(base) / L"Microsoft" / L"Edge" / L"Application" / L"msedge.exe";
    if (std::filesystem::is_regular_file(candidate)) {
      browser = candidate;
      break;
    }
  }
  if (browser.empty())
    return false;
  const auto data_dir = std::filesystem::path(local_app_data) / L"STFC Community Mod" / L"BrowserProfiles" / profile_id;
  std::filesystem::create_directories(data_dir);

  std::wstring command = L"\"" + browser.wstring() + L"\" --user-data-dir=\"" + data_dir.wstring()
                         + L"\" --new-window \"" + address + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  const bool started = CreateProcessW(browser.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                      &startup, &process) != 0;
  if (started) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  return started;
}

bool PresentUrl_Hook(auto, void*, Il2CppString* url)
{
  try {
    if (LaunchProfileBrowser(url))
      return true;
  } catch (...) {
  }
  FailClosed("Could not open the isolated sign-in browser");
}

void* Resolve(Il2CppClass* cls, const char* name, const char* result, std::initializer_list<const char*> args)
{
  return method_contract::Pointer(method_contract::Resolve(cls, name, true, result, args));
}

} // namespace

void PrepareProfile(std::wstring_view id, ProfileOpenMode mode)
{
  if (profile_store)
    FailClosed("Profile preference store was already prepared");
  profile_id = id;
  const auto local_app_data = KnownFolder(FOLDERID_LocalAppData);
  if (local_app_data.empty())
    FailClosed("Local app data is unavailable");
  try {
    profile_store = std::make_unique<ProfilePrefsStore>(local_app_data, profile_id, mode);
  } catch (...) {
    FailClosed("Could not open the isolated preference store");
  }
}

void InstallProfileHooks()
{
  if (!profile_store)
    FailClosed("Profile preference store was not prepared");

  auto prefs = il2cpp_get_class_helper("UnityEngine.CoreModule", "UnityEngine", "PlayerPrefs");
  auto oidc = il2cpp_get_class_helper("Playgami.Sdk.Identity.Runtime", "Playgami.Identity.Api.Internal",
                                     "OidcAuthorizer");
  if (!prefs.get_cls() || !oidc.get_cls())
    FailClosed("Required profile classes are unavailable");

  const std::array<void*, 10> pref_methods = {
      Resolve(prefs.get_cls(), "TrySetInt", "System.Boolean", {"System.String", "System.Int32"}),
      Resolve(prefs.get_cls(), "TrySetFloat", "System.Boolean", {"System.String", "System.Single"}),
      Resolve(prefs.get_cls(), "TrySetSetString", "System.Boolean", {"System.String", "System.String"}),
      Resolve(prefs.get_cls(), "GetInt", "System.Int32", {"System.String", "System.Int32"}),
      Resolve(prefs.get_cls(), "GetFloat", "System.Single", {"System.String", "System.Single"}),
      Resolve(prefs.get_cls(), "GetString", "System.String", {"System.String", "System.String"}),
      Resolve(prefs.get_cls(), "HasKey", "System.Boolean", {"System.String"}),
      Resolve(prefs.get_cls(), "DeleteKey", "System.Void", {"System.String"}),
      Resolve(prefs.get_cls(), "DeleteAll", "System.Void", {}),
      Resolve(prefs.get_cls(), "Save", "System.Void", {}),
  };
  const std::array<void*, 3> browser_methods = {
      method_contract::Pointer(method_contract::Resolve(oidc.get_cls(), "PresentLoginUrlToUser", false,
                                                        "System.Boolean", {"System.String"})),
      method_contract::Pointer(method_contract::Resolve(oidc.get_cls(), "PresentLogoutUrlToUser", false,
                                                        "System.Boolean", {"System.String"})),
      method_contract::Pointer(method_contract::Resolve(oidc.get_cls(), "PresentLinkUrlToUser", false,
                                                        "System.Boolean", {"System.String"})),
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
    return spud::detail::x64::maybe_resolve_jump(reinterpret_cast<uintptr_t>(method));
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
  } catch (...) {
    FailClosed("Could not finish profile enrollment");
  }
  spdlog::info("[ProfileIsolationProbe] Active for a dedicated launch profile");
}

} // namespace stfc::profiles::community_mod

#endif
