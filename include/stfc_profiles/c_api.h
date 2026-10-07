#pragma once

#if defined(_WIN32)
#if defined(STFC_PROFILES_EXPORTS)
#define STFC_PROFILES_API __declspec(dllexport)
#else
#define STFC_PROFILES_API __declspec(dllimport)
#endif
#define STFC_PROFILES_CALL __cdecl
#else
#define STFC_PROFILES_API __attribute__((visibility("default")))
#define STFC_PROFILES_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The stable v1 allocation ABI accepts JSON apiVersion 1 or typed apiVersion 2.
// Returns zero when a response was allocated, including structured operation
// failures. A nonzero return denotes an ABI/input/allocation failure. The caller
// frees every returned response using this module's matching allocator.
STFC_PROFILES_API int STFC_PROFILES_CALL
stfc_profiles_catalog_request_v1(const char* request_utf8, char** response_utf8);
STFC_PROFILES_API void STFC_PROFILES_CALL stfc_profiles_free_v1(void* response);

// Shared profile-data lease: keeps an active profile's directory stable while
// a caller reads/saves configuration. It does not reserve a game session.
STFC_PROFILES_API int STFC_PROFILES_CALL
stfc_profiles_acquire_data_lease_v1(const char* root_utf8, const char* id,
                                  void** lease, char** error_utf8);
STFC_PROFILES_API void STFC_PROFILES_CALL stfc_profiles_release_data_lease_v1(void* lease);

// Shared installation access prevents an updater from entering between a
// consumer's preflight and ordinary process launch. Hold until child exit.
STFC_PROFILES_API int STFC_PROFILES_CALL
stfc_profiles_acquire_installation_lease_v1(const char* root_utf8, const char* game_directory_utf8,
                                          void** lease, char** error_utf8);
// Exclusive update access must be held in the process performing every install
// mutation. Release through the same installation-lease function below.
STFC_PROFILES_API int STFC_PROFILES_CALL
stfc_profiles_acquire_installation_update_lease_v1(const char* root_utf8, const char* game_directory_utf8,
                                                 void** lease, char** error_utf8);
STFC_PROFILES_API void STFC_PROFILES_CALL stfc_profiles_release_installation_lease_v1(void* lease);

#ifdef __cplusplus
}
#endif
