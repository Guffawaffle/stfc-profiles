# Canonical configuration writer admission

Implementation design for producer-owned admission used by Bridge Save/Restore
and participating runtime configuration writers. The exported Profiles ABI does
not implement this contract yet. Shared data/installation leases and session
preference-writer ownership do not supply document-writer admission.

The scope is Windows x64 and Apple Silicon macOS, independently qualified. A
missing native primitive refuses admission before document effects. A
stopped-game-only implementation does not fulfill live-writer participation.

## Ownership and resource identity

Profiles owns canonical target resolution and shared admission. Bridge owns the
transaction, backup inventory and recovery policy. Each configuration producer
owns schema, protected-entry policy and its runtime writes. All participating
writers enter this same Profiles boundary for each configuration write.

Ordinary configuration belongs to the explicitly selected physical installation
and current OS user. Isolated configuration belongs to an active immutable
profile ID and physical catalog/profile directory. Its selected installation is
a captured dependency, not a separate identity for the same profile document.
Preferences, saved launch choices and displayed paths confer no authority.

Resolve approved document roles to producer-declared leaves. Profiles supplies
named `config.toml` and `runtime.toml` locations; ordinary configuration follows
the supplying producer's route. The renderer cannot supply arbitrary native
paths. Under exclusion validate user, physical parent, bounded leaf spelling,
metadata kind/revision, installation identity/revision and existing document
identity. Reject ambiguous aliases, redirects and competing hardlink identities.

The resource key remains stable across replacement of destination bytes/file
identity. Bind it to current OS user, admitted physical parent and approved leaf,
plus canonical catalog/profile identity where relevant. Aliases converge or
refuse. Coordination lives outside movable profile directories and destination
leaves.

## Admission and lifetime

Acquire canonical document-writer exclusion, shared installation access and
isolated profile-data/lifecycle access nonblocking and all-or-none. Revalidate
under custody. A loser creates no stage, backup or operation WAL. Coordination
acquisition/reservation is distinct from configuration data effects.

Opaque leases retain namespace custody and originating-module allocation/thread
ownership through completion, cancellation, observer loss and recovery.
Observational paths and serialized responses do not substitute for leases.

Do not use `SessionLease`: it publishes game-session identity and holds profile
preference-writer ownership. A live session can coexist with a configuration lease
when its configuration writes enter this separate document admission. Exclusive
data admission for archive/restore/delete and installation-update exclusion
must conflict with configuration custody.

## Durable reservation and crash gap

Process death releases a kernel lock; it must not erase unresolved obligations.

Successful acquisition durably publishes a producer-owned reservation before
returning a usable lease. Generate an opaque reservation identity bound to the
resource/user and exact baseline or missing-baseline observation, with a
deterministic native recovery locator. Create no document stage or backup here.
This is neither operation success nor a process-liveness assertion.

Bridge `recovery_binding` composes the reservation and operation identities
without effects. The kernel durably records that reference before native begin.
Native begin durably binds the exact operation and deterministic transaction
subjects before the first document effect.

| Order | Durable boundary |
| --- | --- |
| 1 | Producer reservation exists; exclusions and namespace remain retained. |
| 2 | Kernel records exact execution/recovery reference in operation WAL. |
| 3 | Native owner records begin disposition and exact transaction subjects. |
| 4 | Native owner performs backup/stage and publishes or reconciles destination. |
| 5 | Kernel durably publishes verified owner terminal disposition. |
| 6 | Producer reservation is explicitly settled under canonical custody. |

Death between 1 and 2 leaves a reservation even without operation WAL. Death
between 2 and 3 leaves Executing WAL before native begin. Both require inspection,
and neither silently admits another writer. Uncertain begin never triggers a
second begin.

Drop, PID disappearance, an available lock, cancellation, missing WAL or renderer
completion cannot clear a reservation. Explicit pre-execution abort requires
proof of no native begin/effects and known kernel admission disposition.
Unknown disposition retains the reservation.

## Recovery and lifecycle

Ordinary admission checks durable reservations under document exclusion and
returns recovery-required while an obligation remains. Separate recovery
admission requires exact stored reservation/resource/recovery identities,
reacquires canonical exclusions and inspects physical state. Never reconstruct
lost protected values or overwrite foreign destinations.

Reconcile deterministic owned stage/backup subjects and exact destination
identity/bytes. Missing-baseline publication uses create-new/no-overwrite;
existing-baseline replacement retains/verifies exact prior-byte backup. Restore
admits/revalidates the exact backup. Stage absence alone proves no success or
permission to replace a destination. Caller-supplied terminal booleans are not
physical reconciliation evidence.

Only explicit verified terminal or safe-abort settlement releases a reservation.
Terminal WAL refusal retains cached owner completion and the reservation. Death
after terminal publication but before settlement recovers settlement under exact
custody without replaying mutation.

Pending reservations also block relevant archive/restore/delete, conflicting
configuration writes and installation update/recovery after owner death.
Lifecycle/updater admission must inspect obligations, not only acquire a live
lock. Missing, malformed, ambiguous, changed or inaccessible coordination state
refuses conflicting mutation. Reservations remain discoverable without Bridge
running or the interrupted host's WAL being accessible.

Store bounded identities, phases, digests and deterministic recovery locators;
store no TOML text, protected values, account preferences, browser state or login
data in coordination.

## Implementation sequence

1. Implement resource capture, opaque admission and durable reservation
   creation/inspection in Profiles; unqualified host capabilities stay unavailable.
2. Integrate reservation checks into lifecycle/updater admission, then distinct
   recovery admission and explicit settlement.
3. Export versioned native API and adopt immutable reviewed pins in Bridge and
   configuration-producing runtimes. Save stays unavailable without participation
   and actual native transaction/recovery.
4. Implement Bridge DocumentOwner with qualified native read, backup/stage,
   create-new/replace and Restore/recovery primitives.
5. Independently qualify both native hosts, installed journeys and release
   packaging. Portable models supplement actual native observations.

## Required native falsification cases

| Case | Required observation on both hosts |
| --- | --- |
| CWA-01 | Two processes cannot admit one ordinary physical document through differing registrations, roots or accepted spellings. |
| CWA-02 | One isolated document selected with two installations admits one writer; distinct documents/profiles remain independent. |
| CWA-03 | Busy/stale/unsupported refusal creates no stage, backup or operation WAL and releases partial exclusions. |
| CWA-04 | Live runtime writes contend with Bridge; lifetime session/preference ownership and unrelated browser reads remain correct. |
| CWA-05 | Lifecycle and installation update conflict during custody and after death while a reservation remains unresolved. |
| CWA-06 | Kill after reservation durability before kernel WAL: normal admission refuses and exact no-WAL recovery inspection is possible. |
| CWA-07 | Kill after Executing WAL before native begin: no competing admission and no second begin. |
| CWA-08 | Kill/uncertainty at backup, stage, publication and directory-durability boundaries preserves exact identity-only recovery. |
| CWA-09 | Observer loss/cancel/normal shutdown retains work and reservation until safe explicit settlement. |
| CWA-10 | Terminal WAL refusal retains completion/custody; death after terminal publication recovers settlement without replay. |
| CWA-11 | Parent/leaf aliases, metadata/revision drift and foreign destination/stage/backup subjects refuse without repair or overwrite. |
| CWA-12 | Existing/missing baselines, exact backup Restore and protected-entry loss follow distinct publication/recovery rules. |
| CWA-13 | Invalid coordination and wrong resource/reservation identities fail closed across processes and lifecycle clients. |
| CWA-14 | Current OS-user/catalog identity, module allocator/thread ownership and immutable pins survive real consumer integration. |

Bind evidence to source, native artifacts, processes, resources and environment.
Use disposable fixtures without real account stores or installed game data.
This dependency is incomplete until implementation, runtime participation,
native recovery and both host qualification exist.
