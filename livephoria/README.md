# Livephoria

**Where fans get closer.**

A creator-owned fan experience and monetization platform. A creator gets a channel
they control: public posts, members-only posts, pay-per-view drops, LIVE (free,
members-only, or ticketed), tips, merch, digital downloads, DMs, and an earnings
dashboard. A fan gets one place to find creators, subscribe, watch, unlock, tip,
buy, and message.

It is a working full-stack app — FastAPI + SQLAlchemy behind a single-page
frontend the API serves itself at `/`. `pytest` covers the paywall and the money.

## What is real and what is not

Being precise about this, because a monetization platform's credibility is the
whole product:

| Area | Status |
| --- | --- |
| Accounts, channels, tiers, posts, paywall | Real. Enforced server-side, covered by tests. |
| Subscriptions, PPV unlocks, tips, tickets, orders | Real ledger entries and balances in the database. |
| Money in and out | **Simulated.** Wallet top-ups and payouts move numbers in this database only. No card is charged, no bank transfer happens. `app/services/payments.py` has the provider seam a real PSP drops into. |
| Live video | **Not included.** Events, access rules, and ticketing are real; there is no ingest, transcoding, or playback. `playback_url` stays empty until a streaming provider is wired up. |
| Media uploads | **Not included.** Posts and products take URLs; there is no file storage yet. |
| Reports, review queue, appeals | Real. Reports never auto-action; every decision is recorded and appealable. |
| Age assurance | Real gate, **no identity provider**. Submissions sit at `pending` until an operator approves them — the stub never approves itself. |
| Payout identity checks (KYC) | Real gate. Withdrawals are blocked until a payout account is approved; approval is an operator decision, not a vendor's. |
| Verified badges | Manual. `is_verified` is set by an operator, never inferred. |
| Recurring billing | Subscriptions run 30 days from purchase. Nothing renews them automatically yet — there is no billing scheduler. |
| Demo data | `scripts/seed_demo.py` invents creators, posts, and prices. Fictional, for demos only. |

Running it on a PC, including the control plane: **[QUICKSTART.md](QUICKSTART.md)**.

## Run it

```bash
python -m venv .venv && source .venv/bin/activate
pip install -r requirements-dev.txt
cp .env.example .env                  # optional; defaults work
python scripts/seed_demo.py           # optional demo world
uvicorn app.main:app --reload
```

Open <http://127.0.0.1:8000>. The database defaults to a local SQLite file, so
there is nothing to install or provision. API docs are at `/docs`.

Seeded accounts all use the password `livephoria-demo` — e.g. `sam@example.test`
(fan, $100 of test credit) or `aaliyahrose@example.test` (creator).

```bash
pytest        # 56 tests, no network or database needed
```

## How the paywall works

Every access decision goes through `app/services/access.py`, as pure functions
over plain values. Routers do the database work, then ask those functions the
question — so the rule cannot drift between the feed, a profile, and a single
post, and the rules are tested without a database.

- **public** — anyone, signed in or not.
- **subscribers** — needs an active subscription. A post may also name a minimum
  tier; tiers rank by price, so subscribing at $25 opens anything gated at $10,
  and a $5 tier does not open a $25 gate.
- **ppv** — needs a one-off purchase. A subscription alone never opens it.

Two gates sit in front of all of that:

- **Removed content** is visible only to its author, flagged, so a removal is never
  silent. Everyone else gets the same shape as any locked post, with no body.
- **18+ content** requires a viewer whose age check passed. It is checked before
  the paywall, deliberately: **paying never opens an age gate**. A fan can buy a
  PPV post and still not see it until they verify — the purchase stands, the gate
  holds.

A locked post returns its hook only: title, teaser image, price, counts. The body
and `media_url` are withheld server-side, not hidden in the UI. Unknown
visibility values fail closed. Commenting requires the same access as reading.

## How the money works

Amounts are integer cents everywhere; no float touches a balance.

- A fan's **wallet** holds spendable credit. A creator's **earnings** balance is
  separate, so a creator's take can never be silently spent as fan credit.
- Every movement writes an append-only `LedgerEntry` with `gross`, `fee`, `net`.
- The platform fee is `PLATFORM_FEE_BPS` (default 1000 = 10%), rounded down, so
  rounding never costs the creator a cent.
- A charge that would overdraw a wallet raises and writes nothing — no partial
  purchase, no orphaned unlock.

## Trust and safety

The moderation half of the app is in `app/services/moderation.py`, `app/routers/safety.py`
(what a person can do) and `app/routers/control.py` (what a reviewer does).

Two rules the code enforces rather than merely intends:

- **Nothing is auto-actioned.** A report moves an item into a queue; it never
  touches content. A person decides, the decision is written as a
  `ModerationAction` naming a subject, and that subject can appeal it.
- **Removal is reversible.** Content is marked `removed`, never deleted, so an
  upheld appeal restores it exactly as it was.

Reports can be filed **signed out**. Requiring an account to flag serious content
would suppress exactly the reports that matter most. Reasons are a closed list;
four of them (`csam`, `nonconsensual`, `underage`, `threat`) are urgent — they
sort to the top of the queue and are pushed to Axiome the moment they arrive
rather than waiting for the next heartbeat.

An author sees every decision taken against them at `/api/v1/me/actions`, with
whether it can be appealed. Dismissals and restorations are not appealable;
removals and suspensions are, once each.

### Age assurance

`app/services/verification.py`. A declared birthday under 18 is refused outright.
Being over 18 by that declaration proves nothing — it only lets the submission
through to a provider, which is what actually decides. The bundled provider
returns `pending` and nothing else, on purpose: **an auto-approving stub would be
worse than none**, because the rest of the code would then be enforcing a check
that never happened. An operator approves through the control plane until a real
IDV vendor is connected.

Publishing 18+ content requires a verified author; switching a channel to 18+
requires the same.

### Getting paid

Withdrawing needs an approved payout account (`PayoutAccount`). That table holds
a name, a country, a status, and the provider's reference — **never a bank
number, government ID, or tax identifier**. Those belong with the payment
provider that is legally equipped to hold them.

A withdrawal moves the money out of the earnings balance and sits at `pending`.
Nothing in this build settles it. When something outside reports back, the
control plane records `paid` or `failed`; a failure returns the money to the
creator and leaves the failed row in place, so the attempt stays on the record.

## Axiome control plane

Axiome is the operator's controller app. Livephoria's side of that link is
`app/services/axiome.py` (outbound) and `app/routers/control.py` (inbound).

**Outbound** — on boot the app POSTs a registration to Axiome, then heartbeats
with live metrics on an interval, and emits events. Every call is best-effort:
a controller that is down logs a warning and is otherwise ignored, because an
outage in the controller must not take the platform down with it.

**Inbound** — `/api/v1/control/*`, authenticated with the `X-Axiome-Key` shared
secret:

| Route | Does |
| --- | --- |
| `GET /control/status` | version, uptime, database reachability, maintenance flag, last outbound results |
| `GET /control/metrics` | users, creators, posts, subscriptions, live count, gross volume, fees — plus the safety and payout backlog (open and urgent reports, open appeals, removed posts, suspended users, pending KYC and payouts) |
| `GET /control/config` | effective settings, secrets excluded |
| `POST /control/maintenance` | kill switch — every route except control and health returns 503 |
| `POST /control/users/{id}/suspend` `…/restore` | freeze or restore an account |
| `GET /control/moderation/reports` | the review queue, urgent first then oldest |
| `POST /control/moderation/reports/{id}` | act on a report — remove, restore, suspend, or dismiss |
| `POST /control/moderation/actions` | act with no report behind it (proactive review, legal order) |
| `GET /control/moderation/appeals` `POST …/{id}` | the appeal queue; upholding one reverses the original action |
| `GET /control/age-verifications` `POST …/{user_id}` | approve or reject an age check |
| `GET /control/kyc` `POST …/{account_id}` | approve or reject a payout account |
| `GET /control/payouts` `POST …/{id}` | settle a withdrawal as paid or failed |

If `AXIOME_CONTROL_KEY` is unset those routes refuse **every** caller, so an
unconfigured deployment is closed rather than open. If `AXIOME_BASE_URL` is
unset the outbound half is inert and the app runs standalone.

**A working controller ships in [`axiome/`](axiome/README.md)** — it implements the
other side of this contract, so the two halves are verified against each other
rather than assumed. It is a standalone FastAPI app: run it next to your own
Axiome, or lift its routes into it.

> The contract itself was defined here, not read off an existing Axiome. If your
> Axiome already speaks a different shape, change the three path constants and the
> payload builders at the top of `app/services/axiome.py` — nothing else in this
> codebase moves.

## Layout

```
app/
├── main.py             # app wiring, maintenance gate, heartbeat loop
├── core/
│   ├── config.py       # typed env config, dev-safe defaults
│   ├── db.py           # async engine + session
│   ├── security.py     # PBKDF2 password hashing, JWT bearer tokens
│   ├── deps.py         # who is calling, and may they
│   └── runtime.py      # in-memory state the control plane flips
├── models/             # SQLAlchemy tables + Pydantic schemas
├── services/
│   ├── access.py       # THE PAYWALL — pure, testable rules
│   ├── payments.py     # balances, fee split, ledger, payout gate, provider seam
│   ├── moderation.py   # reasons, urgency, what a decision does
│   ├── verification.py # age assurance — provider seam, never self-approving
│   ├── catalog.py      # shared read queries and serialization
│   └── axiome.py       # control-plane client
├── routers/            # auth, creators, posts, subscriptions, wallet, live,
│                       # shop, messages, discover, safety, control
└── web/index.html      # the app fans and creators actually use
```

## Deploying

`Dockerfile` runs anywhere. `render.yaml` is a Render Blueprint that provisions a
free Postgres and a web service, generates `JWT_SECRET`, and prompts for the
Axiome values. Set `APP_ENV=production` and the app refuses to boot without a
real `JWT_SECRET` rather than falling back to a dev one.

`DATABASE_URL` accepts the bare `postgres://` URLs hosts hand out and rewrites
them to the async driver.

## Known gaps

Named plainly rather than left to be discovered:

- **No money actually moves.** Top-ups, charges, and payouts are balances in this
  database. No PSP, no card, no bank transfer, no tax handling, chargebacks, or
  refunds. The flows and the gates around them are real; the settlement is not.
- **No identity provider.** Age checks and KYC are real gates with real
  consequences, but a human approves each one. Connect a vendor before launch.
- **No proactive detection.** Moderation is reactive: it acts on reports and on
  direct instruction. There is no hash matching, no classifier, and no scanning
  of uploads — which for the `csam` reason in particular is not sufficient on its
  own, and needs both a detection vendor and a reporting obligation met outside
  this app.
- No recurring billing — subscriptions expire after 30 days and are not renewed.
- No media upload or video streaming; posts and products carry URLs.
- No rate limiting, email verification, or password reset.
- `create_all()` builds the schema on boot; there are no migrations yet.
- The maintenance flag is per-process, so a multi-instance deploy needs Axiome to
  flip each instance (or shared state).
