// Compile the production updater in this test TU to exercise its private crash
// checkpoints without making synthetic URLs or interruption controls public API.
#include "../src/installation.cc"
#include <stfc_profiles/catalog.h>
#include <iostream>
using namespace stfc::profiles;
using namespace stfc::profiles::installation_detail;
#if _WIN32
namespace {
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
template<class Action> void ThrowsCode(Action action,const char* code){try{action();}catch(const CatalogError& error){Check(error.Code()==code,"Wrong failure code");return;}throw std::runtime_error("Expected typed failure");}
template<class Action> void Throws(Action action,const char* message){try{action();}catch(const std::exception&){return;}throw std::runtime_error(message);}
struct Fixture {
  fs::path root,game,catalog,transaction,ownership;
  Fixture(){
    wchar_t directory[MAX_PATH+1]{},file[MAX_PATH+1]{};
    Check(GetTempPathW(MAX_PATH+1,directory)>0,"No temporary root");Check(GetTempFileNameW(directory,L"spi",0,file)!=0,"No temporary fixture path");
    root=file;fs::remove(root);fs::create_directory(root);root=fs::canonical(root);game=root/"game";catalog=root/"catalog";fs::create_directories(game/"prime_Data");
    Durable(game/"prime.exe","old executable");Durable(game/"GameAssembly.dll","old assembly");Durable(game/"UnityPlayer.dll","old unity");
    Durable(game/"prime_Data"/"existing.txt","old asset");Durable(game/"version.dll","community mod extra");Durable(game/"config.toml","user config extra");Durable(game/".version","&game=221");
    transaction=Transaction(game,Key(game));ownership=Ownership(game,Key(game));
  }
  ~Fixture(){std::error_code error;fs::remove_all(root,error);}
  Json Call(const char* operation){return Json::parse(ExecuteInstallationRequest(Json{{"apiVersion",1},{"operation",operation},{"root",Utf8(catalog)},{"gameDirectory",Utf8(game)}}.dump()));}
  Json Stage(){
    fs::create_directory(transaction);fs::create_directories(transaction/"stage"/"prime_Data"/"new-dir");
    for(const auto& [path,bytes]:std::vector<std::pair<std::string,std::string>>{{"prime.exe","new executable"},{"GameAssembly.dll","new assembly"},{"UnityPlayer.dll","new unity"},{"prime_Data/existing.txt","new asset"},{"prime_Data/new-dir/new.txt","new file"}})
      Durable(transaction/"stage"/Relative(path),bytes);
    Json files=Json::array();for(const auto& item:fs::recursive_directory_iterator(transaction/"stage"))if(item.is_regular_file()){
      const auto relative=fs::relative(item.path(),transaction/"stage");files.push_back({{"path",RelativeText(relative)},{"size",fs::file_size(item.path())},{"sha256",Digest(item.path())},{"remove",false}});
    }
    Json journal{{"schemaVersion",1},{"key",Key(game)},{"gameDirectory",Utf8(game)},{"targetVersion",267},{"priorVersion","&game=221"},{"phase","staged"},{"files",files},{"directories",Json::array({"prime_Data/new-dir"})}};
    Inventory(game,ownership,journal);Journal(transaction,journal);return journal;
  }
  void Original(){
    Check(Read(game/"prime.exe",100)=="old executable","Executable did not roll back");Check(Read(game/"GameAssembly.dll",100)=="old assembly","Assembly did not roll back");
    Check(Read(game/"UnityPlayer.dll",100)=="old unity","Unity did not roll back");Check(Read(game/"prime_Data"/"existing.txt",100)=="old asset","Asset did not roll back");
    Check(!fs::exists(game/"prime_Data"/"new-dir"/"new.txt"),"New file survived rollback");Check(Read(game/".version",64)=="&game=221","Official version marker did not roll back");Extras();
  }
  void Extras(){Check(Read(game/"version.dll",100)=="community mod extra","Unowned mod was changed");Check(Read(game/"config.toml",100)=="user config extra","Unowned config was changed");}
};
std::string XmlImage(std::uint64_t size=457699340,std::uint64_t extracted=746394598){
  const std::string name="full_game_43d3c276d35e37eaed582eaf3a493ded_.7z.001";
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?> <actions version=\"0\">"
      "<action type=\"extracted_size\" data_size=\""+std::to_string(extracted)+"\"></action>"
      "<action type=\"torrent_download\" torrent_type=\"3\" torrent_link=\"$cdn_url/8301/full_games/full_game_/"+name+".torrent\" torrent_file=\"$temp_path/"+name+".torrent\" to=\"$temp_path/\" alt_data_link=\"https://launcher-game-update.s3.amazonaws.com/8301/full_games/full_game_/"+name+"\" alt_to=\"$temp_path/"+name+"\" data_size=\""+std::to_string(size)+"\"></action>"
      "<action type=\"wait_actions\"></action><action type=\"extract\" file=\"$temp_path/"+name+"\" format=\"7z\" to=\"$game_path/\"></action><action type=\"wait_actions\"></action><action type=\"version\" version=\"267\"></action></actions>";
}
std::string BString(std::string_view value){return std::to_string(value.size())+":"+std::string(value);}
std::string TorrentBytes(const Plan& plan,std::string_view payload){
  std::string hashes;for(std::size_t i=0;i<payload.size();i+=65536){Hash hash(true);hash.Add(payload.data()+i,std::min<std::size_t>(65536,payload.size()-i));hashes+=hash.Finish();}
  const auto name=plan.archive.substr(plan.archive.find_last_of('/')+1);
  return "d4:infod6:lengthi"+std::to_string(payload.size())+"e4:name"+BString(name)+"12:piece lengthi65536e6:pieces"+BString(hashes)+"ee";
}
void ParsersAndPieces(){
  const auto plan=ParsePlan(XmlImage());Check(plan.version==267&&plan.size==457699340&&plan.extracted==746394598,"Official full-image plan parsed incorrectly");
  Check(MarkerVersion("&game=221")==221&&UpdatedMarker("&game=221\r\n",267)=="&game=267\r\n","Official marker format was not preserved");
  Throws([]{MarkerVersion("221");},"Bare numeric version accepted");Throws([]{MarkerVersion("&game=221&extra=3");},"Unknown marker fields accepted");
  auto bad=XmlImage();bad.replace(bad.find("https://"),8,"http://");Throws([&]{ParsePlan(bad);},"HTTP payload accepted");
  Throws([]{ParsePlan("<!DOCTYPE actions><actions version=\"0\"></actions>");},"DTD accepted");
  bad=XmlImage();bad.replace(bad.find("format=\"7z\""),11,"format=\"zip\"");Throws([&]{ParsePlan(bad);},"Unknown extraction format accepted");
  for(const auto* path:{"../outside","/absolute","C:/absolute","sub/../../escape","sub\\escape","x:stream","NUL.txt","COM1","file.","file ","sub/./file"})Throws([&]{Relative(path);},"Unsafe archive path accepted");
  std::string payload(65536+13,'x');auto piecePlan=ParsePlan(XmlImage(payload.size(),payload.size()));const auto torrent=ParseTorrent(TorrentBytes(piecePlan,payload),piecePlan);
  PieceVerifier verifier(torrent);for(std::size_t i=0;i<payload.size();i+=113)verifier.Add(payload.data()+i,std::min<std::size_t>(113,payload.size()-i));verifier.Finish();
  auto corrupt=payload;corrupt[17]='y';Throws([&]{PieceVerifier v(torrent);v.Add(corrupt.data(),corrupt.size());v.Finish();},"Corrupted piece accepted");
  Throws([&]{PieceVerifier v(torrent);v.Add(payload.data(),payload.size()-1);v.Finish();},"Truncated archive accepted");
  Throws([&]{ParseTorrent(TorrentBytes(piecePlan,payload)+"e",piecePlan);},"Trailing torrent content accepted");
  auto wrong=piecePlan;++wrong.size;Throws([&]{ParseTorrent(TorrentBytes(piecePlan,payload),wrong);},"Manifest/torrent length mismatch accepted");
}
void ArchiveFile(const fs::path& destination,const std::vector<std::pair<std::string,std::string>>& files){
  auto* writer=archive_write_new();Check(writer!=nullptr,"Archive fixture allocation failed");
  Check(archive_write_set_format_7zip(writer)==ARCHIVE_OK,"7z writer unavailable");Check(archive_write_open_filename_w(writer,destination.c_str())==ARCHIVE_OK,"Archive fixture open failed");
  for(const auto& [name,bytes]:files){auto* entry=archive_entry_new();archive_entry_set_pathname_utf8(entry,name.c_str());archive_entry_set_filetype(entry,AE_IFREG);archive_entry_set_perm(entry,0600);archive_entry_set_size(entry,bytes.size());
    Check(archive_write_header(writer,entry)==ARCHIVE_OK,"Archive fixture header failed");Check(archive_write_data(writer,bytes.data(),bytes.size())==static_cast<la_ssize_t>(bytes.size()),"Archive fixture data failed");archive_entry_free(entry);}
  Check(archive_write_close(writer)==ARCHIVE_OK,"Archive fixture close failed");archive_write_free(writer);
}
void ExtractSafety(){
  Fixture f;fs::create_directory(f.root/"extracted");
  const std::vector<std::pair<std::string,std::string>> files{{"prime.exe","exe"},{"GameAssembly.dll","assembly"},{"UnityPlayer.dll","unity"},{"prime_Data/asset","data"}};
  ArchiveFile(f.root/"image.7z",files);auto plan=ParsePlan(XmlImage(fs::file_size(f.root/"image.7z"),20));
  const auto inventory=Extract(f.root/"image.7z",f.root/"extracted",plan);Check(inventory.size()==4,"Extractor inventory incorrect");
  fs::create_directory(f.root/"malicious-stage");ArchiveFile(f.root/"malicious.7z",{{"../escape","x"}});plan.extracted=1;
  Throws([&]{Extract(f.root/"malicious.7z",f.root/"malicious-stage",plan);},"Archive traversal accepted");Check(!fs::exists(f.root/"escape"),"Archive escaped staging root");
  fs::create_directory(f.root/"collision-stage");ArchiveFile(f.root/"collision.7z",{{"same.txt","x"},{"SAME.txt","y"}});plan.extracted=2;
  Throws([&]{Extract(f.root/"collision.7z",f.root/"collision-stage",plan);},"Case collision accepted");
}
void CrashRecovery(){
  std::size_t checkpoints=0;
  {Fixture f;auto journal=f.Stage();Commit(f.game,f.transaction,f.ownership,journal,[&](std::string_view name){if(name!="committed")++checkpoints;});Check(Read(f.game/".version",64)=="&game=267","Commit did not write official version marker");VerifyCommitted(f.game,journal);f.Extras();}
  Check(checkpoints>10,"Crash fixture did not cover live mutation boundaries");
  for(std::size_t stop=1;stop<=checkpoints;++stop){
    Fixture f;auto journal=f.Stage();std::size_t seen=0;
    Throws([&]{Commit(f.game,f.transaction,f.ownership,journal,[&](std::string_view name){if(name!="committed"&&++seen==stop)throw std::runtime_error("simulated interruption");});},"Interruption was not injected");
    auto persisted=ParseJson(Read(f.transaction/"journal.json",32u<<20));Identity(persisted,f.game,Key(f.game));
    Rollback(f.game,f.transaction,f.ownership,persisted);f.Original();
    // Replay recovery after interruption between a rename and journal update.
    Rollback(f.game,f.transaction,f.ownership,persisted);f.Original();
  }
  std::cout<<"Crash checkpoints recovered: "<<checkpoints<<"\n";
}
void GateAndConflicts(){
  {Fixture f;auto journal=f.Stage();ExecutableGate gate(f.game/"prime.exe");
    Handle denied(CreateFileW((f.game/"prime.exe").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(denied.value==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"Executable gate permits a direct launch read");
    Commit(f.game,f.transaction,f.ownership,journal,{},&gate);Check(Version(f.game)==267,"Gated commit failed");f.Extras();}
  {Fixture f;auto journal=f.Stage();std::size_t published=0;
    Throws([&]{Commit(f.game,f.transaction,f.ownership,journal,[&](std::string_view point){if(point=="publish"&&++published==1)throw std::runtime_error("interrupted");});},"No publication interruption");
    fs::path changed;for(const auto& item:journal["files"]){const auto relative=Relative(item.at("path").get<std::string>());if(Fold(relative.wstring())!="prime.exe"&&!item.at("remove").get<bool>()&&fs::exists(f.game/relative)&&Digest(f.game/relative)==item.at("sha256").get<std::string>()){changed=relative;break;}}
    Check(!changed.empty(),"No published file found");Durable(f.game/changed,"external change");
    Throws([&]{Rollback(f.game,f.transaction,f.ownership,journal);},"Recovery overwrote an external change");
    Check(fs::exists(f.transaction/"backup"/"prime.exe"),"Recovery lost executable backup");Check(Read(f.game/changed,100)=="external change","Recovery clobbered changed file");}
}
void ReviewRegressions(){
  {Fixture f;auto journal=f.Stage();bool edited=false;
    ThrowsCode([&]{Commit(f.game,f.transaction,f.ownership,journal,[&](std::string_view point){
      if(point=="publish"&&!edited){for(const auto& file:journal.at("files")){const auto relative=Relative(file.at("path").get<std::string>());
        if(Fold(relative.wstring())!="prime.exe"&&fs::exists(f.game/relative)){Durable(f.game/relative,"external published edit");edited=true;break;}}
      }
    });},"transaction_conflict");
    Check(edited&&Version(f.game)==221,"Changed image was advertised as updated");Check(fs::exists(f.transaction/"backup"/"prime.exe"),"Changed image lost backup evidence");}
  for(bool changeVersion:{false,true}){Fixture f;auto journal=f.Stage();
    Throws([&]{Commit(f.game,f.transaction,f.ownership,journal,[](std::string_view point){if(point=="verified")throw std::runtime_error("interrupted");});},"No finalization interruption");
    if(changeVersion)Durable(f.game/".version","&game=999");else Durable(f.ownership,"external owned-image metadata");
    ThrowsCode([&]{Rollback(f.game,f.transaction,f.ownership,journal);},"transaction_conflict");
    Check(Read(f.game/"GameAssembly.dll",100)=="new assembly","Metadata conflict partially rewrote live image");
    if(changeVersion)Check(Read(f.game/".version",64)=="&game=999","Recovery clobbered changed marker");
    else Check(Read(f.ownership,100)=="external owned-image metadata","Recovery clobbered changed ownership");
    Check(fs::exists(f.transaction/"backup"/"prime.exe"),"Metadata conflict lost recovery backup");
  }
  {Fixture f;auto journal=f.Stage();Commit(f.game,f.transaction,f.ownership,journal,[&](std::string_view point){
      if(point=="verified")Throws([&]{Durable(f.game/"GameAssembly.dll","late edit");},"Verified image permits writes before version commit");
    });Check(Version(f.game)==267,"Frozen image failed to commit");}
  {Fixture f;auto journal=f.Stage();Commit(f.game,f.transaction,f.ownership,journal);Durable(f.game/"GameAssembly.dll","later official update");Durable(f.game/".version","&game=300");
    const auto response=f.Call("recover-game-update");Check(response.value("ok",false),"Historical commit traps later official updates");
    Check(Version(f.game)==300&&Read(f.game/"GameAssembly.dll",100)=="later official update","Historical retirement changed current game");Check(!fs::exists(f.transaction),"Historical commit was not retired");}
  {Fixture f;auto journal=f.Stage();journal["directories"].push_back("unowned-empty");
    ThrowsCode([&]{Identity(journal,f.game,Key(f.game));},"invalid_transaction");}
  {Fixture f;for(const auto* name:{"version.dll","CONFIG.TOML","runtime.toml","logs/keep.txt","community_patch_settings.toml"}){
      const auto archive=f.root/"protected.7z",stage=f.root/"protected-stage";if(fs::exists(stage))RemoveTree(stage);fs::create_directory(stage);
      if(fs::exists(archive))fs::remove(archive);ArchiveFile(archive,{{name,"x"}});auto plan=ParsePlan(XmlImage(fs::file_size(archive),1));
      ThrowsCode([&]{Extract(archive,stage,plan);},"protected_host_path");
    }}
  for(const auto* name:{L"COM\u00b9",L"LPT\u00b2.txt",L"COM\u00b3"})ThrowsCode([&]{Relative(Utf8(fs::path(name)));},"unsafe_archive_path");
}
void PreJournalRecovery(){
  Fixture f;fs::create_directory(f.transaction);const auto status=f.Call("installation-status");
  Check(status.value("ok",false)&&status.at("installation").at("requiresRecovery")==true,"Empty pre-journal interruption was hidden");
  const auto result=f.Call("recover-game-update");Check(result.value("ok",false),"Empty orphan could not be recovered");f.Original();Check(!fs::exists(f.transaction),"Recovered empty orphan still blocks installation");
}
void RegisteredRecovery(){
  Fixture f;
  auto registered=Json::parse(ExecuteCatalogRequest(Json{{"apiVersion",2},{"operation","register-installation"},
      {"root",Utf8(f.catalog)},{"name","Interrupted game"},{"gameDirectory",Utf8(f.game)}}.dump()));
  Check(registered.value("ok",false),registered.dump().c_str());
  const auto id=registered.at("installation").at("id");
  auto journal=f.Stage();
  Throws([&]{Commit(f.game,f.transaction,f.ownership,journal,[](std::string_view point){
      if(point=="backup")throw std::runtime_error("interrupted registered image");});},"No interruption");
  Check(!fs::exists(f.game/"prime.exe"),"Fixture did not remove executable");
  auto call=[&](const char* operation){return Json::parse(ExecuteCatalogRequest(Json{{"apiVersion",2},
      {"operation",operation},{"root",Utf8(f.catalog)},{"installationId",id}}.dump()));};
  const auto status=call("installation-status");
  Check(status.value("ok",false)&&status.at("installation").at("requiresRecovery")==true,
        "Registered status hid incomplete-image recovery");
  Check(!call("check-game-update").value("ok",false),"Incomplete registered image reached update check");
  const auto recovered=call("recover-game-update");
  Check(recovered.value("ok",false),recovered.dump().c_str());f.Original();
  fs::rename(f.game,f.root/"original-game");fs::create_directory(f.game);
  for(const auto* operation:{"installation-status","recover-game-update"}){
    const auto replaced=call(operation);
    Check(!replaced.value("ok",false)&&replaced.at("error").at("code")=="installation_changed",
          "Recovery followed a replacement physical directory");
  }
}
void DirectoryCustody(){
  Fixture f;
  {
    InstallationLease lease(f.catalog,f.game,false);
    Check(lease.PhysicalIdentity().size()==64,"Lease omitted physical directory identity");
    Throws([&]{fs::rename(f.game,f.root/"replacement-game");},"Held game directory was renamed");
    Throws([&]{fs::rename(f.root,f.root.wstring()+L"-moved");},"Held installation ancestor was renamed");
    const auto rejected=Json::parse(ExecuteInstallationRequest(Json{{"apiVersion",1},{"operation","installation-status"},
        {"root",Utf8(f.catalog)},{"gameDirectory",Utf8(f.game)},{"installationPhysicalIdentity",std::string(64,'0')}}.dump()));
    Check(!rejected.value("ok",false)&&rejected.at("error").at("code")=="installation_changed",
          "Mismatched physical admission was accepted");
  }
  fs::rename(f.game,f.root/"replacement-game");fs::rename(f.root/"replacement-game",f.game);f.Original();
}
Json InterruptedImage(Fixture& fixture){
  auto journal=fixture.Stage();Throws([&]{Commit(fixture.game,fixture.transaction,fixture.ownership,journal,[](std::string_view point){if(point=="verified")throw std::runtime_error("interrupted image");});},"No completed-image interruption");return journal;
}
void RecoveryFinalization(){
  for(bool version:{false,true}){Fixture f;auto journal=InterruptedImage(f);bool edited=false;
    ThrowsCode([&]{Rollback(f.game,f.transaction,f.ownership,journal,nullptr,[&](std::string_view point){if(point=="restore-old"&&!edited){edited=true;if(version)Durable(f.game/".version","&game=999");else Durable(f.ownership,"recovery external ownership");}});},"transaction_conflict");
    Check(edited,"Recovery metadata edit was not injected");if(version)Check(Read(f.game/".version",64)=="&game=999","Recovery clobbered a mid-loop marker edit");
    else Check(Read(f.ownership,100)=="recovery external ownership","Recovery clobbered a mid-loop ownership edit");
    Check(!fs::exists(f.game/"prime.exe")&&fs::exists(f.transaction/"backup"/"prime.exe"),"Failed recovery exposed a mixed image executable");
  }
  {Fixture f;auto journal=InterruptedImage(f);fs::path first;std::size_t restored=0;
    ThrowsCode([&]{Rollback(f.game,f.transaction,f.ownership,journal,nullptr,[&](std::string_view point){if(point=="restore-old"){
      if(++restored==1){for(const auto& file:journal.at("files")){auto relative=Relative(file.at("path").get<std::string>());if(Fold(relative.wstring())!="prime.exe"&&file.at("hadOriginal").get<bool>()&&fs::exists(f.game/relative)){first=relative;break;}}}
      else if(restored==2)Durable(f.game/first,"changed after recovery restore");
    }});},"transaction_conflict");
    Check(Read(f.game/first,100)=="changed after recovery restore","Recovery overwrote external restored-file edit");
    Check(!fs::exists(f.game/"prime.exe")&&fs::exists(f.transaction/"backup"/"prime.exe"),"Mixed restored image became launchable");
  }
  {Fixture f;auto journal=InterruptedImage(f);Rollback(f.game,f.transaction,f.ownership,journal,nullptr,[&](std::string_view point){if(point=="restore-verified")Throws([&]{Durable(f.game/"GameAssembly.dll","late recovery edit");},"Restored image permits edits before old executable publication");});f.Original();}
  std::size_t points=0;
  {Fixture f;auto journal=InterruptedImage(f);Rollback(f.game,f.transaction,f.ownership,journal,nullptr,[&](std::string_view){++points;});f.Original();}
  for(std::size_t stop=1;stop<=points;++stop){Fixture f;auto journal=InterruptedImage(f);std::size_t seen=0;
    Throws([&]{Rollback(f.game,f.transaction,f.ownership,journal,nullptr,[&](std::string_view){if(++seen==stop)throw std::runtime_error("recovery interrupted");});},"Recovery interruption was not injected");
    auto persisted=ParseJson(Read(f.transaction/"journal.json",32u<<20));Identity(persisted,f.game,Key(f.game));Rollback(f.game,f.transaction,f.ownership,persisted);f.Original();
  }
  std::cout<<"Recovery checkpoints replayed: "<<points<<"\n";
}
void OwnershipAndApi(){
  {Fixture f;Durable(f.game/"obsolete.txt","retired official file");Durable(f.game/"modified-owned.txt","user changed old official file");
    const auto old=Json{{"schemaVersion",1},{"key",Key(f.game)},{"gameDirectory",Utf8(f.game)},{"files",Json::array({{{"path","obsolete.txt"},{"sha256",Digest(f.game/"obsolete.txt")},{"size",21}},{{"path","modified-owned.txt"},{"sha256",std::string(64,'0')},{"size",1}}})}};
    Durable(f.ownership,old.dump());auto journal=f.Stage();Commit(f.game,f.transaction,f.ownership,journal);
    Check(!fs::exists(f.game/"obsolete.txt"),"Unchanged obsolete owned file retained");Check(Read(f.game/"modified-owned.txt",100)=="user changed old official file","Modified old owned file was removed");f.Extras();
    auto status=f.Call("installation-status");Check(status.value("ok",false),status.dump().c_str());Check(status["installation"]["phase"]=="committed","Committed status not reported");
    const auto alias=f.game.parent_path()/"."/f.game.filename();
    auto aliasStatus=Json::parse(ExecuteInstallationRequest(Json{{"apiVersion",1},{"operation","installation-status"},{"root",Utf8(f.catalog)},{"gameDirectory",Utf8(alias)}}.dump()));
    Check(aliasStatus.value("ok",false)&&aliasStatus["installation"]==status["installation"],"Caller alias split the canonical installation transaction");
    auto recovery=f.Call("recover-game-update");Check(recovery.value("ok",false),recovery.dump().c_str());Check(!fs::exists(f.transaction),"Committed history still blocks future installation activity");}
  {Fixture f;auto journal=f.Stage();auto status=f.Call("installation-status");Check(status.value("ok",false),status.dump().c_str());Check(status["installation"]["requiresRecovery"]==true,"Stale staged transaction not blocking");
    auto recovery=f.Call("recover-game-update");Check(recovery.value("ok",false),recovery.dump().c_str());f.Original();Check(!fs::exists(f.transaction),"Recovered transaction still blocks installation");
    bool retained=false;for(const auto& item:fs::directory_iterator(f.game.parent_path()))if(item.path().filename().wstring().find(L"-history-")!=std::wstring::npos)retained=true;Check(retained,"Recovery discarded history");}
  {Fixture f;auto journal=f.Stage();journal["phase"]="downloading";journal["files"]=Json::array();journal["directories"]=Json::array();journal["downloadBytes"]=100;journal["downloadedBytes"]=25;Journal(f.transaction,journal);
    InstallationLease writer(f.catalog,f.game,true);const auto status=f.Call("installation-status");Check(status.value("ok",false),status.dump().c_str());
    Check(status["installation"]["state"]=="updating"&&status["installation"]["progressPercent"]==25,"Live progress snapshot is wrong");
    Throws([&]{InstallationLease conflict(f.root/"other-catalog",f.game,false);},"Different catalog roots split the installation lock");}
  {Fixture f;std::string invalid=Utf8(f.game);invalid+='\0';invalid+="outside";auto response=Json::parse(ExecuteInstallationRequest(Json{{"apiVersion",1},{"operation","installation-status"},{"root",Utf8(f.catalog)},{"gameDirectory",invalid}}.dump()));Check(!response.value("ok",false),"NUL-truncated installation accepted");}
}
}
#endif
int main(){try{
#if _WIN32
  ParsersAndPieces();ExtractSafety();CrashRecovery();GateAndConflicts();OwnershipAndApi();ReviewRegressions();PreJournalRecovery();RegisteredRecovery();DirectoryCustody();RecoveryFinalization();
  std::cout<<"Installation fixtures passed: official protocol, torrent integrity, safe extraction, crash recovery, preserved extras, executable gate and cross-root exclusion\n";
#else
  std::cout<<"Direct updater is Windows-only; runtime qualification required on this platform\n";
#endif
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}}
