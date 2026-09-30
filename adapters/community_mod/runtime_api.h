#pragma once
// A small dynamically resolved IL2CPP boundary. It imports no mod features or
// game import library. Native MethodInfo's first pointer is the client ABI.
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#if _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace stfc::profiles::community_mod::detail {
struct RuntimeApi {
  void* (*domain_get)();
  const void** (*domain_get_assemblies)(const void*,std::size_t*);
  const void* (*assembly_get_image)(const void*);
  const char* (*image_get_name)(const void*);
  void* (*class_from_name)(const void*,const char*,const char*);
  const void* (*class_get_methods)(void*,void**);
  const char* (*method_get_name)(const void*);
  bool (*method_is_generic)(const void*);
  bool (*method_is_inflated)(const void*);
  std::uint32_t (*method_get_flags)(const void*,std::uint32_t*);
  std::uint32_t (*method_get_param_count)(const void*);
  const void* (*method_get_return_type)(const void*);
  const void* (*method_get_param)(const void*,std::uint32_t);
  bool (*type_is_byref)(const void*);
  char* (*type_get_name)(const void*);
  void (*free)(void*);
  std::int32_t (*string_length)(void*);
  char16_t* (*string_chars)(void*);
  void* (*string_new_utf16)(const char16_t*,std::int32_t);

  RuntimeApi() {
#if _WIN32
    auto module = GetModuleHandleW(L"GameAssembly.dll");
    if (!module) throw std::runtime_error("GameAssembly.dll is unavailable");
    const auto symbol = [module](const char* name) -> void* { return reinterpret_cast<void*>(GetProcAddress(module,name)); };
#else
    const auto symbol = [](const char* name) -> void* { return dlsym(RTLD_DEFAULT,name); };
#endif
#define BIND(name) name = reinterpret_cast<decltype(name)>(symbol("il2cpp_" #name)); if (!name) throw std::runtime_error("required il2cpp API is unavailable: " #name)
    BIND(domain_get); BIND(domain_get_assemblies); BIND(assembly_get_image); BIND(image_get_name);
    BIND(class_from_name); BIND(class_get_methods); BIND(method_get_name); BIND(method_is_generic);
    BIND(method_is_inflated); BIND(method_get_flags); BIND(method_get_param_count);
    BIND(method_get_return_type); BIND(method_get_param); BIND(type_is_byref); BIND(type_get_name);
    BIND(free); BIND(string_length); BIND(string_chars); BIND(string_new_utf16);
#undef BIND
  }
  void* Class(const char* assembly,const char* space,const char* name) const {
    std::size_t count=0;
    auto assemblies=domain_get_assemblies(domain_get(),&count);
    if (!assemblies || count>4096) throw std::runtime_error("invalid il2cpp assembly inventory");
    const auto dll_name=std::string(assembly)+".dll";
    void* found=nullptr;
    bool matched=false;
    for (std::size_t i=0;i<count;++i) {
      if (!assemblies[i]) throw std::runtime_error("invalid il2cpp assembly entry");
      const auto image=assembly_get_image(assemblies[i]);
      const auto actual=image ? image_get_name(image) : nullptr;
      if (!actual || (std::strcmp(actual,assembly)!=0 && actual!=dll_name)) continue;
      if (matched) throw std::runtime_error("ambiguous il2cpp assembly");
      matched=true;
      found=class_from_name(image,space,name);
    }
    return found;
  }
  bool Type(const void* type,const char* expected) const {
    if (!type || type_is_byref(type)) return false;
    auto actual=type_get_name(type);
    const bool matches=actual && std::strcmp(actual,expected)==0;
    free(actual);
    return matches;
  }
  void* Resolve(void* cls,const char* name,bool is_static,const char* result,
                std::initializer_list<const char*> args) const {
    if (!cls) return nullptr;
    const void* found=nullptr;
    void* iterator=nullptr;
    while (const auto method=class_get_methods(cls,&iterator)) {
      const auto actual=method_get_name(method);
      std::uint32_t implementation_flags=0;
      const auto flags=method_get_flags(method,&implementation_flags);
      if (!actual || std::strcmp(actual,name)!=0 || method_is_generic(method) || method_is_inflated(method)
          || bool(flags & 0x10)!=is_static || method_get_param_count(method)!=args.size()
          || !Type(method_get_return_type(method),result)) continue;
      bool matches=true;
      std::uint32_t index=0;
      for (const auto arg:args) matches=Type(method_get_param(method,index++),arg) && matches;
      if (!matches) continue;
      if (found) return nullptr;
      found=method;
    }
    if (!found) return nullptr;
    void* pointer=nullptr;
    std::memcpy(&pointer,found,sizeof(pointer));
    return pointer;
  }
};
} // namespace stfc::profiles::community_mod::detail
