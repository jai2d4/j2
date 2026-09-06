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
| Verified badges | Manual. `is_verified` is set by an operator, never inferred. |
| Recurring billing | Subscriptions run 30 days from purchase. Nothing renews them automatically yet — there is no billing scheduler. |
| Demo data | `scripts/seed_demo.py` invents creators, posts, and prices. Fictional, for demos only. |

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
pytest        # 40 tests, no network or database needed
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
| `GET /control/metrics` | users, creators, posts, active subscriptions, live count, gross volume, platform fees |
| `GET /control/config` | effective settings, secrets excluded |
| `POST /control/maintenance` | kill switch — every route except control and health returns 503 |
| `POST /control/users/{id}/suspend` `…/restore` | freeze or restore an account |

If `AXIOME_CONTROL_KEY` is unset those routes refuse **every** caller, so an
unconfigured deployment is closed rather than open. If `AXIOME_BASE_URL` is
unset the outbound half is inert and the app runs standalone.

> **The request shapes are this app's proposal, not a contract read off Axiome.**
> Nothing here has been verified against a running Axiome instance. When the real
> endpoints are known, change the three path constants and the payload builders
> at the top of `app/services/axiome.py` — nothing else in the codebase moves.

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
│   ├── payments.py     # balances, fee split, ledger, provider seam
│   ├── catalog.py      # shared read queries and serialization
│   └── axiome.py       # control-plane client
├── routers/            # auth, creators, posts, subscriptions, wallet,
│                       # live, shop, messages, discover, control
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

- No real payments, payouts, KYC, tax handling, chargebacks, or refunds.
- No recurring billing — subscriptions expire after 30 days and are not renewed.
- No media upload or video streaming; posts and products carry URLs.
- No content moderation, age verification, or report/appeal flow. A platform in
  this category needs all three before it takes a real payment.
- No rate limiting, email verification, or password reset.
- `create_all()` builds the schema on boot; there are no migrations yet.
- The maintenance flag is per-process, so a multi-instance deploy needs Axiome to
  flip each instance (or shared state).
