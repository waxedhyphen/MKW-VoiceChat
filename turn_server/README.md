# TURN server

This directory deploys coturn for the ICE fallback path. It is intended for a Linux VPS with a public IPv4 address.

## Deploy

Install Docker and the Docker Compose plugin on the VPS, then copy this directory there.

Create the runtime environment:

```bash
cp .env.example .env
openssl rand -hex 32
```

Put the generated value into `TURN_SECRET`, set `TURN_EXTERNAL_IP` to the VPS public IPv4 address, and set `TURN_REALM` to the TURN hostname or server name.

Start coturn:

```bash
docker compose up -d
```

Allow UDP port `3478` and UDP relay ports `49152-65535` through the VPS firewall and provider firewall.

## Signaling server

The signaling server must use the exact same secret but the secret is never sent to clients.

```text
MKWVC_TURN_HOST=turn.example.com
MKWVC_TURN_PORT=3478
MKWVC_TURN_SECRET=<same secret as TURN_SECRET>
MKWVC_TURN_TTL=3600
```

When a player creates or joins a room, the signaling server generates a short-lived TURN username and password and sends only those temporary credentials to the client. ICE still prefers a direct P2P path and falls back to coturn when required.

For a raw IP deployment, `MKWVC_TURN_HOST` may be the public IPv4 address instead of a DNS name.
