# Retro Rewind session verifier

Small Node.js service that checks whether a Retro Rewind GPCM session (profile ID + session key) is valid by asking RR GPSP directly. The signaling Worker would use it for authenticated voice admission (`RR_AUTH`).

> [!IMPORTANT]
> **Not used in production.** RR GPSP (`gpsp.gs.play.rwfc.net:29901`) is not reachable from normal networks, so running this on a PC or VPS does not work. It only helps if it runs somewhere with access to GPSP, ideally as a verification endpoint on the RR side. Until then, voice rooms use the unverified admission path described in [`signaling_worker/`](../signaling_worker/).

The verifier only validates sessions. It does not handle voice, signaling, ICE, TURN or rosters, and it never logs request bodies or session keys.

## API

`POST /verify` with `Authorization: Bearer <MKWVC_RR_VERIFY_SECRET>`:

```json
{ "profileId": "123456789", "sessionKey": "12345678", "gameName": "mariokartwii" }
```

Response: `{"valid": true, "profileId": "123456789"}` or `{"valid": false, "profileId": null}`.

`GET /health` returns the service status and the configured GPSP target.

## Run

Node.js 20 or newer.

| Variable | Default |
|---|---|
| `MKWVC_RR_VERIFY_SECRET` | required, long random value |
| `HOST` / `PORT` | `0.0.0.0` / `8788` |
| `MKWVC_RR_GPSP_HOST` / `MKWVC_RR_GPSP_PORT` | `gpsp.gs.play.rwfc.net` / `29901` |
| `MKWVC_RR_VERIFY_TIMEOUT_MS` | `5000` |

```sh
npm start
```

Smoke test (sends PID/session key `1/1` and expects `valid=false`, which proves GPSP is reachable):

```sh
npm run smoke -- [--url http://127.0.0.1:8788/verify] [--secret <secret>]
```

## Connecting the Worker

Set the public HTTPS URL of `/verify` and the same secret on the Worker:

```sh
npx wrangler secret put MKWVC_RR_VERIFY_URL
npx wrangler secret put MKWVC_RR_VERIFY_SECRET
```

`/health` of the Worker then reports `rrVerificationMode: "https"`. Test with `npm run abuse-test -- rr-invalid-auth` in `signaling_worker/`.
