# TURN server

coturn deployment for the relay fallback, used when no direct peer-to-peer path works. Needs a Linux VPS with a public IPv4 address, Docker and the Docker Compose plugin.

## Deploy

```sh
cp .env.example .env
openssl rand -hex 32
```

In `.env`, set `TURN_SECRET` to the generated value, `TURN_EXTERNAL_IP` to the public IPv4 address and `TURN_REALM` to the TURN hostname.

```sh
docker compose up -d
```

Open UDP `3478` and the relay range UDP `49152-65535` in both the VPS firewall and the provider firewall.

## Connecting the signaling Worker

In `signaling_worker/wrangler.jsonc`, set `MKWVC_TURN_HOST` (hostname or public IPv4), `MKWVC_TURN_PORT` (`3478`) and `MKWVC_TURN_TTL` (seconds), then store the same secret:

```sh
npx wrangler secret put MKWVC_TURN_SECRET
```

The Python development server in `signaling_server/` reads the same four values as environment variables.

The secret never leaves the server. For every admission the signaling server derives a short-lived TURN username and password and sends only those to the client. ICE still prefers a direct path and only relays through coturn when needed.
