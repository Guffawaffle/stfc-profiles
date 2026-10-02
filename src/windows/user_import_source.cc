// Read-only native Windows account sources; see LICENSE (GPL-3.0).
#if _WIN32
#include "stfc_profiles/user_import.h"
#include "stfc_profiles/catalog.h"
#include <Windows.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace stfc::profiles {
namespace detail {
// Internal read-only component verifier; the catalog exposes no handle/path entry.
LSTATUS OpenImportRegistryComponent(HKEY parent, const wchar_t* component, HKEY* opened);
}
namespace {
constexpr std::size_t MaxHive = 512u * 1024u * 1024u;
constexpr std::size_t MaxPreferences = 16u * 1024u * 1024u;
constexpr std::size_t MaxValues = 10000;
constexpr std::size_t MaxCells = 1000000;
struct WipeBytes {
  std::vector<std::uint8_t>& bytes;
  ~WipeBytes() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
};
constexpr wchar_t ProfileList[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList";
[[noreturn]] void Invalid() { throw CatalogError("source_invalid", "The source preferences are invalid, dirty, or unsupported. Sign out of that Windows account cleanly and try again."); }
[[noreturn]] void Missing() { throw CatalogError("source_missing", "This Windows account has no available Star Trek Fleet Command preferences."); }
[[noreturn]] void WinError(LSTATUS error) {
  if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)
    throw CatalogError("elevation_required", "Windows denied read access to this account's preferences. Approve elevation to import this account.");
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) Missing();
  if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_KEY_DELETED)
    throw CatalogError("source_unstable", "The account's preference hive is in use or changing. Finish signing in or sign out cleanly, then retry.");
  throw CatalogError("source_unavailable", "Windows could not read this account's preferences.");
}
struct RegKey {
  HKEY value = nullptr;
  ~RegKey() { if (value) RegCloseKey(value); }
  RegKey() = default;
  RegKey(const RegKey&) = delete;
};
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle() = default;
  Handle(const Handle&) = delete;
};
std::string Utf8(std::wstring_view value) {
  if (value.empty()) return {};
  const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (!size) Invalid();
  std::string result(size, '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size) Invalid();
  return result;
}
std::wstring Wide(std::string_view value) {
  if (value.empty() || value.size() > MaxPreferences) Invalid();
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!size) Invalid();
  std::wstring result(size, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) != size) Invalid();
  return result;
}
std::string UnityAnsiName(std::u16string_view name) {
  if (name.empty()) return {};
  // The current Unity player calls Reg* A with UTF-8 bytes and has no UTF-8 ACP
  // manifest. Registry W names therefore hold ACP-decoded UTF-8, not the Unity
  // key itself. Require a reversible conversion, never best-fit replacement.
  const auto codepage = GetACP();
  const auto flags = codepage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
  BOOL substituted = FALSE;
  auto* used = codepage == CP_UTF8 ? nullptr : &substituted;
  const auto* wide = reinterpret_cast<const wchar_t*>(name.data());
  const auto size = WideCharToMultiByte(codepage, flags, wide, static_cast<int>(name.size()), nullptr, 0, nullptr, used);
  if (!size || substituted) Invalid();
  std::string bytes(size, '\0');
  if (WideCharToMultiByte(codepage, flags, wide, static_cast<int>(name.size()), bytes.data(), size, nullptr, used) != size || substituted) Invalid();
  const auto count = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, bytes.data(), size, nullptr, 0);
  if (count != name.size()) Invalid();
  std::wstring roundtrip(count, L'\0');
  if (MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, bytes.data(), size, roundtrip.data(), count) != count || !std::equal(name.begin(), name.end(), roundtrip.begin())) Invalid();
  return bytes;
}
void ValidateName(std::u16string_view value) {
  if (value.size() > 16383) Invalid();
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto c = value[i];
    if (!c) Invalid();
    if (c >= 0xd800 && c <= 0xdbff) {
      if (++i >= value.size() || value[i] < 0xdc00 || value[i] > 0xdfff) Invalid();
    } else if (c >= 0xdc00 && c <= 0xdfff) Invalid();
  }
}
bool OrdinarySid(const std::wstring& name) {
  PSID sid = nullptr;
  if (!ConvertStringSidToSidW(name.c_str(), &sid)) return false;
  const auto cleanup = [&] { LocalFree(sid); };
  const auto count = *GetSidSubAuthorityCount(sid);
  const auto* authority = GetSidIdentifierAuthority(sid);
  const bool domain = count == 5 && authority->Value[5] == 5 && *GetSidSubAuthority(sid, 0) == 21;
  const bool azure = count == 5 && authority->Value[5] == 12 && *GetSidSubAuthority(sid, 0) == 1;
  bool canonical = false;
  LPWSTR text = nullptr;
  if ((domain || azure) && ConvertSidToStringSidW(sid, &text)) { canonical = name == text; LocalFree(text); }
  cleanup();
  return canonical;
}
std::wstring RegistryString(HKEY key, const wchar_t* name) {
  DWORD size = 0, type = 0;
  auto error = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
  if (error != ERROR_SUCCESS) WinError(error);
  if ((type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t) || size > 65536 || size % sizeof(wchar_t)) Invalid();
  std::vector<wchar_t> bytes(size / sizeof(wchar_t));
  const auto expected_type = type;
  error = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(bytes.data()), &size);
  if (error != ERROR_SUCCESS) WinError(error);
  if (type != expected_type || !size || size % sizeof(wchar_t) || size > bytes.size() * sizeof(wchar_t)) Invalid();
  const auto end = size / sizeof(wchar_t);
  if (bytes[end - 1] || std::find(bytes.begin(), bytes.begin() + end - 1, L'\0') != bytes.begin() + end - 1) Invalid();
  std::wstring result(bytes.data(), end - 1);
  if (type == REG_EXPAND_SZ) {
    const auto count = ExpandEnvironmentStringsW(result.c_str(), nullptr, 0);
    if (!count || count > 32768) Invalid();
    std::wstring expanded(count, L'\0');
    if (ExpandEnvironmentStringsW(result.c_str(), expanded.data(), count) != count) Invalid();
    expanded.pop_back(); result = std::move(expanded);
  }
  return result;
}
std::string UserName(std::wstring_view sid_text, const std::filesystem::path& directory) {
  PSID sid = nullptr;
  if (!ConvertStringSidToSidW(std::wstring(sid_text).c_str(), &sid)) Invalid();
  DWORD name_size = 0, domain_size = 0; SID_NAME_USE use{};
  LookupAccountSidW(nullptr, sid, nullptr, &name_size, nullptr, &domain_size, &use);
  std::wstring name(name_size, L'\0'), domain(domain_size, L'\0');
  bool found = name_size && LookupAccountSidW(nullptr, sid, name.data(), &name_size, domain.data(), &domain_size, &use);
  LocalFree(sid);
  if (found) { name.resize(name_size); domain.resize(domain_size); return Utf8(domain.empty() ? name : domain + L"\\" + name); }
  return Utf8(directory.filename().wstring());
}
void Sort(std::vector<NativePreference>& values) {
  std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
  for (std::size_t i = 1; i < values.size(); ++i) if (values[i-1].key == values[i].key) Invalid();
}
void Append(std::vector<NativePreference>& values, std::size_t& total, std::u16string name, DWORD type, std::vector<std::uint8_t> bytes) {
  ValidateName(name);
  if (name.rfind(u"_h") == std::u16string::npos) return; // Non-Unity metadata is outside the import contract.
  auto key = UnityPreferenceKey(name);
  // RegEnumValueA/RegQueryValueExA present string types as ACP bytes to Unity.
  // Binary and numeric payloads (including 8-byte REG_DWORD floats) stay exact.
  if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ) {
    if (bytes.size() % 2) Invalid();
    std::u16string wide;
    struct WipeText { std::u16string& text; ~WipeText() { if (!text.empty()) SecureZeroMemory(text.data(), text.size()*sizeof(char16_t)); } } wipe_text{wide};
    wide.reserve(bytes.size()/2);
    for (std::size_t i = 0; i < bytes.size(); i += 2) wide.push_back(bytes[i] | (std::uint16_t(bytes[i+1]) << 8));
    if (!wide.empty()) {
      auto converted = UnityAnsiName(wide);
      struct WipeString { std::string& text; ~WipeString() { if (!text.empty()) SecureZeroMemory(text.data(), text.size()); } } wipe_string{converted};
      SecureZeroMemory(bytes.data(), bytes.size());
      bytes.assign(converted.begin(), converted.end());
      SecureZeroMemory(wide.data(), wide.size()*sizeof(char16_t));
    }
  }
  const auto amount = bytes.size() + key.size() * sizeof(char16_t);
  if (amount > MaxPreferences || total > MaxPreferences - amount || values.size() >= MaxValues) Invalid();
  total += amount; values.push_back({std::move(key), type, std::move(bytes)});
}
struct RegistrySnapshot { FILETIME written{}; DWORD count = 0; std::vector<NativePreference> values; bool stable = false; };
RegistrySnapshot ReadRegistry(HKEY key) {
  RegistrySnapshot result;
  DWORD max_name = 0, max_data = 0;
  auto error = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &result.count, &max_name, &max_data, nullptr, &result.written);
  if (error != ERROR_SUCCESS) WinError(error);
  if (result.count > MaxValues || max_name > 16383 || max_data > MaxPreferences) Invalid();
  std::vector<wchar_t> name(max_name + 2);
  std::vector<std::uint8_t> data(max_data);
  WipeBytes wipe{data};
  std::size_t total = 0;
  for (DWORD index = 0; index < result.count; ++index) {
    DWORD name_size = static_cast<DWORD>(name.size()), data_size = static_cast<DWORD>(data.size()), type = 0;
    error = RegEnumValueW(key, index, name.data(), &name_size, nullptr, &type, data.empty() ? nullptr : data.data(), &data_size);
    if (error == ERROR_MORE_DATA || error == ERROR_NO_MORE_ITEMS) return result;
    if (error != ERROR_SUCCESS) WinError(error);
    if (name_size > max_name || data_size > data.size()) return result;
    Append(result.values, total, {reinterpret_cast<const char16_t*>(name.data()), name_size}, type, {data.begin(), data.begin() + data_size});
  }
  DWORD count = 0; FILETIME after{};
  error = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, nullptr, nullptr, nullptr, &after);
  if (error != ERROR_SUCCESS) WinError(error);
  Sort(result.values);
  result.stable = count == result.count && CompareFileTime(&after, &result.written) == 0;
  return result;
}
std::vector<NativePreference> StableRegistry(HKEY key) {
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    auto first = ReadRegistry(key), second = ReadRegistry(key);
    if (first.stable && second.stable && first.count == second.count && CompareFileTime(&first.written, &second.written) == 0 && first.values == second.values) return std::move(second.values);
  }
  throw CatalogError("source_unstable", "The source preferences changed during capture. Close the game and retry.");
}
bool Loaded(const ImportUser& user, RegKey& key) {
  RegKey account, software, company;
  const auto sid = Wide(user.sid);
  auto error = detail::OpenImportRegistryComponent(HKEY_USERS, sid.c_str(), &account.value);
  if (error == ERROR_FILE_NOT_FOUND) return false;
  if (error != ERROR_SUCCESS) WinError(error);
  // Each name is one component. Opening a multi-component path would let a
  // link in an intermediate component redirect the read before verification.
  // QUERY_VALUE on these selected ancestors is needed only to reject REG_LINK;
  // no unrelated user keys or values are enumerated.
  error = detail::OpenImportRegistryComponent(account.value, L"Software", &software.value);
  if (error != ERROR_SUCCESS) WinError(error);
  error = detail::OpenImportRegistryComponent(software.value, L"Digit Game Studios Ltd.", &company.value);
  if (error != ERROR_SUCCESS) WinError(error);
  error = detail::OpenImportRegistryComponent(company.value, L"Star Trek Fleet Command", &key.value);
  if (error != ERROR_SUCCESS) WinError(error);
  return true;
}
void OpenHive(const ImportUser& user, Handle& file) {
  // Resolve filesystem identity only for this selected source. A protected
  // unrelated profile must never prevent current-user discovery or capture.
  std::error_code ec;
  const auto directory = std::filesystem::weakly_canonical(user.directory, ec);
  if (ec) { if (ec.value() == ERROR_ACCESS_DENIED) WinError(ERROR_ACCESS_DENIED); Invalid(); }
  file.value = CreateFileW((directory / L"NTUSER.DAT").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file.value == INVALID_HANDLE_VALUE) WinError(GetLastError());
}
std::vector<std::uint8_t> CaptureHive(HANDLE file) {
  BY_HANDLE_FILE_INFORMATION before{}, after{};
  if (!GetFileInformationByHandle(file, &before)) WinError(GetLastError());
  const auto size = (std::uint64_t(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
  if ((before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || size < 8192 || size > MaxHive) Invalid();
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  WipeBytes wipe{bytes};
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD read = 0;
    if (!ReadFile(file, bytes.data() + offset, static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024)), &read, nullptr)) WinError(GetLastError());
    if (!read) Invalid();
    offset += read;
  }
  if (!GetFileInformationByHandle(file, &after)) WinError(GetLastError());
  if (before.nFileSizeLow != after.nFileSizeLow || before.nFileSizeHigh != after.nFileSizeHigh || CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0)
    throw CatalogError("source_unstable", "The account's preference hive changed while it was being read. Retry after a clean sign-out.");
  auto result = std::move(bytes); bytes.clear();
  return result; // FILE_SHARE_READ rejects concurrent writable/deleting handles; no loading/recovery/copy.
}
class Hive {
  std::span<const std::uint8_t> bytes;
  struct Cell { std::size_t at, size; };
  std::unordered_map<std::uint32_t, Cell> cells;
  std::uint32_t minor = 0;
  void Bounds(std::size_t at, std::size_t count) const { if (at > bytes.size() || count > bytes.size() - at) Invalid(); }
  std::uint16_t U16(std::size_t at) const { Bounds(at, 2); return bytes[at] | (std::uint16_t(bytes[at+1]) << 8); }
  std::uint32_t U32(std::size_t at) const { Bounds(at, 4); return bytes[at] | (std::uint32_t(bytes[at+1]) << 8) | (std::uint32_t(bytes[at+2]) << 16) | (std::uint32_t(bytes[at+3]) << 24); }
  Cell Get(std::uint32_t offset, std::size_t minimum = 0) const { const auto found = cells.find(offset); if (found == cells.end() || found->second.size < minimum) Invalid(); return found->second; }
  bool Signature(Cell c, char a, char b) const { return c.size >= 2 && bytes[c.at] == a && bytes[c.at+1] == b; }
  std::u16string Name(Cell cell, std::size_t at, std::size_t size, bool compressed) const {
    if (at > cell.size || size > cell.size - at || (!compressed && size % 2)) Invalid();
    std::u16string value;
    for (std::size_t i = 0; i < size; i += compressed ? 1 : 2) value.push_back(compressed ? bytes[cell.at+at+i] : U16(cell.at+at+i));
    ValidateName(value); return value;
  }
  Cell Node(std::uint32_t offset, bool selected = true) const { auto c = Get(offset, 76); if (!Signature(c, 'n', 'k') || (selected && (U16(c.at+2) & (0x1 | 0x2 | 0x10 | 0x40)))) Invalid(); return c; }
  void Children(std::uint32_t offset, std::vector<std::uint32_t>& result, std::unordered_set<std::uint32_t>& seen, unsigned depth = 0) const {
    if (depth > 64 || !seen.insert(offset).second) Invalid();
    const auto c = Get(offset, 4); const auto count = U16(c.at+2);
    const bool indirect = Signature(c, 'r', 'i');
    const bool leaf = Signature(c, 'l', 'i');
    const bool hash = Signature(c, 'l', 'f') || Signature(c, 'l', 'h');
    if (!indirect && !leaf && !hash) Invalid();
    const std::size_t stride = hash ? 8 : 4;
    if (count > MaxValues || 4 + count * stride > c.size) Invalid();
    for (unsigned i = 0; i < count; ++i) {
      const auto child = U32(c.at + 4 + i * stride);
      if (indirect) Children(child, result, seen, depth + 1);
      else { if (result.size() >= MaxValues) Invalid(); Node(child, false); result.push_back(child); }
    }
  }
  std::uint32_t Child(std::uint32_t parent, std::u16string_view wanted) const {
    const auto c = Node(parent); const auto count = U32(c.at+20);
    if (count > MaxValues || U32(c.at+24) != 0) Invalid(); // A disk source must not borrow volatile nodes.
    if (!count) Missing();
    std::vector<std::uint32_t> children; std::unordered_set<std::uint32_t> seen;
    Children(U32(c.at+28), children, seen);
    if (children.size() != count) Invalid();
    std::unordered_set<std::uint32_t> distinct;
    std::optional<std::uint32_t> match;
    for (const auto offset : children) {
      if (offset == parent || !distinct.insert(offset).second) Invalid();
      const auto node = Node(offset, false);
      if (U32(node.at+16) != parent) Invalid();
      const auto name = Name(node, 76, U16(node.at+72), U16(node.at+2) & 0x20);
      const auto same = name.size() == wanted.size() && std::equal(name.begin(), name.end(), wanted.begin(), [](char16_t a, char16_t b) { if (a >= u'A' && a <= u'Z') a += 32; if (b >= u'A' && b <= u'Z') b += 32; return a == b; });
      if (same) { if (match) Invalid(); match = offset; }
    }
    if (!match) Missing(); Node(*match); return *match;
  }
  std::vector<std::uint8_t> Data(Cell value) const {
    const auto encoded = U32(value.at+4), size = encoded & 0x7fffffff;
    if (size > MaxPreferences) Invalid();
    if (encoded & 0x80000000) { if (size > 4) Invalid(); return {bytes.begin()+value.at+8, bytes.begin()+value.at+8+size}; }
    if (!size) return {};
    const auto c = Get(U32(value.at+8));
    if (size <= 16344 || minor == 3) { if (size > c.size) Invalid(); return {bytes.begin()+c.at, bytes.begin()+c.at+size}; }
    if (!Signature(c, 'd', 'b') || c.size < 8) Invalid();
    const auto count = U16(c.at+2); const auto expected = (size + 16343) / 16344;
    if (count != expected) Invalid();
    const auto list_offset = U32(c.at+4); const auto list = Get(list_offset, std::size_t(count)*4);
    std::unordered_set<std::uint32_t> seen{U32(value.at+8), list_offset};
    std::vector<std::uint8_t> result; result.reserve(size);
    for (unsigned i = 0; i < count; ++i) {
      const auto offset = U32(list.at+i*4);
      if (!seen.insert(offset).second) Invalid();
      const auto segment = Get(offset);
      const auto take = std::min<std::size_t>(16344, size-result.size());
      if (take > segment.size) Invalid();
      result.insert(result.end(), bytes.begin()+segment.at, bytes.begin()+segment.at+take);
    }
    return result;
  }
public:
  explicit Hive(const std::vector<std::uint8_t>& source) : bytes(source) {
    if (bytes.size() < 8192 || bytes.size() > MaxHive || bytes[0] != 'r' || bytes[1] != 'e' || bytes[2] != 'g' || bytes[3] != 'f') Invalid();
    minor = U32(24);
    if (U32(4) != U32(8) || U32(20) != 1 || minor < 3 || minor > 6 || U32(28) != 0 || U32(32) != 1 || (U32(144) & 1)) Invalid();
    std::uint32_t checksum = 0;
    for (unsigned at = 0; at < 508; at += 4) checksum ^= U32(at);
    if (checksum == 0xffffffff) checksum = 0xfffffffe; if (!checksum) checksum = 1;
    if (U32(508) != checksum) Invalid();
    const auto extent = U32(40);
    if (!extent || extent % 4096 || extent > bytes.size()-4096) Invalid();
    std::size_t bin = 4096, end = bin + extent;
    std::size_t count = 0;
    while (bin < end) {
      Bounds(bin, 32);
      if (bytes[bin] != 'h' || bytes[bin+1] != 'b' || bytes[bin+2] != 'i' || bytes[bin+3] != 'n' || U32(bin+4) != bin-4096) Invalid();
      const auto size = U32(bin+8);
      if (size < 4096 || size % 4096 || size > end-bin) Invalid();
      const auto limit = bin+size;
      for (auto at = bin+32; at < limit;) {
        if (limit-at < 4 || ++count > MaxCells) Invalid();
        const auto encoded = U32(at);
        if (!encoded || encoded == 0x80000000) Invalid();
        const bool allocated = encoded & 0x80000000;
        const auto cell_size = allocated ? (0u-encoded) : encoded;
        if (cell_size < 8 || cell_size % 8 || cell_size > limit-at) Invalid();
        if (allocated) cells.emplace(static_cast<std::uint32_t>(at-4096), Cell{at+4, cell_size-4});
        at += cell_size;
      }
      bin = limit;
    }
    Node(U32(36));
  }
  std::uint32_t PreferenceNode() const {
    auto key = U32(36);
    std::unordered_set<std::uint32_t> ancestry{key};
    for (const auto part : {u"Software", u"Digit Game Studios Ltd.", u"Star Trek Fleet Command"}) {
      key = Child(key, part); if (!ancestry.insert(key).second) Invalid();
    }
    Node(key); return key;
  }
  bool HasPreferences() const { const auto count = U32(Node(PreferenceNode()).at+36); if (count > MaxValues) Invalid(); return count != 0; }
  std::vector<NativePreference> Preferences() const {
    const auto node = Node(PreferenceNode()); const auto count = U32(node.at+36);
    if (count > MaxValues) Invalid();
    std::vector<NativePreference> result; std::size_t total = 0;
    if (!count) return result;
    const auto list = Get(U32(node.at+40), std::size_t(count)*4);
    std::unordered_set<std::uint32_t> seen;
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto offset = U32(list.at+i*4);
      if (!seen.insert(offset).second) Invalid();
      const auto value = Get(offset, 20);
      if (!Signature(value, 'v', 'k') || (U16(value.at+16) & ~1u)) Invalid();
      const auto name = Name(value, 20, U16(value.at+2), U16(value.at+16) & 1);
      if (name.rfind(u"_h") != std::u16string::npos) Append(result, total, name, U32(value.at+12), Data(value));
    }
    Sort(result); return result;
  }
};
} // namespace
namespace detail {
LSTATUS OpenImportRegistryComponent(HKEY parent, const wchar_t* component, HKEY* opened) {
  *opened = nullptr;
  const auto status = RegOpenKeyExW(parent, component, REG_OPTION_OPEN_LINK, KEY_QUERY_VALUE, opened);
  if (status != ERROR_SUCCESS) return status;
  DWORD type = 0, size = 0;
  const auto query = RegQueryValueExW(*opened, L"SymbolicLinkValue", nullptr, &type, nullptr, &size);
  if (query != ERROR_SUCCESS && query != ERROR_FILE_NOT_FOUND) {
    RegCloseKey(*opened); *opened = nullptr; return query;
  }
  if (query == ERROR_SUCCESS && type == REG_LINK) {
    RegCloseKey(*opened); *opened = nullptr;
    throw CatalogError("source_invalid", "The selected account preference path contains a registry link. Import requires a direct account preference location.");
  }
  return ERROR_SUCCESS;
}
} // namespace detail
std::u16string UnityPreferenceKey(std::u16string_view name) {
  ValidateName(name);
  const auto split = name.rfind(u"_h");
  if (split == std::u16string_view::npos || split+2 == name.size()) Invalid();
  std::uint64_t suffix = 0;
  for (const auto c : name.substr(split+2)) {
    if (c < u'0' || c > u'9' || suffix > (std::numeric_limits<std::uint32_t>::max() - (c-u'0'))/10) Invalid();
    suffix = suffix*10 + (c-u'0');
  }
  if (name[split+2] == u'0' && split+3 != name.size()) Invalid();
  const auto native_bytes = UnityAnsiName(name.substr(0, split));
  const auto decoded = native_bytes.empty() ? std::wstring{} : Wide(native_bytes); // Strict UTF-8, as supplied to Unity.
  std::uint32_t hash = 5381;
  for (const auto byte : native_bytes) hash = (hash*33u) ^ static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(static_cast<unsigned char>(byte))));
  if (hash != suffix) Invalid();
  return {reinterpret_cast<const char16_t*>(decoded.data()), decoded.size()};
}
std::string CurrentUserSid() {
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) WinError(GetLastError());
  DWORD size = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  if (!size || size > 65536) WinError(GetLastError());
  std::vector<std::uint8_t> data(size);
  if (!GetTokenInformation(token.value, TokenUser, data.data(), size, &size)) WinError(GetLastError());
  LPWSTR text = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &text)) WinError(GetLastError());
  auto result = Utf8(text); LocalFree(text); return result;
}
void ValidateImportUserSid(std::string_view sid) {
  if (sid.empty() || sid.size() > 200 || sid.front() != 'S'
      || std::any_of(sid.begin()+1, sid.end(), [](char c) { return (c < '0' || c > '9') && c != '-'; })
      || !OrdinarySid(std::wstring(sid.begin(), sid.end())))
    throw CatalogError("source_user_missing", "The selected Windows account is no longer available.");
}
ImportUser CurrentImportUser() {
  const auto sid = CurrentUserSid();
  return {sid, UserName(Wide(sid), std::filesystem::path(Wide(sid))), {}, true};
}
namespace {
ImportUser ReadImportUser(HKEY profiles, const std::wstring& sid, const std::string& current) {
  RegKey profile;
  const auto opened = RegOpenKeyExW(profiles, sid.c_str(), 0, KEY_QUERY_VALUE, &profile.value);
  if (opened != ERROR_SUCCESS) WinError(opened);
  const std::filesystem::path supplied(RegistryString(profile.value, L"ProfileImagePath"));
  if (!supplied.is_absolute() || supplied.native().rfind(L"\\\\", 0) == 0) Invalid();
  // OS-owned metadata only. Do not stat another user's private folder here.
  const auto directory = supplied.lexically_normal();
  const auto text = Utf8(sid);
  return {text, UserName(sid, directory), directory, text == current};
}
bool HasSourcePreferences(const ImportUser& user) {
  RegKey key;
  if (Loaded(user, key)) {
    DWORD count = 0;
    const auto status = RegQueryInfoKeyW(key.value, nullptr, nullptr, nullptr, nullptr, nullptr,
                                         nullptr, &count, nullptr, nullptr, nullptr, nullptr);
    if (status != ERROR_SUCCESS) WinError(status);
    return count != 0; // Presence/count only: never enumerate credential values.
  }
  Handle file; OpenHive(user, file);
  auto hive = CaptureHive(file.value); WipeBytes wipe{hive};
  return RegistryHiveHasPreferences(hive);
}
}
std::vector<ImportUser> ImportUsers(bool* requires_elevation, std::size_t* unavailable_users) {
  RegKey profiles;
  const auto error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ProfileList, 0,
      KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &profiles.value);
  if (error != ERROR_SUCCESS) WinError(error);
  const auto current = CurrentUserSid();
  std::vector<ImportUser> result;
  for (DWORD index = 0; index < 10000; ++index) {
    std::array<wchar_t, 256> name{}; DWORD size = static_cast<DWORD>(name.size());
    const auto status = RegEnumKeyExW(profiles.value, index, name.data(), &size, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) { std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.sid < b.sid; }); return result; }
    if (status != ERROR_SUCCESS) WinError(status);
    const std::wstring sid(name.data(), size);
    if (!OrdinarySid(sid)) continue;
    try { result.push_back(ReadImportUser(profiles.value, sid, current)); }
    catch (const CatalogError& failure) {
      if (failure.Code() == "elevation_required" && requires_elevation) *requires_elevation = true;
      else if (unavailable_users) ++*unavailable_users;
      else throw;
    }
  }
  Invalid();
}
ImportUser ResolveImportUser(std::string_view sid) {
  ValidateImportUserSid(sid);
  RegKey profiles;
  const auto error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ProfileList, 0,
                                  KEY_QUERY_VALUE | KEY_WOW64_64KEY, &profiles.value);
  if (error != ERROR_SUCCESS) WinError(error);
  // Resolve only the selected SID, never every unrelated Windows user.
  return ReadImportUser(profiles.value, Wide(sid), CurrentUserSid());
}
ImportUserDiscovery DiscoverImportUsers(std::string_view destination_sid) {
  ValidateImportUserSid(destination_sid);
  ImportUserDiscovery result;
  std::vector<ImportUser> candidates;
  try { candidates = ImportUsers(&result.requires_elevation, &result.unavailable_users); }
  catch (const CatalogError& error) {
    if (error.Code() != "elevation_required") throw;
    result.requires_elevation = true;
    // Even a protected global profile list must not hide a readable current user.
    if (destination_sid == CurrentUserSid()) {
      try { candidates.push_back(ResolveImportUser(destination_sid)); }
      catch (const CatalogError&) { /* The incomplete scan remains explicit. */ }
    }
  }
  for (auto& user : candidates) {
    try {
      if (HasSourcePreferences(user)) {
        user.current_user = user.sid == destination_sid;
        result.users.push_back(std::move(user));
      }
    } catch (const CatalogError& error) {
      if (error.Code() == "elevation_required") result.requires_elevation = true;
      else if (error.Code() != "source_missing") ++result.unavailable_users;
    }
  }
  return result;
}
void CheckImportAccess(const ImportUser& supplied) {
  const auto user = ResolveImportUser(supplied.sid);
  RegKey key; if (Loaded(user, key)) return;
  Handle file; OpenHive(user, file);
}
std::vector<NativePreference> CaptureUserPreferences(const ImportUser& supplied) {
  const auto user = ResolveImportUser(supplied.sid);
  RegKey key; if (Loaded(user, key)) return StableRegistry(key.value);
  Handle file; OpenHive(user, file);
  auto hive = CaptureHive(file.value);
  WipeBytes wipe{hive};
  return ReadRegistryHivePreferences(hive);
}
bool RegistryHiveHasPreferences(const std::vector<std::uint8_t>& hive) { return Hive(hive).HasPreferences(); }
std::vector<NativePreference> ReadRegistryHivePreferences(const std::vector<std::uint8_t>& hive) { return Hive(hive).Preferences(); }
} // namespace stfc::profiles
#endif
