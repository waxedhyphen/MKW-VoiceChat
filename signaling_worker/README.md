# Signaling Worker

Production signaling service for MKW VoiceChat, running on Cloudflare Workers + Durable Objects. It matches players into voice rooms, relays ICE offers/answers and hands out STUN/TURN servers. Voice audio never passes through it; audio goes directly peer-to-peer or through TURN.

All WebSockets are routed into one Durable Object (`SignalingHub`) using the WebSocket Hibernation API. Per-socket state lives in the WebSocket attachment, so it survives hibernation.

## Deploy

A free Cloudflare account and the `workers.dev` subdomain are enough.

```sh
cd signaling_worker
npm install
npx wrangler login
npm run deploy
```

`https://<name>.<subdomain>.workers.dev/health` returns the service status, `version` (Worker generation) and `rrVerificationMode`.

The production endpoint `wss://mkw-voicechat-signaling.mkwvoicechat.workers.dev` is compiled into the clients (`patcher/payload/runtime/voicechat/src/EmbeddedCore.cpp`, `patcher/payload/runtime/src/retro_rewind_voice_bridge.cpp`) and used by `abuse_test.mjs`. A self-hosted deployment has to change it there as well.

Local development runs on `ws://127.0.0.1:8787`:

```sh
npm run dev
```

## Configuration

Variables in `wrangler.jsonc`:

| Variable | Purpose |
|---|---|
| `MKWVC_TURN_HOST`, `MKWVC_TURN_PORT`, `MKWVC_TURN_TTL` | coturn REST credentials, see [`turn_server/`](../turn_server/) |
| `MKWVC_TURN_URLS` | Static TURN URLs separated by `;`, development only |
| `MKWVC_DEBUG_LOGS` | `1` enables structured debug logs |

Secrets, never commit them:

```sh
npx wrangler secret put MKWVC_TURN_SECRET
npx wrangler secret put MKWVC_RR_VERIFY_URL
npx wrangler secret put MKWVC_RR_VERIFY_SECRET
```

Without TURN configuration, clients only get `stun:stun.l.google.com:19302`. Without the verifier secrets, the authenticated RR path stays disabled.

## Protocol

Text frames over one WebSocket per client.

### Retro Rewind voice (used by the game)

| Client command | Meaning |
|---|---|
| `RR_DEV_ADMIT <pid> [<roomInstanceId>] [PIDS=<pid,...>]` | Join the voice room of the RR room containing `<pid>`, resolved from the public RWFC roster (`https://rwfc.net/api/wfc/groups`, cached 5 s). `PIDS=` is the local RKNet player list, used when the roster has no match. |
| `RR_DEBUG_PRESENCE <pid>` / `RR_DEBUG_PRESENCE_CLEAR` | Mark or unmark a PID as a voice user in roster snapshots (the `[Voice Chat]` tag in game). |
| `RR_DEBUG_LOOKUP <pid>` | One-off roster lookup. |
| `ALIVE` | Heartbeat. |

Replies: `RR_DEV_ADMITTED`, `RR_DEV_ADMIT_FAIL <reason>`, `RR_DEBUG_STATUS`, `RR_DEV_PEER_INFO`, `ICE_SERVERS`, `PEER_READY <memberId>`, `PEER_LEFT <memberId>`.

> [!WARNING]
> `RR_DEV_ADMIT` does **not** prove that the client owns the PID. Anyone can claim a PID from the public roster and join that voice room. The game still uses this path because RR GPSP is not reachable for verification, see [`rr_verifier/`](../rr_verifier/).

### Authenticated Retro Rewind voice (inactive)

`RR_AUTH <pid> <sesskey> <gamename>` → `RR_STATUS` or `RR_AUTH_FAIL`, then `RR_ADMIT <roomInstanceId>` → `RR_ADMITTED` or `RR_ADMIT_FAIL`, refreshed with `RR_SYNC`. Verified identities expire after 10 minutes. Every `RR_AUTH` fails unless `MKWVC_RR_VERIFY_URL` and `MKWVC_RR_VERIFY_SECRET` are set.

### Manual rooms (standalone test client)

`CREATE [<memberId> <nameHex>]` → `ROOM <code>`, `JOIN <code> [<memberId> <nameHex>]` → `JOINED <code>`, `LEAVE` → `LEFT`. `STATE ...` is an equivalent desired-state form. Rooms have no owner and exist while at least one member is connected. Peers receive `PEER_INFO` with member ID, Cloudflare-derived country and display name.

### ICE relay

`SIGNAL <targetMemberId>\n<ICE bundle>` is relayed to one member of the same room. `SIGNAL\n<ICE bundle>` works only when exactly one other member is present. Frames are capped at 128 KiB.

### Liveness

Room members must send `ALIVE`. A sweep every 30 s closes sockets without a heartbeat for 90 s, and the remaining peers receive `PEER_LEFT`.

## Rate limits

Cloudflare rate-limit bindings, per IP (or per verified RR identity where available) and minute:

| Binding | Limit |
|---|---|
| New WebSocket connections | 64 |
| Inbound frames | 512 |
| Room commands | 96 |
| ICE signals | 384 |

Per WebSocket session and minute: more than 160 frames closes the socket, room commands are rejected above 16 and the socket is closed above 32, more than 96 ICE signals closes the socket. A room membership allows at most 64 ICE signals, a room at most 12 members. A closed socket is not a ban, the client can reconnect.

These limits protect the Durable Object, not the Workers Free daily request quota. Hostile connection attempts can still exhaust it.

## Generations

`WORKER_VERSION` (reported by `/health`) and `HUB_INSTANCE` in `src/index.js` identify the deployment. Change `HUB_INSTANCE` for incompatible room-state changes: new connections then land in a fresh Durable Object while old sockets drain.

## Abuse test harness

`abuse_test.mjs` talks to the WebSocket directly and bypasses all client-side batching. It targets production unless `--local` is passed.

```sh
npm run abuse-test -- <mode> [--count N] [--interval MS] [--local]
```

Modes: `health`, `room-spam`, `signal-spam`, `multi-peer`, `liveness-timeout`, `liveness-steady`, `rr-invalid-auth`, `rr-roster-debug --pid <pid>`, `unknown-spam`, `binary-spam`, `mixed-spam`, `reconnect-spam`.

Wait at least 60 s between production runs so the per-minute windows don't overlap.
