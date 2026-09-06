# Axiome

**One place to add, watch, and drive your apps.**

Axiome is a control plane. Apps register with it, heartbeat metrics to it, and
push events at it; Axiome polls them, shows them on one board, and can reach back
into each one — put it in maintenance, read its queues, call any route on its
control surface.

It is deliberately small and standalone: FastAPI, SQLite by default, no build
step. Run it next to your apps, or lift the routes into an existing controller.

## Run it

```bash
pip install -r requirements-dev.txt
uvicorn app.main:app --reload --port 8200
```

Open <http://127.0.0.1:8200>. On first boot Axiome prints an admin key to the
terminal — paste it into the unlock screen. Set `ADMIN_KEY` in `.env` to keep a
stable one.

```bash
pytest        # 16 tests, no network needed
```

## Adding an app

Click **+ Add app**, give it a name, the app's base URL, and the app's control
key. **Test connection** tells you what it found before you save, and
distinguishes the three failures that actually happen:

- *Nothing answered* — wrong URL, or the app isn't running.
- *Reachable, key rejected* — the app is up but your control key doesn't match its
  `AXIOME_CONTROL_KEY`.
- *Reachable, no control surface* — it answered, but has nothing at
  `/api/v1/control/status`.

Saving works even when the probe fails, because an app that is down now can come
up later. Axiome then hands you an app key, shown once:

```
AXIOME_BASE_URL=http://127.0.0.1:8200
AXIOME_APP_KEY=axk_...
```

Put those in the app's environment and restart it, and the app starts pushing.
That is optional — **polling alone is enough** for an app to appear and stay
current. Push just makes it instant and lets the app send events.

## The two directions

**Apps → Axiome** (`/api/apps/*`, authenticated with the app key):

| Route | Purpose |
| --- | --- |
| `POST /api/apps/register` | announce itself: version, public URL, capabilities |
| `POST /api/apps/heartbeat` | status plus a flat dict of metrics |
| `POST /api/apps/events` | anything worth telling the operator about |

**Axiome → apps** (using the stored control key as `X-Axiome-Key`):

- polls `<base_url>/api/v1/health` and `<control_path>/status` and `/metrics`
- `POST /api/admin/apps/{slug}/maintenance` flips an app's kill switch
- `POST /api/admin/apps/{slug}/action` passes any call through to the app's own
  control surface — which is how one dashboard drives apps that expose different
  things. An app's `capabilities` list says what it has.

## What makes an app connectable

Anything that exposes:

- `GET /api/v1/health` — open, no auth
- `GET <control_path>/status` — behind a shared secret, returning at least
  `{"version": ..., "maintenance": false}`
- `GET <control_path>/metrics` — a flat object of numbers

Livephoria (in the parent directory) implements exactly this; see its
`AXIOME.md`. Metric keys ending in `_cents` are rendered as money, and these keys
raise a "needs attention" banner when non-zero: `urgent_reports`, `open_reports`,
`open_appeals`, `kyc_pending`, `payouts_pending`.

## Security

- The dashboard and every `/api/admin/*` route need `ADMIN_KEY`.
- Apps present an **app key** Axiome issued. Only its SHA-256 hash is stored; the
  plain value is shown once, and rotating invalidates the old one immediately.
- Unknown apps **cannot self-register**. Add them here first. `OPEN_REGISTRATION=true`
  relaxes that for local development only.
- **Control keys are stored in plain text**, because Axiome has to replay them to
  the app. That is the weakest point in this design. In production put them in a
  secret manager or an encrypted column, and don't run Axiome on a shared host.

## Known gaps

- No TLS, rate limiting, or audit trail beyond the event log.
- No users or roles — one admin key, all of it.
- Polling is in-process; two Axiome instances would both poll.
- `create_all()` builds the schema on boot; there are no migrations.
