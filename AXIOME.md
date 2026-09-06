# Livephoria ↔ Axiome

What this app expects of the controller, and what it offers back. Written so the
two halves can be reconciled without reading the code.

**Status: unverified.** These shapes were designed here, not read off a running
Axiome. Treat every payload below as a proposal to be matched against the real
controller, then corrected in `app/services/axiome.py`.

## Outbound — Livephoria calls Axiome

Base URL from `AXIOME_BASE_URL`. `AXIOME_APP_KEY`, if set, is sent as
`Authorization: Bearer <key>`. Every call is fire-and-forget: failures are logged
and dropped, never raised into a request.

### `POST /api/apps/register` — once, on boot

```json
{
  "slug": "livephoria",
  "kind": "creator-platform",
  "name": "Livephoria",
  "version": "0.1.0",
  "public_url": "https://livephoria.example",
  "control_url": "/api/v1/control",
  "capabilities": ["subscriptions", "pay-per-view", "tips", "live-events",
                   "live-tickets", "merch", "digital-downloads",
                   "direct-messages", "creator-earnings"],
  "registered_at": "2026-09-06T12:00:00+00:00"
}
```

`control_url` is the path Axiome calls back on. `capabilities` is how one
controller can drive several different apps without special-casing each.

### `POST /api/apps/heartbeat` — every `AXIOME_HEARTBEAT_SECONDS`

```json
{
  "slug": "livephoria",
  "at": "2026-09-06T12:01:00+00:00",
  "status": "ok",
  "metrics": {
    "users": 7, "creators": 3, "posts": 7,
    "active_subscriptions": 4, "live_now": 2,
    "gross_volume_cents": 3200, "platform_fees_cents": 320
  }
}
```

If metrics can't be collected, `metrics` carries `{"error": "..."}` and the
heartbeat is still sent — a heartbeat that stops is a different signal from one
that reports trouble.

### `POST /api/apps/events` — as things happen

```json
{"slug": "livephoria", "event": "<name>", "at": "...", "data": { }}
```

## Inbound — Axiome calls Livephoria

All under `/api/v1/control`, all requiring header `X-Axiome-Key:
<AXIOME_CONTROL_KEY>`. Wrong key → 401. Key not configured → 503 for everyone.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/status` | health, version, uptime, database, maintenance, last outbound results |
| GET | `/metrics` | the same metrics the heartbeat carries |
| GET | `/config` | effective settings, secrets excluded |
| POST | `/maintenance` | `{"enabled": true, "message": "..."}` — kill switch |
| POST | `/users/{id}/suspend` | freeze an account (it can read, not act) |
| POST | `/users/{id}/restore` | undo a suspension |

`/api/v1/health` stays open and unauthenticated for uptime checks, and keeps
answering during maintenance.

## Changing this to match the real Axiome

1. Path constants at the top of `app/services/axiome.py` (`REGISTER_PATH`,
   `HEARTBEAT_PATH`, `EVENTS_PATH`).
2. Payload builders in `AxiomeClient.register` / `.heartbeat` / `.emit`.
3. Auth header in `AxiomeClient._post`, if Axiome expects something other than a
   bearer token.
4. If Axiome drives apps rather than polling them, the inbound table above is the
   surface to extend — add routes to `app/routers/control.py`; they inherit the
   shared-secret dependency.

Nothing outside those files knows Axiome exists.
