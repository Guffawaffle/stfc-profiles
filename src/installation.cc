#include "stfc_profiles/installation.h"
#include "stfc_profiles/catalog.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <climits>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstring>
#include <cstdio>
#include <map>
#include <set>
#include <vector>
#if _WIN32
#include <Windows.h>
#include <TlHelp32.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <archive.h>
#include <archive_entry.h>
#endif

namespace stfc::profiles {
namespace installation_detail {
namespace fs = std::filesystem;
using Json = nlohmann::json;
[[noreturn]] void Fail(std::string code, std::string message)
{ throw CatalogError(std::move(code), std::move(message)); }
std::string Utf8(const fs::path& path)
{ const auto bytes=path.u8string(); return {bytes.begin(),bytes.end()}; }
std::string RelativeText(const fs::path& path)
{const auto bytes=path.generic_u8string();return {bytes.begin(),bytes.end()};}
fs::path Path(std::string_view text)
{ if(text.find('\0')!=text.npos)Fail("invalid_path","Paths must not contain an embedded NUL.");return fs::u8path(text.begin(),text.end()); }
#if _WIN32
constexpr std::uint64_t ArchiveLimit = 8ull << 30;
constexpr std::uint64_t ExtractedLimit = 16ull << 30;
constexpr std::size_t EntryLimit = 100000;
struct Handle {
  HANDLE value=INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
  ~Handle(){if(value!=INVALID_HANDLE_VALUE && value!=nullptr)CloseHandle(value);}
  Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
};
void Plain(const fs::path& path,bool directory)
{
  const auto attributes=GetFileAttributesW(path.c_str());
  if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)
      || bool(attributes&FILE_ATTRIBUTE_DIRECTORY)!=directory)
    Fail("unsafe_path","Expected an ordinary "+std::string(directory?"directory: ":"file: ")+Utf8(path));
  if(!directory) {
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION information{};
    if(file.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value,&information)
       || information.nNumberOfLinks!=1) Fail("unsafe_path","Linked files cannot be updated: "+Utf8(path));
  }
}
void TreePlain(const fs::path& root)
{
  Plain(root,true);
  for(const auto& entry:fs::recursive_directory_iterator(root)) {
    const auto status=fs::symlink_status(entry.path());
    Plain(entry.path(),fs::is_directory(status));
  }
}
void RemoveTree(const fs::path& root)
{ if(fs::exists(root)){TreePlain(root);fs::remove_all(root);} }
std::string Read(const fs::path& path,std::uint64_t limit)
{
  Plain(path,false); const auto length=fs::file_size(path);
  if(length>limit) Fail("size_limit","File exceeds its supported size: "+Utf8(path));
  std::ifstream input(path,std::ios::binary); std::string result(static_cast<std::size_t>(length),'\0');
  if(!input || (length&&!input.read(result.data(),static_cast<std::streamsize>(length))))
    Fail("read_failed","Could not read: "+Utf8(path));
  return result;
}
void Durable(const fs::path& path,std::string_view bytes,bool exclusive=false)
{
  Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,exclusive?CREATE_NEW:CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(file.value==INVALID_HANDLE_VALUE) Fail("write_failed","Could not create: "+Utf8(path));
  DWORD count=0;
  if(bytes.size()>MAXDWORD || !WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)
     || count!=bytes.size() || !FlushFileBuffers(file.value))
    Fail("write_failed","Could not durably write: "+Utf8(path));
}
void Move(const fs::path& from,const fs::path& to)
{
  if(!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH))
    Fail("move_failed","Could not move "+Utf8(from)+" to "+Utf8(to)+"; recovery data was retained.");
}
void Atomic(const fs::path& path,std::string_view bytes)
{
  const auto temporary=path.parent_path()/(path.filename().wstring()+L".writing");
  if(fs::exists(temporary)) {Plain(temporary,false);fs::remove(temporary);}
  Durable(temporary,bytes,true);
  if(fs::exists(path))Plain(path,false);
  if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    Fail("write_failed","Could not commit metadata: "+Utf8(path));
}
Json ParseJson(std::string_view text)
{
  std::vector<std::set<std::string>> keys;
  return Json::parse(text,[&](int,Json::parse_event_t event,Json& value){
    if(event==Json::parse_event_t::object_start)keys.emplace_back();
    else if(event==Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
      Fail("invalid_json","Duplicate JSON keys are not supported.");
    else if(event==Json::parse_event_t::object_end)keys.pop_back();
    return true;
  });
}
std::uint64_t Decimal(std::string_view text,std::uint64_t maximum)
{
  if(text.empty() || text.size()>20)Fail("invalid_metadata","Expected an unsigned decimal integer.");
  std::uint64_t value=0;
  for(char c:text) {
    if(c<'0'||c>'9'||value>(maximum-static_cast<unsigned>(c-'0'))/10)
      Fail("invalid_metadata","Decimal integer exceeds its supported range.");
    value=value*10+static_cast<unsigned>(c-'0');
  }
  return value;
}
std::uint64_t MarkerVersion(std::string_view marker)
{
  while(!marker.empty()&&(marker.back()=='\n'||marker.back()=='\r'))marker.remove_suffix(1);
  if(!marker.starts_with("&game="))Fail("invalid_version_marker","Expected the official &game=<version> marker.");
  return Decimal(marker.substr(6),INT_MAX);
}
std::uint64_t Version(const fs::path& game)
{return MarkerVersion(Read(game/".version",64));}
std::string UpdatedMarker(std::string_view prior,std::uint64_t version)
{
  (void)MarkerVersion(prior);const auto end=prior.find_last_not_of("\r\n");
  return "&game="+std::to_string(version)+std::string(prior.substr(end+1));
}
std::string Fold(std::wstring_view text)
{
  const auto count=LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,text.data(),static_cast<int>(text.size()),
                               nullptr,0,nullptr,nullptr,0);
  if(count<=0)Fail("invalid_path","Could not normalize a path.");
  std::wstring normalized(count,L'\0');
  if(!LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,text.data(),static_cast<int>(text.size()),
                   normalized.data(),count,nullptr,nullptr,0))Fail("invalid_path","Could not normalize a path.");
  return Utf8(fs::path(normalized));
}
fs::path Relative(std::string_view name)
{
  if(name.empty()||name.size()>1024||name.front()=='/'||name.find('\\')!=name.npos||name.find('\0')!=name.npos)
    Fail("unsafe_archive_path","Archive paths must be bounded relative forward-slash paths.");
  auto result=Path(name);
  if(result.is_absolute()||result.has_root_name()||result.has_root_directory())Fail("unsafe_archive_path","Rooted archive path.");
  for(const auto& part:result) {
    const auto text=part.wstring();
    if(text.empty()||text==L"."||text==L".."||text.back()==L'.'||text.back()==L' ')
      Fail("unsafe_archive_path","Unsafe archive path component.");
    for(wchar_t c:text)if(c<32||std::wstring_view(L"<>:\"|?*").find(c)!=std::wstring_view::npos)
      Fail("unsafe_archive_path","Archive path contains a Windows special character.");
    const auto dot=text.find(L'.');const auto base=Fold(std::wstring_view(text).substr(0,dot));
    if(base=="con"||base=="prn"||base=="aux"||base=="nul"||base=="conin$"||base=="conout$"||base=="clock$"
       || (base.size()==4&&(base.starts_with("com")||base.starts_with("lpt"))&&base[3]>='1'&&base[3]<='9'))
      Fail("unsafe_archive_path","Archive path uses a Windows reserved device name.");
    if((base.starts_with("com")||base.starts_with("lpt")) && (base.substr(3)==Utf8(fs::path(L"\u00b9"))||base.substr(3)==Utf8(fs::path(L"\u00b2"))||base.substr(3)==Utf8(fs::path(L"\u00b3"))))
      Fail("unsafe_archive_path","Archive path uses a Windows reserved device name.");
  }
  return result;
}
void Protected(const fs::path& relative)
{
  const auto text=Fold(relative.wstring());const auto first=text.substr(0,text.find_first_of("/\\"));
  const std::set<std::string> reserved{"version.dll","stfc-profiles.exe","config.toml","runtime.toml","logs",
    "community_patch_settings.toml","community_patch_runtime.vars","community_path_runtime.vars",
    "community_patch.log","patch_battlelogs_sent.json"};
  if(reserved.contains(first)||first.starts_with(".stfc-profiles-"))
    Fail("protected_host_path","The official image collides with reserved Profiles/mod runtime or configuration storage.");
}
void Parents(const fs::path& root,const fs::path& relative)
{
  Plain(root,true);auto current=root;
  for(const auto& component:relative.parent_path()) {current/=component;if(fs::exists(current))Plain(current,true);}
}
class Hash {
  BCRYPT_ALG_HANDLE algorithm_=nullptr;BCRYPT_HASH_HANDLE hash_=nullptr;
  std::vector<unsigned char> object_;std::vector<unsigned char> output_;
public:
  explicit Hash(bool sha1=false) {
    if(BCryptOpenAlgorithmProvider(&algorithm_,sha1?BCRYPT_SHA1_ALGORITHM:BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)
      Fail("hash_unavailable","Operating-system hashing is unavailable.");
    DWORD size=0,count=0,digest=0;
    if(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&count,0)<0
       || BCryptGetProperty(algorithm_,BCRYPT_HASH_LENGTH,reinterpret_cast<PUCHAR>(&digest),sizeof(digest),&count,0)<0)
      Fail("hash_failed","Could not initialize hashing.");
    object_.resize(size);output_.resize(digest);
    if(BCryptCreateHash(algorithm_,&hash_,object_.data(),size,nullptr,0,0)<0)Fail("hash_failed","Could not initialize hashing.");
  }
  ~Hash(){if(hash_)BCryptDestroyHash(hash_);if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);}
  void Add(const void* data,std::size_t count) {
    if(count>MAXDWORD||BCryptHashData(hash_,reinterpret_cast<PUCHAR>(const_cast<void*>(data)),static_cast<ULONG>(count),0)<0)
      Fail("hash_failed","Could not hash payload bytes.");
  }
  std::string Finish(){
    if(BCryptFinishHash(hash_,output_.data(),static_cast<ULONG>(output_.size()),0)<0)Fail("hash_failed","Could not finish hashing.");
    return {reinterpret_cast<const char*>(output_.data()),output_.size()};
  }
};
std::string Hex(std::string_view bytes)
{constexpr char digits[]="0123456789abcdef";std::string result;for(unsigned char c:bytes){result+=digits[c>>4];result+=digits[c&15];}return result;}
std::string Digest(const fs::path& file)
{
  Plain(file,false);std::ifstream input(file,std::ios::binary);Hash hash;std::array<char,65536> bytes{};
  if(!input)Fail("read_failed","Could not open file for integrity verification.");
  while(input){input.read(bytes.data(),bytes.size());hash.Add(bytes.data(),static_cast<std::size_t>(input.gcount()));}
  if(!input.eof())Fail("read_failed","Could not read file for integrity verification.");
  return Hex(hash.Finish());
}
struct Action {std::string type;std::map<std::string,std::string> attributes;};
class Xml {
  std::string_view text_;std::size_t position_=0;
  void Space(){while(position_<text_.size()&&std::string_view(" \r\n\t").find(text_[position_])!=std::string_view::npos)++position_;}
  void Take(std::string_view token){if(text_.substr(position_,token.size())!=token)Fail("invalid_manifest","Unsupported update XML syntax.");position_+=token.size();}
  std::string Name(){const auto start=position_;while(position_<text_.size()&&(std::isalnum(static_cast<unsigned char>(text_[position_]))||text_[position_]=='_'))++position_;if(start==position_)Fail("invalid_manifest","Expected an XML name.");return std::string(text_.substr(start,position_-start));}
  std::string Value(){
    if(position_==text_.size()||(text_[position_]!='\"'&&text_[position_]!='\''))Fail("invalid_manifest","Expected a quoted XML attribute.");
    const char quote=text_[position_++];std::string result;
    while(position_<text_.size()&&text_[position_]!=quote){
      char c=text_[position_++];if(c=='<'||static_cast<unsigned char>(c)<32)Fail("invalid_manifest","Invalid XML attribute.");
      if(c!='&'){result+=c;continue;}
      const auto end=text_.find(';',position_);if(end==text_.npos||end-position_>5)Fail("invalid_manifest","Unsupported XML entity.");
      const auto entity=text_.substr(position_,end-position_);position_=end+1;
      if(entity=="amp")result+='&';else if(entity=="quot")result+='\"';else if(entity=="apos")result+='\'';
      else if(entity=="lt")result+='<';else if(entity=="gt")result+='>';else Fail("invalid_manifest","Unsupported XML entity.");
    }
    if(position_==text_.size())Fail("invalid_manifest","Unterminated XML attribute.");++position_;return result;
  }
  std::map<std::string,std::string> Attributes(){
    std::map<std::string,std::string> result;Space();
    while(position_<text_.size()&&text_[position_]!='>'&&text_[position_]!='/'){
      auto name=Name();Space();Take("=");Space();auto value=Value();
      if(!result.emplace(name,value).second)Fail("invalid_manifest","Duplicate update XML attribute.");Space();
    }return result;
  }
public:
  explicit Xml(std::string_view text):text_(text){}
  std::vector<Action> Parse(){
    Space();if(text_.substr(position_,5)=="<?xml"){const auto end=text_.find("?>",position_);if(end==text_.npos||end-position_>100)Fail("invalid_manifest","Invalid XML declaration.");position_=end+2;Space();}
    Take("<actions");auto root=Attributes();if(root.size()!=1||root["version"]!="0")Fail("invalid_manifest","Unsupported update manifest version.");Take(">");Space();
    std::vector<Action> result;
    while(text_.substr(position_,10)!="</actions>"){
      if(result.size()>=32)Fail("size_limit","Too many update actions.");Take("<action");auto attributes=Attributes();if(text_.substr(position_,2)=="/>")Take("/>");else{Take(">");Space();Take("</action>");}
      const auto it=attributes.find("type");if(it==attributes.end())Fail("invalid_manifest","An update action has no type.");
      const auto type=it->second;attributes.erase(it);result.push_back({type,std::move(attributes)});Space();
    }
    Take("</actions>");Space();if(position_!=text_.size())Fail("invalid_manifest","Trailing update XML content.");return result;
  }
};
struct Plan {std::uint64_t version=0,size=0,extracted=0;std::string archive,torrent,manifest;};
constexpr std::string_view Cdn="https://launcher-game-update.s3.amazonaws.com";
Plan ParsePlan(std::string_view xml)
{
  const auto actions=Xml(xml).Parse();
  if(actions.size()!=6 || actions[0].type!="extracted_size"||actions[1].type!="torrent_download"
     ||actions[2].type!="wait_actions"||actions[3].type!="extract"||actions[4].type!="wait_actions"||actions[5].type!="version")
    Fail("unsupported_update_plan","The official launcher returned an unsupported full-image update plan.");
  if(actions[0].attributes.size()!=1||!actions[2].attributes.empty()||!actions[4].attributes.empty()
     ||actions[5].attributes.size()!=1)Fail("unsupported_update_plan","Unexpected update action attributes.");
  Plan plan;plan.manifest=xml;plan.extracted=Decimal(actions[0].attributes.at("data_size"),ExtractedLimit);
  plan.version=Decimal(actions[5].attributes.at("version"),INT_MAX);
  const auto& download=actions[1].attributes;const auto& extract=actions[3].attributes;
  if(download.size()!=7||download.at("torrent_type")!="3"||download.at("to")!="$temp_path/"
     ||extract.size()!=3||extract.at("format")!="7z"||extract.at("to")!="$game_path/"
     ||extract.at("file")!=download.at("alt_to"))Fail("unsupported_update_plan","Unsupported download or extraction action.");
  plan.size=Decimal(download.at("data_size"),ArchiveLimit);plan.archive=download.at("alt_data_link");
  if(!plan.version||!plan.size||!plan.extracted||!plan.archive.starts_with(std::string(Cdn)+"/8301/full_games/full_game_/"))
    Fail("unsupported_update_plan","The update does not name an approved full Windows image.");
  const auto name=plan.archive.substr(plan.archive.find_last_of('/')+1);
  if(!name.starts_with("full_game_")||!name.ends_with("_.7z.001")||name.size()!=50
     ||name.substr(10,32).find_first_not_of("0123456789abcdef")!=std::string::npos)
    Fail("unsupported_update_plan","Unexpected archive filename.");
  if(download.at("alt_to")!="$temp_path/"+name||download.at("torrent_file")!="$temp_path/"+name+".torrent"
     ||download.at("torrent_link")!="$cdn_url"+plan.archive.substr(Cdn.size())+".torrent")
    Fail("unsupported_update_plan","The update archive and torrent paths disagree.");
  plan.torrent=plan.archive+".torrent";return plan;
}
class Bencode {
  std::string_view bytes_;std::size_t position_=0;
  Json Parse(unsigned depth){
    if(depth>12||position_>=bytes_.size())Fail("invalid_torrent","Invalid torrent nesting.");
    const char kind=bytes_[position_];
    if(kind=='d'){
      ++position_;Json result=Json::object();std::string previous;bool first=true;
      while(position_<bytes_.size()&&bytes_[position_]!='e'){
        auto key=String();if(!first&&key<=previous)Fail("invalid_torrent","Torrent dictionary keys are not unique and sorted.");
        first=false;previous=key;result[key]=Parse(depth+1);
      }
      if(position_==bytes_.size())Fail("invalid_torrent","Unterminated torrent dictionary.");++position_;return result;
    }
    if(kind=='l'){
      ++position_;Json result=Json::array();while(position_<bytes_.size()&&bytes_[position_]!='e'){
        if(result.size()>1024)Fail("size_limit","Torrent list is too large.");result.push_back(Parse(depth+1));
      }if(position_==bytes_.size())Fail("invalid_torrent","Unterminated torrent list.");++position_;return result;
    }
    if(kind=='i'){
      ++position_;const auto end=bytes_.find('e',position_);if(end==bytes_.npos)Fail("invalid_torrent","Unterminated torrent integer.");
      const auto text=bytes_.substr(position_,end-position_);if(text.size()>1&&text.front()=='0')Fail("invalid_torrent","Noncanonical torrent integer.");
      const auto value=Decimal(text,ArchiveLimit);position_=end+1;return value;
    }
    return String();
  }
  std::string String(){
    const auto colon=bytes_.find(':',position_);if(colon==bytes_.npos||colon-position_>8)Fail("invalid_torrent","Invalid torrent string.");
    const auto text=bytes_.substr(position_,colon-position_);if(text.size()>1&&text.front()=='0')Fail("invalid_torrent","Noncanonical torrent string.");
    const auto length=Decimal(text,16u<<20);position_=colon+1;
    if(length>bytes_.size()-position_)Fail("invalid_torrent","Truncated torrent string.");
    std::string result(bytes_.substr(position_,static_cast<std::size_t>(length)));position_+=static_cast<std::size_t>(length);return result;
  }
public:
  explicit Bencode(std::string_view bytes):bytes_(bytes){}
  Json All(){auto result=Parse(0);if(position_!=bytes_.size())Fail("invalid_torrent","Trailing torrent content.");return result;}
};
struct Torrent {std::uint64_t size=0,pieceSize=0;std::string pieces;};
Torrent ParseTorrent(std::string_view bytes,const Plan& plan)
{
  const auto data=Bencode(bytes).All();const auto& info=data.at("info");
  if(!info.is_object()||info.contains("files")||info.value("name",std::string{})!=plan.archive.substr(plan.archive.find_last_of('/')+1))
    Fail("unsupported_torrent","Expected one full-image archive in the torrent.");
  Torrent result{info.at("length").get<std::uint64_t>(),info.at("piece length").get<std::uint64_t>(),info.at("pieces").get<std::string>()};
  if(result.size!=plan.size||result.pieceSize<65536||result.pieceSize>(16u<<20)
     ||(result.pieceSize&(result.pieceSize-1))||result.pieces.size()!=20*((result.size+result.pieceSize-1)/result.pieceSize))
    Fail("invalid_torrent","Torrent size or piece inventory does not match the official manifest.");
  return result;
}
struct Internet {HINTERNET value=nullptr;explicit Internet(HINTERNET h):value(h){}~Internet(){if(value)WinHttpCloseHandle(value);}Internet(const Internet&)=delete;};
void Http(std::string_view url,std::uint64_t limit,const std::function<void(const char*,std::size_t)>& consume,
          std::uint64_t exact=0)
{
  const auto wide=Path(url).wstring();URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);
  parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=parts.dwUserNameLength=parts.dwPasswordLength=static_cast<DWORD>(-1);
  if(!WinHttpCrackUrl(wide.c_str(),static_cast<DWORD>(wide.size()),0,&parts)||parts.nScheme!=INTERNET_SCHEME_HTTPS
     ||parts.nPort!=443||parts.dwUserNameLength||parts.dwPasswordLength)Fail("unsafe_url","Only credential-free approved HTTPS URLs are supported.");
  const std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
  if(host!=L"gus.xsolla.com"&&host!=L"launcher-game-update.s3.amazonaws.com")Fail("unsafe_url","The update URL names an unapproved host.");
  Internet session(WinHttpOpen(L"STFC Profiles/1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
  if(!session.value||!WinHttpSetTimeouts(session.value,15000,15000,30000,30000))Fail("network_failed","Could not initialize HTTPS download.");
  Internet connection(WinHttpConnect(session.value,host.c_str(),443,0));
  const std::wstring target=std::wstring(parts.lpszUrlPath,parts.dwUrlPathLength)+std::wstring(parts.lpszExtraInfo?parts.lpszExtraInfo:L"",parts.dwExtraInfoLength);
  Internet request(WinHttpOpenRequest(connection.value,L"GET",target.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
  DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if(!connection.value||!request.value||!WinHttpSetOption(request.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects))
     ||!WinHttpSendRequest(request.value,L"Accept-Encoding: identity\r\n",static_cast<DWORD>(-1),nullptr,0,0,0)
     ||!WinHttpReceiveResponse(request.value,nullptr))Fail("network_failed","Official update HTTPS request failed.");
  DWORD status=0,length=sizeof(status);
  if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&length,nullptr)||status!=200)
    Fail("http_failed","Official update server did not return HTTP 200; redirects are not followed.");
  wchar_t encoding[64]{};length=sizeof(encoding);
  if(WinHttpQueryHeaders(request.value,WINHTTP_QUERY_CONTENT_ENCODING,nullptr,encoding,&length,nullptr)&&std::wstring_view(encoding)!=L"identity")
    Fail("unsupported_encoding","Compressed HTTP transfer encoding is unsupported.");
  wchar_t declared[64]{};length=sizeof(declared);
  if(WinHttpQueryHeaders(request.value,WINHTTP_QUERY_CONTENT_LENGTH,nullptr,declared,&length,nullptr)){
    const auto count=Decimal(Utf8(fs::path(declared)),limit);if(exact&&count!=exact)Fail("size_mismatch","Official payload length disagrees with its manifest.");
  }
  std::uint64_t total=0;std::array<char,65536> buffer{};const auto start=std::chrono::steady_clock::now();
  while(true){DWORD count=0;if(!WinHttpReadData(request.value,buffer.data(),static_cast<DWORD>(buffer.size()),&count))Fail("network_failed","Official payload download was interrupted.");
    if(!count)break;if(count>limit-total)Fail("size_limit","Official payload exceeds its supported size.");
    total+=count;consume(buffer.data(),count);
    if(std::chrono::steady_clock::now()-start>std::chrono::minutes(30))Fail("download_timeout","The payload download exceeded thirty minutes.");
  }
  if(exact&&total!=exact)Fail("size_mismatch","Official payload ended before its declared length.");
}
std::string Fetch(std::string_view url,std::uint64_t limit)
{std::string result;Http(url,limit,[&](const char* data,std::size_t size){result.append(data,size);});return result;}
Plan CurrentPlan()
{return ParsePlan(Fetch("https://gus.xsolla.com/updates?version=0&project_id=152033&region=&platform=windows",65536));}
class PieceVerifier {
  Torrent torrent_;std::unique_ptr<Hash> hash_=std::make_unique<Hash>(true);
  std::uint64_t inPiece_=0,total_=0;std::size_t piece_=0;
public:
  explicit PieceVerifier(Torrent torrent):torrent_(std::move(torrent)){}
  void Add(const char* bytes,std::size_t size){
    if(size>torrent_.size-total_)Fail("size_mismatch","Downloaded archive exceeds its torrent length.");total_+=size;
    while(size){const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(size,torrent_.pieceSize-inPiece_));hash_->Add(bytes,count);inPiece_+=count;bytes+=count;size-=count;
      if(inPiece_==torrent_.pieceSize){if(hash_->Finish()!=torrent_.pieces.substr(piece_*20,20))Fail("integrity_failed","A downloaded torrent piece failed SHA-1 verification.");++piece_;inPiece_=0;hash_=std::make_unique<Hash>(true);}}
  }
  void Finish(){
    if(total_!=torrent_.size)Fail("size_mismatch","Downloaded archive is shorter than its torrent length.");
    if(inPiece_){if(hash_->Finish()!=torrent_.pieces.substr(piece_*20,20))Fail("integrity_failed","The last torrent piece failed SHA-1 verification.");++piece_;inPiece_=0;}
    if(piece_*20!=torrent_.pieces.size())Fail("integrity_failed","Not every archive piece was verified.");
  }
};
void Download(const fs::path& path,const Plan& plan,const Torrent& torrent,const std::function<void(std::uint64_t)>& progress={})
{
  Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
  if(file.value==INVALID_HANDLE_VALUE)Fail("write_failed","Could not create staged archive.");
  PieceVerifier verifier(torrent);std::uint64_t total=0;
  Http(plan.archive,plan.size,[&](const char* bytes,std::size_t size){
    verifier.Add(bytes,size);DWORD written=0;
    if(!WriteFile(file.value,bytes,static_cast<DWORD>(size),&written,nullptr)||written!=size)Fail("write_failed","Could not write staged archive.");
    total+=size;if(progress)progress(total);
  },plan.size);
  verifier.Finish();if(!FlushFileBuffers(file.value))Fail("write_failed","Could not durably stage the verified archive.");
}
std::vector<DWORD> Running(const fs::path& game)
{
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));if(snapshot.value==INVALID_HANDLE_VALUE)Fail("process_check_failed","Could not enumerate game processes.");
  PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);std::vector<DWORD> result;
  if(!Process32FirstW(snapshot.value,&entry))Fail("process_check_failed","Could not enumerate game processes.");
  const auto expected=Fold((game/"prime.exe").wstring());
  do {
    if(_wcsicmp(entry.szExeFile,L"prime.exe")!=0)continue;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID));
    if(process.value==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_INVALID_PARAMETER)continue;Fail("process_check_incomplete","A running prime.exe could not be identified; stop it before updating.");}
    std::wstring image(32768,L'\0');DWORD length=static_cast<DWORD>(image.size());
    if(!QueryFullProcessImageNameW(process.value,0,image.data(),&length))Fail("process_check_incomplete","A running prime.exe could not be identified.");
    image.resize(length);std::error_code error;const auto canonical=fs::canonical(image,error);
    if(error)Fail("process_check_incomplete","A running prime.exe installation could not be resolved.");
    if(Fold(canonical.wstring())==expected)result.push_back(entry.th32ProcessID);
  }while(Process32NextW(snapshot.value,&entry));
  if(GetLastError()!=ERROR_NO_MORE_FILES)Fail("process_check_failed","Game process enumeration was incomplete.");
  return result;
}
void Stopped(const fs::path& game)
{if(!Running(game).empty())Fail("installation_running","Stop every game session from the selected installation before updating or recovering it.");}
struct ExecutableGate {
  Handle file;
  explicit ExecutableGate(const fs::path& path):file(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)) {
    if(file.value==INVALID_HANDLE_VALUE)Fail("installation_busy","Could not exclude direct prime.exe launches; stop the game and retry.");
  }
  void Rename(const fs::path& destination)const {
    const auto& name=destination.native();const auto length=name.size()*sizeof(wchar_t);
    // FileName is terminated; FileNameLength excludes that final wide NUL.
    std::vector<unsigned char> buffer(offsetof(FILE_RENAME_INFO,FileName)+length+sizeof(wchar_t));
    auto* information=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());information->ReplaceIfExists=FALSE;information->RootDirectory=nullptr;
    information->FileNameLength=static_cast<DWORD>(length);std::memcpy(information->FileName,name.data(),length);
    if(!SetFileInformationByHandle(file.value,FileRenameInfo,information,static_cast<DWORD>(buffer.size()))||!FlushFileBuffers(file.value))
      Fail("move_failed","Could not rename the held executable; recovery data was retained.");
  }
};
fs::path Transaction(const fs::path& game,std::string_view key)
{return game.parent_path()/(".stfc-profiles-update-"+std::string(key));}
fs::path Ownership(const fs::path& game,std::string_view key)
{return game.parent_path()/(".stfc-profiles-owned-"+std::string(key)+".json");}
void Journal(const fs::path& transaction,const Json& journal)
{Atomic(transaction/"journal.json",journal.dump()+"\n");}
std::string Key(const fs::path& game)
{Hash hash;const auto text=Fold(game.wstring());hash.Add(text.data(),text.size());return Hex(hash.Finish());}
void Retain(const fs::path& transaction)
{
  std::array<unsigned char,16> bytes{};if(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)Fail("random_failed","Could not name retained update history.");
  const auto suffix=Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
  Move(transaction,transaction.parent_path()/(transaction.filename().wstring()+L"-history-"+Path(suffix).wstring()));
}
void Identity(const Json& journal,const fs::path& game,std::string_view key)
{
  if(journal.value("schemaVersion",0)!=1||journal.value("key",std::string{})!=key
     ||Fold(Path(journal.value("gameDirectory",std::string{})).wstring())!=Fold(game.wstring()))Fail("invalid_transaction","Update journal is not bound to this exact installation.");
  const auto phase=journal.at("phase").get<std::string>();
  if(phase!="downloading"&&phase!="extracting"&&phase!="staged"&&phase!="committing"&&phase!="committed"&&phase!="rolling-back"&&phase!="recovered")
    Fail("invalid_transaction","Unsupported update journal phase.");
  if(!journal.at("targetVersion").is_number_unsigned() && !journal.at("targetVersion").is_number_integer())Fail("invalid_transaction","Invalid target version.");
  const auto target=journal.at("targetVersion").get<std::uint64_t>();if(!target||target>INT_MAX)Fail("invalid_transaction","Invalid target version.");
  auto prior=journal.at("priorVersion").get<std::string>();if(prior.size()>64)Fail("invalid_transaction","Invalid prior version.");
  (void)MarkerVersion(prior);
  if(!journal.at("files").is_array()||journal.at("files").size()>EntryLimit)Fail("invalid_transaction","Invalid update file inventory.");
  std::set<std::string> paths;
  for(const auto& file:journal.at("files")){
    const auto relative=Relative(file.at("path").get<std::string>());Protected(relative);const auto folded=Fold(relative.wstring());
    if(folded==".version"||!paths.insert(folded).second)Fail("invalid_transaction","Duplicate or reserved update inventory path.");
    if(!file.at("hadOriginal").is_boolean()||!file.at("remove").is_boolean())Fail("invalid_transaction","Invalid inventory disposition.");
    if(!file.at("remove").get<bool>()){
      if(file.at("sha256").get<std::string>().size()!=64||file.at("size").get<std::uint64_t>()>ExtractedLimit)Fail("invalid_transaction","Invalid staged file size or digest.");
    }
    if(file.at("hadOriginal").get<bool>()){
      if(file.at("oldSha256").get<std::string>().size()!=64||file.at("oldSize").get<std::uint64_t>()>ExtractedLimit)Fail("invalid_transaction","Invalid original file size or digest.");
    }else if(file.at("remove").get<bool>())Fail("invalid_transaction","A removal requires a recorded original file.");
    for(const auto* field:{"sha256","oldSha256"}){
      const auto digest=file.value(field,std::string{});
      if(!digest.empty()&&(digest.size()!=64||digest.find_first_not_of("0123456789abcdef")!=std::string::npos))Fail("invalid_transaction","Invalid inventory digest.");
    }
  }
  if(!journal.at("directories").is_array()||journal.at("directories").size()>EntryLimit)Fail("invalid_transaction","Invalid created-directory inventory.");
  std::set<std::string> parentPaths;
  for(const auto& file:journal.at("files")){
    auto parent=Relative(file.at("path").get<std::string>()).parent_path();
    while(!parent.empty()){parentPaths.insert(Fold(parent.wstring()));parent=parent.parent_path();}
  }
  std::set<std::string> directories;
  for(const auto& item:journal.at("directories")){
    const auto relative=Relative(item.get<std::string>());Protected(relative);const auto folded=Fold(relative.wstring());
    if(!directories.insert(folded).second||!parentPaths.contains(folded))Fail("invalid_transaction","Created-directory inventory is outside the image.");
  }
}
Json Extract(const fs::path& archivePath,const fs::path& stage,const Plan& plan,
             const std::function<void(std::uint64_t)>& progress={})
{
  struct Reader {struct archive* value=archive_read_new();~Reader(){if(value)archive_read_free(value);}} reader;
  if(!reader.value||archive_read_support_filter_none(reader.value)!=ARCHIVE_OK||archive_read_support_format_7zip(reader.value)!=ARCHIVE_OK
     ||archive_read_open_filename_w(reader.value,archivePath.c_str(),65536)!=ARCHIVE_OK)
    Fail("archive_failed","Could not open the verified 7z image with the built-in extractor.");
  Json inventory=Json::array();std::set<std::string> names;std::uint64_t total=0;struct archive_entry* entry=nullptr;
  while(true){
    const auto status=archive_read_next_header(reader.value,&entry);if(status==ARCHIVE_EOF)break;
    if(status!=ARCHIVE_OK)Fail("archive_failed","The verified archive has an unreadable header.");
    const auto* pathname=archive_entry_pathname_utf8(entry);if(!pathname)Fail("unsafe_archive_path","Archive path is not valid UTF-8.");
    std::string name=pathname;while(!name.empty()&&name.back()=='/')name.pop_back();
    const auto relative=Relative(name);Protected(relative);const auto folded=Fold(relative.wstring());
    if(!names.insert(folded).second||names.size()>EntryLimit)Fail("unsafe_archive_path","Duplicate archive path or too many entries.");
    if(archive_entry_symlink(entry)||archive_entry_hardlink(entry))Fail("unsafe_archive_path","Archive links are unsupported.");
    const auto type=archive_entry_filetype(entry);
    if(type==AE_IFDIR){Parents(stage,relative/"child");fs::create_directories(stage/relative);continue;}
    if(type!=AE_IFREG||archive_entry_size(entry)<0)Fail("unsafe_archive_path","Only ordinary files and directories are accepted.");
    const auto declared=static_cast<std::uint64_t>(archive_entry_size(entry));
    if(declared>plan.extracted-total)Fail("size_limit","Extracted image exceeds its official byte inventory.");
    Parents(stage,relative);fs::create_directories((stage/relative).parent_path());
    Handle output(CreateFileW((stage/relative).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(output.value==INVALID_HANDLE_VALUE)Fail("write_failed","Could not create a staged image file.");
    Hash hash;std::array<char,65536> bytes{};std::uint64_t written=0;
    while(true){const auto count=archive_read_data(reader.value,bytes.data(),bytes.size());
      if(count<0)Fail("archive_failed","The verified archive failed decompression.");if(!count)break;
      if(static_cast<std::uint64_t>(count)>declared-written)Fail("size_limit","An archive file exceeds its declared length.");
      DWORD actual=0;if(!WriteFile(output.value,bytes.data(),static_cast<DWORD>(count),&actual,nullptr)||actual!=count)Fail("write_failed","Could not write a staged image file.");
      written+=static_cast<std::uint64_t>(count);hash.Add(bytes.data(),static_cast<std::size_t>(count));
    }
    if(written!=declared||!FlushFileBuffers(output.value))Fail("archive_failed","Staged file length or durable write failed.");
    total+=written;
    // The protocol's version action owns .version, never an archive entry.
    if(folded!=".version")inventory.push_back({{"path",RelativeText(relative)},{"size",written},{"sha256",Hex(hash.Finish())},{"remove",false}});
    if(progress)progress(total);
  }
  if(total!=plan.extracted)Fail("size_mismatch","Extracted image size disagrees with the official manifest.");
  for(const auto* required:{"prime.exe","GameAssembly.dll","UnityPlayer.dll"}){
    if(!fs::exists(stage/required))Fail("invalid_game_image","The full image is missing a required Windows game binary.");Plain(stage/required,false);
  }
  Plain(stage/"prime_Data",true);return inventory;
}
void Inventory(const fs::path& game,const fs::path& ownership,Json& journal)
{
  std::set<std::string> incoming;
  for(auto& file:journal["files"]){
    const auto relative=Relative(file.at("path").get<std::string>());Protected(relative);incoming.insert(Fold(relative.wstring()));Parents(game,relative);
    const auto destination=game/relative;const bool present=fs::exists(destination);file["hadOriginal"]=present;
    if(present){Plain(destination,false);file["oldSha256"]=Digest(destination);file["oldSize"]=fs::file_size(destination);}
  }
  journal["priorOwnership"]=nullptr;
  if(fs::exists(ownership)){
    const auto text=Read(ownership,32u<<20);const auto previous=ParseJson(text);
    if(previous.value("schemaVersion",0)!=1||previous.at("key")!=journal.at("key")||previous.at("gameDirectory")!=journal.at("gameDirectory")
       ||!previous.at("files").is_array()||previous.at("files").size()>EntryLimit)Fail("invalid_ownership","Owned-image inventory does not match this installation.");
    journal["priorOwnership"]=text;
    std::set<std::string> priorPaths;
    for(const auto& file:previous.at("files")){
      const auto relative=Relative(file.at("path").get<std::string>());Protected(relative);const auto folded=Fold(relative.wstring());
      const auto oldDigest=file.at("sha256").get<std::string>();
      if(folded==".version"||!priorPaths.insert(folded).second||oldDigest.size()!=64||oldDigest.find_first_not_of("0123456789abcdef")!=std::string::npos)Fail("invalid_ownership","Invalid previous owned-image inventory.");
      if(incoming.contains(folded))continue;
      if(!fs::exists(game/relative))continue;Parents(game,relative);Plain(game/relative,false);
      const auto digest=Digest(game/relative);if(digest!=file.at("sha256").get<std::string>())continue; // Modified old files are preserved as extras.
      journal["files"].push_back({{"path",RelativeText(relative)},{"remove",true},{"hadOriginal",true},{"oldSha256",digest},{"oldSize",fs::file_size(game/relative)}});
    }
  }
  if(journal["files"].size()>EntryLimit)Fail("size_limit","Combined image inventory is too large.");
}
void CheckFile(const fs::path& path,std::string_view digest,std::uint64_t size)
{if(!fs::exists(path)||fs::file_size(path)!=size||Digest(path)!=digest)Fail("transaction_conflict","An image file changed outside the transaction: "+Utf8(path));}
std::string GateDigest(HANDLE handle,std::uint64_t& size);
std::string OwnershipBytes(const Json& journal)
{
  Json files=Json::array();for(const auto& file:journal.at("files"))if(!file.at("remove").get<bool>())
    files.push_back({{"path",file.at("path")},{"sha256",file.at("sha256")},{"size",file.at("size")}});
  return Json{{"schemaVersion",1},{"key",journal.at("key")},{"gameDirectory",journal.at("gameDirectory")},{"files",files}}.dump()+"\n";
}
void Metadata(const fs::path& game,const fs::path& ownership,const Json& journal,bool recovering,bool produced=false)
{
  const auto version=Read(game/".version",64);const auto prior=journal.at("priorVersion").get<std::string>();
  const auto next=UpdatedMarker(prior,journal.at("targetVersion").get<std::uint64_t>());
  if((produced&&version!=next)||(!produced&&version!=prior&&(!recovering||version!=next)))
    Fail("transaction_conflict","The installed version marker changed outside this transaction.");
  const bool present=fs::exists(ownership);const auto bytes=present?Read(ownership,32u<<20):std::string{};
  const bool oldMatches=journal.at("priorOwnership").is_null()?!present:(present&&bytes==journal.at("priorOwnership").get<std::string>());
  const bool newMatches=present&&bytes==OwnershipBytes(journal);
  if((produced&&!newMatches)||(!produced&&!oldMatches&&(!recovering||!newMatches)))
    Fail("transaction_conflict","Owned-image metadata changed outside this transaction.");
}
std::vector<std::unique_ptr<Handle>> FreezeImage(const fs::path& game,const Json& journal,const ExecutableGate& executable)
{
  std::vector<std::unique_ptr<Handle>> files;
  for(const auto& file:journal.at("files")){
    const auto relative=Relative(file.at("path").get<std::string>());Parents(game,relative);
    if(file.at("remove").get<bool>()){
      if(fs::exists(game/relative))Fail("transaction_conflict","A retired owned file reappeared during commit.");continue;
    }
    HANDLE handle=executable.file.value;
    if(Fold(relative.wstring())!="prime.exe"){
      auto held=std::make_unique<Handle>(CreateFileW((game/relative).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
      if(held->value==INVALID_HANDLE_VALUE)Fail("transaction_conflict","Could not hold a published file against writes and deletion.");
      handle=held->value;files.push_back(std::move(held));
    }
    if(Fold(relative.wstring())=="prime.exe"){
      Handle named(CreateFileW((game/relative).c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
      BY_HANDLE_FILE_INFORMATION original{},current{};
      if(named.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(handle,&original)||!GetFileInformationByHandle(named.value,&current)
         ||original.dwVolumeSerialNumber!=current.dwVolumeSerialNumber||original.nFileIndexHigh!=current.nFileIndexHigh||original.nFileIndexLow!=current.nFileIndexLow)
        Fail("transaction_conflict","The published executable path no longer names the held file.");
    }
    BY_HANDLE_FILE_INFORMATION information{};std::uint64_t size=0;
    if(!GetFileInformationByHandle(handle,&information)||(information.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||information.nNumberOfLinks!=1
       ||GateDigest(handle,size)!=file.at("sha256").get<std::string>()||size!=file.at("size").get<std::uint64_t>())
      Fail("transaction_conflict","The complete published image failed final integrity verification.");
  }return files;
}
void CheckLive(const fs::path& game,const fs::path& relative,std::string_view digest,std::uint64_t size,const ExecutableGate* gate);
void Commit(const fs::path& game,const fs::path& transaction,const fs::path& ownership,Json& journal,
            const std::function<void(std::string_view)>& checkpoint={},const ExecutableGate* gate=nullptr)
{
  Metadata(game,ownership,journal,false);
  const auto stage=transaction/"stage",backup=transaction/"backup";fs::create_directory(backup);
  journal["phase"]="committing";journal["completedFiles"]=0;Journal(transaction,journal);
  auto point=[&](std::string_view name){if(checkpoint)checkpoint(name);};point("committing");
  // Remove prime.exe first while its read gate prevents new direct launches.
  // Publish the new executable after every other image file has committed.
  auto& files=journal["files"];
  auto& sorted=files.get_ref<Json::array_t&>();
  std::stable_sort(sorted.begin(),sorted.end(),[](const Json& a,const Json& b){return Fold(Relative(a.at("path").get<std::string>()).wstring())=="prime.exe" && Fold(Relative(b.at("path").get<std::string>()).wstring())!="prime.exe";});
  Journal(transaction,journal);
  for(const auto& file:files){
    const auto relative=Relative(file.at("path").get<std::string>());Parents(game,relative);
    if(file.at("hadOriginal").get<bool>()){
      CheckLive(game,relative,file.at("oldSha256").get<std::string>(),file.at("oldSize").get<std::uint64_t>(),gate);
      fs::create_directories((backup/relative).parent_path());
      if(gate&&Fold(relative.wstring())=="prime.exe")gate->Rename(backup/relative);else Move(game/relative,backup/relative);point("backup");
    }
  }
  std::size_t completed=0;
  for(const auto& file:files){
    const auto relative=Relative(file.at("path").get<std::string>());
    if(Fold(relative.wstring())=="prime.exe")continue;
    if(!file.at("remove").get<bool>()){
      CheckFile(stage/relative,file.at("sha256").get<std::string>(),file.at("size").get<std::uint64_t>());
      Parents(game,relative);fs::create_directories((game/relative).parent_path());Move(stage/relative,game/relative);point("publish");
    }
    journal["completedFiles"]=++completed;Journal(transaction,journal);
  }
  std::unique_ptr<ExecutableGate> publishedExecutable;
  for(const auto& file:files)if(Fold(Relative(file.at("path").get<std::string>()).wstring())=="prime.exe"){
    const auto relative=Relative(file.at("path").get<std::string>());
    CheckFile(stage/relative,file.at("sha256").get<std::string>(),file.at("size").get<std::uint64_t>());
    publishedExecutable=std::make_unique<ExecutableGate>(stage/relative);
    publishedExecutable->Rename(game/relative);point("executable");journal["completedFiles"]=++completed;Journal(transaction,journal);
  }
  if(!publishedExecutable)Fail("invalid_game_image","The committed image has no executable.");
  auto frozen=FreezeImage(game,journal,*publishedExecutable);point("verified");
  Metadata(game,ownership,journal,false);
  Atomic(ownership,OwnershipBytes(journal));point("ownership");
  if(Read(game/".version",64)!=journal.at("priorVersion").get<std::string>())Fail("transaction_conflict","The version marker changed during finalization.");
  Atomic(game/".version",UpdatedMarker(journal.at("priorVersion").get<std::string>(),journal.at("targetVersion").get<std::uint64_t>()));point("version");
  Metadata(game,ownership,journal,false,true);
  journal["phase"]="committed";Journal(transaction,journal);point("committed");
}
std::string GateDigest(HANDLE handle,std::uint64_t& size)
{
  LARGE_INTEGER zero{},length{};if(!GetFileSizeEx(handle,&length)||length.QuadPart<0||!SetFilePointerEx(handle,zero,nullptr,FILE_BEGIN))Fail("read_failed","Could not inspect the blocked game executable.");
  size=static_cast<std::uint64_t>(length.QuadPart);Hash hash;std::array<char,65536> bytes{};
  while(true){DWORD count=0;if(!ReadFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr))Fail("read_failed","Could not verify the blocked game executable.");if(!count)break;hash.Add(bytes.data(),count);}
  return Hex(hash.Finish());
}
void CheckLive(const fs::path& game,const fs::path& relative,std::string_view digest,std::uint64_t size,const ExecutableGate* gate)
{
  if(gate&&Fold(relative.wstring())=="prime.exe"){
    std::uint64_t actual=0;if(GateDigest(gate->file.value,actual)!=digest||actual!=size)Fail("transaction_conflict","The game executable changed before admission.");
  }else CheckFile(game/relative,digest,size);
}
std::vector<std::unique_ptr<Handle>> FreezeRestored(const fs::path& game,const Json& journal)
{
  std::vector<std::unique_ptr<Handle>> held;
  for(const auto& file:journal.at("files")){
    const auto relative=Relative(file.at("path").get<std::string>());if(Fold(relative.wstring())=="prime.exe")continue;Parents(game,relative);
    if(!file.at("hadOriginal").get<bool>()){
      if(fs::exists(game/relative))Fail("transaction_conflict","A newly published file reappeared during recovery.");continue;
    }
    auto handle=std::make_unique<Handle>(CreateFileW((game/relative).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(handle->value==INVALID_HANDLE_VALUE)Fail("transaction_conflict","Could not hold an original image file during recovery.");
    BY_HANDLE_FILE_INFORMATION information{};std::uint64_t size=0;
    if(!GetFileInformationByHandle(handle->value,&information)||(information.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||information.nNumberOfLinks!=1
       ||GateDigest(handle->value,size)!=file.at("oldSha256").get<std::string>()||size!=file.at("oldSize").get<std::uint64_t>())
      Fail("transaction_conflict","The complete restored image failed final integrity verification.");
    held.push_back(std::move(handle));
  }return held;
}
void Rollback(const fs::path& game,const fs::path& transaction,const fs::path& ownership,Json& journal,
              const ExecutableGate* gate=nullptr,const std::function<void(std::string_view)>& checkpoint={})
{
  Metadata(game,ownership,journal,true);
  const auto backup=transaction/"backup";journal["phase"]="rolling-back";Journal(transaction,journal);
  auto point=[&](std::string_view name){if(checkpoint)checkpoint(name);};
  const Json* executable=nullptr;
  for(const auto& file:journal.at("files"))if(Fold(Relative(file.at("path").get<std::string>()).wstring())=="prime.exe")executable=&file;
  if(!executable||!executable->at("hadOriginal").get<bool>()||executable->at("remove").get<bool>())Fail("invalid_transaction","Recovery requires the recorded original executable.");
  const bool executableBackup=fs::exists(backup/"prime.exe");std::unique_ptr<ExecutableGate> original;
  const ExecutableGate* recoveredExecutable=gate;
  if(executableBackup){
    original=std::make_unique<ExecutableGate>(backup/"prime.exe");recoveredExecutable=original.get();
    CheckLive(game,fs::path("prime.exe"),executable->at("oldSha256").get<std::string>(),executable->at("oldSize").get<std::uint64_t>(),recoveredExecutable);
    // A failed recovery must not leave the new executable launching against
    // partly restored old files. Quarantine it before restoring anything else.
    if(fs::exists(game/"prime.exe")){
      CheckLive(game,fs::path("prime.exe"),executable->at("sha256").get<std::string>(),executable->at("size").get<std::uint64_t>(),gate);
      if(gate)gate->Rename(transaction/"rejected-prime.exe");else Move(game/"prime.exe",transaction/"rejected-prime.exe");point("remove-new");
    }
  }else{
    if(!gate){original=std::make_unique<ExecutableGate>(game/"prime.exe");recoveredExecutable=original.get();}
    CheckLive(game,fs::path("prime.exe"),executable->at("oldSha256").get<std::string>(),executable->at("oldSize").get<std::uint64_t>(),recoveredExecutable);
  }
  for(const auto& file:journal.at("files")){
    const auto relative=Relative(file.at("path").get<std::string>());if(Fold(relative.wstring())=="prime.exe")continue;Parents(game,relative);
    const auto destination=game/relative;const bool old=file.at("hadOriginal").get<bool>();
    if(fs::exists(backup/relative)){
      CheckFile(backup/relative,file.at("oldSha256").get<std::string>(),file.at("oldSize").get<std::uint64_t>());
      if(fs::exists(destination)){
        if(file.at("remove").get<bool>())Fail("transaction_conflict","A removed file was recreated outside the transaction.");
        CheckFile(destination,file.at("sha256").get<std::string>(),file.at("size").get<std::uint64_t>());fs::remove(destination);point("remove-new");
      }
      fs::create_directories(destination.parent_path());Move(backup/relative,destination);point("restore-old");
    }else if(old)CheckFile(destination,file.at("oldSha256").get<std::string>(),file.at("oldSize").get<std::uint64_t>());
    else if(fs::exists(destination)){
      CheckFile(destination,file.at("sha256").get<std::string>(),file.at("size").get<std::uint64_t>());fs::remove(destination);point("remove-new");
    }
  }
  auto frozen=FreezeRestored(game,journal);point("restore-verified");
  Metadata(game,ownership,journal,true);
  if(journal.at("priorOwnership").is_null()){
    if(fs::exists(ownership)){Plain(ownership,false);fs::remove(ownership);}
  }else Atomic(ownership,journal.at("priorOwnership").get<std::string>());
  point("restore-ownership");Metadata(game,ownership,journal,true);
  Atomic(game/".version",journal.at("priorVersion").get<std::string>());point("restore-version");Metadata(game,ownership,journal,false);
  if(executableBackup){original->Rename(game/"prime.exe");point("restore-old");}
  CheckLive(game,fs::path("prime.exe"),executable->at("oldSha256").get<std::string>(),executable->at("oldSize").get<std::uint64_t>(),recoveredExecutable);
  Handle named(CreateFileW((game/"prime.exe").c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  BY_HANDLE_FILE_INFORMATION held{},current{};
  if(named.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(recoveredExecutable->file.value,&held)||!GetFileInformationByHandle(named.value,&current)
     ||held.dwVolumeSerialNumber!=current.dwVolumeSerialNumber||held.nFileIndexHigh!=current.nFileIndexHigh||held.nFileIndexLow!=current.nFileIndexLow)
    Fail("transaction_conflict","The restored executable path changed during recovery.");
  if(journal.contains("directories"))for(auto it=journal["directories"].rbegin();it!=journal["directories"].rend();++it){
    const auto directory=game/Relative(it->get<std::string>());if(fs::exists(directory)){Plain(directory,true);if(fs::is_empty(directory))fs::remove(directory);}
  }
  Metadata(game,ownership,journal,false);
}
void VerifyCommitted(const fs::path& game,const Json& journal)
{
  for(const auto& file:journal.at("files")){
    const auto relative=Relative(file.at("path").get<std::string>());Parents(game,relative);
    if(!file.at("remove").get<bool>())CheckFile(game/relative,file.at("sha256").get<std::string>(),file.at("size").get<std::uint64_t>());
    else if(fs::exists(game/relative))Fail("transaction_conflict","A deleted owned file reappeared before transaction cleanup.");
  }
  if(Version(game)!=journal.at("targetVersion").get<std::uint64_t>())Fail("transaction_conflict","Installed version changed before transaction cleanup.");
}
Json Snapshot(const fs::path& game,const fs::path& transaction,bool active)
{
  Json result{{"gameDirectory",Utf8(game)},{"state","ready"},{"phase","idle"},{"requiresRecovery",false}};
  const auto processes=Running(game);result["runningProcessIds"]=processes;
  if(!processes.empty())result["state"]="running";
  if(fs::exists(game/".version"))result["installedVersion"]=Version(game);else result["installedVersion"]=nullptr;
  if(fs::exists(transaction)){
    Plain(transaction,true);
    if(!fs::exists(transaction/"journal.json")){
      result["transactionId"]=Key(game);result["state"]=active?"updating":"recovery-required";result["phase"]=active?"initializing":"invalid";result["requiresRecovery"]=!active;
      result["message"]="The update directory has no journal; an empty orphan can be retained by explicit recovery.";return result;
    }
    const auto journal=ParseJson(Read(transaction/"journal.json",32u<<20));Identity(journal,game,Key(game));
    const auto phase=journal.at("phase").get<std::string>();const bool incomplete=phase!="committed"&&phase!="recovered";
    result["phase"]=phase;result["transactionId"]=journal.at("key");result["availableVersion"]=journal.at("targetVersion");
    result["requiresRecovery"]=incomplete&&!active;result["state"]=active?"updating":(incomplete?"recovery-required":result.at("state"));
    if(phase=="downloading"){
      const auto bytes=journal.value("downloadedBytes",0ull),total=journal.value("downloadBytes",0ull);
      result["downloadedBytes"]=bytes;result["downloadBytes"]=total;if(total)result["progressPercent"]=(100.0*bytes)/total;
    }else if(phase=="extracting"){
      const auto bytes=journal.value("extractedBytes",0ull),total=journal.value("extractedTotalBytes",0ull);
      result["extractedBytes"]=bytes;result["extractedTotalBytes"]=total;if(total)result["progressPercent"]=(100.0*bytes)/total;
    }else if(phase=="committing"){
      const auto count=journal.value("completedFiles",0u);const auto total=journal.at("files").size();result["completedFiles"]=count;result["totalFiles"]=total;
      if(total)result["progressPercent"]=(100.0*count)/total;
    }else if(phase=="committed")result["progressPercent"]=100;
  }return result;
}
void Baseline(const fs::path& game)
{Plain(game,true);Plain(game/"prime.exe",false);Plain(game/"GameAssembly.dll",false);Plain(game/"UnityPlayer.dll",false);Plain(game/"prime_Data",true);(void)Version(game);}
void Space(const fs::path& directory,std::uint64_t needed)
{if(fs::space(directory).available<needed)Fail("insufficient_space","The selected volume lacks space for the archive, extracted image and recovery backups.");}
Json Run(const Json& request)
{
  const auto operation=request.at("operation").get<std::string>();
  if(operation!="installation-status"&&operation!="check-game-update"&&operation!="update-game"&&operation!="recover-game-update")Fail("unknown_operation","Unknown installation operation.");
  const auto requested=Path(request.at("gameDirectory").get<std::string>());
  if(!requested.is_absolute())Fail("invalid_installation","Select an absolute game installation directory.");
  Plain(requested,true);const auto game=fs::canonical(requested);Plain(game.parent_path(),true);
  const auto root=request.contains("root")?Path(request.at("root").get<std::string>()):DefaultCatalogRoot();
  const auto key=Key(game);const auto transaction=Transaction(game,key),ownership=Ownership(game,key);
  if(operation=="installation-status"){
    bool active=false;std::unique_ptr<InstallationLease> inspection;
    try {inspection=std::make_unique<InstallationLease>(root,game,false);}
    catch(const CatalogError& error){
      if(error.Code()!="busy")throw;active=true;
      inspection=std::make_unique<InstallationLease>(root,game,false,true);
    }
    if(request.contains("installationPhysicalIdentity")
        && request.at("installationPhysicalIdentity")!=Json(inspection->PhysicalIdentity()))
      Fail("installation_changed","The installation directory changed before status admission.");
    return Json{{"apiVersion",1},{"ok",true},{"installation",Snapshot(game,transaction,active)}};
  }
  const bool mutation=operation=="update-game"||operation=="recover-game-update";InstallationLease lease(root,game,mutation);
  if (request.contains("installationPhysicalIdentity")
      && request.at("installationPhysicalIdentity")!=Json(lease.PhysicalIdentity()))
    Fail("installation_changed","The installation directory changed before operation admission.");
  if(operation=="recover-game-update"){
    Stopped(game);
    if(!fs::exists(transaction))return Json{{"apiVersion",1},{"ok",true},{"installation",Snapshot(game,transaction,false)},{"recovered",false}};
    TreePlain(transaction);
    if(!fs::exists(transaction/"journal.json")){
      if(!fs::is_empty(transaction))Fail("invalid_transaction","The update directory has unknown contents and no journal; preserve it for inspection.");
      Durable(transaction/"orphan-recovery.json",Json{{"schemaVersion",1},{"key",lease.Key()},{"gameDirectory",Utf8(game)},{"reason","empty pre-journal interruption"}}.dump()+"\n",true);
      Retain(transaction);return Json{{"apiVersion",1},{"ok",true},{"installation",Snapshot(game,transaction,false)},{"recovered",true},{"historyRetained",true}};
    }
    auto journal=ParseJson(Read(transaction/"journal.json",32u<<20));Identity(journal,game,lease.Key());
    const auto phase=journal.at("phase").get<std::string>();
    if(phase=="committed"){Retain(transaction);return Json{{"apiVersion",1},{"ok",true},{"installation",Snapshot(game,transaction,false)},{"recovered",false},{"historyRetained",true}};}
    else if(phase=="committing"||phase=="rolling-back"){
      std::unique_ptr<ExecutableGate> gate;if(fs::exists(game/"prime.exe"))gate=std::make_unique<ExecutableGate>(game/"prime.exe");Stopped(game);
      try{Rollback(game,transaction,ownership,journal,gate.get());}catch(const std::exception& error){
        const auto* typed=dynamic_cast<const CatalogError*>(&error);journal["recoveryFailure"]={{"code",typed?typed->Code():"operation_failed"},{"message",std::string(error.what()).substr(0,4096)}};
        try{Journal(transaction,journal);}catch(...){}throw;
      }
    }
    if(phase!="committed"){journal["phase"]="recovered";Journal(transaction,journal);Retain(transaction);}
    return Json{{"apiVersion",1},{"ok",true},{"installation",Snapshot(game,transaction,false)},{"recovered",phase!="committed"},{"historyRetained",true}};
  }
  Baseline(game);const auto installed=Version(game);const auto plan=CurrentPlan();
  if(operation=="check-game-update"){
    auto status=Snapshot(game,transaction,false);status["availableVersion"]=plan.version;status["updateAvailable"]=plan.version>installed;
    status["downloadBytes"]=plan.size;status["extractedTotalBytes"]=plan.extracted;
    return Json{{"apiVersion",1},{"ok",true},{"installation",status}};
  }
  if(request.contains("expectedVersion")&&request.at("expectedVersion").get<std::uint64_t>()!=plan.version)Fail("update_changed","The available version changed after the update check; check again.");
  if(fs::exists(transaction)){
    auto prior=ParseJson(Read(transaction/"journal.json",32u<<20));Identity(prior,game,lease.Key());
    if(prior.at("phase")!="committed"&&prior.at("phase")!="recovered")Fail("recovery_required","Recover the unfinished installation transaction before starting another update.");
    Retain(transaction);
  }
  if(plan.version<=installed){auto status=Snapshot(game,transaction,false);status["availableVersion"]=plan.version;status["updateAvailable"]=false;return Json{{"apiVersion",1},{"ok",true},{"installation",status},{"updated",false}};}
  Stopped(game);Space(game.parent_path(),plan.size+2*plan.extracted+(64ull<<20));
  const auto torrentBytes=Fetch(plan.torrent,16u<<20);const auto torrent=ParseTorrent(torrentBytes,plan);
  if(!fs::create_directory(transaction))Fail("recovery_required","The installation already has an update transaction.");
  Json journal{{"schemaVersion",1},{"key",key},{"gameDirectory",Utf8(game)},{"targetVersion",plan.version},
    {"priorVersion",Read(game/".version",64)},{"phase","downloading"},{"downloadBytes",plan.size},{"downloadedBytes",0},
    {"extractedTotalBytes",plan.extracted},{"extractedBytes",0},{"files",Json::array()},{"directories",Json::array()}};
  Journal(transaction,journal);
  try {
  Durable(transaction/"manifest.xml",plan.manifest,true);
  Durable(transaction/"image.torrent",torrentBytes,true);std::uint64_t publishedDownload=0;
  Download(transaction/"image.7z",plan,torrent,[&](std::uint64_t bytes){
    if(bytes-publishedDownload>=(4ull<<20)||bytes==plan.size){journal["downloadedBytes"]=bytes;Journal(transaction,journal);publishedDownload=bytes;}
  });
  journal["downloadedBytes"]=plan.size;journal["phase"]="extracting";Journal(transaction,journal);
  const auto stage=transaction/"stage";fs::create_directory(stage);std::uint64_t lastProgress=0;
  journal["files"]=Extract(transaction/"image.7z",stage,plan,[&](std::uint64_t bytes){
    if(bytes-lastProgress>=(8ull<<20)||bytes==plan.extracted){journal["extractedBytes"]=bytes;Journal(transaction,journal);lastProgress=bytes;}
  });
  Inventory(game,ownership,journal);
  std::set<fs::path> directories;std::uint64_t backupBytes=0;
  for(const auto& file:journal.at("files")){
    const auto relative=Relative(file.at("path").get<std::string>());fs::path prefix;
    for(const auto& component:relative.parent_path()){prefix/=component;if(!fs::exists(game/prefix))directories.insert(prefix);}
    backupBytes+=file.value("oldSize",0ull);
  }
  for(const auto& directory:directories)journal["directories"].push_back(RelativeText(directory));
  Space(game.parent_path(),backupBytes+(64ull<<20));journal["phase"]="staged";Identity(journal,game,key);Journal(transaction,journal);
  {
    Stopped(game);ExecutableGate gate(game/"prime.exe");Stopped(game);
    if(Read(game/".version",64)!=journal.at("priorVersion").get<std::string>())Fail("transaction_conflict","The installation version changed during staging.");
    Commit(game,transaction,ownership,journal,{},&gate);
  }
  const auto completed=Snapshot(game,transaction,false);Retain(transaction);
  return Json{{"apiVersion",1},{"ok",true},{"installation",completed},{"updated",true},{"historyRetained",true}};
  }catch(const std::exception& error){
    const auto* typed=dynamic_cast<const CatalogError*>(&error);
    journal["failure"]={{"code",typed?typed->Code():"operation_failed"},{"message",std::string(error.what()).substr(0,4096)}};
    if(fs::exists(transaction)){try{Journal(transaction,journal);}catch(...){}}
    throw;
  }
}
#endif
} // namespace installation_detail
std::string ExecuteInstallationRequest(std::string_view request_utf8)
{
  using namespace installation_detail;
  try {
    if(request_utf8.size()>65536)Fail("request_too_large","Installation request exceeds its supported size.");
#if _WIN32
    const auto request=ParseJson(request_utf8);
    if(!request.is_object()||request.value("apiVersion",0)!=1)Fail("api_version","Installation requests require apiVersion1.");
    return Run(request).dump();
#else
    Fail("unsupported_platform","The direct game updater is not yet qualified for this platform.");
#endif
  }catch(const CatalogError& error){return Json{{"apiVersion",1},{"ok",false},{"error",{{"code",error.Code()},{"message",error.what()}}}}.dump();}
  catch(const std::exception& error){return Json{{"apiVersion",1},{"ok",false},{"error",{{"code","operation_failed"},{"message",error.what()}}}}.dump();}
}
} // namespace stfc::profiles
