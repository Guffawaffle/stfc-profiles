// Extracted from Guffawaffle/stfc-mod at 323fb857f51f4cb08231d4b150ea8b5bb59340d1.
// See docs/PROVENANCE.json and LICENSE (GPL-3.0).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace stfc::profiles::legacy
{

inline bool ValidId(std::string_view id)
{
  if (id.empty() || id.size() > 32)
    return false;
  for (const char ch : id)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
      return false;
  if (id == "con" || id == "prn" || id == "aux" || id == "nul")
    return false;
  return !(id.size() == 4 && id[3] >= '1' && id[3] <= '9'
           && (id.substr(0, 3) == "com" || id.substr(0, 3) == "lpt"));
}

inline std::optional<std::string_view> ParseMarker(std::string_view bytes)
{
  if (bytes.ends_with("\r\n"))
    bytes.remove_suffix(2);
  else if (bytes.ends_with('\n'))
    bytes.remove_suffix(1);
  if (!bytes.starts_with("v1:"))
    return std::nullopt;
  const auto id = bytes.substr(3);
  return ValidId(id) ? std::optional{id} : std::nullopt;
}

// The v2 receipt identifies this preference-store generation. A receipt from
// the earlier registry-prefix prototype must not enroll the encrypted store.
inline std::string Receipt(std::string_view id, std::string_view canonical_path)
{ return "v2:profile-prefs-v1\n" + std::string(id) + "\n" + std::string(canonical_path) + "\n"; }

inline bool ReceiptClaimsId(std::string_view receipt, std::string_view id)
{
  constexpr std::string_view header = "v2:profile-prefs-v1\n";
  if (!receipt.starts_with(header))
    return false;
  receipt.remove_prefix(header.size());
  return receipt.size() > id.size() && receipt.starts_with(id) && receipt[id.size()] == '\n';
}

enum class SelectionState {
  Default,
  Enroll,
  Resume,
  Bound,
  MissingMarker,
  InvalidMarker,
  ReceiptConflict
};

struct SelectionDecision {
  SelectionState   state;
  std::string_view id;
};

inline SelectionDecision Decide(std::optional<std::string_view> marker, std::optional<std::string_view> receipt,
                                std::optional<std::string_view> pending, std::string_view canonical_path)
{
  if (!marker) {
    if (receipt || pending)
      return {SelectionState::MissingMarker, {}};
    return {SelectionState::Default, {}};
  }
  const auto id = ParseMarker(*marker);
  if (!id)
    return {SelectionState::InvalidMarker, {}};
  const auto expected = Receipt(*id, canonical_path);
  if ((receipt && *receipt != expected) || (pending && *pending != expected))
    return {SelectionState::ReceiptConflict, {}};
  if (receipt)
    return {SelectionState::Bound, *id};
  return pending ? SelectionDecision{SelectionState::Resume, *id}
                 : SelectionDecision{SelectionState::Enroll, *id};
}

inline std::uint64_t PathHash(std::string_view canonical_path)
{
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char ch : canonical_path) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

} // namespace stfc::profiles::legacy
