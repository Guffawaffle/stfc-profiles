#!/bin/bash
# Synthetic CI fixture only. Never alters a developer's login Keychain.
set +x
set -euo pipefail
if [[ "$(uname -s)" != "Darwin" || "${GITHUB_ACTIONS:-}" != "true" || -z "${RUNNER_TEMP:-}" ]]; then
  echo "The synthetic Keychain fixture requires an ephemeral native macOS GitHub Actions runner." >&2
  exit 2
fi
runner_temp="$(cd "$RUNNER_TEMP" && pwd -P)"
case "${1:-}" in
  setup)
    if [[ -z "${GITHUB_ENV:-}" ]]; then echo "GITHUB_ENV is required." >&2; exit 2; fi
    fixture="$(mktemp -d "$runner_temp/stfc-profiles-keychain.XXXXXX")"
    chmod 700 "$fixture"
    /usr/bin/security default-keychain -d user > "$fixture/previous-default.txt"
    /usr/bin/security list-keychains -d user > "$fixture/previous-search-list.txt"
    printf 'STFC_PROFILES_TEST_KEYCHAIN_DIRECTORY=%s\n' "$fixture" >> "$GITHUB_ENV"
    keychain="$fixture/SyntheticProfiles.keychain-db"
    password="$(/usr/bin/openssl rand -hex 32)"
    /usr/bin/security create-keychain -p "$password" "$keychain"
    /usr/bin/security set-keychain-settings -lut 21600 "$keychain"
    /usr/bin/security unlock-keychain -p "$password" "$keychain"
    unset password
    # The fixture is the only user search target during synthetic tests. System
    # anchors are unchanged, and every original setting is restored in cleanup.
    /usr/bin/security list-keychains -d user -s "$keychain"
    /usr/bin/security default-keychain -d user -s "$keychain"
    echo "Prepared isolated native macOS synthetic Keychain."
    ;;
  cleanup)
    if [[ -z "${STFC_PROFILES_TEST_KEYCHAIN_DIRECTORY:-}" ]]; then exit 0; fi
    fixture="$(cd "$STFC_PROFILES_TEST_KEYCHAIN_DIRECTORY" && pwd -P)"
    case "$fixture" in "$runner_temp"/stfc-profiles-keychain.*) ;; *) echo "Refusing Keychain cleanup outside the owned runner fixture." >&2; exit 2;; esac
    old_default="$(sed -E 's/^[[:space:]]*"//; s/"[[:space:]]*$//' "$fixture/previous-default.txt")"
    previous=()
    while IFS= read -r line; do
      entry="${line#*\"}"
      entry="${entry%\"*}"
      if [[ -n "$entry" ]]; then previous+=("$entry"); fi
    done < "$fixture/previous-search-list.txt"
    /usr/bin/security default-keychain -d user -s "$old_default"
    /usr/bin/security list-keychains -d user -s "${previous[@]}"
    if [[ -f "$fixture/SyntheticProfiles.keychain-db" ]]; then
      /usr/bin/security delete-keychain "$fixture/SyntheticProfiles.keychain-db"
    fi
    rm -rf -- "$fixture"
    echo "Restored native macOS runner Keychain settings and removed synthetic fixture."
    ;;
  *) echo "Use setup or cleanup." >&2; exit 2;;
esac
