// macOS user-Keychain protection and durable encrypted storage. GPL-3.0.
#if __APPLE__
#include "../prefs_crypto.h"
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace stfc::profiles::detail {
namespace {
template<typename T> class CFHandle {
public:
  explicit CFHandle(T value = nullptr) : value_(value) {}
  ~CFHandle() { if (value_) CFRelease(value_); }
  CFHandle(const CFHandle&) = delete;
  CFHandle& operator=(const CFHandle&) = delete;
  T Get() const { return value_; }
private: T value_;
};
[[noreturn]] void Failure(const char* reason) { throw std::runtime_error(reason); }
void Set(CFMutableDictionaryRef dict, const void* key, const void* value)
{ CFDictionarySetValue(dict,key,value); }
CFMutableDictionaryRef Dictionary()
{ return CFDictionaryCreateMutable(nullptr,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks); }
SecKeyRef ProfileKey(std::string_view id, bool allow_create)
{
  const auto tag_string = std::string("org.stfc-profiles.prefs.v1.") + std::string(id);
  CFHandle<CFDataRef> tag(CFDataCreate(nullptr,reinterpret_cast<const UInt8*>(tag_string.data()),tag_string.size()));
  CFHandle<CFMutableDictionaryRef> query(Dictionary());
  Set(query.Get(),kSecClass,kSecClassKey);
  Set(query.Get(),kSecAttrKeyType,kSecAttrKeyTypeECSECPrimeRandom);
  Set(query.Get(),kSecAttrKeyClass,kSecAttrKeyClassPrivate);
  Set(query.Get(),kSecAttrApplicationTag,tag.Get());
  Set(query.Get(),kSecMatchLimit,kSecMatchLimitAll);
  Set(query.Get(),kSecReturnRef,kCFBooleanTrue);
  CFTypeRef result = nullptr;
  const auto status = SecItemCopyMatching(query.Get(),&result);
  if (status == errSecSuccess) {
    CFHandle<CFTypeRef> items(result);
    if (!result || CFGetTypeID(result) != CFArrayGetTypeID()
        || CFArrayGetCount(static_cast<CFArrayRef>(result)) != 1)
      Failure("profile Keychain key identity is ambiguous");
    const auto found=CFArrayGetValueAtIndex(static_cast<CFArrayRef>(result),0);
    if (!found || CFGetTypeID(found) != SecKeyGetTypeID())
      Failure("profile Keychain item is not a private key");
    return reinterpret_cast<SecKeyRef>(const_cast<void*>(CFRetain(found)));
  }
  if (result) CFRelease(result);
  if (status != errSecItemNotFound || !allow_create)
    Failure("profile Keychain key is unavailable; existing preferences cannot be replaced");
  CFHandle<CFMutableDictionaryRef> private_attributes(Dictionary());
  Set(private_attributes.Get(),kSecAttrIsPermanent,kCFBooleanTrue);
  Set(private_attributes.Get(),kSecAttrApplicationTag,tag.Get());
  Set(private_attributes.Get(),kSecAttrAccessible,kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
  CFHandle<CFMutableDictionaryRef> attributes(Dictionary());
  const int bits = 256;
  CFHandle<CFNumberRef> size(CFNumberCreate(nullptr,kCFNumberIntType,&bits));
  Set(attributes.Get(),kSecAttrKeyType,kSecAttrKeyTypeECSECPrimeRandom);
  Set(attributes.Get(),kSecAttrKeySizeInBits,size.Get());
  Set(attributes.Get(),kSecPrivateKeyAttrs,private_attributes.Get());
  CFErrorRef error = nullptr;
  auto key = SecKeyCreateRandomKey(attributes.Get(),&error);
  if (error) CFRelease(error);
  if (!key) Failure("could not create protected profile Keychain key");
  return key;
}
void SyncDirectory(const std::filesystem::path& directory)
{
  int handle = open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if (handle < 0) Failure("could not open preference directory for durability");
  const bool flushed = fsync(handle) == 0;
  const bool closed = close(handle) == 0;
  if (!flushed || !closed) Failure("could not flush preference directory");
}
void WriteNew(const std::filesystem::path& file, std::span<const std::uint8_t> bytes)
{
  int handle = open(file.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if (handle < 0) Failure("could not stage protected preferences");
  bool ok = true;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = write(handle,bytes.data()+offset,bytes.size()-offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ok = false; break; }
    offset += count;
  }
  if (ok) ok = fsync(handle) == 0 && fcntl(handle,F_FULLFSYNC) == 0;
  if (close(handle) != 0) ok = false;
  if (!ok) { unlink(file.c_str()); Failure("could not durably write protected preferences"); }
}
std::vector<std::uint8_t> Read(const std::filesystem::path& file)
{
  int handle = open(file.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
  if (handle < 0) Failure("protected preferences are unavailable");
  struct stat info{};
  if (fstat(handle,&info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0
      || info.st_size > static_cast<off_t>(MaxEncryptedPrefsBytes)) {
    close(handle); Failure("invalid protected preference file");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = read(handle,bytes.data()+offset,bytes.size()-offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { close(handle); Failure("could not read protected preferences"); }
    offset += count;
  }
  if (close(handle) != 0) Failure("could not close protected preferences");
  return bytes;
}
} // namespace

std::vector<std::uint8_t> ReadProtectedPrefs(const std::filesystem::path& file, std::string_view id)
{
  auto bytes = Read(file);
  CFHandle<SecKeyRef> key(ProfileKey(id,false));
  const auto algorithm = kSecKeyAlgorithmECIESEncryptionStandardX963SHA256AESGCM;
  if (!SecKeyIsAlgorithmSupported(key.Get(),kSecKeyOperationTypeDecrypt,algorithm))
    Failure("profile Keychain key does not support authenticated preference decryption");
  CFHandle<CFDataRef> input(CFDataCreate(nullptr,bytes.data(),bytes.size()));
  CFErrorRef error = nullptr;
  CFHandle<CFDataRef> plain(SecKeyCreateDecryptedData(key.Get(),algorithm,input.Get(),&error));
  if (error) CFRelease(error);
  if (!plain.Get() || CFDataGetLength(plain.Get()) > static_cast<CFIndex>(MaxPlainPrefsBytes))
    Failure("profile preference authentication failed");
  return {CFDataGetBytePtr(plain.Get()),CFDataGetBytePtr(plain.Get())+CFDataGetLength(plain.Get())};
}
void WriteProtectedPrefs(const std::filesystem::path& file, std::string_view id,
                         std::span<const std::uint8_t> plaintext, bool existing)
{
  if (plaintext.size() > MaxPlainPrefsBytes) Failure("profile preferences exceed storage limit");
  CFHandle<SecKeyRef> private_key(ProfileKey(id,!existing));
  CFHandle<SecKeyRef> public_key(SecKeyCopyPublicKey(private_key.Get()));
  const auto algorithm = kSecKeyAlgorithmECIESEncryptionStandardX963SHA256AESGCM;
  if (!public_key.Get() || !SecKeyIsAlgorithmSupported(public_key.Get(),kSecKeyOperationTypeEncrypt,algorithm))
    Failure("profile Keychain key does not support authenticated preference encryption");
  CFHandle<CFDataRef> input(CFDataCreate(nullptr,plaintext.data(),plaintext.size()));
  CFErrorRef error = nullptr;
  CFHandle<CFDataRef> encrypted(SecKeyCreateEncryptedData(public_key.Get(),algorithm,input.Get(),&error));
  if (error) CFRelease(error);
  if (!encrypted.Get() || CFDataGetLength(encrypted.Get()) <= 0
      || CFDataGetLength(encrypted.Get()) > static_cast<CFIndex>(MaxEncryptedPrefsBytes))
    Failure("could not protect profile preferences");
  static std::atomic_uint64_t sequence{0};
  auto temporary = file;
  temporary += ".tmp."+std::to_string(getpid())+"."
               +std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"."
               +std::to_string(++sequence);
  WriteNew(temporary,{CFDataGetBytePtr(encrypted.Get()),static_cast<std::size_t>(CFDataGetLength(encrypted.Get()))});
  auto backup = file; backup += ".bak";
  if (existing) {
    if (link(file.c_str(),backup.c_str()) != 0) Failure("could not retain previous protected preferences");
    SyncDirectory(file.parent_path());
    if (rename(temporary.c_str(),file.c_str()) != 0) Failure("could not replace protected preferences");
    SyncDirectory(file.parent_path());
    ErasePrefsFile(backup);
  } else {
    if (link(temporary.c_str(),file.c_str()) != 0) Failure("could not publish protected preferences");
    ErasePrefsFile(temporary);
  }
  SyncDirectory(file.parent_path());
}
void InitializePrefsMarker(const std::filesystem::path& marker)
{
  if (std::filesystem::is_regular_file(marker)) return;
  constexpr std::uint8_t bytes[]{'v','1','\n'};
  WriteNew(marker,bytes);
  SyncDirectory(marker.parent_path());
}
void RecoverPrefsBackup(const std::filesystem::path& backup, const std::filesystem::path& target)
{
  if (link(backup.c_str(),target.c_str()) != 0) Failure("could not recover protected preference backup");
  SyncDirectory(target.parent_path());
  ErasePrefsFile(backup);
}
void ErasePrefsFile(const std::filesystem::path& file)
{
  if (unlink(file.c_str()) != 0 && errno != ENOENT) Failure("could not remove preference recovery data");
  SyncDirectory(file.parent_path());
}
void EraseProtectedPrefsIdentity(std::string_view id)
{
  const auto text=std::string("org.stfc-profiles.prefs.v1.")+std::string(id);
  CFHandle<CFDataRef> tag(CFDataCreate(nullptr,reinterpret_cast<const UInt8*>(text.data()),text.size()));
  CFHandle<CFMutableDictionaryRef> query(Dictionary());
  Set(query.Get(),kSecClass,kSecClassKey);
  Set(query.Get(),kSecAttrKeyType,kSecAttrKeyTypeECSECPrimeRandom);
  Set(query.Get(),kSecAttrKeyClass,kSecAttrKeyClassPrivate);
  Set(query.Get(),kSecAttrApplicationTag,tag.Get());
  const auto status=SecItemDelete(query.Get());
  if (status!=errSecSuccess && status!=errSecItemNotFound)
    Failure("profile directory was deleted but its protected Keychain key could not be removed");
}
void WipePrefsBytes(std::span<std::uint8_t> bytes) noexcept
{ volatile std::uint8_t* out = bytes.data(); for (std::size_t i=0;i<bytes.size();++i) out[i]=0; }
} // namespace stfc::profiles::detail
#endif
