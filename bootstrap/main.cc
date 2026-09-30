// Profile-only bootstrap: no community-mod feature/config initialization.
#include "stfc_profiles/catalog.h"
#include "stfc_profiles/community_mod_adapter.h"
#include "stfc_profiles/prefs_store.h"
#include "stfc_profiles/session.h"
#include "stfc_profiles/installation.h"
#include <spud/detour.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#if _WIN32
#include <Windows.h>
#include <shellapi.h>
void VersionDllInit();
extern "C" __declspec(dllexport) const unsigned int STFCProfilesExplicitLaunchContractV1=1;
#else
#include <crt_externs.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <unistd.h>
extern "C" __attribute__((visibility("default"))) const unsigned int STFCProfilesExplicitLaunchContractV1=1;
#endif
namespace {
std::string requested_id;
std::unique_ptr<stfc::profiles::SessionLease> session;
std::unique_ptr<stfc::profiles::InstallationLease> installation;
[[noreturn]] void Stop(const char* reason) noexcept
{
  if (session) { try { session->MarkFailed(reason); } catch (...) {} }
  std::fprintf(stderr,"STFC Profiles: %s\nUse stfc-profiles list and launch --profile <id>.\n",reason);
#if _WIN32
  char guidance[2048]{};
  std::snprintf(guidance,sizeof(guidance),"%s\n\nUse stfc-profiles list, then stfc-profiles launch --profile <id>.",reason);
  MessageBoxA(nullptr,guidance,"STFC Profiles: launch stopped",MB_OK|MB_ICONERROR);
  ExitProcess(190);
#else
  _exit(190);
#endif
}
void Consume(std::string_view value)
{
  if (!requested_id.empty()) throw std::runtime_error("Specify -stfc-profile exactly once");
  if (value.size()!=32 || value.find_first_not_of("0123456789abcdef")!=std::string_view::npos)
    throw std::runtime_error("-stfc-profile requires an immutable 32-character profile ID");
  requested_id=value;
}
void Parse()
{
#if _WIN32
  int count=0;
  auto arguments=CommandLineToArgvW(GetCommandLineW(),&count);
  if (!arguments) throw std::runtime_error("could not read game launch arguments");
  struct Release { LPWSTR* arguments; ~Release(){ LocalFree(arguments); } } release{arguments};
  for (int i=1;i<count;++i) {
    if (std::wstring_view(arguments[i])!=L"-stfc-profile") continue;
    if (++i==count) throw std::runtime_error("-stfc-profile requires an ID");
    std::string value;
    for (auto ch:std::wstring_view(arguments[i])) {
      if (ch>127) throw std::runtime_error("invalid profile ID");
      value.push_back(static_cast<char>(ch));
    }
    Consume(value);
  }
#else
  const auto count=*_NSGetArgc(); auto arguments=*_NSGetArgv();
  for (int i=1;i<count;++i) {
    if (std::string_view(arguments[i])!="-stfc-profile") continue;
    if (++i==count) throw std::runtime_error("-stfc-profile requires an ID");
    Consume(arguments[i]);
  }
#endif
}
int InitHook(auto original,const char* name)
{
  try {
    const auto root=stfc::profiles::DefaultCatalogRoot();
    session=std::make_unique<stfc::profiles::SessionLease>(root,requested_id);
    const auto directory=session->Directory();
    const bool initialized=std::filesystem::exists(directory/"player_prefs.bin.initialized");
    const bool exists=std::filesystem::exists(directory/"player_prefs.bin");
    const auto mode=session->PreferencesInitialized() || initialized
        ? stfc::profiles::ProfileOpenMode::Existing
        : exists ? stfc::profiles::ProfileOpenMode::Resume : stfc::profiles::ProfileOpenMode::New;
    stfc::profiles::community_mod::PrepareProfile(root,requested_id,mode,*session);
    const auto result=original(name);
    if (!result) Stop("the game failed to initialize its managed runtime");
    stfc::profiles::community_mod::InstallProfileHooks();
    return result;
  } catch (const std::exception& error) { Stop(error.what()); }
  catch (...) { Stop("could not install the requested profile"); }
}
void Initialize()
{
  try {
#if _WIN32
    std::vector<wchar_t> path(32768);
    const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if (!length || length==path.size()) throw std::runtime_error("game executable path is unavailable");
    const auto directory=std::filesystem::path(std::wstring(path.data(),length)).parent_path();
#else
    std::vector<char> path(32768);auto length=static_cast<std::uint32_t>(path.size());
    if (_NSGetExecutablePath(path.data(),&length)!=0) throw std::runtime_error("game executable path is unavailable");
    const auto directory=std::filesystem::canonical(path.data()).parent_path();
#endif
    const auto root=stfc::profiles::DefaultCatalogRoot();
    installation=std::make_unique<stfc::profiles::InstallationLease>(root,directory,false);
    stfc::profiles::CheckInstallationReady(root,directory);
    Parse();
    if (requested_id.empty()) return;
    stfc::profiles::community_mod::InstallProcessAdmission(requested_id);
#if _WIN32
    auto assembly=LoadLibraryExW(L"GameAssembly.dll",nullptr,LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!assembly) throw std::runtime_error("GameAssembly.dll is unavailable");
    auto initializer=GetProcAddress(assembly,"il2cpp_init");
#else
    std::vector<char> executable(32768); auto size=static_cast<std::uint32_t>(executable.size());
    if (_NSGetExecutablePath(executable.data(),&size)!=0) throw std::runtime_error("game executable path is unavailable");
    const auto library=std::filesystem::path(executable.data()).parent_path()/"../Frameworks/GameAssembly.dylib";
    auto assembly=dlopen(library.c_str(),RTLD_NOW|RTLD_GLOBAL);
    if (!assembly) throw std::runtime_error("GameAssembly.dylib is unavailable");
    auto initializer=dlsym(assembly,"il2cpp_init");
#endif
    if (!initializer || !SPUD_STATIC_DETOUR(initializer,InitHook))
      throw std::runtime_error("the early game initialization hook is unavailable");
  } catch (const std::exception& error) { Stop(error.what()); }
  catch (...) { Stop("could not initialize the requested profile"); }
}
} // namespace
#if _WIN32
namespace {
// Keep all process-only work out of the thread-notification frame. The static
// CRT requires thread notifications and graphics threads may have small stacks.
__declspec(noinline) BOOL ProcessAttach()
{
  try {
    std::vector<wchar_t> executable(32768);
    const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if (!length || length==executable.size()) Stop("could not identify the game executable");
    const auto name=std::filesystem::path(std::wstring(executable.data(),length)).filename().wstring();
    if (CompareStringOrdinal(name.c_str(),-1,L"prime.exe",-1,TRUE)!=CSTR_EQUAL) return TRUE;
    VersionDllInit();
    Initialize();
    return TRUE;
  } catch (const std::exception& error) { Stop(error.what()); }
  catch (...) { Stop("could not initialize the game bootstrap"); }
}
} // namespace
BOOL WINAPI DllMain(HINSTANCE,DWORD reason,LPVOID)
{
  if (reason==DLL_PROCESS_ATTACH) return ProcessAttach();
  return TRUE;
}
#else
__attribute__((constructor)) static void ProfileBootstrap() { Initialize(); }
#endif
