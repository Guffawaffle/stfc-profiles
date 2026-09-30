#include "stfc_profiles/catalog.h"
#include "stfc_profiles/prefs_store.h"
#include "stfc_profiles/session.h"
#if __APPLE__
#include "../src/prefs_crypto.h"
#endif
#include <nlohmann/json.hpp>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#if _WIN32
#include <Windows.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif
using namespace stfc::profiles;
using Json=nlohmann::json;
namespace {
namespace fs=std::filesystem;
void Check(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Action> void Throws(Action action,const char* message)
{ try { action(); } catch (const std::exception&) { return; } throw std::runtime_error(message); }
std::string Bytes(const fs::path& file)
{
  std::ifstream input(file,std::ios::binary);
  Check(input.is_open(),"synthetic preferences are unavailable");
  return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
struct Fixture {
  fs::path root;
  std::vector<std::string> created_ids;
  Fixture() {
#if _WIN32
    wchar_t directory[MAX_PATH+1]{},file[MAX_PATH+1]{};
    auto length=GetTempPathW(MAX_PATH+1,directory);
    Check(length>0 && length<=MAX_PATH,"temporary directory unavailable");
    Check(GetTempFileNameW(directory,L"spp",0,file)!=0,"temporary path unavailable");
    root=file; fs::remove(root); fs::create_directory(root);
#else
    char directory[]="/tmp/stfc-profiles-prefs-XXXXXX";
    Check(mkdtemp(directory)!=nullptr,"temporary directory unavailable"); root=directory;
#endif
  }
  ~Fixture() {
#if __APPLE__
    for (const auto& id:created_ids) { try { detail::EraseProtectedPrefsIdentity(id); } catch (...) {} }
#endif
    std::error_code error; fs::remove_all(root,error);
  }
  Json Request(Json request) {
    request["apiVersion"]=1;
    const auto path=root.u8string(); request["root"]=std::string(path.begin(),path.end());
    if ((request.at("operation")=="archive" || request.at("operation")=="restore" || request.at("operation")=="delete")
        && !request.contains("expectedRevision")) {
      Json query{{"apiVersion",1},{"root",request.at("root")},{"operation","list"},
                 {"archived",request.at("operation")=="restore" || request.value("archived",false)}};
      const auto catalog=Json::parse(ExecuteCatalogRequest(query.dump()));
      for (const auto& profile:catalog.at("profiles"))
        if (profile.at("id")==request.at("id")) request["expectedRevision"]=profile.at("revision");
    }
    return Json::parse(ExecuteCatalogRequest(request.dump()));
  }
  std::string Create(const char* name) {
    auto response=Request({{"operation","create"},{"name",name}});
    Check(response.value("ok",false),response.dump().c_str());
    auto id=response.at("profile").at("id").get<std::string>();created_ids.push_back(id);return id;
  }
  fs::path File(std::string_view id) const { return root/"profiles"/std::string(id)/"player_prefs.bin"; }
};
void PersistenceAndEncryption()
{
  Fixture f; const auto id=f.Create("Synthetic Science");
  {
    SessionLease lease(f.root,id);
    ProfilePrefsStore store(f.root,id,ProfileOpenMode::New,lease);
    Check(!fs::exists(f.File(id)),"first use committed before hook readiness");
    store.FinishNewProfile();
    store.SetInt(u"int",42); store.SetFloat(u"float",-0.0f);
    store.SetString(u"token",u"SYNTHETIC_SECRET_12345");
    lease.MarkReady();
    Check(Bytes(f.File(id)).find("SYNTHETIC_SECRET_12345")==std::string::npos,"secret appeared as plaintext");
  }
  SessionLease lease(f.root,id);
  Check(lease.PreferencesInitialized(),"successful initialization was not bound to profile metadata");
  ProfilePrefsStore store(f.root,id,ProfileOpenMode::Existing,lease);
  Check(store.GetInt(u"int",-1)==42,"integer lost after reopen");
  Check(std::bit_cast<std::uint32_t>(store.GetFloat(u"float",1.0f))==std::bit_cast<std::uint32_t>(-0.0f),"float bits lost");
  Check(store.GetString(u"token")==u"SYNTHETIC_SECRET_12345","string lost after reopen");
  Check(store.GetInt(u"missing",19)==19,"Unity missing-key fallback changed");
  const auto bytes=Bytes(f.File(id)); store.SetInt(u"int",42); store.Save();
  Check(Bytes(f.File(id))==bytes,"unchanged value/save rewrote encrypted data");
}
void MissingEstablishedDataNeverReinitializes()
{
  Fixture f; const auto id=f.Create("Synthetic Missing");
  {
    SessionLease lease(f.root,id); ProfilePrefsStore store(f.root,id,ProfileOpenMode::New,lease);
    store.FinishNewProfile(); lease.MarkReady();
  }
  fs::remove(f.File(id)); auto marker=f.File(id);marker+=".initialized"; fs::remove(marker);
  SessionLease lease(f.root,id);
  Throws([&]{ ProfilePrefsStore empty(f.root,id,ProfileOpenMode::New,lease); },"metadata allowed replacement enrollment");
  Throws([&]{ ProfilePrefsStore empty(f.root,id,ProfileOpenMode::Resume,lease); },"metadata allowed empty resume");
  Throws([&]{ ProfilePrefsStore empty(f.root,id,ProfileOpenMode::Existing,lease); },"missing established data accepted");
  Check(!fs::exists(f.File(id)),"failure silently recreated account preferences");
}
void WrongIdentityAndLeaseRefused()
{
  Fixture f; const auto first=f.Create("Synthetic First"),second=f.Create("Synthetic Second");
  {
    SessionLease lease(f.root,first); ProfilePrefsStore store(f.root,first,ProfileOpenMode::New,lease);
    store.SetString(u"secret",u"synthetic one"); lease.MarkReady();
  }
  fs::copy_file(f.File(first),f.File(second));
  SessionLease lease(f.root,second);
  Throws([&]{ ProfilePrefsStore store(f.root,first,ProfileOpenMode::Existing,lease); },"mismatched lease accepted");
  Throws([&]{ ProfilePrefsStore store(f.root,second,ProfileOpenMode::Existing,lease); },"cross-ID ciphertext accepted");
  Check(fs::exists(f.File(second)),"failed cross-ID open destroyed evidence");
}
void StableLeaseExcludesWritersAndMoves()
{
  Fixture f;const auto first=f.Create("Synthetic A"),second=f.Create("Synthetic B");
  {
    SessionLease one(f.root,first),two(f.root,second);
    ProfilePrefsStore a(f.root,first,ProfileOpenMode::New,one),b(f.root,second,ProfileOpenMode::New,two);
    a.SetInt(u"account",1);b.SetInt(u"account",2);one.MarkReady();two.MarkReady();
    Throws([&]{ SessionLease duplicate(f.root,first); },"duplicate writer acquired live profile");
    const auto result=f.Request({{"operation","archive"},{"id",first}});
    Check(!result.value("ok",false),"active profile archived beneath writer");
  }
  auto archived=f.Request({{"operation","archive"},{"id",first}});
  Check(archived.value("ok",false),archived.dump().c_str());
  Check(fs::exists(f.root/"archives"/first/"player_prefs.bin"),"archive lost encrypted data");
  auto restored=f.Request({{"operation","restore"},{"id",first}});
  Check(restored.value("ok",false),restored.dump().c_str());
  SessionLease lease(f.root,first); ProfilePrefsStore a(f.root,first,ProfileOpenMode::Existing,lease);
  Check(a.GetInt(u"account",0)==1,"archive/restore changed immutable-ID preference identity");
}
void RecoveryValidatesBeforeReplacing()
{
  Fixture f;const auto id=f.Create("Synthetic Recovery");
  {
    SessionLease lease(f.root,id);ProfilePrefsStore store(f.root,id,ProfileOpenMode::New,lease);
    store.SetInt(u"durable",314);lease.MarkReady();
  }
  auto backup=f.File(id);backup+=".bak";fs::rename(f.File(id),backup);
  auto temporary=f.File(id);temporary+=".tmp.interrupted";std::ofstream(temporary)<<"partial";
  {
    SessionLease lease(f.root,id);ProfilePrefsStore store(f.root,id,ProfileOpenMode::Existing,lease);
    Check(store.GetInt(u"durable",0)==314,"committed recovery value lost");
    Check(!fs::exists(backup) && !fs::exists(temporary),"recovery debris remained");
  }
  fs::rename(f.File(id),backup); std::ofstream(backup,std::ios::trunc)<<"invalid ciphertext";
  SessionLease lease(f.root,id);
  Throws([&]{ProfilePrefsStore store(f.root,id,ProfileOpenMode::Existing,lease);},"invalid backup restored");
  Check(fs::exists(backup) && !fs::exists(f.File(id)),"failed backup validation altered evidence");
}
void RootAliasesPreserveLeaseIdentity()
{
  Fixture f;const auto id=f.Create("Synthetic Root Alias");
  auto spelling=f.root;
#if _WIN32
  auto native=spelling.native();
  for (auto& ch:native) if (ch>=L'a' && ch<=L'z') ch-=L'a'-L'A';
  spelling=fs::path(native);
#endif
  SessionLease lease(f.root,id);ProfilePrefsStore store(spelling,id,ProfileOpenMode::New,lease);
  store.SetInt(u"alias-bound",27);
  Check(fs::is_regular_file(lease.Directory()/"player_prefs.bin"),"alias store did not use held directory authority");
  Fixture other;
  Throws([&]{ProfilePrefsStore wrong(other.root,id,ProfileOpenMode::New,lease);},"different physical root admitted through an existing lease");
  auto truncated=spelling.native();truncated.push_back(fs::path::value_type{});truncated+=fs::path("different").native();
  Throws([&]{ProfilePrefsStore wrong(fs::path(truncated),id,ProfileOpenMode::New,lease);},"NUL root spelling admitted through a truncated alias");
}
void InterruptedFirstUsePreservesCommittedValues()
{
  Fixture f;const auto id=f.Create("Synthetic Interrupted");
  {
    SessionLease lease(f.root,id);ProfilePrefsStore store(f.root,id,ProfileOpenMode::New,lease);
    store.SetInt(u"in-progress",7);
  }
  SessionLease lease(f.root,id);ProfilePrefsStore store(f.root,id,ProfileOpenMode::Resume,lease);
  Check(store.GetInt(u"in-progress",0)==7,"interrupted first use reset committed values");
  store.FinishNewProfile();lease.MarkReady();
}
} // namespace
int main()
{
  try {
    PersistenceAndEncryption();MissingEstablishedDataNeverReinitializes();WrongIdentityAndLeaseRefused();
    StableLeaseExcludesWritersAndMoves();RecoveryValidatesBeforeReplacing();RootAliasesPreserveLeaseIdentity();InterruptedFirstUsePreservesCommittedValues();
    std::cout<<"profile preference store tests passed\n";return 0;
  } catch (const std::exception& error) { std::cerr<<"profile preference tests failed: "<<error.what()<<'\n';return 1; }
}
