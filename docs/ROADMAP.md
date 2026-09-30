# Extraction and runtime sequence

The first checkpoint moves the existing Windows implementation into an independent
library with preserved formats, provenance and consumer compilation. It deliberately
retains legacy selection while the shared-install contract is studied separately.

Next steps, in order:

1. Integrate a reviewed immutable library revision into the community-mod profile
   candidate. Remove duplicate implementation and verify a full mod build. The
   current mod checkout has separate coroutine work and was left untouched.
2. Define the explicit per-launch profile request and compatibility transition.
   Bare launch uses ordinary state; named launch isolates or fails. Legacy marked
   installations require deliberate migration rather than silent reinterpretation.
3. Prove two different profile IDs from the same game installation, including
   per-profile writer exclusion, early hook order, readiness and failure behavior.
   Preserve interrupted-enrollment recovery and existing account state.
4. Implement a minimal profile-only `version.dll` host. Qualify it independently
   of the full mod and exercise stopped-install composition switching.
5. Connect Bridge's CLI and immutable-ID shortcuts to the qualified runtime
   contract, then qualify macOS selection, storage and loading separately.

The independent repository does not remove these runtime requirements. No player
binary release, shared-install support or macOS parity is claimed by this checkpoint.
