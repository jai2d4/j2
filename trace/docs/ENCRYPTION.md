# Encryption at rest

What TRACE encrypts, how the keys are arranged, what it deliberately does not
protect, and what has actually been tested.

`docs/AUTHENTICATION.md` §6 used to end by saying that local accounts gate the
application and not the data — that a case database was a plain SQLite file
anyone with the disk could read. This document is what closed that gap, and §6
below is the equivalent honest list for this feature.

---

## 1. What is encrypted

| | Encrypted | Notes |
|---|---|---|
| The case database (`trace.db`) | **Yes** | SQLCipher, AES-256 per page with HMAC-SHA512. Page one is encrypted too, so table names and the shape of the case load are not readable either. |
| Managed originals (the evidence) | **Yes** | TRACE encrypted containers, AES-256-GCM in chunks. |
| Derived assets — thumbnails, waveforms, exported frames, clips | **No** | See §6. This is the largest gap and it is real. |
| Exhibit bundles and reports | **No, by design** | Their purpose is to leave TRACE and be verified by somebody who does not have it. See §6. |
| The keyring (`keyring.tkr`) | Not secret | Holds usernames, salts and *wrapped* keys. No password and no key in the clear. |
| Logs | Not encrypted | They name files and actions, not evidence contents. |

A workspace is encrypted if it has a keyring. That is the flag, not the state of
the database file — deciding it the other way round would make an interrupted
conversion look unencrypted, and the next ingestion would write evidence in the
clear into a workspace that already holds containers.

---

## 2. The key hierarchy

```
password ──PBKDF2-HMAC-SHA256(600,000)─▶ wrapping key
                                              │
                                          AES-256-GCM
                                              │
                                              ▼
                                     [wrapped master key]   ← one per operator,
                                              │                in keyring.tkr
                                              ▼
                                        master key
                                         │      │
              SQLCipher key for trace.db ┘      └─ HKDF-SHA256(case id) ─▶ case key
                                                                              │
                                                       HKDF-SHA256(file salt) ┘
                                                                              │
                                                                              ▼
                                                                          file key
```

**One master key per workspace, stored once per operator.** Two things follow,
and they are the reason for the indirection rather than encrypting directly
under a password:

- Changing a password rewraps 32 bytes. It does not re-encrypt a single byte of
  evidence, so it takes the same time on a workstation holding one case as on
  one holding a thousand.
- A second investigator gets their own access without anyone sharing a password,
  and removing one does not mean re-keying the workspace.

Each wrap is bound to the workspace identifier *and* the username, so a keyring
entry cannot be copied into another workspace or relabelled to a different
operator — either edit changes the associated data and the unwrap fails.

**Per-case keys are derived, not stored.** There is no table of wrapped case
keys to fall out of step with the list of cases; a row that went missing would
make a case unreadable while everything else still believed it was fine. The
cost is named rather than hidden: a derived key cannot be handed to another
agency without handing over the master, so disclosure of a single case means
exporting an exhibit bundle. If per-case key transfer is ever wanted, wrapped
case keys become a migration — the container format takes a key per file and
does not care where it came from.

**Each file derives its own key** from the case key and a random 16-byte salt.
That is what makes the chunk nonce a bare counter that *provably* cannot repeat,
rather than one that is merely unlikely to. Two identical recordings ingested
into two cases also produce completely different ciphertext, so the storage does
not reveal that they are the same file.

---

## 3. Where the primitives come from

`core/security/password.cpp` implements PBKDF2 by hand and says in its header
that authentication should add no third-party cryptographic dependency to
software that has to be auditable. This feature reverses that, and the reasoning
is worth stating rather than quietly dropping.

PBKDF2 is iteration over a hash the codebase already had, it is specified in
RFC 8018, and it has published test vectors: a wrong implementation fails them
loudly. **AES-GCM is not like that.** Reusing a nonce under one key reveals the
XOR of two plaintexts and leaks the authentication subkey outright, and a
table-driven AES written here would leak the key through cache timing while
passing every test vector. Correct ciphertext is not evidence of a correct
implementation.

The same argument applies to the database: a hand-written page-encrypting
SQLite VFS that is subtly wrong corrupts case databases.

So: **libcrypto** for AES-256-GCM, **SQLCipher** for the database. What TRACE
adds is the part that is its responsibility — key hierarchy, nonce discipline,
and a container format that cannot be silently truncated or reordered.

Both are optional at build time (`TRACE_WITH_ENCRYPTION`). A build without them
still works on unencrypted workspaces and refuses encrypted ones with a stated
reason; `crypto::available()` and the compile definition come from the same
place, so the two cannot disagree.

---

## 4. The container format

```
magic        8   "TRACEEV1"
version      2   little-endian, currently 1
algorithm    2   1 = AES-256-GCM
chunkBytes   4   plaintext bytes per chunk (default 256 KiB)
plainSize    8   total plaintext length
salt        16   per-file, for the subkey derivation
reserved     8   zero
---------------  48 bytes, then one record per chunk:
ciphertext  n    n = chunkBytes, except the last
tag         16
```

Chunked so that seeking to a frame decrypts one chunk rather than everything
before it. Each chunk's associated data is **the whole header plus that chunk's
index**, which is what makes the following all fail rather than succeed quietly:

- a chunk moved, duplicated, or lifted from another file;
- a file truncated — the header carries the plaintext length and is
  authenticated, so a container cut short fails on the missing chunk instead of
  reading as a shorter recording;
- the header edited to hide a truncation — that invalidates every chunk.

For evidence, the difference between a detected fault and a silently altered
exhibit is the whole point.

### Reading it back

FFmpeg reads through a custom `AVIOContext` whose read and seek callbacks
decrypt on demand (`media/ffmpeg/encrypted_io.cpp`). The obvious alternative —
decrypt to a temporary file, point FFmpeg at it, delete it afterwards — writes
the entire recording to disk in the clear, which is the exact thing being
defended against, and a machine that loses power mid-playback leaves that copy
behind. The plaintext exists only in the buffer FFmpeg is holding at the time.

---

## 5. The evidence digest does not change

This is the property that had to survive, and the one most easily broken by
accident.

The SHA-256 recorded against a piece of evidence is the digest **an outside
party computes from the original recording**. It appears in reports and exhibit
bundles and is verifiable without TRACE. Encryption does not change it:

- Ingestion hashes the plaintext as it reads the source.
- The verification pass decrypts the container and hashes *that*.
- An integrity check compares the stored digest against the decrypted contents.

`hashFile` and `hashStoredEvidence` are deliberately separate functions. One
tells you what is on the disk; the other tells you what the evidence is. Only
the second can be compared against a digest recorded before encryption existed,
and conflating them would make every integrity check on an encrypted workspace
either meaningless or permanently failing.

A failed check still never rewrites the stored digest — the rule from Phase 0 is
unchanged.

---

## 6. What this does **not** protect

Read this section before telling anyone the evidence is encrypted.

1. **Derived assets are encrypted too, as of the change that removed this from
   the list.** Thumbnails, waveform envelopes, exported frames and clips all go
   into the same AES-256-GCM containers under the same case key. A thumbnail is
   a frame of the recording, so leaving it in the clear made "the evidence is
   encrypted" false in the way that mattered.

   Two properties are worth stating because they are easy to get backwards:

   - **The recorded digest and size describe the plaintext**, not the container
     — the same rule evidence follows. A report citing the SHA-256 of an
     exported frame is citing the frame, so an examiner handed that frame can
     check it.
   - **A locked workspace refuses to produce derived assets** rather than
     writing them in the clear. `DerivedAssetService::registerAsset` fails with
     `PermissionDenied` and the caller removes the file it had written.

   Whether a stored asset is a container is decided by looking at the file, not
   by whether a key is available, so a workspace encrypted after the fact keeps
   working: assets written before the switch stay readable as plain files.

2. **Exhibit bundles and reports are unencrypted, on purpose.** They exist to be
   handed to somebody who does not have TRACE and verified with `sha256sum`. An
   encrypted exhibit would defeat the point, so `BundleWriter::addExhibit`
   decrypts on the way into the bundle. Protecting a bundle is a matter of how
   it is transported, and that is outside the software.

   A bundle export from a locked workspace fails rather than copying containers
   in: a manifest of ciphertext digests would verify perfectly and still not
   hand anyone the exhibit.

3. **A running TRACE holds the key in memory.** This protects a disk at rest — a
   stolen laptop, a decommissioned drive, a backup tape. It does nothing against
   somebody with access to the machine while a workspace is unlocked, and
   nothing against a memory dump of the running process.

4. **The keyring is an offline guessing target.** Somebody holding
   `keyring.tkr` can attempt passwords at 600,000 PBKDF2 iterations per guess
   per account. That makes each attempt expensive, not impossible. A weak
   password is still a weak password, and encryption at rest is not a reason to
   accept a short one.

5. **A lost password is a lost workspace.** There is no recovery, no escrow, no
   back door. TRACE holds no copy of the master key outside the wrapped entries.
   This is deliberate — a recovery mechanism is another way in — but it means the
   only real protection against a forgotten password is a second operator, which
   is why the keyring supports several and refuses to remove the last one.

6. **File sizes and counts are visible.** A container is a little larger than
   its plaintext and sits in a directory named after the case. How many items a
   case holds and roughly how large each is can be seen without any key.

7. **`trace.db.plain` is left behind by a conversion.** Deliberately: an
   operator who has just encrypted a workspace by mistake still has their case
   index, and TRACE deleting the only copy of it to tidy up after itself is not
   a trade it gets to make on somebody's behalf. **It still holds the case index
   in the clear, and deleting it is a manual step.** The conversion says so when
   it finishes.

8. **Nothing here is a substitute for disk encryption.** Operating-system full
   disk encryption protects everything, including the temporary files any
   application writes and the scratch space FFmpeg and Qt use. TRACE's
   encryption is narrower and complements it; it is not a reason to turn the
   other off.

9. **A bundle you have exported is plaintext on disk.** That is the point of it
   (see 2), but it means an exported bundle sitting in an operator's downloads
   folder is not protected by any of this. It is a file to be handed over and
   then removed, not a second copy of the case to leave lying about.

---

## 7. Using it

### A new workspace

The first launch against an empty data directory offers encryption, on by
default. The password chosen there creates the keyring; the sign-in that follows
creates the administrator account, pre-filled with the same operator name so it
is one credential rather than two.

An operator sees two prompts on an encrypted workstation. That is honest rather
than redundant: the first proves they can decrypt the workspace, the second
establishes who is accountable for what happens inside it.

### An existing workspace

```
trace --data-dir /path/to/workspace --encrypt-workspace
```

Conversion is a mode of its own rather than a menu item that does the work,
because every managed original is rewritten and the database is re-keyed;
running that underneath an open case is how a half-converted workspace happens.
The File menu's **Encrypt this workspace…** explains this and closes TRACE.

**It is resumable**, and that follows from a property rather than from
bookkeeping: both forms of every file are valid at once, because readers decide
per file by looking at it. A directory holding some containers and some plain
recordings is a working workspace. So a conversion can stop anywhere — a power
cut, a cancelled dialog — and running it again finishes the job.

Each file individually is all-or-nothing. The container is written beside the
original, decrypted, and its digest checked against the one recorded at
ingestion before the original is replaced. A mismatch stops the run with that
file untouched, because at that point something is wrong that re-running will
not fix.

The database is re-keyed last, with `sqlcipher_export`, so the audit trail and
every row identifier survive exactly as they were. An encrypted workspace has
the same history as the one it replaced.

### More than one operator

`Keyring::addOperator` requires the master key, so only somebody who has already
unlocked the workspace can extend access to it. `removeOperator` refuses to
remove the last one — a keyring with no entries is a workspace nobody can ever
open again, and no confirmation dialog makes that recoverable.

---

## 8. What has been tested, and what has not

**Verified, by test:**

- HKDF-SHA256 against an independent implementation (Python's `hmac`/`hashlib`),
  not against itself — the check that catches passing the salt as the message,
  which still produces 32 plausible bytes.
- Containers round-trip exactly at 0, 1, 1023, 1024, 1025, 4096 and 5000 bytes,
  i.e. across every chunk boundary where an off-by-one would hide.
- Random-access reads over 200 random offset/length pairs match the plaintext at
  the same offsets.
- Truncation, chunk reordering, header editing, a single flipped bit, and the
  wrong key all fail — each one a case where succeeding would mean accepting
  altered evidence.
- The database file does not contain the case number, and does not contain its
  own table names.
- The keyring file contains neither the password nor the master key, in bytes or
  as hex.
- Frames decoded from an encrypted container are **identical pixel for pixel**
  to the same recording unencrypted, and seeks land on the same frames.
- Ingestion into an encrypted workspace records the plaintext digest, stores a
  container, and still passes an integrity check.
- Ingestion into a *locked* encrypted workspace fails and writes nothing —
  rather than falling back to plaintext.
- A conversion of an existing workspace preserves the digest, the audit trail
  and the schema version, and running it twice is safe.
- The unlock dialog admits the right operator and refuses a wrong password, an
  unknown operator and an empty name — driven as a real widget.

**Verified on both platforms:**

- **Windows builds with encryption on and its encryption tests run there.**
  This used to sit under "not verified" with the argument that SQLCipher and
  libcrypto are in vcpkg and nothing in the code is platform-specific. That was
  an argument, not a result, so CI now builds Windows with
  `TRACE_WITH_ENCRYPTION=ON`.

  Two assertions make the job prove something rather than merely pass.
  `FindEncryptionBackend.cmake` degrades rather than failing when SQLCipher is
  missing, and every encryption test opens with a skip guard on
  `crypto::available()` — so a broken install would have produced a green run
  that tested an unencrypted TRACE. The job therefore requires the configure
  summary to report encryption `TRUE`, and requires the encryption tests to have
  run rather than skipped.

  The find module needed no changes: the vcpkg port installs its header at
  `include/sqlcipher/sqlite3.h` and its import library as `sqlcipher.lib`, which
  is what the module already searched for. What the job did find was a genuine
  platform difference elsewhere — `ClipExportService` held the written clip open
  while registering it, and Windows will not replace an open file where Linux
  will. That failed exactly one test out of 319, and only in an encrypted
  workspace, because only there does registration rewrite the file in place.

**Not verified:**

- **No timing analysis of any kind.** The keyring answers a wrong password and
  an unknown operator identically by construction, and both perform the same
  derivation, but no measurement has been taken to confirm the two are
  indistinguishable in practice.
- **No timing measurement of the container itself.** Chunk decryption is
  bounded by the chunk size by construction, but no figure has been taken.
- **No large-file measurement.** The largest recording put through a container
  in testing is the few-megabyte sample. Chunked random access should make a
  multi-gigabyte file behave the same way, but "should" is the accurate word.
- **No adversarial review.** This is one implementation, tested against its own
  expectations. The primitives are audited; the way they are assembled here is
  not.
