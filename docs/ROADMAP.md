# Remaining work

The shared library now contains profile identity and Windows preference storage.
The game adapter accepts explicit host inputs. Installation-marker selection,
its enrollment receipts and compatibility wrapper have been removed completely.

1. Integrate an immutable library revision into the mod and provide explicit
   per-launch ID/lifecycle selection through its single bootstrap.
2. Complete readiness and startup/admission coordination, then prove two named
   processes from the same canonical executable with per-profile writer exclusion,
   browser callbacks and correct account state.
3. Build and qualify the minimal profile-only bootstrap using the same library.
4. Qualify macOS selection, protected storage and loading separately.

Bridge retains its independent profile registry and passes the stable ID at
launch. No marker-file adoption, fallback or migration work is planned.
