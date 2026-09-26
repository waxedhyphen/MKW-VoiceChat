# Retro Rewind session verifier

This is the small non-Cloudflare bridge used by MKW VoiceChat to validate a live Retro Rewind GPCM session.

This reference bridge is retained only for environments with privileged/network access to RR GPSP. Runtime testing from a normal public client network showed that `gpsp.gs.play.rwfc.net:29901` is not publicly reachable, so running this verifier on an ordinary PC/VPS does not solve production authentication.

It does **not** handle voice traffic, room signaling, ICE, TURN or player rosters.

## Security contract

The public endpoint is:

`POST /verify`

Required header:

`Authorization: Bearer <MKWVC_RR_VERIFY_SECRET>`

JSON request:

```json
{
  "profileId": "123456789",
  "sessionKey": "12345678",
  "gameName": "mariokartwii"
}
```

Successful validation response:

```json
{
  "valid": true,
  "profileId": "123456789"
}
```

Rejected live-session response:

```json
{
  "valid": false,
  "profileId": null
}
```

The service never logs the request body or session key. The shared secret must be long/random and must not be committed to the repository.

## Local test

Use Node.js 20 or newer.

PowerShell:

```powershell
$env:MKWVC_RR_VERIFY_SECRET="replace-with-a-long-random-test-secret"
npm.cmd start
```

In a second PowerShell window:

```powershell
$env:MKWVC_RR_VERIFY_SECRET="replace-with-a-long-random-test-secret"
npm.cmd run smoke
```

The smoke test deliberately submits PID/session key `1/1` and must return `PASS` with `valid=false`. That proves the machine running the verifier can reach the real RR GPSP TCP endpoint.

## Production

Do not assume a normal public host can use this bridge. It is only suitable if deployed somewhere that is actually allowed to reach RR GPSP TCP. The preferred production solution is a small RR-side HTTPS verification endpoint that calls the existing GPCM session validator internally.

Set:

- `MKWVC_RR_VERIFY_SECRET` to a long random value.
- `PORT` if required by the host.
- optionally `MKWVC_RR_GPSP_HOST`, `MKWVC_RR_GPSP_PORT`, and `MKWVC_RR_VERIFY_TIMEOUT_MS`.

Then configure the Cloudflare signaling Worker with the public HTTPS `/verify` URL and the exact same shared secret.


## Temporary end-to-end test with a tunnel

For development, the verifier can run on the test PC while a temporary HTTPS tunnel exposes it to the signaling Worker. This is useful for proving the full Cloudflare -> HTTPS verifier -> RR GPSP path before choosing a permanent host.

1. Start the verifier locally on port 8788.
2. Expose `http://127.0.0.1:8788` through any trusted HTTPS tunnel.
3. Put the resulting public URL plus `/verify` into the Worker's `MKWVC_RR_VERIFY_URL` secret.
4. Put the same verifier secret into the Worker's `MKWVC_RR_VERIFY_SECRET` secret.
5. Deploy the Worker and run `npm.cmd run abuse-test -- rr-invalid-auth`.

A temporary tunnel is only for development. Production authorization must not depend on one developer PC being online.
