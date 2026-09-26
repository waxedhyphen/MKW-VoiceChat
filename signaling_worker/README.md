# Cloudflare signaling worker

This is the no-rental public signaling deployment for the standalone MKW VoiceChat client.

The standalone client coalesces room changes locally and then sends one stable room command:

- `CREATE <MEMBER_ID> <NAME_HEX>`
- `JOIN <CODE> <MEMBER_ID> <NAME_HEX>`
- `LEAVE`
- `SIGNAL\n<ICE bundle>`
- `ICE_SERVERS`
- `ROOM <CODE>`
- `JOINED <CODE>`
- `LEFT`
- `PEER_READY`
- `PEER_LEFT`
- `ERROR ...`

The Worker also accepts the newer `STATE ...` command shape, but the standalone client deliberately uses the stable CREATE/JOIN/LEAVE protocol after batching so existing deployments remain compatible.

The Worker routes WebSockets into a Durable Object using the WebSocket Hibernation API. Room membership is stored in each WebSocket attachment so the Durable Object can hibernate without dropping room state. Rooms have no owner after creation: CREATE only allocates a room code, and the room remains valid while at least one member is still connected.

The public deployment applies layered server-side abuse controls:

- At most 8 new signaling WebSocket connections per IP per minute at the Worker edge.
- Every inbound WebSocket frame is counted, including unknown commands and binary frames.
- At most 60 inbound WebSocket frames per IP per minute through the generic Cloudflare rate-limit binding.
- More than 24 inbound frames in one WebSocket session/minute hard-closes that session.
- At most 20 room commands per IP per minute.
- Up to 8 room commands per session/minute are accepted; 9-16 are soft-rejected and more than 16 hard-closes the session.
- At most 20 ICE SIGNAL messages per IP per minute.
- More than 12 ICE SIGNAL messages per session/minute hard-closes the session.
- At most 4 ICE SIGNAL messages for one room membership.
- Repeated JOIN for the room already joined and LEAVE while already outside a room are idempotent no-ops instead of state transitions.
- Signaling payloads are capped at 128 KiB.

Rate limiting is intentionally split into soft and hard limits. A normal room-command burst is first rejected while the WebSocket stays alive; only clearly sustained per-session abuse closes that one signaling WebSocket. A closed WebSocket is not a ban: the client may reconnect, subject to the temporary connection/IP rate window. The per-session counters live in the WebSocket attachment, so they survive Durable Object hibernation. IP limits are deliberately looser because multiple legitimate players can share one public IP.

These Worker-side rate limits protect the Durable Object and signaling state, but they run after a Worker request has already reached the script. They therefore do not by themselves make the Workers Free daily request quota impossible to exhaust with hostile raw HTTP/WebSocket connection attempts.

Room changes are coalesced before they reach Cloudflare with a trailing-edge debounce. Every local room-state change restarts a one-second quiet timer, and only the final desired state is sent after one full second without another change. Continuous button spam therefore produces no room command at all until the spam stops, at which point one final state command is sent. When the user does nothing, the client sends no periodic room-state heartbeat. Peer-presence notifications are also coalesced for one second inside the Durable Object so a rapid leave/rejoin/leave sequence emits only the final state to the remaining member.

## First deployment

A free Cloudflare account is sufficient. No custom domain is required.

```bat
cd signaling_worker
npm install
npx wrangler login
npm run deploy
```

Wrangler prints the deployed `workers.dev` URL. Use the same URL with `wss://` in the MKW VoiceChat Signaling server field.

Example shape:

```text
wss://mkw-voicechat-signaling.<account-subdomain>.workers.dev
```

The exact account subdomain is assigned by Cloudflare.

The health endpoint is:

```text
https://mkw-voicechat-signaling.<account-subdomain>.workers.dev/health
```

## Local development

```bat
cd signaling_worker
npm install
npm run dev
```

Wrangler prints the local URL. Connect the client to the corresponding `ws://` address.

## TURN

TURN is optional. With no TURN configuration the Worker supplies only the existing Google STUN server and direct ICE continues to work.

Static development TURN URLs can be placed in `MKWVC_TURN_URLS` separated by semicolons.

For coturn REST credentials set `MKWVC_TURN_HOST`, `MKWVC_TURN_PORT` and `MKWVC_TURN_TTL` in `wrangler.jsonc`, then store the shared secret separately:

```bat
npx wrangler secret put MKWVC_TURN_SECRET
```

Do not commit the TURN shared secret.


## Trust boundary

The standalone executable is never trusted as a security boundary. A user can patch it, call the WebSocket protocol directly, or replace it completely. Client-side batching exists only to reduce legitimate traffic and accidental spam.

All security-relevant limits and state validation therefore also exist on the Cloudflare side. No server secret may ever be embedded in the executable. Production Retro Rewind admission must additionally authenticate a live RR identity before allowing normal room signaling and must rate-limit by the verified participant identity rather than by a client-supplied identifier.

Cloudflare's Worker Rate Limiting API executes after a Worker request has started and its counters are intentionally eventually consistent and location-local. It is useful defense in depth, not a mathematical guarantee that a hostile distributed source can never consume a Free-plan quota. Cloudflare's automatic DDoS protection provides another outer layer, but low-rate application abuse must still be handled by application authentication and server-side limits.


The standalone client sends the stable `LEAVE`, `CREATE` and `JOIN <CODE>` commands after local debouncing. The Worker treats those commands as atomic desired-state replacements: CREATE replaces any stale server-side membership instead of depending on a separate LEAVE arriving first, and failed JOINs clear stale prior membership so the client and server cannot remain indefinitely desynchronized. The Worker still accepts `STATE ...` commands for compatibility, but the standalone client does not depend on them.

A final-state request has a five-second acknowledgement timeout. If the Worker/WebSocket stalls, the client retries the same final state on the existing socket up to two times. It replaces the socket only after an actual send failure/close or bounded protocol-desync recovery. It then stops and shows an error instead of waiting forever. Normal idle clients generate no traffic.


## Deployment generation

The current signaling generation is `roomstate-v7` and routes new WebSocket connections to Durable Object hub key `global-roomstate-v7`. This is intentional: incompatible room-state-machine changes can otherwise leave long-lived WebSockets attached to an older deployment. Changing the generation gives new connections a clean room namespace while old sockets drain naturally.

The health endpoint reports the active generation. After deployment, verify that `/health` returns `"version":"roomstate-v7"` before testing a rebuilt client.


## Abuse test harness

`abuse_test.mjs` intentionally bypasses the standalone client's one-second debounce and talks directly to the project signaling WebSocket. It is bounded so a test invocation cannot accidentally burn a large part of the free quota.

Run from `signaling_worker`:

```bat
npm.cmd run abuse-test -- health
npm.cmd run abuse-test -- room-spam --count 40
npm.cmd run abuse-test -- signal-spam --count 10
npm.cmd run abuse-test -- unknown-spam --count 40
npm.cmd run abuse-test -- binary-spam --count 40
npm.cmd run abuse-test -- mixed-spam --count 40
npm.cmd run abuse-test -- reconnect-spam --count 12 --interval 100
```

Production tests are restricted to this project's workers.dev endpoint. Add `--local` to target `ws://127.0.0.1:8787`.

Wait at least 60 seconds between production abuse modes so the IP-level one-minute windows from one mode do not contaminate the next mode.

Expected results:

- `room-spam`: server closes the abusive session with WebSocket code 1008 after the per-session hard room limit.
- `signal-spam`: the fifth SIGNAL within one room membership closes the sender with code 1008.
- `unknown-spam`: unknown commands cannot bypass protection; the generic inbound-frame limit closes the session.
- `binary-spam`: binary frames cannot bypass protection; they count toward the generic inbound-frame limit.
- `mixed-spam`: generic or command-specific limits close the abusive session.
- `reconnect-spam`: reports how many WebSocket upgrades were opened/rejected. Cloudflare's rate-limit binding is eventually consistent, so this mode reports the observed cutoff rather than asserting an exact connection number.


## Peer-owned room lifecycle

Standalone room codes model a room, not a host.

- CREATE allocates a new code and joins the requester as the first equal member.
- JOIN adds another equal member if the room exists and has space.
- If either member leaves or loses its signaling socket, only that member is removed.
- The other member keeps the same room code and can accept a reconnecting or replacement peer.
- The room disappears naturally only when no connected member remains.
- Reconnecting clients JOIN the previous code instead of creating a replacement room.
- ICE offer/answer roles are temporary negotiation roles only. They do not imply room ownership or a gameplay host.

This is intentionally not a host-migration design. No participant inherits authority from another participant; the Worker simply tracks equal room membership.


## Reconnect identity

Each standalone client process generates a random 128-bit logical member identity and keeps it stable across signaling WebSocket reconnects. The value is not an authentication credential and gives no room authority; it exists only to recognize that a new WebSocket is replacing the same logical room member.

When a reconnecting member sends `JOIN <CODE> <MEMBER_ID>`, the Worker checks the room for an older socket carrying the same member ID. If one exists, the Worker clears and closes that stale socket before admitting the replacement. This prevents a half-dead WebSocket that Cloudflare has not detected as closed yet from making the two-peer room appear full.

A different member ID cannot replace an occupied slot. Legacy CREATE/JOIN commands without a member ID remain accepted for the direct abuse harness, but the official client always sends the member ID.


## Peer metadata

The standalone client includes its temporary display name in CREATE/JOIN. The name is UTF-8 hex encoded so whitespace and non-ASCII names do not alter command tokenization.

For each signaling WebSocket, the public Worker derives a two-letter country code from Cloudflare IP geolocation (`request.cf.country`) and passes it into the Durable Object. The client does not choose or claim its own country value.

When two room members are paired, the Worker sends:

```text
PEER_INFO
<MEMBER_ID>
<COUNTRY>
<DISPLAY_NAME>
```

This metadata is presentation-only. It does not authorize room membership and the temporary display name must not become a production identity key.

The current standalone UI can therefore list the connected peer display name and IP-derived country. The final Mario Kart integration will replace the temporary config display name with the name associated with the verified MKW/RR license/profile.


### Retro Rewind authorization

Public GPSP TCP is not a usable production dependency. Runtime testing of the actual RR endpoint `gpsp.gs.play.rwfc.net:29901` from a normal client network resolved DNS and pinged successfully, but TCP port 29901 was unreachable.

The Worker therefore has two deliberately separate paths:

- `RR_AUTH`: secure path; fail-closed unless a trusted HTTPS verifier is configured.
- `RR_DEBUG_LOOKUP <pid>`: public-roster development lookup; never authenticates the PID and never authorizes voice membership.

For the secure path, configure a trusted RR-side HTTPS verifier using secrets:

```powershell
npx wrangler secret put MKWVC_RR_VERIFY_URL
npx wrangler secret put MKWVC_RR_VERIFY_SECRET
```

The configured endpoint must validate the current RR GPCM session server-side and return `{"valid":true,"profileId":"..."}` or `{"valid":false,"profileId":null}`.

For current room/lifecycle development without that endpoint:

```powershell
npm.cmd run abuse-test -- health
npm.cmd run abuse-test -- rr-roster-debug --pid <activeProfileId>
```

The health response reports `roomstate-v13-rr-roster-debug`. A successful roster debug test returns `RR_DEBUG_STATUS`; this proves only that the PID was found through the public room feed.

Do not use `RR_DEBUG_LOOKUP` as a production trust decision.


## Authenticated RR voice admission

The production RR path uses a separate admission step after `RR_AUTH`:

```text
RR_AUTH <profileid> <sesskey> <gamename>
RR_STATUS
RR_ADMIT <roomInstanceId>
ICE_SERVERS
RR_ADMITTED <MEMBER_ID>
<roomInstanceId>
```

`RR_ADMIT` is accepted only while the socket has a fresh server-verified RR identity and only when a new authoritative RWFC lookup still resolves that verified PID to the exact requested `roomInstanceId`. Public `RR_DEBUG_LOOKUP` / `RR_DEBUG_PRESENCE` state cannot satisfy this gate.

Authenticated RR voice membership is separate from manual six-character rooms. Peer introductions are scoped to the exact RR room generation. The Worker sends `RR_PEER_INFO` with the server-bound signaling member ID, verified RR participant ID and roomInstanceId before `PEER_READY`. ICE `SIGNAL` relay is allowed only between members of that same authenticated RR voice room.

Admitted clients refresh authoritative room state with `RR_SYNC` every 30 seconds and use the existing liveness heartbeat. Room changes, auth expiry, failed revalidation, socket loss or reconnect revoke RR voice membership and emit `PEER_LEFT` to unaffected peers.

Current signaling generation: `roomstate-v15-rr-admission`. Current Durable Object hub: `global-roomstate-v10`.


## Unverified RR development admission

`RR_DEV_ADMIT <pid>` is the current no-RR-access development path. The Worker resolves that PID through the official public RWFC roster once at admission, derives the current room instance server-side and returns the room snapshot in `RR_DEV_ADMITTED`. This does not prove that the connecting client owns that PID, so the path is explicitly UNVERIFIED and separate from production `RR_AUTH` / `RR_ADMIT`.

Development peer introductions use `RR_DEV_PEER_INFO`. The payload includes the RR display name and public friend-code field. SIGNAL relay is scoped to the resolved development room instance and cannot target production-authorized RR membership.

WiiCompiled uses this same WebSocket for room admission, peer introduction and ICE signaling; the previous separate RR-debug presence WebSocket is no longer started. There is no automatic 30-second `RR_SYNC`. Admitted clients use the existing one-minute ALIVE cadence, normal room transitions are driven by WiiCompiled/RKNet lifecycle, and audio remains direct P2P.

Current signaling generation: `roomstate-v17-single-socket-rr-voice`; hub: `global-roomstate-v11`.
