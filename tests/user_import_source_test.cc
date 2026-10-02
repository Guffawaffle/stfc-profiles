// Generated in-memory fixtures only: no real hives or account preferences.
#include "stfc_profiles/user_import.h"
#include "stfc_profiles/catalog.h"
#include <Windows.h>
#include <algorithm>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace stfc::profiles::detail {
LSTATUS OpenImportRegistryComponent(HKEY parent, const wchar_t* component, HKEY* opened);
}
using namespace stfc::profiles;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F action, const char* code = "source_invalid") {
  try { action(); } catch (const CatalogError& error) { Check(error.Code() == code, "wrong source error"); return; }
  throw std::runtime_error("malformed fixture was accepted");
}
void Put16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t n) { v.at(at)=n; v.at(at+1)=n>>8; }
void Put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t n) { for (unsigned i=0;i<4;++i) v.at(at+i)=n>>(8*i); }
std::uint32_t Get32(const std::vector<std::uint8_t>& v, std::size_t at) { std::uint32_t n=0; for(unsigned i=0;i<4;++i)n|=std::uint32_t(v.at(at+i))<<(8*i); return n; }
std::string Hashed(std::string key) {
  std::uint32_t hash=5381;
  for(const unsigned char byte:key)hash=(hash*33u)^std::uint32_t(std::int32_t(std::int8_t(byte)));
  return key+"_h"+std::to_string(hash);
}
std::u16string NativeName(std::string_view bytes) {
  const auto n=MultiByteToWideChar(GetACP(),MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
  Check(n>0,"synthetic ANSI name conversion failed");
  std::wstring wide(n,L'\0');
  Check(MultiByteToWideChar(GetACP(),MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),wide.data(),n)==n,"synthetic ANSI conversion failed");
  return {reinterpret_cast<const char16_t*>(wide.data()),wide.size()};
}
std::vector<std::uint8_t> WideBytes(std::u16string_view text) {
  std::vector<std::uint8_t> data; for(const auto c:text){data.push_back(c&255);data.push_back(c>>8);} return data;
}
struct Fixture {
  std::vector<std::uint8_t> bytes=std::vector<std::uint8_t>(4096+32);
  std::uint32_t root=0, game=0, value_list=0;
  std::vector<std::uint32_t> values;
  std::size_t cursor=32;
  std::uint32_t Cell(const std::vector<std::uint8_t>& data) {
    const auto offset=static_cast<std::uint32_t>(cursor); const auto size=(data.size()+4+7)&~std::size_t(7);
    bytes.resize(4096+cursor+size);
    Put32(bytes,4096+cursor,0u-static_cast<std::uint32_t>(size));
    std::copy(data.begin(),data.end(),bytes.begin()+4096+cursor+4);cursor+=size;return offset;
  }
  std::uint32_t Value(std::u16string name, std::uint32_t type, std::vector<std::uint8_t> data, bool inlined=false, bool big=false) {
    std::uint32_t offset=0;
    if(!inlined && !data.empty()) {
      if(big) {
        std::vector<std::uint32_t> segments;
        for(std::size_t at=0;at<data.size();at+=16344)segments.push_back(Cell({data.begin()+at,data.begin()+std::min(at+16344,data.size())}));
        std::vector<std::uint8_t> list(segments.size()*4);
        for(std::size_t i=0;i<segments.size();++i)Put32(list,i*4,segments[i]);
        const auto list_offset=Cell(list);std::vector<std::uint8_t> db(8);db[0]='d';db[1]='b';Put16(db,2,static_cast<std::uint16_t>(segments.size()));Put32(db,4,list_offset);offset=Cell(db);
      } else offset=Cell(data);
    }
    std::vector<std::uint8_t> vk(20+name.size()*2);vk[0]='v';vk[1]='k';Put16(vk,2,static_cast<std::uint16_t>(name.size()*2));Put32(vk,4,static_cast<std::uint32_t>(data.size())|(inlined?0x80000000u:0));Put32(vk,8,offset);Put32(vk,12,type);
    if(inlined){Check(data.size()<=4,"fixture inline too large");std::copy(data.begin(),data.end(),vk.begin()+8);}
    for(std::size_t i=0;i<name.size();++i)Put16(vk,20+i*2,name[i]);
    const auto result=Cell(vk);values.push_back(result);return result;
  }
  std::uint32_t List(const std::vector<std::uint32_t>& children, std::string_view kind) {
    std::vector<std::uint8_t> list(4+children.size()*(kind=="lf"||kind=="lh"?8:4));list[0]=kind[0];list[1]=kind[1];Put16(list,2,static_cast<std::uint16_t>(children.size()));
    const auto stride=kind=="lf"||kind=="lh"?8:4;
    for(std::size_t i=0;i<children.size();++i)Put32(list,4+i*stride,children[i]);
    const auto offset=Cell(list);if(kind!="ri")return offset;
    return offset;
  }
  std::uint32_t Node(std::string_view name, std::vector<std::uint32_t> children, std::string_view kind, std::vector<std::uint32_t> node_values={}) {
    auto children_list=std::uint32_t(0xffffffff);
    if(!children.empty())children_list=kind=="ri"?List({List(children,"li")},"ri"):List(children,kind);
    std::uint32_t values_offset=0xffffffff;
    if(!node_values.empty()) {std::vector<std::uint8_t> list(node_values.size()*4);for(std::size_t i=0;i<node_values.size();++i)Put32(list,i*4,node_values[i]);values_offset=Cell(list);value_list=values_offset;}
    std::vector<std::uint8_t> nk(76+name.size());nk[0]='n';nk[1]='k';Put16(nk,2,0x20);Put32(nk,16,0xffffffff);Put32(nk,20,static_cast<std::uint32_t>(children.size()));Put32(nk,28,children_list);Put32(nk,36,static_cast<std::uint32_t>(node_values.size()));Put32(nk,40,values_offset);Put16(nk,72,static_cast<std::uint16_t>(name.size()));std::copy(name.begin(),name.end(),nk.begin()+76);
    const auto offset=Cell(nk);for(const auto child:children)Put32(bytes,4096+child+4+16,offset);return offset;
  }
  void Finish(std::string_view kind="li", bool missing=false, std::uint32_t sibling=0xffffffff) {
    game=Node(missing?"Other App":"Star Trek Fleet Command",{},kind,values);
    const auto company=Node("Digit Game Studios Ltd.",sibling==0xffffffff?std::vector<std::uint32_t>{game}:std::vector<std::uint32_t>{game,sibling},kind);
    const auto software=Node("Software",{company},kind);
    root=Node("ROOT",{software},kind);
    const auto extent=(cursor+4095)&~std::size_t(4095);bytes.resize(4096+extent);
    if(extent>cursor)Put32(bytes,4096+cursor,static_cast<std::uint32_t>(extent-cursor));
    bytes[0]='r';bytes[1]='e';bytes[2]='g';bytes[3]='f';Put32(bytes,4,7);Put32(bytes,8,7);Put32(bytes,20,1);Put32(bytes,24,6);Put32(bytes,32,1);Put32(bytes,36,root);Put32(bytes,40,static_cast<std::uint32_t>(extent));Put32(bytes,44,1);
    bytes[4096]='h';bytes[4097]='b';bytes[4098]='i';bytes[4099]='n';Put32(bytes,4104,static_cast<std::uint32_t>(extent));Checksum();
  }
  void Checksum(){std::uint32_t c=0;for(unsigned i=0;i<508;i+=4)c^=Get32(bytes,i);if(c==0xffffffff)c=0xfffffffe;if(!c)c=1;Put32(bytes,508,c);}
};
void AllListKindsAndRawTypes() {
  for(const auto kind:{"li","lf","lh","ri"}) {
    Fixture f;
    f.Value(NativeName(Hashed("foo")),REG_DWORD,{42,0,0,0},true);
    const std::vector<std::uint8_t> eight{1,2,3,4,5,6,7,8};
    f.Value(NativeName(Hashed("float")),REG_DWORD,eight);
    f.Value(NativeName(Hashed("unknown")),99,{0,255,0,1,3});
    f.Value(NativeName(Hashed("string")),REG_BINARY,{'S','Y','N','T','H',0});
    f.Value(NativeName(Hashed("ansi")),REG_SZ,WideBytes(std::u16string_view(u"SYNTH\0",6)));
    const auto metadata=f.Value(u"SyntheticMetadata",REG_BINARY,{});
    Put32(f.bytes,4096+metadata+4+4,0x7fffffff); // Unselected metadata is not interpreted.
    f.Finish(kind);
    const auto preferences=ReadRegistryHivePreferences(f.bytes);
    Check(preferences.size()==5,"wrong preference count");
    const auto find=[&](std::u16string_view key)->const NativePreference&{const auto it=std::find_if(preferences.begin(),preferences.end(),[&](const auto& p){return p.key==key;});Check(it!=preferences.end(),"fixture key missing");return *it;};
    Check(find(u"foo").type==REG_DWORD && find(u"foo").bytes==std::vector<std::uint8_t>({42,0,0,0}),"inline integer bytes changed");
    Check(find(u"float").type==REG_DWORD && find(u"float").bytes==eight,"eight-byte REG_DWORD float truncated");
    Check(find(u"unknown").type==99 && find(u"unknown").bytes==std::vector<std::uint8_t>({0,255,0,1,3}),"unknown native representation changed");
    Check(find(u"ansi").bytes==std::vector<std::uint8_t>({'S','Y','N','T','H',0}),"REG_SZ did not match Unity ANSI API");
  }
}
void UnicodeAndHash() {
  Check(UnityPreferenceKey(u"foo_h193410979")==u"foo","known Unity hash changed");
  Check(UnityPreferenceKey(u"_h5381").empty(),"empty Unity key was rejected");
  Fixture empty;empty.Value(u"_h5381",REG_BINARY,{1,2});empty.Finish();
  const auto empty_values=ReadRegistryHivePreferences(empty.bytes);Check(empty_values.size()==1&&empty_values[0].key.empty(),"empty hive key rejected");
  const auto unicode=NativeName("caf\xc3\xa9_h176734187");
  Check(UnityPreferenceKey(unicode)==u"caf\u00e9","signed UTF-8 hash or ANSI name conversion wrong");
  Fixture f;f.Value(unicode,REG_BINARY,{0,255});f.Finish();
  const auto values=ReadRegistryHivePreferences(f.bytes);Check(values.size()==1&&values[0].key==u"caf\u00e9","offline Unicode VK name changed");
  Reject([]{UnityPreferenceKey(u"foo_h193410978");});
  Reject([]{UnityPreferenceKey(u"foo_h0193410979");});
  Reject([]{UnityPreferenceKey(u"foo_h42949672960");});
  Reject([]{UnityPreferenceKey(std::u16string{u'x',char16_t(0xd800),u'_',u'h',u'1'});});
}
void BigDataAndBounds() {
  Fixture f;std::vector<std::uint8_t> data(20000);for(std::size_t i=0;i<data.size();++i)data[i]=static_cast<std::uint8_t>(i*7);
  const auto value=f.Value(NativeName(Hashed("big")),REG_BINARY,data,false,true);f.Finish();
  const auto values=ReadRegistryHivePreferences(f.bytes);Check(values.size()==1&&values[0].bytes==data,"segmented db data changed");
  const auto db=Get32(f.bytes,4096+value+4+8), list=Get32(f.bytes,4096+db+4+4);
  auto bad=f.bytes;Put32(bad,4096+list+4+4,Get32(bad,4096+list+4));Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;Put32(bad,4096+value+4+8,db+8);Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;Put32(bad,4096+value+4+4,16u*1024u*1024u+1);Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;Put32(bad,4096+db+4+4,0xffffffff);Reject([&]{ReadRegistryHivePreferences(bad);});
}
std::wstring NativeRegistryName(HKEY key) {
  using Query = LONG (NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
  const auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryKey"));
  Check(query!=nullptr,"native fixture registry query unavailable");
  ULONG size=0;query(key,3,nullptr,0,&size);Check(size>=4&&size<=65536,"native fixture registry name unavailable");
  std::vector<std::uint8_t> data(size);
  Check(query(key,3,data.data(),size,&size)>=0,"native fixture registry name query failed");
  const auto length=Get32(data,0);Check(length%2==0&&length<=data.size()-4,"native fixture registry name invalid");
  return {reinterpret_cast<const wchar_t*>(data.data()+4),length/2};
}
struct NativeRegistryFixture {
  HKEY root=nullptr;
  std::wstring path;
  std::vector<std::wstring> links;
  NativeRegistryFixture() {
    path=L"Software\\STFCProfilesSynthetic-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    DWORD disposition=0;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&root,&disposition)==ERROR_SUCCESS,"neutral registry fixture unavailable");
    if(disposition!=REG_CREATED_NEW_KEY){RegCloseKey(root);root=nullptr;throw std::runtime_error("neutral fixture namespace collision");}
  }
  void Cleanup() {
    if(!root)return;
    using Delete = LONG (NTAPI*)(HANDLE);
    const auto remove=reinterpret_cast<Delete>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtDeleteKey"));
    Check(remove!=nullptr,"native fixture cleanup unavailable");
    for(const auto& name:links) {
      HKEY link=nullptr;
      const auto status=RegOpenKeyExW(root,name.c_str(),REG_OPTION_OPEN_LINK,DELETE,&link);
      if(status==ERROR_FILE_NOT_FOUND)continue;
      Check(status==ERROR_SUCCESS,"neutral fixture link cleanup open failed");
      const auto result=remove(link);RegCloseKey(link);Check(result>=0,"neutral fixture link cleanup failed");
    }
    links.clear();
    Check(RegDeleteTreeW(root,nullptr)==ERROR_SUCCESS,"neutral fixture child cleanup failed");
    RegCloseKey(root);root=nullptr;
    Check(RegDeleteKeyW(HKEY_CURRENT_USER,path.c_str())==ERROR_SUCCESS,"neutral fixture root cleanup failed");
  }
  ~NativeRegistryFixture(){try{Cleanup();}catch(...){}}
};
void InvalidUserSelection() {
  for(const auto* sid:{"", "NOT-A-WINDOWS-SID", "S-1-5-18", "S-1-5-21-1-2-3-4.bak", "S-1-5-21-01-2-3-4"})
    Reject([&]{ResolveImportUser(sid);},"source_user_missing");
}
void LoadedRegistryLinks() {
  NativeRegistryFixture fixture;
  HKEY target=nullptr;DWORD disposition=0;
  Check(RegCreateKeyExW(fixture.root,L"SyntheticTarget",0,nullptr,0,KEY_ALL_ACCESS,nullptr,&target,&disposition)==ERROR_SUCCESS,"neutral target creation failed");
  const DWORD sentinel=314;
  const auto written=RegSetValueExW(target,L"SyntheticSentinel",0,REG_DWORD,reinterpret_cast<const BYTE*>(&sentinel),sizeof(sentinel));
  const auto destination=NativeRegistryName(target);RegCloseKey(target);Check(written==ERROR_SUCCESS,"neutral target setup failed");
  HKEY ordinary=nullptr;
  Check(detail::OpenImportRegistryComponent(fixture.root,L"SyntheticTarget",&ordinary)==ERROR_SUCCESS,"ordinary selected component was rejected");RegCloseKey(ordinary);
  for(const auto* component:{L"SelectedAccount",L"SelectedSoftware",L"SelectedCompany",L"SelectedGame"}) {
    HKEY link=nullptr;
    Check(RegCreateKeyExW(fixture.root,component,0,nullptr,REG_OPTION_CREATE_LINK,KEY_ALL_ACCESS,nullptr,&link,&disposition)==ERROR_SUCCESS,"neutral registry link creation failed");
    fixture.links.emplace_back(component);
    const auto assigned=RegSetValueExW(link,L"SymbolicLinkValue",0,REG_LINK,reinterpret_cast<const BYTE*>(destination.data()),static_cast<DWORD>(destination.size()*sizeof(wchar_t)));
    RegCloseKey(link);Check(assigned==ERROR_SUCCESS,"neutral registry link target setup failed");
    HKEY followed=nullptr;
    Check(RegOpenKeyExW(fixture.root,component,0,KEY_QUERY_VALUE,&followed)==ERROR_SUCCESS,"neutral link did not resolve");
    DWORD value=0,type=0,size=sizeof(value);
    const auto read=RegQueryValueExW(followed,L"SyntheticSentinel",nullptr,&type,reinterpret_cast<BYTE*>(&value),&size);RegCloseKey(followed);
    Check(read==ERROR_SUCCESS&&type==REG_DWORD&&value==sentinel,"fixture did not establish link following");
    HKEY selected=nullptr;
    Reject([&]{detail::OpenImportRegistryComponent(fixture.root,component,&selected);});
    Check(selected==nullptr,"rejected link retained an open handle");
  }
  fixture.Cleanup();
}
void RejectDirtyAndMalformed() {
  Fixture f;f.Value(NativeName(Hashed("foo")),REG_DWORD,{1,0,0,0},true);f.Finish("ri");
  auto bad=f.bytes;Put32(bad,8,8);Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;Put16(bad,4096+f.game+4+2,0x30);Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;bad[508]^=1;Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;Put32(bad,4100,4096);Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;Put32(bad,4096+f.root,0xfffffff0);Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;Put32(bad,4096+f.value_list+4,f.values[0]+8);Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;Put32(bad,4096+f.game+4+36,10001);Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;const auto ri=Get32(bad,4096+f.root+4+28);Put32(bad,4096+ri+4+4,ri);Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;Put32(bad,4096+f.values[0]+4+4,0x80000008);Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;Put32(bad,4096+f.values[0],16);Reject([&]{ReadRegistryHivePreferences(bad);});
  bad=f.bytes;bad.resize(bad.size()-1);Reject([&]{ReadRegistryHivePreferences(bad);});
  Fixture missing;missing.Finish("li",true);Reject([&]{ReadRegistryHivePreferences(missing.bytes);},"source_missing");
  Fixture unrelated;unrelated.Value(NativeName(Hashed("foo")),REG_DWORD,{1,0,0,0},true);
  const auto sibling=unrelated.Node("UnrelatedLink",{},"li");Put16(unrelated.bytes,4096+sibling+4+2,0x30);unrelated.Finish("li",false,sibling);
  Check(ReadRegistryHivePreferences(unrelated.bytes).size()==1,"unrelated sibling link was followed or rejected");
  Fixture duplicate;duplicate.Value(NativeName(Hashed("foo")),REG_DWORD,{1,0,0,0},true);duplicate.values.push_back(duplicate.values[0]);duplicate.Finish();Reject([&]{ReadRegistryHivePreferences(duplicate.bytes);});
}
}
int main(){try{AllListKindsAndRawTypes();UnicodeAndHash();BigDataAndBounds();RejectDirtyAndMalformed();InvalidUserSelection();LoadedRegistryLinks();std::cout<<"PASS: synthetic Windows import source fixtures\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
