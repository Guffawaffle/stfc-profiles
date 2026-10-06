# Direct game updater

STFC Profiles owns installation status, update checks, full-image updates and
interrupted-update recovery. Bridge and the CLI call the same native JSON
operations. They do not download or patch the game independently.

## Current scope and evidence

The Windows MVP uses the official public GUS full-image protocol. On
2026-09-30, a read-only check of the selected dev installation reported client
221 and available client 267. The full archive is 457,699,340 bytes and its
extracted inventory is 746,394,598 bytes. No account data is needed to check or
update an installation. Direct macOS updating remains unqualified and returns
`unsupported_platform`; its platform build numbering differs from Windows.

The check fetches bounded metadata, not the game image. Runtime qualification
of a real downloaded full image, the updated dev installation and concurrent
sessions is separate from the disposable fixture evidence described below.

## Protocol and integrity

The current public manifest endpoint is:

`https://gus.xsolla.com/updates?version=0&project_id=152033&region=&platform=windows`

The native reader accepts the observed full-image action sequence only:
`extracted_size`, `torrent_download`, `wait_actions`, `extract`, `wait_actions`,
`version`. XML declarations, attributes, entity escapes and both empty action
forms are bounded and parsed explicitly. DTDs, external entities, duplicate
attributes and unknown actions are rejected. Delta patch plans are unsupported.

Payload and torrent URLs must match the full-image basename and official
`https://launcher-game-update.s3.amazonaws.com/8301/full_games/full_game_/`
prefix. HTTPS certificate validation uses Windows defaults. HTTP and redirects
are rejected. The torrent is parsed locally, without tracker requests, peer
connections or P2P libraries. Its single-file name, length, piece length and
piece count must match the manifest. Every downloaded piece is checked against
its torrent SHA-1 before the image is extracted. These hashes establish payload
integrity against metadata delivered over HTTPS. They are not an independent
publisher signature or a claim that every game binary is Authenticode signed.

Limits are 64 KiB for XML, 16 MiB for torrent metadata, 8 GiB for a compressed image,
16 GiB for extracted data, 100,000 inventory entries and 30 minutes per HTTP
payload. Archive piece sizes must be powers of two between 64 KiB and 16 MiB.
Unexpected format changes fail explicitly rather than weakening these limits.

Extraction uses statically linked libarchive 3.8.9 with XZ/LZMA and zlib support;
no developer-installed extractor is required. Its immutable source SHA-256 is
`f5a6539059cf5e597dbeda37bfa4874b1e8dea063c8d93bf85a2b44af90a5bd4`, from
[the publisher checksum](https://libarchive.org/downloads/libarchive-3.8.9.tar.gz.sums.txt).

Only ordinary files and directories are extracted. Absolute paths, traversal,
backslashes, alternate data streams, reserved Windows names, trailing dots or
spaces, case collisions, symlinks and hardlinks are rejected. Existing targeted
parents and files must also be ordinary and not reparse points or multiply
linked files. Extracted file lengths and total bytes must match their declared
inventory. The image must contain prime.exe, GameAssembly.dll, UnityPlayer.dll
and prime_Data. The official installed marker is `&game=<number>`; the updater
preserves its optional existing CR/LF suffix and rejects unknown marker fields.

## Installation admission and transaction

Every product game process holds shared access to its exact canonical
installation for its lifetime. Updating and recovering require exclusive
access. The access namespace is independent of the caller's catalog root.
Ordinary game launches retain their ordinary preferences while using this
installation gate. An unfinished transaction blocks product game admission.

Before live changes, the updater identifies every running prime.exe by its
canonical image path and rejects a selected installation with live processes.
An unidentified prime.exe makes the check incomplete and blocks mutation.
A deny-read handle excludes direct executable launches while the old prime.exe
is moved to backup. The new executable is published after all other image files
are in place. `.version` is committed last. This is a journaled sequence of
same-volume file renames, not one atomic multi-file replacement.

Staging and backup live beside the selected game directory in
`.stfc-profiles-update-<canonical-installation-key>`. The journal is bound to
that key and directory; metadata commits are flushed and replaced atomically.
It records original and staged file sizes and SHA-256 digests before changing
live files. A changed or missing file causes an explicit conflict and retains
recovery data. Recovery restores originals or removes only matching newly
published files. It can be replayed after another interruption.

Files absent from the official image are preserved. After the first successful
update, a separate owned-image inventory records files this updater installed.
A later update can retire an obsolete owned file only when its current digest
still matches that recorded image. Modified old files become preserved extras.
The full mod DLL, local configurations and other unowned files are not deleted.
An incoming image colliding with protected host paths, including `version.dll`,
profile configuration and logs, is rejected before live replacement.

Completed transactions retain their journal, archive and backups in a unique
same-volume history directory immediately after success. Explicit recovery
rolls back an incomplete transaction and then moves it into retained history.
A crash-left committed record is validated for schema and installation identity
and retired without requiring current live files to match its historical image.
No automatic history cleanup or retention policy is implemented.

## JSON and CLI

All requests use `apiVersion:1`, `operation`, an explicit `gameDirectory`, and
optional `root`. Operations are `installation-status`, `check-game-update`,
`update-game` and `recover-game-update`. `expectedVersion` can bind an update
to the target returned by the preceding check; a changed target aborts before
payload download. Paths containing an embedded NUL are rejected.

The corresponding CLI commands are `game status`, `game check`, `game update`,
and `game recover`, each with `--game <directory>` or `--profile <id>` to resolve
its saved preferred installation. `game update --expected-version <number>`
binds the target to the preceding check.

Successful responses contain `installation` with canonical `gameDirectory`,
`installedVersion`, `state`, `phase` and `requiresRecovery`. A check additionally
reports `availableVersion`, `updateAvailable`, `downloadBytes` and
`extractedTotalBytes`. A transaction reports its ID and actual phase byte or
file counts, with `progressPercent` derived from those counts. Status polling
can read atomic journal snapshots while the synchronous native update runs on
a caller's background worker. Errors use `{ok:false,error:{code,message}}`.

States are `ready`, `running`, `updating`, and `recovery-required`. Phases are
`idle`, `downloading`, `extracting`, `staged`, `committing`, `committed`,
`rolling-back`, and transient `recovered` before history rotation.

## Focused verification

`installation-tests` uses disposable synthetic files and in-process 7z images;
it never reads or copies real preferences, browser stores or enrollment data.
It exercises the observed marker and XML forms, streaming piece verification
across arbitrary chunks, corrupted and truncated payload rejection, archive
traversal and case collision rejection, preserved extras, owned-file retirement,
modified-file preservation, executable exclusion, cross-root installation
exclusion and JSON NUL path rejection.

The fixture interrupts all 13 live commit checkpoints, restores original files,
replays rollback and verifies the official version marker. It also proves that
external file changes stop recovery while preserving the old executable backup,
and that API recovery retains transaction history. These tests passed locally
on Windows; they do not establish real full-image or macOS runtime qualification.
