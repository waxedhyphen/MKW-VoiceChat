import argparse
import asyncio
import base64
import hashlib
import hmac
import os
import secrets
import time
import urllib.parse

import websockets

ALPHABET="ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
ROOMS={}
STATIC_ICE_SERVERS=[]
TURN_REST=None
PRESENCE_TASKS={}
PRESENCE_DEBOUNCE_SECONDS=1.0

def make_code():
    while True:
        code="".join(secrets.choice(ALPHABET) for _ in range(6))
        if code not in ROOMS:
            return code

def valid_code(code):
    return len(code)==6 and all(ch in ALPHABET for ch in code)

async def send_safe(socket,message):
    try:
        await socket.send(message)
        return True
    except Exception:
        return False

def make_turn_rest_url():
    if TURN_REST is None:
        return None

    expires=int(time.time())+TURN_REST["ttl"]
    username=f"{expires}:mkwvc-{secrets.token_hex(8)}"
    digest=hmac.new(
        TURN_REST["secret"].encode("utf-8"),
        username.encode("utf-8"),
        hashlib.sha1
    ).digest()
    password=base64.b64encode(digest).decode("ascii")
    encoded_user=urllib.parse.quote(username,safe="")
    encoded_password=urllib.parse.quote(password,safe="")
    return f"turn:{encoded_user}:{encoded_password}@{TURN_REST['host']}:{TURN_REST['port']}"

def ice_servers_message():
    servers=["stun:stun.l.google.com:19302",*STATIC_ICE_SERVERS]
    turn_url=make_turn_rest_url()
    if turn_url:
        servers.append(turn_url)
    return "ICE_SERVERS\n"+"\n".join(servers)

async def schedule_presence(room_code):
    if room_code in PRESENCE_TASKS:
        return

    async def flush():
        try:
            await asyncio.sleep(PRESENCE_DEBOUNCE_SECONDS)
            room=ROOMS.get(room_code)
            if room is None:
                return
            creator=room["creator"]
            if creator is None:
                return
            await send_safe(creator,"PEER_READY" if room["guest"] is not None else "PEER_LEFT")
        finally:
            PRESENCE_TASKS.pop(room_code,None)

    PRESENCE_TASKS[room_code]=asyncio.create_task(flush())

async def handler(socket,path=None):
    room_code=None
    role=None

    async def leave_membership():
        nonlocal room_code,role

        if room_code is None:
            return

        old_code=room_code
        old_role=role
        room=ROOMS.get(old_code)
        room_code=None
        role=None

        if room is None:
            return

        if old_role=="creator" and room["creator"] is socket:
            guest=room["guest"]
            ROOMS.pop(old_code,None)
            if guest is not None:
                await send_safe(guest,"PEER_LEFT")
        elif old_role=="guest" and room["guest"] is socket:
            room["guest"]=None
            await schedule_presence(old_code)

    async def apply_state(state_text):
        nonlocal room_code,role

        normalized=state_text.strip().upper()
        mode=normalized
        code=None

        if normalized.startswith("JOIN "):
            mode="JOIN"
            code=normalized[5:].strip()
            if not valid_code(code):
                await leave_membership()
                await send_safe(socket,"ERROR Room not found")
                return
        elif mode not in ("NONE","CREATE"):
            await send_safe(socket,"ERROR Unknown room state")
            return

        await leave_membership()

        if mode=="NONE":
            await send_safe(socket,"LEFT")
            return

        if mode=="CREATE":
            room_code=make_code()
            role="creator"
            ROOMS[room_code]={"creator":socket,"guest":None}
            await send_safe(socket,ice_servers_message())
            await send_safe(socket,"ROOM "+room_code)
            return

        room=ROOMS.get(code)
        if room is None:
            await send_safe(socket,"ERROR Room not found")
            return
        if room["guest"] is not None:
            await send_safe(socket,"ERROR Room is full")
            return

        room_code=code
        role="guest"
        room["guest"]=socket
        await send_safe(socket,ice_servers_message())
        await send_safe(socket,"JOINED "+code)
        await schedule_presence(code)

    try:
        async for message in socket:
            if not isinstance(message,str):
                continue

            if message.startswith("STATE "):
                await apply_state(message[6:])
                continue

            if message=="CREATE":
                await apply_state("CREATE")
                continue

            if message.startswith("JOIN "):
                await apply_state(message)
                continue

            if message=="LEAVE":
                await apply_state("NONE")
                continue

            if message.startswith("SIGNAL\n"):
                if room_code is None:
                    await send_safe(socket,"ERROR Join a room first")
                    continue

                room=ROOMS.get(room_code)
                if room is None:
                    await send_safe(socket,"ERROR Room no longer exists")
                    continue

                target=room["guest"] if role=="creator" else room["creator"]
                if target is None:
                    await send_safe(socket,"ERROR Peer has not joined yet")
                    continue

                if not await send_safe(target,message):
                    await send_safe(socket,"ERROR Peer disconnected")
                continue

            await send_safe(socket,"ERROR Unknown command")
    finally:
        await leave_membership()

async def main():
    global TURN_REST

    parser=argparse.ArgumentParser()
    parser.add_argument("--host",default="0.0.0.0")
    parser.add_argument("--port",type=int,default=8765)
    parser.add_argument("--turn",action="append",default=[])
    parser.add_argument("--turn-host",default=os.environ.get("MKWVC_TURN_HOST",""))
    parser.add_argument("--turn-port",type=int,default=int(os.environ.get("MKWVC_TURN_PORT","3478")))
    parser.add_argument("--turn-secret",default=os.environ.get("MKWVC_TURN_SECRET",""))
    parser.add_argument("--turn-ttl",type=int,default=int(os.environ.get("MKWVC_TURN_TTL","3600")))
    args=parser.parse_args()

    STATIC_ICE_SERVERS.extend(args.turn)
    env_turn=os.environ.get("MKWVC_TURN_URLS","")
    if env_turn:
        STATIC_ICE_SERVERS.extend(value.strip() for value in env_turn.split(";") if value.strip())

    for url in STATIC_ICE_SERVERS:
        if not url.startswith(("turn:","turns:")):
            raise SystemExit("TURN URLs must start with turn: or turns:")

    if bool(args.turn_host)!=bool(args.turn_secret):
        raise SystemExit("--turn-host and --turn-secret must be configured together")
    if args.turn_port<1 or args.turn_port>65535:
        raise SystemExit("--turn-port must be between 1 and 65535")
    if args.turn_ttl<60:
        raise SystemExit("--turn-ttl must be at least 60 seconds")

    if args.turn_host:
        TURN_REST={
            "host":args.turn_host,
            "port":args.turn_port,
            "secret":args.turn_secret,
            "ttl":args.turn_ttl
        }

    async with websockets.serve(handler,args.host,args.port,max_size=128*1024,ping_interval=20,ping_timeout=20):
        turn_status="enabled" if TURN_REST or STATIC_ICE_SERVERS else "disabled"
        print(f"MKW VoiceChat signaling server listening on {args.host}:{args.port} (TURN {turn_status})")
        await asyncio.Future()

if __name__=="__main__":
    asyncio.run(main())
