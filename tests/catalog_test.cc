#include "stfc_profiles/catalog.h"
#include "stfc_profiles/identity.h"
#include "stfc_profiles/prefs_store.h"
#include "stfc_profiles/session.h"
#if __APPLE__
#include "../src/prefs_crypto.h"
#endif
#include <nlohmann/json.hpp>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if _WIN32
#include <Windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
using namespace stfc::profiles;
using Json=nlohmann::json;
namespace {
namespace fs=std::filesystem;
void Check(bool condition,const std::string& message) { if (!condition) throw std::runtime_error(message); }
template<class Action> void Throws(Action action,const char* message)
{ try { action(); } catch (const std::exception&) { return; } throw std::runtime_error(message); }
struct Fixture {
  fs::path root;
  std::vector<std::string> created_ids;
  Fixture() {
#if _WIN32
    wchar_t directory[MAX_PATH+1]{},file[MAX_PATH+1]{};
    const auto length=GetTempPathW(MAX_PATH+1,directory);
    Check(length>0 && length<=MAX_PATH,"temporary directory unavailable");
    Check(GetTempFileNameW(directory,L"spc",0,file)!=0,"temporary test path unavailable");
    root=file;fs::remove(root);fs::create_directory(root);
#else
    char directory[]="/tmp/stfc-profiles-catalog-XXXXXX";
    Check(mkdtemp(directory)!=nullptr,"temporary directory unavailable");root=directory;
#endif
  }
  ~Fixture() {
#if __APPLE__
    for (const auto& id:created_ids) { try { detail::EraseProtectedPrefsIdentity(id); } catch (...) {} }
#endif
    std::error_code error;fs::remove_all(root,error);
  }
  std::string RootText() const { const auto bytes=root.u8string();return {bytes.begin(),bytes.end()}; }
  Json Call(Json request) {
    if (!request.contains("apiVersion")) request["apiVersion"]=1;request["root"]=RootText();
    return Json::parse(ExecuteCatalogRequest(request.dump()));
  }
  Json Create(const char* name) {
    auto response=Call({{"operation","create"},{"name",name}});
    Check(response.value("ok",false),response.dump());created_ids.push_back(response.at("profile").at("id").get<std::string>());return response.at("profile");
  }
  Json Find(const std::string& id,bool archived=false) {
    const auto response=Call({{"operation","list"},{"archived",archived}});
    Check(response.value("ok",false),response.dump());
    for (const auto& profile:response.at("profiles")) if (profile.at("id")==id) return profile;
    throw std::runtime_error("synthetic profile not found");
  }
  Json Mutate(const char* operation,const std::string& id,bool archived=false) {
    return Call({{"operation",operation},{"id",id},{"archived",archived},
                 {"permanent",std::string_view(operation)=="delete"},{"expectedRevision",Find(id,archived).at("revision")}});
  }
};
void CatalogLocationIsReadOnly()
{
  const auto response=Json::parse(ExecuteCatalogRequest(Json{{"apiVersion",1},{"operation","catalog-location"}}.dump()));
  Check(response.value("ok",false),response.dump());
  const auto root=fs::u8path(response.at("catalogRoot").get<std::string>());
  Check(root==DefaultCatalogRoot(),"catalog location disagrees with the host root");
#if _WIN32
  PWSTR value=nullptr;
  Check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_NO_PACKAGE_REDIRECTION,nullptr,&value)),"OS-user data path unavailable");
  const auto expected=fs::path(value)/"STFC Profiles";CoTaskMemFree(value);
  Check(root==expected,"profile root is redirected into a private package catalog");
#endif
  Fixture f;
  const auto rejected=f.Call({{"operation","catalog-location"}});
  Check(!rejected.value("ok",false)&&fs::is_empty(f.root),"read-only location request published catalog state");
}
#if _WIN32
void NativeApiProjectsTypedDefault()
{
  Fixture f;wchar_t executable[32768]{};
  Check(GetModuleFileNameW(nullptr,executable,32768)>0,"native API fixture path unavailable");
  const auto module=LoadLibraryExW((fs::path(executable).parent_path()/"stfc-profiles-native.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
  Check(module!=nullptr,"native catalog API unavailable");
  struct Unload { HMODULE module; ~Unload(){ FreeLibrary(module); } } unload{module};
  using Request=int(__cdecl*)(const char*,char**);using Free=void(__cdecl*)(void*);
  const auto call=reinterpret_cast<Request>(GetProcAddress(module,"stfc_profiles_catalog_request_v1"));
  const auto release=reinterpret_cast<Free>(GetProcAddress(module,"stfc_profiles_free_v1"));
  Check(call && release,"stable catalog allocation ABI is missing");
  const auto request=Json{{"apiVersion",2},{"operation","ensure-default"},{"root",f.RootText()}}.dump();
  char* output=nullptr;Check(call(request.c_str(),&output)==0 && output,"native typed request failed allocation");
  struct Release { char* output; Free release; ~Release(){release(output);} } allocation{output,release};
  const auto response=Json::parse(output);
  Check(response.at("apiVersion")==2 && response.value("ok",false) && response.at("profile").at("kind")=="windows-user",response.dump());
  Check(f.Call({{"apiVersion",2},{"operation","resolve-default"}}).at("profile")==response.at("profile"),"native ABI and direct core disagree on shared Default identity");
}
void MakeSyntheticInstallation(const fs::path& directory)
{
  fs::create_directory(directory);fs::create_directory(directory/"prime_Data");
  for (const auto* file:{"prime.exe","GameAssembly.dll","UnityPlayer.dll"}) std::ofstream(directory/file)<<"synthetic";
  std::ofstream(directory/".version")<<"&game=270\n";
}
void InstallationRegistrationsBindPhysicalDirectories()
{
  Fixture f;const auto game=f.root/"registered-game";MakeSyntheticInstallation(game);
  auto registered=f.Call({{"apiVersion",2},{"operation","register-installation"},{"name","Primary"},{"gameDirectory",game.string()}});
  Check(registered.value("ok",false) && registered.at("created")==true,registered.dump());
  const auto installation=registered.at("installation");const auto id=installation.at("id").get<std::string>();
  const auto registration_file=f.root/"installations"/id/"metadata.json";
  std::ifstream registration_input(registration_file,std::ios::binary);
  const std::string registration_bytes{std::istreambuf_iterator<char>(registration_input),{}};
  registration_input.close();
  for(const auto& version:{Json(1.5),Json(std::uint64_t{4294967297})}){
    auto malformed=Json::parse(registration_bytes);malformed["schemaVersion"]=version;
    std::ofstream(registration_file,std::ios::binary)<<malformed.dump();
    const auto refused=f.Call({{"apiVersion",2},{"operation","installation-paths"},{"installationId",id}});
    Check(!refused.value("ok",false)&&refused.at("error").at("code")=="invalid_metadata",
          "Unsupported numeric installation schema was coerced to version one");
  }
  std::ofstream(registration_file,std::ios::binary)<<registration_bytes;
  Check(ValidId(id) && installation.at("state")=="available" && installation.at("physicalIdentity").get<std::string>().size()==64,"installation lacks stable physical binding");
  auto alias=(game/"..").lexically_normal()/game.filename();
  auto alias_name=alias.wstring();for (auto& ch:alias_name) ch=std::towupper(ch);
  const auto same=f.Call({{"apiVersion",2},{"operation","register-installation"},{"name","Different label"},{"gameDirectory",fs::path(alias_name).string()}});
  Check(same.value("ok",false) && same.at("created")==false && same.at("installation")==installation,"alias registration duplicated or retargeted original installation");
  const auto newer=f.Call({{"apiVersion",2},{"operation","create"},{"name","Bound account"},{"preferredInstallationId",id}});
  Check(newer.value("ok",false),newer.dump());
  auto account=newer.at("profile");const auto account_id=account.at("id").get<std::string>();
  Check(account.at("preferredInstallationId")==id && account.at("gameDirectory")==installation.at("gameDirectory"),"create did not atomically bind preferred registration");
  auto setup=f.Call({{"apiVersion",2},{"operation","ensure-default"}}).at("profile");
  const auto setup_id=setup.at("id").get<std::string>();
  const auto edit=f.Call({{"apiVersion",2},{"operation","edit"},{"id",setup_id},{"preferredInstallationId",id},{"expectedRevision",setup.at("revision")}});
  Check(edit.value("ok",false) && edit.at("profile").at("preferredInstallationId")==id,"Default could not bind selected registration");
  setup=edit.at("profile");
  const auto other=f.root/"different-game";MakeSyntheticInstallation(other);
  const auto mismatch=f.Call({{"apiVersion",2},{"operation","create"},{"name","No partial account"},{"preferredInstallationId",id},{"gameDirectory",other.string()}});
  Check(!mismatch.value("ok",false) && mismatch.at("error").at("code")=="installation_changed","contradictory registration/path published profile");
  const auto clear=f.Call({{"apiVersion",2},{"operation","edit"},{"id",setup_id},{"preferredInstallationId",""},{"gameDirectory",other.string()},{"expectedRevision",setup.at("revision")}});
  Check(clear.value("ok",false) && clear.at("profile").at("preferredInstallationId")==""
      && fs::u8path(clear.at("profile").at("gameDirectory").get<std::string>())==fs::canonical(other),"explicit clear-binding/path edit ignored the new path");
  const auto rebound=f.Call({{"apiVersion",2},{"operation","edit"},{"id",setup_id},{"preferredInstallationId",id},{"expectedRevision",clear.at("profile").at("revision")}});
  Check(rebound.value("ok",false),rebound.dump());setup=rebound.at("profile");
  const auto moved=f.root/"moved-game";fs::rename(game,moved);
  const auto stale=f.Call({{"apiVersion",2},{"operation","installation-paths"},{"installationId",id}});
  Check(stale.value("ok",false) && stale.at("installation").at("state")=="unknown" && stale.at("installation").at("gameDirectory")==installation.at("gameDirectory"),"missing installation silently changed registration");
  const auto moved_registration=f.Call({{"apiVersion",2},{"operation","register-installation"},{"name","Moved"},{"gameDirectory",moved.string()}});
  Check(!moved_registration.value("ok",false) && moved_registration.at("error").at("code")=="installation_unknown","registration silently relocated a saved installation");
  MakeSyntheticInstallation(game);
  const auto replaced=f.Call({{"apiVersion",2},{"operation","installation-paths"},{"installationId",id}});
  Check(replaced.value("ok",false) && replaced.at("installation").at("state")=="unknown","new folder at old path passed physical binding");
  const auto launch=f.Call({{"apiVersion",2},{"operation","launch-ordinary"},{"id",setup_id}});
  Check(!launch.value("ok",false) && launch.at("error").at("code")=="installation_changed","ordinary launch used a replacement installation");
  const auto isolated_launch=f.Call({{"apiVersion",2},{"operation","launch"},{"id",account_id}});
  Check(!isolated_launch.value("ok",false) && isolated_launch.at("error").at("code")=="installation_changed","isolated launch used a replacement installation");
  const auto select=f.Call({{"apiVersion",2},{"operation","edit"},{"id",setup_id},{"preferredInstallationId",id},{"expectedRevision",setup.at("revision")}});
  Check(!select.value("ok",false),"selection accepted a stale registration");
  const auto new_registration=f.Call({{"apiVersion",2},{"operation","register-installation"},{"name","Replacement"},{"gameDirectory",game.string()}});
  Check(new_registration.value("ok",false) && new_registration.at("installation").at("id")!=id,"explicit replacement did not receive a distinct registration ID");
  const auto listed=f.Call({{"apiVersion",2},{"operation","installations"}});
  Check(listed.at("installations").size()==2 && listed.at("issues").empty(),"registrations did not retain unknown original alongside deliberate new installation");
  Check(f.Call({{"apiVersion",2},{"operation","paths"},{"id",setup_id}}).at("profile").at("preferredInstallationId")==id,"replacement retargeted existing profile preference");
  Check(fs::is_empty(f.root/"sessions"),"registration generated game session authority");
}
struct OrdinaryChildren {
  std::vector<HANDLE> handles;
  ~OrdinaryChildren() {
    for (auto process:handles) {
      if (WaitForSingleObject(process,0)==WAIT_TIMEOUT) TerminateProcess(process,190);
      WaitForSingleObject(process,5000);CloseHandle(process);
    }
  }
  void Add(const Json& response) {
    const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE|PROCESS_TERMINATE,FALSE,response.at("processId").get<DWORD>());
    Check(process!=nullptr,"ordinary synthetic child is not observable");handles.push_back(process);
  }
};
void OrdinaryLaunchDoesNotRequestIsolation()
{
  Fixture f;const auto profile=f.Call({{"apiVersion",2},{"operation","ensure-default"}}).at("profile");
  const auto id=profile.at("id").get<std::string>();
  const auto game=f.root/"ordinary-game";fs::create_directory(game);
  wchar_t executable[32768]{};Check(GetModuleFileNameW(nullptr,executable,32768)>0,"fixture executable unavailable");
  fs::copy_file(executable,game/"prime.exe");
  OrdinaryChildren children;
  for (int attempt=0;attempt<2;++attempt) {
    const auto launch=f.Call({{"apiVersion",2},{"operation","launch-ordinary"},{"id",id},{"gameDirectory",game.string()}});
    Check(launch.value("ok",false),launch.dump());children.Add(launch);
    Check(launch.at("readiness")=="ordinary" && !launch.at("started").get<std::string>().empty()
      && fs::u8path(launch.at("executable").get<std::string>())==fs::canonical(game/"prime.exe"),"ordinary launch omitted exact process snapshot");
    const auto marker=game/("arguments-"+std::to_string(launch.at("processId").get<DWORD>())+".txt");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!fs::exists(marker) && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(fs::exists(marker),"ordinary child did not record its command line");
    std::ifstream stream(marker);std::string text{std::istreambuf_iterator<char>(stream),{}};
    Check(text.find("-stfc-profile")==std::string::npos && text.find("-logFile")==std::string::npos,"ordinary launch activated isolation or profile log routing");
  }
  Check(fs::is_empty(f.root/"sessions"),"ordinary launch published isolated session authority");
  Check(f.Call({{"apiVersion",2},{"operation","sessions"}}).at("sessions").empty(),"ordinary process falsely reported isolated readiness");
  Check(f.Call({{"apiVersion",2},{"operation","resolve-default"}}).at("profile")==profile,"ordinary launches changed persistent descriptor");
  Check(!fs::exists(f.root/".locks"/(id+".lock")) && !fs::exists(f.root/".locks"/(id+".data.lock")),"ordinary launch entered isolated writer/browser exclusion");
  std::ofstream(game/"ordinary-release")<<"release";
  for (auto handle:children.handles) Check(WaitForSingleObject(handle,5000)==WAIT_OBJECT_0,"ordinary child failed to stop");
}
void DefaultIsTypedMetadataOnly()
{
  Fixture f;
  for (const auto& version: {Json(2.5),Json(4294967298ull)}) {
    const auto rejected=f.Call({{"apiVersion",version},{"operation","ensure-default"}});
    Check(!rejected.value("ok",false) && rejected.at("error").at("code")=="api_version","malformed version entered typed catalog API");
  }
  const auto isolated=f.Create("Account");
  const auto isolated_before=fs::file_size(f.root/"profiles"/isolated.at("id").get<std::string>()/"metadata.json");
  const auto missing=f.Call({{"apiVersion",2},{"operation","resolve-default"}});
  Check(!missing.value("ok",false) && missing.at("error").at("code")=="default_missing","resolve created a Default descriptor");
  auto response=f.Call({{"apiVersion",2},{"operation","ensure-default"}});
  Check(response.value("ok",false) && response.at("apiVersion")==2,response.dump());
  const auto profile=response.at("profile");const auto id=profile.at("id").get<std::string>();
  Check(ValidId(id) && profile.at("kind")=="windows-user" && profile.at("name")=="Default"
      && profile.at("builtIn")==true && !profile.at("ownerUserId").get<std::string>().empty(),"Default lacks typed immutable user identity");
  Check(!profile.contains("preferencesInitialized") && !profile.contains("configPath") && !profile.contains("logPath"),"Default claims isolated data ownership");
  Check(profile.at("preferenceScope")=="windows-user" && profile.at("configurationScope")=="installation","Default data scopes are incorrect");
  Check(std::distance(fs::directory_iterator(f.root/"profiles"/id),fs::directory_iterator{})==1,"Default created isolated account files");
  Check(f.Call({{"apiVersion",2},{"operation","ensure-default"}}).at("profile")==profile,"ensure changed persistent Default identity");
  Check(f.Call({{"apiVersion",2},{"operation","resolve-default"}}).at("profile")==profile,"resolve disagrees with ensure");
  Check(f.Call({{"operation","list"}}).at("profiles").size()==1,"older client received typed Default as isolated profile");
  const auto old_direct=f.Call({{"operation","paths"},{"id",id}});
  Check(!old_direct.value("ok",false) && old_direct.at("error").at("code")=="api_version","older client opened Default as isolated profile");
  const auto list=f.Call({{"apiVersion",2},{"operation","list"}});
  Check(list.at("profiles").size()==2 && list.at("issues").empty(),"typed discovery lost existing isolated profile");
  for (const auto& item:list.at("profiles"))
    if (item.at("id")==isolated.at("id")) Check(item.at("kind")=="isolated" && item.at("configPath")==isolated.at("configPath"),"existing isolated projection changed ownership");
  for (const auto* operation:{"archive","rename","delete","launch"}) {
    const auto denied=f.Call({{"apiVersion",2},{"operation",operation},{"id",id},{"name","Renamed"},{"expectedRevision",profile.at("revision")}});
    Check(!denied.value("ok",false) && denied.at("error").at("code")=="profile_kind",std::string("Default accepted isolated operation ")+operation);
  }
  Throws([&]{ SessionLease access(f.root,id); },"Default acquired an isolated writer lease");
  Throws([&]{ BrowserLease access(f.root,id); },"Default acquired an isolated browser/data lease");
  const auto other_launch=f.Call({{"apiVersion",2},{"operation","launch-ordinary"},{"id",isolated.at("id")}});
  Check(!other_launch.value("ok",false) && other_launch.at("error").at("code")=="profile_kind","ordinary launch bypassed isolated account routing");
  const auto missing_install=f.Call({{"apiVersion",2},{"operation","launch-ordinary"},{"id",id}});
  Check(!missing_install.value("ok",false) && missing_install.at("error").at("code")=="installation_required","ordinary launch requires no installation");
  const auto game=f.root/"synthetic-game";fs::create_directory(game);
  const auto edited=f.Call({{"apiVersion",2},{"operation","edit"},{"id",id},{"gameDirectory",game.string()},{"expectedRevision",profile.at("revision")}});
  Check(edited.value("ok",false) && edited.at("profile").at("id")==id,edited.dump());
  Check(f.Call({{"apiVersion",2},{"operation","ensure-default"}}).at("profile").at("id")==id,"installation change recreated Default");
  Check(fs::file_size(f.root/"profiles"/isolated.at("id").get<std::string>()/"metadata.json")==isolated_before,"Default changed isolated metadata");
  Check(fs::is_empty(f.root/"sessions"),"Default published isolated readiness/session state");
  const auto duplicate=std::string(32,'e');fs::create_directory(f.root/"profiles"/duplicate);
  fs::copy_file(f.root/"profiles"/id/"metadata.json",f.root/"profiles"/duplicate/"metadata.json");
  const auto conflict=f.Call({{"apiVersion",2},{"operation","ensure-default"}});
  Check(!conflict.value("ok",false) && conflict.at("error").at("code")=="duplicate_default","duplicate Default identity accepted");
  fs::remove_all(f.root/"profiles"/duplicate);
  auto metadata=Json::parse(std::ifstream(f.root/"profiles"/id/"metadata.json"));
  metadata["ownerUserId"]="S-1-5-21-0-0-0-1";
  std::ofstream(f.root/"profiles"/id/"metadata.json",std::ios::trunc)<<metadata.dump();
  const auto mismatch=f.Call({{"apiVersion",2},{"operation","ensure-default"}});
  Check(!mismatch.value("ok",false) && mismatch.at("error").at("code")=="invalid_metadata","Default owner mismatch silently replaced identity");
}
#endif
void IdentityRevisionAndConflict()
{
  Fixture f;const auto first=f.Create("Science");const auto id=first.at("id").get<std::string>();
  Check(id.size()==32 && id.find_first_not_of("0123456789abcdef")==std::string::npos,"generated ID is not immutable hex form");
  const auto duplicate=Json::parse(ExecuteCatalogRequest(
      R"({"apiVersion":1,"operation":"list","operation":"create","name":"Science"})"));
  Check(!duplicate.value("ok",false),"duplicate JSON authority properties accepted");
  const auto truncated_root=f.RootText()+std::string(1,'\0')+"/different";
  const auto malformed_root=Json::parse(ExecuteCatalogRequest(Json{{"apiVersion",1},
      {"root",truncated_root},{"operation","create"},{"name","Truncated"}}.dump()));
  Check(!malformed_root.value("ok",false),"NUL path silently selected truncated catalog root");
  Check(f.Call({{"operation","list"}}).at("profiles").size()==1,"malformed root request mutated real prefix catalog");
  const auto malformed_game=f.Call({{"operation","edit"},{"id",id},{"gameDirectory",truncated_root},
      {"expectedRevision",first.at("revision")}});
  Check(!malformed_game.value("ok",false),"NUL preferred installation silently accepted truncated path");
  const auto renamed=f.Call({{"operation","rename"},{"id",id},{"name","Engineering"},{"expectedRevision",first.at("revision")}});
  Check(renamed.value("ok",false),renamed.dump());
  Check(renamed.at("profile").at("id")==id,"rename changed immutable identity");
  const auto stale=f.Call({{"operation","rename"},{"id",id},{"name","Stale"},{"expectedRevision",first.at("revision")}});
  Check(!stale.value("ok",false),"stale metadata update overwrote newer name");
  Check(f.Find(id).at("name")=="Engineering","failed stale update altered metadata");
  const auto conflict=f.root/"archives"/id;
  fs::create_directories(conflict);
  fs::copy_file(f.root/"profiles"/id/"metadata.json",conflict/"metadata.json");
  const auto catalog=f.Call({{"operation","list"}});
  Check(!catalog.value("ok",false) || (catalog.contains("issues") && !catalog.at("issues").empty()),"duplicate active/archive identity concealed");
  const auto archived=f.Call({{"operation","archive"},{"id",id},{"expectedRevision",renamed.at("profile").at("revision")}});
  Check(!archived.value("ok",false),"archive overwrote existing identity");
}
void IncompleteEntriesAreVisibleFailures()
{
  Fixture f;f.Create("Valid");
  const auto incomplete=f.root/"profiles"/std::string(32,'1');fs::create_directory(incomplete);
  auto response=f.Call({{"operation","list"}});
  Check(!response.value("ok",false) || (response.contains("issues") && !response.at("issues").empty()),"incomplete creation silently disappeared");
  std::ofstream(incomplete/"metadata.json")<<"{invalid";
  response=f.Call({{"operation","list"}});
  Check(!response.value("ok",false) || (response.contains("issues") && !response.at("issues").empty()),"corrupt metadata silently disappeared");
}
void LifecycleRequiresBothWriterAndBrowserInactivity()
{
  Fixture f;const auto profile=f.Create("Lifecycle");const auto id=profile.at("id").get<std::string>();
  {
    BrowserLease browser(f.root,id);
    BrowserLease shared_browser(f.root,id);
    const auto denied=f.Mutate("archive",id);
    Check(!denied.value("ok",false),"archive moved profile with live isolated browser");
  }
  {
    SessionLease lease(f.root,id);ProfilePrefsStore store(f.root,id,ProfileOpenMode::New,lease);
    store.SetInt(u"synthetic-account",22);lease.MarkReady();
    const auto denied=f.Mutate("archive",id);
    Check(!denied.value("ok",false),"archive moved profile with live preference writer");
  }
  auto archive=f.Mutate("archive",id);Check(archive.value("ok",false),archive.dump());
  Check(f.Find(id,true).at("id")==id,"archive replaced profile identity");
  Throws([&]{SessionLease archived(f.root,id);},"archived profile admitted game session");
  auto restore=f.Mutate("restore",id,true);Check(restore.value("ok",false),restore.dump());
  {
    SessionLease lease(f.root,id);ProfilePrefsStore store(f.root,id,ProfileOpenMode::Existing,lease);
    Check(store.GetInt(u"synthetic-account",0)==22,"archive/restore lost ID-bound account preferences");
  }
  archive=f.Mutate("archive",id);Check(archive.value("ok",false),archive.dump());
  const auto removed=f.Mutate("delete",id,true);Check(removed.value("ok",false),removed.dump());
  Check(!fs::exists(f.root/"archives"/id),"permanent deletion retained archived profile directory");
}
#if _WIN32
std::wstring Quote(std::wstring_view text)
{
  std::wstring result=L"\"";std::size_t slashes=0;
  for (auto ch:text) { if (ch==L'\\') { ++slashes;continue; } result.append(ch==L'\"'?slashes*2+1:slashes,L'\\');slashes=0;result+=ch; }
  result.append(slashes*2,L'\\');result+=L'\"';return result;
}
#endif
struct Child {
#if _WIN32
  HANDLE process=nullptr;
#else
  pid_t process=0;
#endif
  Child(const fs::path& root,const std::string& id) {
#if _WIN32
    wchar_t executable[32768]{};Check(GetModuleFileNameW(nullptr,executable,32768)>0,"test executable path unavailable");
    auto command=Quote(executable)+L" --hold-lease "+Quote(root.wstring())+L" "+Quote(std::wstring(id.begin(),id.end()));
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION created{};
    Check(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&created)!=0,"could not spawn synthetic lease holder");
    process=created.hProcess;CloseHandle(created.hThread);
#else
    std::vector<char> executable(32768);auto size=static_cast<std::uint32_t>(executable.size());
    Check(_NSGetExecutablePath(executable.data(),&size)==0,"test executable path unavailable");
    auto path=root.string(),id_copy=id;
    char* args[]{executable.data(),const_cast<char*>("--hold-lease"),path.data(),id_copy.data(),nullptr};
    Check(posix_spawn(&process,executable.data(),nullptr,nullptr,args,environ)==0,"could not spawn synthetic lease holder");
#endif
  }
  ~Child() {
#if _WIN32
    if (process) { if (WaitForSingleObject(process,0)==WAIT_TIMEOUT) TerminateProcess(process,190);WaitForSingleObject(process,5000);CloseHandle(process); }
#else
    if (process) { kill(process,SIGTERM);int status=0;waitpid(process,&status,0); }
#endif
  }
  void Finish() {
#if _WIN32
    Check(WaitForSingleObject(process,5000)==WAIT_OBJECT_0,"synthetic child did not finish");DWORD code=190;
    Check(GetExitCodeProcess(process,&code) && code==0,"synthetic child failed");CloseHandle(process);process=nullptr;
#else
    int status=0;Check(waitpid(process,&status,0)==process && WIFEXITED(status) && WEXITSTATUS(status)==0,"synthetic child failed");process=0;
#endif
  }
};
void CrossProcessExclusionAndIdentity()
{
  Fixture f;const auto id=f.Create("Cross Process").at("id").get<std::string>();
  Child child(f.root,id);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while (!fs::exists(f.root/"child-ready") && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  Check(fs::exists(f.root/"child-ready"),"child lease readiness not observed");
  Throws([&]{SessionLease duplicate(f.root,id);},"another process acquired duplicate profile writer");
  const auto sessions=f.Call({{"operation","sessions"}});
  Check(sessions.value("ok",false) && !sessions.at("sessions").empty(),"live child session identity not discovered");
  const auto receipt=sessions.at("sessions").front();
  Check(receipt.at("id")==id && receipt.at("processId").get<std::uint32_t>()>0
        && !receipt.at("started").get<std::string>().empty()
        && !receipt.at("executable").get<std::string>().empty()
        && receipt.at("readiness")=="ready","session omitted verified process identity or readiness");
  auto wrong_identity=receipt;wrong_identity["started"]="not-the-current-process-start";
  std::ofstream(f.root/"sessions"/(id+".json"),std::ios::trunc)<<wrong_identity.dump();
  const auto reused=f.Call({{"operation","sessions"}});
  Check(reused.value("ok",false) && reused.at("sessions").empty(),"same PID with different process start was accepted");
  auto wrapped_pid=receipt;wrapped_pid["processId"]=receipt.at("processId").get<std::uint64_t>()+(1ull<<32);
  wrapped_pid["phase"]="pending";
  std::ofstream(f.root/"sessions"/(id+".json"),std::ios::trunc)<<wrapped_pid.dump();
  const auto malformed_pid=f.Call({{"operation","sessions"}});
  Check(malformed_pid.value("ok",false) && malformed_pid.at("sessions").empty()
      && !malformed_pid.at("issues").empty(),"oversized session PID wrapped into live admission authority");
  auto wrong_phase=receipt;wrong_phase["phase"]="unrecognized";
  std::ofstream(f.root/"sessions"/(id+".json"),std::ios::trunc)<<wrong_phase.dump();
  const auto malformed_phase=f.Call({{"operation","sessions"}});
  Check(malformed_phase.value("ok",false) && malformed_phase.at("sessions").empty()
      && !malformed_phase.at("issues").empty(),"unknown session phase accepted as authority");
  std::ofstream(f.root/"sessions"/(id+".json"),std::ios::trunc)<<receipt.dump();
  const auto denied=f.Mutate("archive",id);Check(!denied.value("ok",false),"archive moved another process's live profile");
  std::ofstream(f.root/"child-release")<<"release";child.Finish();
  const auto stopped=f.Call({{"operation","sessions"}});
  Check(stopped.value("ok",false) && stopped.at("sessions").empty(),"stale PID accepted as live session ownership");
  SessionLease now_available(f.root,id);Check(now_available.Owns(),"stopped child left permanent writer exclusion");
}
int HoldLease(const fs::path& root,const std::string& id)
{
  SessionLease lease(root,id);ProfilePrefsStore store(root,id,ProfileOpenMode::New,lease);store.FinishNewProfile();lease.MarkReady();
  std::ofstream(root/"child-ready")<<"ready";
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  while (!fs::exists(root/"child-release") && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return fs::exists(root/"child-release")?0:190;
}
} // namespace
int main(int argc,char** argv)
{
  try {
#if _WIN32
    wchar_t test_executable[32768]{};
    Check(GetModuleFileNameW(nullptr,test_executable,32768)>0,"synthetic process path unavailable");
    if (fs::path(test_executable).filename()==L"prime.exe") {
      const auto directory=fs::path(test_executable).parent_path();
      const auto command=std::wstring(GetCommandLineW());
      const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,command.data(),static_cast<int>(command.size()),nullptr,0,nullptr,nullptr);
      Check(count>0,"synthetic command line conversion failed");std::string text(count,'\0');
      Check(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,command.data(),static_cast<int>(command.size()),text.data(),count,nullptr,nullptr)==count,"synthetic command line unavailable");
      std::ofstream(directory/("arguments-"+std::to_string(GetCurrentProcessId())+".txt"))<<text;
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);
      while (!fs::exists(directory/"ordinary-release") && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      return fs::exists(directory/"ordinary-release")?0:190;
    }
#endif
    if (argc==4 && std::string_view(argv[1])=="--hold-lease") return HoldLease(fs::u8path(argv[2]),argv[3]);
    CatalogLocationIsReadOnly();
#if _WIN32
    DefaultIsTypedMetadataOnly();NativeApiProjectsTypedDefault();InstallationRegistrationsBindPhysicalDirectories();OrdinaryLaunchDoesNotRequestIsolation();
#endif
    IdentityRevisionAndConflict();IncompleteEntriesAreVisibleFailures();LifecycleRequiresBothWriterAndBrowserInactivity();CrossProcessExclusionAndIdentity();
    std::cout<<"profile catalog tests passed\n";return 0;
  } catch (const std::exception& error) { std::cerr<<"profile catalog tests failed: "<<error.what()<<'\n';return 1; }
}
