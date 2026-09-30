#include "stfc_profiles/catalog.h"
#include "stfc_profiles/session.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#if _WIN32
#include <Windows.h>
#include <shobjidl.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
using Json=nlohmann::json;
namespace {
std::filesystem::path Path(std::string_view value)
{ return std::filesystem::u8path(value.begin(),value.end()); }
std::string Utf8Path(const std::filesystem::path& path)
{ const auto value=path.u8string(); return {value.begin(),value.end()}; }

struct Arguments {
  std::string operation;
  std::map<std::string,std::string> values;
  std::vector<std::string> positional;
  bool json=false,archived=false,permanent=false;
};
Arguments Parse(const std::vector<std::string>& input)
{
  Arguments result;
  for (std::size_t i=1;i<input.size();++i) {
    const auto& argument=input[i];
    if (argument=="--json") { result.json=true; continue; }
    if (argument=="--archived") { result.archived=true; continue; }
    if (argument=="--permanent") { result.permanent=true; continue; }
    if (argument=="--profile" || argument=="--game" || argument=="--game-dir" || argument=="--root"
        || argument=="--output" || argument=="--expected-revision" || argument=="--expected-version"
        || argument=="--url" || argument=="--ready-fd") {
      if (i+1==input.size() || input[i+1].starts_with("--"))
        throw std::runtime_error(argument+" requires a separate value");
      if (!result.values.emplace(argument=="--game-dir"?"--game":argument,input[++i]).second)
        throw std::runtime_error(argument+" was specified more than once");
      continue;
    }
    if (argument.starts_with("--") && argument!="--help" && argument!="--internal-browser")
      throw std::runtime_error("unknown option: "+argument);
    if (result.operation.empty()) result.operation=argument;
    else result.positional.push_back(argument);
  }
  return result;
}
std::string Required(const Arguments& args,const char* key)
{
  const auto value=args.values.find(key);
  if (value==args.values.end() || value->second.empty()) throw std::runtime_error(std::string(key)+" is required");
  return value->second;
}
Json Call(Json request)
{
  request["apiVersion"]=1;
  return Json::parse(stfc::profiles::ExecuteCatalogRequest(request.dump()));
}
Json Failure(std::string_view code,std::string_view message)
{ return {{"apiVersion",1},{"ok",false},{"error",{{"code",code},{"message",message}}}}; }
void Help()
{
  std::cout<<"stfc-profiles list [--archived]\n"
           <<"stfc-profiles create NAME [--game PATH]\n"
           <<"stfc-profiles rename --profile ID NAME\n"
           <<"stfc-profiles edit --profile ID [--game PATH]\n"
           <<"stfc-profiles launch --profile ID [--game PATH]\n"
           <<"stfc-profiles sessions\n"
           <<"stfc-profiles archive --profile ID\n"
           <<"stfc-profiles restore --profile ID\n"
           <<"stfc-profiles delete --profile ID --archived --permanent\n"
           <<"stfc-profiles shortcut --profile ID --output PATH [--game PATH]\n"
           <<"stfc-profiles game status [--game PATH | --profile ID]\n"
           <<"stfc-profiles game check [--game PATH | --profile ID]\n"
           <<"stfc-profiles game update [--game PATH | --profile ID] [--expected-version VERSION]\n"
           <<"stfc-profiles game recover [--game PATH | --profile ID]\n"
           <<"Options: --json, --root PATH, --expected-revision REVISION\n";
}
#if _WIN32
std::wstring Wide(std::string_view value)
{
  const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
  if (count<=0) throw std::runtime_error("invalid UTF-8 argument");
  std::wstring result(count,L'\0');
  if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count))
    throw std::runtime_error("could not convert UTF-8 argument");
  return result;
}
std::wstring Quote(std::wstring_view value)
{
  std::wstring result=L"\"";
  std::size_t slashes=0;
  for (const auto ch:value) {
    if (ch==L'\\') { ++slashes; continue; }
    result.append(ch==L'\"'?slashes*2+1:slashes,L'\\'); slashes=0; result+=ch;
  }
  result.append(slashes*2,L'\\'); result+=L'\"'; return result;
}
Json Shortcut(const Arguments& args,const Json& profile)
{
  const auto output=std::filesystem::absolute(Path(Required(args,"--output"))).lexically_normal();
  if (output.extension()!=L".lnk") throw std::runtime_error("shortcut output must end with .lnk");
  if (std::filesystem::exists(output)) throw std::runtime_error("shortcut output already exists");
  const auto init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  if (FAILED(init)) throw std::runtime_error("could not initialize native shortcut creation");
  struct Uninit { ~Uninit(){ CoUninitialize(); } } uninit;
  IShellLinkW* link=nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,reinterpret_cast<void**>(&link))))
    throw std::runtime_error("could not create native shortcut");
  struct Release { IShellLinkW* link; ~Release(){ link->Release(); } } release{link};
  std::vector<wchar_t> executable_path(32768);
  const auto length=GetModuleFileNameW(nullptr,executable_path.data(),static_cast<DWORD>(executable_path.size()));
  if (!length || length==executable_path.size()) throw std::runtime_error("could not resolve the running profile coordinator");
  const auto target=std::filesystem::path(std::wstring(executable_path.data(),length));
  std::wstring arguments=L"launch --profile "+Quote(Wide(profile.at("id").get<std::string>()));
  if (args.values.contains("--game")) arguments+=L" --game "+Quote(Wide(args.values.at("--game")));
  if (args.values.contains("--root")) arguments+=L" --root "+Quote(Wide(args.values.at("--root")));
  if (FAILED(link->SetPath(target.c_str())) || FAILED(link->SetArguments(arguments.c_str()))
      || FAILED(link->SetWorkingDirectory(target.parent_path().c_str()))
      || FAILED(link->SetDescription(Wide("STFC profile: "+profile.at("name").get<std::string>()).c_str())))
    throw std::runtime_error("could not configure native shortcut");
  IPersistFile* file=nullptr;
  if (FAILED(link->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&file))))
    throw std::runtime_error("native shortcut persistence is unavailable");
  GUID guid{};wchar_t suffix[40]{};
  if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid,suffix,40)) {
    file->Release();throw std::runtime_error("could not allocate native shortcut staging path");
  }
  auto temporary=output;temporary+=L"."+std::wstring(suffix)+L".tmp.lnk";
  struct Stage { std::filesystem::path path;~Stage(){DeleteFileW(path.c_str());} } stage{temporary};
  const auto saved=file->Save(temporary.c_str(),TRUE);
  file->Release();
  if (FAILED(saved)) throw std::runtime_error("could not save native shortcut");
  HANDLE handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if (handle==INVALID_HANDLE_VALUE) throw std::runtime_error("could not flush native shortcut");
  const bool flushed=FlushFileBuffers(handle)!=0;
  const bool closed=CloseHandle(handle)!=0;
  if (!flushed || !closed || !MoveFileExW(temporary.c_str(),output.c_str(),MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("could not publish native shortcut without overwriting another file");
  return {{"apiVersion",1},{"ok",true},{"profile",profile},{"shortcut",Utf8Path(output)}};
}
#else
int InternalBrowser(const Arguments& args)
{
  const auto root=args.values.contains("--root") ? Path(args.values.at("--root")) : stfc::profiles::DefaultCatalogRoot();
  const auto id=Required(args,"--profile"),url=Required(args,"--url");
  if (!url.starts_with("https://") || url.size()>16384 || url.find_first_of("\"\r\n\t ")!=std::string::npos)
    throw std::runtime_error("invalid isolated browser URL");
  const auto descriptor=Required(args,"--ready-fd");
  std::size_t used=0;
  const int ready_fd=std::stoi(descriptor,&used);
  if (used!=descriptor.size() || ready_fd<3 || fcntl(ready_fd,F_GETFD)==-1)
    throw std::runtime_error("invalid browser readiness descriptor");
  fcntl(ready_fd,F_SETFD,FD_CLOEXEC);
  stfc::profiles::BrowserLease lease(root,id);
  auto data=lease.Directory()/"browser";
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(data)))
    throw std::runtime_error("isolated browser directory is redirected");
  std::filesystem::create_directory(data);
  std::filesystem::path browser;
  for (const auto& candidate:{std::filesystem::path("/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"),
                             std::filesystem::path("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")})
    if (std::filesystem::is_regular_file(candidate)) { browser=candidate; break; }
  if (browser.empty()) throw std::runtime_error("Edge or Chrome is required for isolated sign-in");
  std::vector<std::string> parts{browser.string(),"--user-data-dir="+data.string(),"--no-first-run",
                                "--disable-background-mode","--new-window",url};
  std::vector<char*> argv; for (auto& value:parts) argv.push_back(value.data()); argv.push_back(nullptr);
  posix_spawnattr_t attributes;
  if (posix_spawnattr_init(&attributes)!=0) throw std::runtime_error("could not initialize browser spawn");
  posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes,0);
  pid_t pid=0;
  const auto status=posix_spawn(&pid,browser.c_str(),nullptr,&attributes,argv.data(),environ);
  posix_spawnattr_destroy(&attributes);
  if (status!=0) throw std::runtime_error("could not start isolated browser");
  const char ready='R';
  if (write(ready_fd,&ready,1)!=1) { kill(-pid,SIGTERM); throw std::runtime_error("browser readiness handoff failed"); }
  close(ready_fd);
  int result=0;
  while (waitpid(pid,&result,0)<0) if (errno!=EINTR) throw std::runtime_error("could not observe isolated browser");
  while (kill(-pid,0)==0 || errno==EPERM) sleep(1);
  return WIFEXITED(result) ? WEXITSTATUS(result) : 190;
}
#endif

int Run(const std::vector<std::string>& input)
{
  const bool json_output=std::find(input.begin(),input.end(),"--json")!=input.end();
  try {
    auto args=Parse(input);
    if (args.operation.empty() || args.operation=="--help" || args.operation=="help") { Help(); return 0; }
#if ! _WIN32
    if (args.operation=="--internal-browser") return InternalBrowser(args);
#endif
    bool installation=false;
    if (args.operation=="game") {
      if (args.positional.size()!=1) throw std::runtime_error("game requires status, check, update, or recover");
      const std::map<std::string,std::string> operations{{"status","installation-status"},{"check","check-game-update"},
                                                       {"update","update-game"},{"recover","recover-game-update"}};
      const auto operation=operations.find(args.positional.front());
      if (operation==operations.end()) throw std::runtime_error("unknown game command");
      args.operation=operation->second;args.positional.clear();installation=true;
      if (!args.values.contains("--game")) {
        Json query{{"operation","paths"},{"id",Required(args,"--profile")}};
        if (args.values.contains("--root")) query["root"]=args.values.at("--root");
        const auto choice=Call(query);
        if (!choice.value("ok",false)) {
          if (json_output) std::cout<<choice.dump()<<'\n';else std::cerr<<choice.dump(2)<<'\n';
          return 1;
        }
        const auto game=choice.at("profile").at("gameDirectory").get<std::string>();
        if (game.empty()) throw std::runtime_error("profile has no preferred installation; provide --game PATH");
        args.values["--game"]=game;
      }
      Required(args,"--game");
    }
    if (!installation && args.operation!="list" && args.operation!="create" && args.operation!="rename" && args.operation!="edit"
        && args.operation!="archive" && args.operation!="restore" && args.operation!="delete"
        && args.operation!="launch" && args.operation!="sessions" && args.operation!="shortcut")
      throw std::runtime_error("unknown command: "+args.operation);
    if (args.values.contains("--profile") && (args.operation=="list" || args.operation=="create" || args.operation=="sessions"))
      throw std::runtime_error("--profile is not accepted for this command");
    if (args.values.contains("--output") && args.operation!="shortcut")
      throw std::runtime_error("--output applies only to shortcut");
    if (args.values.contains("--game") && !installation && args.operation!="create" && args.operation!="edit"
        && args.operation!="launch" && args.operation!="shortcut")
      throw std::runtime_error("--game is not accepted for this command");
    if (args.values.contains("--expected-revision") && args.operation!="rename" && args.operation!="edit"
        && args.operation!="archive" && args.operation!="restore" && args.operation!="delete")
      throw std::runtime_error("--expected-revision applies only to catalog mutation commands");
    if (args.values.contains("--expected-version") && args.operation!="update-game")
      throw std::runtime_error("--expected-version applies only to game update");
    if (args.values.contains("--url") || args.values.contains("--ready-fd"))
      throw std::runtime_error("browser guardian arguments require the internal browser command");
    if (args.operation=="edit" && !args.values.contains("--game"))
      throw std::runtime_error("edit requires --game PATH; use rename to change the display name");
    Json request{{"operation",args.operation},{"archived",args.archived}};
    if (args.values.contains("--root")) request["root"]=args.values.at("--root");
    if (args.values.contains("--game")) request["gameDirectory"]=args.values.at("--game");
    if (args.values.contains("--expected-version")) {
      const auto& version=args.values.at("--expected-version");
      if (version.empty() || version.size()>10 || version.find_first_not_of("0123456789")!=version.npos)
        throw std::runtime_error("--expected-version requires a positive game version number");
      const auto value=std::stoull(version);
      if (!value || value>2147483647) throw std::runtime_error("--expected-version is outside the supported range");
      request["expectedVersion"]=value;
    }
    if (args.operation=="create" || args.operation=="rename") {
      if (args.positional.size()!=1) throw std::runtime_error("provide one display name; quote names containing spaces");
      request["name"]=args.positional.front();
    } else if (!args.positional.empty()) throw std::runtime_error("unexpected positional argument");
    if (!installation && args.operation!="create" && args.operation!="list" && args.operation!="sessions")
      request["id"]=Required(args,"--profile");
    if (args.operation=="delete") {
      if (!args.permanent || !args.archived) throw std::runtime_error("permanent deletion requires --archived --permanent");
      request["permanent"]=true;
    } else if (args.permanent) throw std::runtime_error("--permanent applies only to delete");
    if (args.archived && args.operation!="list" && args.operation!="delete")
      throw std::runtime_error("--archived applies only to list and delete");
    Json selected;
    if (args.operation=="rename" || args.operation=="edit" || args.operation=="archive"
        || args.operation=="restore" || args.operation=="delete" || args.operation=="shortcut") {
      Json query{{"operation","list"},{"archived",args.operation=="restore" || args.archived}};
      if (request.contains("root")) query["root"]=request["root"];
      auto catalog=Call(query);
      if (!catalog.value("ok",false)) {
        if (json_output) std::cout<<catalog.dump()<<'\n'; else std::cerr<<catalog.dump(2)<<'\n';
        return 1;
      }
      for (const auto& profile:catalog.at("profiles"))
        if (profile.at("id")==request.at("id")) { selected=profile; break; }
      if (selected.is_null()) throw std::runtime_error("profile ID was not found in the requested catalog");
      request["expectedRevision"]=args.values.contains("--expected-revision")
          ? args.values.at("--expected-revision") : selected.at("revision").get<std::string>();
    }
    Json response;
    if (args.operation=="shortcut") {
#if _WIN32
      response=Shortcut(args,selected);
#else
      throw std::runtime_error("native macOS shortcut creation requires platform qualification");
#endif
    } else response=Call(request);
    if (json_output) std::cout<<response.dump()<<'\n';
    else if (!response.value("ok",false)) std::cerr<<response.dump(2)<<'\n';
    else if (response.contains("profiles")) {
      for (const auto& profile:response.at("profiles"))
        std::cout<<profile.at("id").get<std::string>()<<"  "<<profile.at("name").get<std::string>()<<'\n';
      if (response.contains("issues") && !response.at("issues").empty()) std::cerr<<response.at("issues").dump(2)<<'\n';
    } else std::cout<<response.dump(2)<<'\n';
    return response.value("ok",false)?0:1;
  } catch (const std::exception& error) {
    if (json_output) std::cout<<Failure("invalid_command",error.what()).dump()<<'\n';
    else std::cerr<<"STFC Profiles: "<<error.what()<<"\nUse stfc-profiles --help for commands.\n";
    return 2;
  }
}
} // namespace
#if _WIN32
int wmain(int argc,wchar_t** argv)
{
  std::vector<std::string> input;
  for (int i=0;i<argc;++i) {
    const auto length=static_cast<int>(wcslen(argv[i]));
    const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],length,nullptr,0,nullptr,nullptr);
    if (count<0 || (length && !count)) return 2;
    std::string value(count,'\0');
    if (count && !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],length,value.data(),count,nullptr,nullptr)) return 2;
    input.push_back(std::move(value));
  }
  return Run(input);
}
#else
int main(int argc,char** argv) { return Run({argv,argv+argc}); }
#endif
