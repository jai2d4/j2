# Local accounts

Who TRACE thinks you are, how it decides, and what it deliberately does not do.

---

## 1. Why this exists

Before local accounts, TRACE trusted whoever opened it. The `users` table existed
and the audit trail named an operator, but that name came from the operating system
and nothing checked it. Anyone who could launch the application was that person as
far as every audit row was concerned.

For software whose stated purpose is establishing **who did what to a piece of
evidence**, that is a gap in the chain of custody rather than a missing convenience.
An audit trail that cannot distinguish two people is a log, not a record.

---

## 2. How a password is stored

`core/security/password.{h,cpp}` — no Qt, no third-party cryptography.

### Not SHA-256

TRACE already contains a SHA-256 implementation, and it is **deliberately not used
on its own** for passwords. SHA-256 is fast, which is precisely the property an
attacker wants: a modern GPU tries billions of plain SHA-256 guesses a second
against a stolen database. A password store has to be slow on purpose.

### PBKDF2-HMAC-SHA256, 600,000 iterations

| Property | Value | Why |
|---|---|---|
| Algorithm | PBKDF2-HMAC-SHA256 | RFC 8018, NIST SP 800-132 |
| Iterations | 600,000 | OWASP's floor for this construction |
| Salt | 128 bits, per account | One table cannot cover two accounts |
| Output | 256 bits | Matches the hash function |

PBKDF2 is a conservative choice. **Argon2id resists custom hardware better** and
would be the better algorithm in isolation. It was not chosen because it means a
third-party dependency in software that has to be auditable, and because the
alternative is not "no protection" — it is a well-specified, standards-approved KDF
at a work factor that makes offline attack expensive.

That trade-off is recorded rather than hidden, and the design leaves the door open:
the stored record names its **algorithm and its work factor**, `needsRehash()`
already identifies records written under a lower one, and verification refuses an
algorithm it does not recognise rather than guessing. Moving to Argon2id later is a
migration, not a rewrite.

### The work factor is asserted, not assumed

`kDefaultIterations` is checked by a test. A later edit that quietly lowered it to
something fast would defeat the entire mechanism while every other test still
passed.

### Known-answer tests

A hand-rolled KDF that has not been checked against somebody else's numbers is a
guess. HMAC-SHA256 is verified against the **RFC 4231** vectors — including the
case with a key longer than the hash block, which exercises a branch nothing else
reaches — and PBKDF2 against the published SHA-256 vectors, including a derived key
longer than one block, which is the only case that runs the block-concatenation
loop more than once.

---

## 3. Signing in

### Failure is uniform, on purpose

A wrong password and a username that does not exist produce the **same message and
the same error code**. Saying "no such user" turns the sign-in prompt into a
directory of who holds an account.

It is not enough to word the messages identically. Returning early for a missing
account makes it measurably *faster*, and that difference alone enumerates
usernames over a few hundred attempts. So when the account does not exist, the
password is verified against a **decoy record** with the same work factor. The
wrong answer costs the same as the right one.

### Lockout

Five consecutive failures lock an account for fifteen minutes — against the correct
password too, or it is not a lockout.

The work factor slows an attacker who has stolen the database and is guessing
offline. It does nothing about an attacker at the keyboard, who is limited by the
application rather than by arithmetic. These are different attacks and need
different answers.

The window is fixed rather than requiring an administrator to unlock, because TRACE
runs on single-operator workstations where there may be no second administrator to
ask. A successful sign-in clears the counter, so an operator who mistypes twice and
then gets it right is not one attempt from a lockout the following day.

### What the audit trail records

Every attempt, successful or not. A run of failures against one account is exactly
what a trail like this exists to show.

**The password is never recorded in any form** — not the plaintext, not a hash of
it, not its length. This is enforced by structure as well as by care:
`StoredAccount` is a separate type from the `UserAccount` the rest of TRACE passes
around, so an identity value **cannot** carry credential material into a log line,
an audit record or an exported report. A test asserts that an attempted password
appears nowhere in the trail.

| Action | Recorded as |
|---|---|
| Successful sign-in | `auth.sign_in_succeeded` |
| Failed attempt | `auth.sign_in_failed`, with the username and the reason |
| Lockout | `auth.account_locked`, with the expiry |
| Sign out | `auth.signed_out` |
| Account created | `auth.account_created` |
| Password changed by its holder | `auth.password_changed` |
| Password reset by an administrator | `auth.password_reset`, marked a warning |
| Role changed / account deactivated | `auth.role_changed`, `auth.account_deactivated` |

---

## 4. Accounts

### First run

An installation with no usable account cannot show a sign-in prompt, because nobody
could satisfy it. It runs first-run setup instead and creates an administrator.

That path mints an administrator **without anybody authenticating**, so it closes
behind itself: `createFirstAdministrator` re-checks inside the same call and refuses
once any usable account exists. Otherwise it is a permanent back door.

### Issued passwords must be replaced

An account an administrator creates — or resets — starts with `must_change_password`
set, and TRACE will not open the main window until it has been replaced.

The reason is attribution, not hygiene. While the administrator knows the password,
an action taken under that account is not attributable to its holder alone, and the
audit trail would be asserting something it cannot support.

Changing a password requires the current one, so an unattended unlocked workstation
is not a permanent account takeover. The new password is typed twice, because it
cannot be seen and a typo would leave the operator holding a password nobody knows.

### Roles

The existing role gate (`Viewer`, `Analyst`, `Supervisor`, `Administrator`) now has
teeth: managing accounts requires `Permission::ManageUsers`, and a test asserts an
analyst cannot promote themselves.

### Password rules

**Twelve characters minimum, and no composition rules.**

Length is what resists guessing. Requiring a capital, a digit and a symbol reliably
produces `Password1!` — it narrows the space an attacker searches rather than
widening it. NIST SP 800-63B sets 8 as the floor for user-chosen secrets; TRACE asks
for 12 because one workstation account guards a whole case load.

---

## 5. Randomness

Salts come from the operating system's cryptographic generator: `/dev/urandom` on
Unix, `rand_s` (backed by `RtlGenRandom`) on Windows.

There is **deliberately no fallback**. Not `std::random_device`, whose quality is
implementation-defined and has historically been a fixed sequence on some
toolchains; not a time seed. If the system generator is unavailable, account
creation fails and says so. A salt from a predictable source looks like security
while providing none, which is worse than an honest failure.

---

## 6. What this does not do

Stated plainly, because a security feature that is oversold is a liability.

- **The database is not encrypted.** Password *hashes* are useless without an
  expensive attack, but case data, evidence paths and the audit trail sit in a
  plain SQLite file. Anyone with the file and an ordinary SQLite client can read
  everything except the passwords. Local accounts control who uses *the
  application*; they are not access control over the data at rest. Encryption at
  rest is listed in `docs/ROADMAP.md` and is not built.
- **Evidence files are not protected.** They are ordinary files in the managed
  storage directory, readable by anyone with filesystem access. Their *integrity* is
  checkable by hash; their confidentiality depends on the operating system.
- **There are no sessions or timeouts.** Signing in lasts until the application
  closes. An unlocked workstation left running is an unlocked TRACE.
- **An unclaimed identity can be taken over by whoever runs first-run setup.**
  That is the point — it is how an existing installation's operator row becomes
  a real account without losing its audit history — but it means the window
  between installing TRACE and completing setup is a window in which anybody at
  the keyboard becomes the administrator. Complete setup before the machine is
  left unattended.
- **There is no password recovery.** An administrator can reset another account. A
  lone administrator who forgets their password has no route back in, by design —
  a recovery mechanism is also an attack surface, and a single-workstation
  deployment has nowhere trustworthy to put one.
- **No second factor, no directory integration.** Single-workstation local accounts
  only. No LDAP, no Active Directory, no SSO.
- **Nothing here has been penetration tested.** The cryptographic primitives are
  verified against published vectors and the policy behaviours are covered by
  tests, which is not the same as somebody competent having attacked it.

---

## 7. Coverage

`tests/unit/password_test.cpp` (11) and `tests/integration/auth_test.cpp` (15).

**Cryptography** — RFC 4231 HMAC vectors including the long-key branch; PBKDF2
vectors including multi-block output; the work factor is not silently lowered; the
same password hashes differently every time; the stored record never contains the
password; an unknown algorithm, a zero work factor and unparseable hex all fail
closed; a raised work factor marks older records for rehashing; constant-time
comparison still compares correctly; length is required and composition is not; the
random source does not return a fixed buffer.

**Policy** — a fresh installation asks for setup; the setup path closes behind
itself; signing in establishes the identity the audit trail names and signing out
clears it; an unknown username and a wrong password are indistinguishable; every
attempt is recorded and the password is not; repeated failures lock the account
against even the correct password; a success clears the counter; an issued password
must be replaced before it attributes anything; changing one requires the current
one; a weak password is refused and creates nothing as a side effect; an inactive
account cannot sign in; managing accounts requires the Administrator role;
credentials survive a restart; first-run setup claims an identity that was
recorded but never given a password — the upgrade path for an installation
predating accounts — while an account that already has one is never silently
overwritten.
