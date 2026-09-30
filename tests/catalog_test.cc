#include "stfc_profiles/catalog.h"
#include "stfc_profiles/prefs_store.h"
#include "stfc_profiles/session.h"
#if __APPLE__
#include "../src/prefs_crypto.h"
#endif
#include <nlohmann/json.hpp>
#include <chrono>
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
    request["apiVersion"]=1;request["root"]=RootText();
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
    if (argc==4 && std::string_view(argv[1])=="--hold-lease") return HoldLease(fs::u8path(argv[2]),argv[3]);
    CatalogLocationIsReadOnly();IdentityRevisionAndConflict();IncompleteEntriesAreVisibleFailures();LifecycleRequiresBothWriterAndBrowserInactivity();CrossProcessExclusionAndIdentity();
    std::cout<<"profile catalog tests passed\n";return 0;
  } catch (const std::exception& error) { std::cerr<<"profile catalog tests failed: "<<error.what()<<'\n';return 1; }
}
