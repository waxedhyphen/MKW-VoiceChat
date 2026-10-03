import { DurableObject } from "cloudflare:workers";

const ALPHABET="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const MAX_SIGNAL_MESSAGE_BYTES=128*1024;
const PRESENCE_DEBOUNCE_MS=1000;
const WINDOW_MS=60*1000;
const MAX_ROOM_MEMBERS=12;
const SESSION_MESSAGE_HARD_LIMIT=160;
const SESSION_ROOM_SOFT_LIMIT=16;
const SESSION_ROOM_HARD_LIMIT=32;
const SESSION_SIGNAL_HARD_LIMIT=96;
const ROOM_SIGNAL_LIMIT=64;
const ROOM_HEARTBEAT_TIMEOUT_MS=90*1000;
const LIVENESS_SWEEP_MS=30*1000;
const WORKER_VERSION="roomstate-v21-online-roster";
const RR_GROUPS_URL="https://rwfc.net/api/wfc/groups";
const RR_ROSTER_CACHE_MS=5000;
const RR_AUTH_TTL_MS=10*60*1000;
const HUB_INSTANCE="global-roomstate-v11";

function sessionOf(socket) {
    const value=socket.deserializeAttachment();
    if(!value || typeof value!=="object") {
        return {
            sessionId:null,
            ip:null,
            roomCode:null,
            role:null,
            memberId:null,
            displayName:"Player",
            country:"??",
            roomWindowStart:0,
            roomActions:0,
            signalWindowStart:0,
            signalActions:0,
            messageWindowStart:0,
            messageActions:0,
            signalsInRoom:0,
            lastHeartbeat:0,
            rrParticipantId:null,
            rrGameName:null,
            rrVerifiedAt:0,
            rrRoomId:null,
            rrRoomInstanceId:null,
            rrRoomCreated:null,
            rrDebugPresence:false,
            rrVoiceRoomInstanceId:null,
            rrVoiceMemberId:null,
            rrVoiceParticipantId:null,
            rrVoiceDisplayName:null,
            rrVoiceFriendCode:null,
            rrVoiceOpenHost:false,
            rrVoiceMode:null,
            voiceOnline:false,
            voiceProfileId:null,
            voiceDisplayName:null,
            voiceFriendCode:null,
            voiceOpenHost:false
        };
    }

    return {
        sessionId:typeof value.sessionId==="string" ? value.sessionId : null,
        ip:typeof value.ip==="string" ? value.ip : null,
        roomCode:typeof value.roomCode==="string" ? value.roomCode : null,
        role:value.role==="member" ? value.role : null,
        memberId:typeof value.memberId==="string" ? value.memberId : null,
        displayName:typeof value.displayName==="string" && value.displayName ? value.displayName : "Player",
        country:typeof value.country==="string" && value.country ? value.country : "??",
        roomWindowStart:Number.isFinite(value.roomWindowStart) ? value.roomWindowStart : 0,
        roomActions:Number.isInteger(value.roomActions) ? value.roomActions : 0,
        signalWindowStart:Number.isFinite(value.signalWindowStart) ? value.signalWindowStart : 0,
        signalActions:Number.isInteger(value.signalActions) ? value.signalActions : 0,
        messageWindowStart:Number.isFinite(value.messageWindowStart) ? value.messageWindowStart : 0,
        messageActions:Number.isInteger(value.messageActions) ? value.messageActions : 0,
        signalsInRoom:Number.isInteger(value.signalsInRoom) ? value.signalsInRoom : 0,
        lastHeartbeat:Number.isFinite(value.lastHeartbeat) ? value.lastHeartbeat : 0,
        rrParticipantId:typeof value.rrParticipantId==="string" ? value.rrParticipantId : null,
        rrGameName:typeof value.rrGameName==="string" ? value.rrGameName : null,
        rrVerifiedAt:Number.isFinite(value.rrVerifiedAt) ? value.rrVerifiedAt : 0,
        rrRoomId:typeof value.rrRoomId==="string" ? value.rrRoomId : null,
        rrRoomInstanceId:typeof value.rrRoomInstanceId==="string" ? value.rrRoomInstanceId : null,
        rrRoomCreated:typeof value.rrRoomCreated==="string" ? value.rrRoomCreated : null,
        rrDebugPresence:value.rrDebugPresence===true,
        rrVoiceRoomInstanceId:typeof value.rrVoiceRoomInstanceId==="string" ? value.rrVoiceRoomInstanceId : null,
        rrVoiceMemberId:typeof value.rrVoiceMemberId==="string" ? value.rrVoiceMemberId : null,
        rrVoiceParticipantId:typeof value.rrVoiceParticipantId==="string" ? value.rrVoiceParticipantId : null,
        rrVoiceDisplayName:typeof value.rrVoiceDisplayName==="string" ? value.rrVoiceDisplayName : null,
        rrVoiceFriendCode:typeof value.rrVoiceFriendCode==="string" ? value.rrVoiceFriendCode : null,
        rrVoiceOpenHost:value.rrVoiceOpenHost===true,
        rrVoiceMode:value.rrVoiceMode==="production" || value.rrVoiceMode==="development" ? value.rrVoiceMode : null,
        voiceOnline:value.voiceOnline===true,
        voiceProfileId:typeof value.voiceProfileId==="string" ? value.voiceProfileId : null,
        voiceDisplayName:typeof value.voiceDisplayName==="string" ? value.voiceDisplayName : null,
        voiceFriendCode:typeof value.voiceFriendCode==="string" ? value.voiceFriendCode : null,
        voiceOpenHost:value.voiceOpenHost===true
    };
}

function updateSession(socket,changes) {
    socket.serializeAttachment({...sessionOf(socket),...changes});
}

function sendSafe(socket,message) {
    try {
        socket.send(message);
        return true;
    } catch {
        return false;
    }
}

function randomCode() {
    const bytes=new Uint8Array(6);
    crypto.getRandomValues(bytes);
    let code="";
    for(const value of bytes) code+=ALPHABET[value&(ALPHABET.length-1)];
    return code;
}

function validRoomCode(code) {
    return code.length===6 && [...code].every(character=>ALPHABET.includes(character));
}

function validMemberId(memberId) {
    return /^[0-9A-F]{32}$/.test(memberId);
}

function decodeDisplayName(encoded) {
    if(typeof encoded!=="string" || encoded.length===0 || encoded.length>126 || encoded.length%2!==0 || !/^[0-9A-F]+$/.test(encoded)) {
        return "Player";
    }

    const bytes=new Uint8Array(encoded.length/2);
    for(let i=0;i<bytes.length;++i) bytes[i]=Number.parseInt(encoded.slice(i*2,i*2+2),16);

    let name;
    try {
        name=new TextDecoder("utf-8",{fatal:true}).decode(bytes);
    } catch {
        return "Player";
    }

    name=[...name].filter(ch=>ch>=" " && ch!=="\u007F").join("").trim();
    if(!name) return "Player";
    return name.slice(0,32);
}

function peerInfoMessage(socket) {
    const session=sessionOf(socket);
    return "PEER_INFO\n"+(session.memberId??"")+"\n"+session.country+"\n"+session.displayName;
}

function rrPeerInfoMessage(socket) {
    const session=sessionOf(socket);
    const prefix=session.rrVoiceMode==="production" ? "RR_PEER_INFO" : "RR_DEV_PEER_INFO";
    return prefix+"\n"+
        (session.rrVoiceMemberId??"")+"\n"+
        (session.rrVoiceParticipantId??"")+"\n"+
        (session.rrVoiceRoomInstanceId??"")+"\n"+
        session.country+"\n"+
        encodeHexText(session.rrVoiceDisplayName??"Player")+"\n"+
        (session.rrVoiceFriendCode??"")+"\n"+
        (session.rrVoiceOpenHost ? "1" : "0");
}

function parsePositiveInteger(value,fallback,min,max) {
    const parsed=Number.parseInt(value??"",10);
    if(!Number.isFinite(parsed)) return fallback;
    return Math.min(max,Math.max(min,parsed));
}

function base64(bytes) {
    let binary="";
    for(const value of bytes) binary+=String.fromCharCode(value);
    return btoa(binary);
}

async function hmacSha1(secret,value) {
    const encoder=new TextEncoder();
    const key=await crypto.subtle.importKey(
        "raw",
        encoder.encode(secret),
        {name:"HMAC",hash:"SHA-1"},
        false,
        ["sign"]
    );
    return new Uint8Array(await crypto.subtle.sign("HMAC",key,encoder.encode(value)));
}


function validRrProfileId(value) {
    return /^[0-9]{1,10}$/.test(value);
}

function validRrSessionKey(value) {
    return /^-?[0-9]{1,10}$/.test(value);
}

function validRrGameName(value) {
    return /^[A-Za-z0-9_-]{1,32}$/.test(value);
}

function validRrRoomInstanceId(value) {
    return /^[0-9A-F]{64}$/i.test(value);
}

function encodeHexText(value) {
    const bytes=new TextEncoder().encode(String(value??""));
    let result="";
    for(const byte of bytes) result+=byte.toString(16).padStart(2,"0").toUpperCase();
    return result;
}

async function sha256Hex(value) {
    const bytes=new TextEncoder().encode(value);
    const digest=new Uint8Array(await crypto.subtle.digest("SHA-256",bytes));
    return [...digest].map(byte=>byte.toString(16).padStart(2,"0")).join("").toUpperCase();
}

function rrVerifierConfig(env) {
    const rawUrl=String(env.MKWVC_RR_VERIFY_URL??"").trim();
    const secret=String(env.MKWVC_RR_VERIFY_SECRET??"").trim();
    if(!rawUrl || !secret) return null;

    let url;
    try {
        url=new URL(rawUrl);
    } catch {
        throw new Error("RR verifier URL is invalid");
    }

    if(url.protocol!=="https:") {
        throw new Error("RR verifier URL must use HTTPS");
    }

    return {url:url.toString(),secret};
}

async function verifyRrSession(env,profileId,sessionKey,gameName) {
    const config=rrVerifierConfig(env);
    if(!config) {
        throw new Error("Secure RR verification is unavailable until RR exposes a reachable HTTPS verification endpoint");
    }

    const controller=new AbortController();
    const timer=setTimeout(()=>controller.abort(),5000);

    try {
        const response=await fetch(config.url,{
            method:"POST",
            headers:{
                "Accept":"application/json",
                "Content-Type":"application/json",
                "Authorization":"Bearer "+config.secret
            },
            body:JSON.stringify({
                profileId,
                sessionKey,
                gameName
            }),
            signal:controller.signal
        });

        if(response.status===401 || response.status===403) {
            throw new Error("RR verifier rejected signaling credentials");
        }
        if(!response.ok) {
            throw new Error("RR verifier HTTP "+response.status);
        }

        let payload;
        try {
            payload=await response.json();
        } catch {
            throw new Error("RR verifier returned invalid JSON");
        }

        if(!payload || typeof payload.valid!=="boolean") {
            throw new Error("RR verifier response is malformed");
        }

        if(payload.valid===true) {
            const verifiedProfileId=String(payload.profileId??"");
            if(verifiedProfileId!==profileId) {
                throw new Error("RR verifier returned a mismatched profile ID");
            }
        }

        return payload.valid;
    } catch(error) {
        if(error?.name==="AbortError") {
            throw new Error("RR verifier request timed out");
        }
        throw error;
    } finally {
        clearTimeout(timer);
    }
}

export class SignalingHub extends DurableObject {
    constructor(ctx,env) {
        super(ctx,env);
        this.ctx=ctx;
        this.env=env;
        this.presenceTimers=new Map();
        this.rrRosterCache=null;
        this.rrRosterCacheAt=0;
        this.rrRosterFetch=null;
    }

    debug(event,details={}) {
        if(this.env.MKWVC_DEBUG_LOGS!=="1") return;
        console.log(JSON.stringify({event,...details}));
    }

    async fetch(request) {
        if(request.headers.get("Upgrade")?.toLowerCase()!=="websocket") {
            return new Response("Expected WebSocket",{status:426});
        }

        const pair=new WebSocketPair();
        const [client,server]=Object.values(pair);
        const ip=request.headers.get("CF-Connecting-IP")??"unknown";
        const country=(request.headers.get("X-MKWVC-Country")??"??").toUpperCase();
        const sessionId=crypto.randomUUID();
        const now=Date.now();

        this.ctx.acceptWebSocket(server);
        server.serializeAttachment({
            sessionId,
            ip,
            roomCode:null,
            role:null,
            memberId:null,
            displayName:"Player",
            country,
            roomWindowStart:now,
            roomActions:0,
            signalWindowStart:now,
            signalActions:0,
            messageWindowStart:now,
            messageActions:0,
            signalsInRoom:0,
            lastHeartbeat:now,
            rrParticipantId:null,
            rrGameName:null,
            rrVerifiedAt:0,
            rrRoomId:null,
            rrRoomInstanceId:null,
            rrRoomCreated:null,
            rrDebugPresence:false,
            rrVoiceRoomInstanceId:null,
            rrVoiceMemberId:null,
            rrVoiceParticipantId:null,
            rrVoiceDisplayName:null,
            rrVoiceFriendCode:null,
            rrVoiceOpenHost:false,
            rrVoiceMode:null,
            voiceOnline:false,
            voiceProfileId:null,
            voiceDisplayName:null,
            voiceFriendCode:null,
            voiceOpenHost:false
        });

        this.debug("ws_open",{session:sessionId.slice(0,8)});
        return new Response(null,{status:101,webSocket:client});
    }

    sockets() {
        return this.ctx.getWebSockets();
    }


    async rrGroups() {
        const now=Date.now();
        if(this.rrRosterCache && now-this.rrRosterCacheAt<RR_ROSTER_CACHE_MS) {
            return this.rrRosterCache;
        }

        if(this.rrRosterFetch) return this.rrRosterFetch;

        this.rrRosterFetch=(async()=>{
            const response=await fetch(RR_GROUPS_URL,{
                headers:{"Accept":"application/json"}
            });
            if(!response.ok) throw new Error("RWFC roster request failed: "+response.status);

            const groups=await response.json();
            if(!Array.isArray(groups)) throw new Error("RWFC roster response is invalid");

            this.rrRosterCache=groups;
            this.rrRosterCacheAt=Date.now();
            return groups;
        })();

        try {
            return await this.rrRosterFetch;
        } finally {
            this.rrRosterFetch=null;
        }
    }

    async rrRoomForParticipant(participantId) {
        const groups=await this.rrGroups();
        const matches=[];

        for(const group of groups) {
            const players=group?.players??group?.Players??{};
            const entries=Array.isArray(players) ? players : Object.values(players);
            if(!entries.some(player=>String(player?.pid??player?.Pid??"")===participantId)) continue;
            matches.push({group,players:entries});
        }

        if(matches.length===0) return null;
        if(matches.length!==1) throw new Error("RR participant appears in multiple active rooms");

        const {group,players}=matches[0];
        const roomId=String(group?.id??group?.Id??"");
        const created=String(group?.created??group?.Created??"");
        if(!roomId || !created) throw new Error("RWFC room identity is incomplete");

        const roomInstanceId=await sha256Hex(roomId+"\n"+created);
        const roster=players
            .map(player=>({
                pid:String(player?.pid??player?.Pid??""),
                name:String(player?.name??player?.Name??"Player"),
                friendCode:String(player?.fc??player?.Fc??""),
                openHost:String(player?.openhost??player?.Openhost??player?.openHost??player?.OpenHost??"").toLowerCase()==="true"
            }))
            .filter(player=>player.pid);

        return {roomId,created,roomInstanceId,roster};
    }

    developmentRoomFromLocalProfiles(profileId,localProfileIds) {
        const local=new Set(
            (localProfileIds??[])
                .map(value=>String(value))
                .filter(value=>validRrProfileId(value))
        );
        local.add(profileId);

        const candidates=new Map();
        for(const socket of this.sockets()) {
            const session=sessionOf(socket);
            if(session.rrVoiceMode!=="development" ||
               !session.rrVoiceRoomInstanceId ||
               !session.rrVoiceParticipantId ||
               !local.has(session.rrVoiceParticipantId)) {
                continue;
            }

            if(!candidates.has(session.rrVoiceRoomInstanceId)) {
                candidates.set(session.rrVoiceRoomInstanceId,{
                    roomId:session.rrRoomId??"",
                    created:session.rrRoomCreated??"",
                    roomInstanceId:session.rrVoiceRoomInstanceId
                });
            }
        }

        if(candidates.size!==1) return null;
        const room=[...candidates.values()][0];
        if(!room.roomId || !room.created) return null;

        const rosterByPid=new Map();
        for(const socket of this.rrVoiceMembers(room.roomInstanceId,"development")) {
            const session=sessionOf(socket);
            if(!session.rrVoiceParticipantId) continue;
            rosterByPid.set(session.rrVoiceParticipantId,{
                pid:session.rrVoiceParticipantId,
                name:session.rrVoiceDisplayName||session.rrVoiceParticipantId,
                friendCode:session.rrVoiceFriendCode??"",
                openHost:session.rrVoiceOpenHost===true
            });
        }
        for(const pid of local) {
            if(!rosterByPid.has(pid)) {
                rosterByPid.set(pid,{pid,name:pid,friendCode:"",openHost:false});
            }
        }

        return {
            ...room,
            roster:[...rosterByPid.values()]
        };
    }

    sendRrDebugStatus(socket,profileId,room) {
        const lines=[
            profileId??"",
            room?.roomId??"",
            room?.roomInstanceId??"",
            room?.created??"",
            String(room?.roster?.length??0)
        ];

        const voiceIds=this.rrVoiceParticipantIds(room?.roomInstanceId??null);
        for(const player of room?.roster??[]) {
            lines.push(
                player.pid+"\t"+
                (voiceIds.has(player.pid) ? "1" : "0")+"\t"+
                encodeHexText(player.name)+"\t"+
                String(player.friendCode??"")+"\t"+
                (player.openHost ? "1" : "0")
            );
        }

        sendSafe(socket,"RR_DEBUG_STATUS\n"+lines.join("\n"));
    }

    async debugLookupRr(socket,profileId) {
        if(!validRrProfileId(profileId)) {
            sendSafe(socket,"RR_DEBUG_FAIL Invalid Retro Rewind profile ID");
            return;
        }

        try {
            const room=await this.rrRoomForParticipant(profileId);
            this.sendRrDebugStatus(socket,profileId,room);
        } catch(error) {
            this.debug("rr_debug_lookup_error",{
                participant:profileId,
                error:String(error?.message??error),
                session:sessionOf(socket).sessionId?.slice(0,8)??null
            });
            sendSafe(socket,"RR_DEBUG_FAIL RWFC room lookup unavailable");
        }
    }

    rrVoiceParticipantIds(roomInstanceId) {
        const ids=new Set();
        if(!roomInstanceId) return ids;

        for(const socket of this.sockets()) {
            const session=sessionOf(socket);
            if(session.rrRoomInstanceId===roomInstanceId && session.rrParticipantId) {
                ids.add(session.rrParticipantId);
            }
        }
        return ids;
    }

    rrVoiceOnlineSockets() {
        return this.sockets().filter(socket=>sessionOf(socket).voiceOnline===true);
    }

    setVoiceOnlinePresence(socket,online) {
        const current=sessionOf(socket);
        updateSession(socket,{
            voiceOnline:online,
            voiceProfileId:online ? current.voiceProfileId : null,
            voiceDisplayName:online ? current.voiceDisplayName : null,
            voiceFriendCode:online ? current.voiceFriendCode : null,
            voiceOpenHost:online ? current.voiceOpenHost : false,
            lastHeartbeat:Date.now()
        });
        if(current.voiceOnline!==online || online) {
            this.broadcastRrVoiceOnlineCount();
        }
    }

    findVoiceMetadata(profileId) {
        if(!profileId) return null;
        for(const socket of this.sockets()) {
            const session=sessionOf(socket);
            if(session.rrVoiceParticipantId===profileId &&
               session.rrVoiceFriendCode) {
                return {
                    displayName:session.rrVoiceDisplayName??"Player",
                    friendCode:session.rrVoiceFriendCode,
                    openHost:session.rrVoiceOpenHost===true
                };
            }
        }
        return null;
    }

    setVoiceOnlineIdentity(socket,profileId) {
        const current=sessionOf(socket);
        if(!current.voiceOnline) return;

        if(!profileId) {
            updateSession(socket,{
                voiceProfileId:null,
                voiceDisplayName:null,
                voiceFriendCode:null,
                voiceOpenHost:false
            });
            this.broadcastRrVoiceOnlineCount();
            return;
        }

        const metadata=this.findVoiceMetadata(profileId);
        updateSession(socket,{
            voiceProfileId:profileId,
            voiceDisplayName:metadata?.displayName??null,
            voiceFriendCode:metadata?.friendCode??null,
            voiceOpenHost:metadata?.openHost===true
        });
        this.broadcastRrVoiceOnlineCount();
    }

    syncVoicePresenceMetadata(profileId,displayName,friendCode,openHost=false) {
        if(!profileId || !friendCode) return;
        for(const socket of this.rrVoiceOnlineSockets()) {
            const current=sessionOf(socket);
            if(current.voiceProfileId!==profileId) continue;
            updateSession(socket,{
                voiceDisplayName:String(displayName??"Player").slice(0,32),
                voiceFriendCode:String(friendCode).replace(/[\\r\\n\\t]/g,"").slice(0,32),
                voiceOpenHost:openHost===true
            });
        }
    }

    broadcastRrVoiceOnlineCount() {
        const sockets=this.rrVoiceOnlineSockets();
        const countMessage="RR_ONLINE_COUNT "+sockets.length;

        const rosterByProfile=new Map();
        for(const socket of sockets) {
            const session=sessionOf(socket);
            if(!session.voiceProfileId ||
               !session.voiceDisplayName ||
               !session.voiceFriendCode ||
               rosterByProfile.has(session.voiceProfileId)) {
                continue;
            }
            rosterByProfile.set(session.voiceProfileId,{
                profileId:session.voiceProfileId,
                displayName:session.voiceDisplayName,
                friendCode:session.voiceFriendCode,
                openHost:session.voiceOpenHost===true
            });
        }

        const roster=[...rosterByProfile.values()]
            .sort((a,b)=>a.displayName.localeCompare(b.displayName));
        const rosterMessage="RR_ONLINE_ROSTER\n"+roster
            .map(user=>
                user.profileId+"\t"+
                encodeHexText(user.displayName)+"\t"+
                user.friendCode+"\t"+
                (user.openHost ? "1" : "0"))
            .join("\n");

        for(const socket of sockets) {
            sendSafe(socket,countMessage);
            sendSafe(socket,rosterMessage);
        }
    }

    rrVoiceMembers(roomInstanceId,mode=null) {
        if(!roomInstanceId) return [];
        return this.sockets().filter(socket=>{
            const session=sessionOf(socket);
            return session.rrVoiceRoomInstanceId===roomInstanceId &&
                   Boolean(session.rrVoiceMemberId) &&
                   (mode===null || session.rrVoiceMode===mode);
        });
    }

    rrVoiceMemberSocket(roomInstanceId,memberId,mode=null) {
        if(!roomInstanceId || !memberId) return null;
        return this.rrVoiceMembers(roomInstanceId,mode)
            .find(socket=>sessionOf(socket).rrVoiceMemberId===memberId)??null;
    }

    leaveRrVoiceMembership(socket) {
        const session=sessionOf(socket);
        if(!session.rrVoiceRoomInstanceId || !session.rrVoiceMemberId) return;

        const roomInstanceId=session.rrVoiceRoomInstanceId;
        const memberId=session.rrVoiceMemberId;
        const mode=session.rrVoiceMode;
        updateSession(socket,{
            rrVoiceRoomInstanceId:null,
            rrVoiceMemberId:null,
            rrVoiceParticipantId:null,
            rrVoiceDisplayName:null,
            rrVoiceFriendCode:null,
            rrVoiceOpenHost:false,
            rrVoiceMode:null,
            signalsInRoom:0
        });

        if(mode && this.rrVoiceMembers(roomInstanceId,mode).length>0) {
            this.scheduleRrVoicePresence(roomInstanceId,mode,memberId);
        }
        this.broadcastRrVoiceOnlineCount();
    }

    scheduleRrVoicePresence(roomInstanceId,mode,leftMemberId=null,joinedMemberId=null) {
        const timerKey="rr:"+mode+":"+roomInstanceId;
        const existing=this.presenceTimers.get(timerKey);

        if(existing) {
            if(leftMemberId) existing.leftMemberIds.add(leftMemberId);
            if(joinedMemberId) existing.joinedMemberIds.add(joinedMemberId);
            return;
        }

        const entry={leftMemberIds:new Set(),joinedMemberIds:new Set(),timer:null};
        if(leftMemberId) entry.leftMemberIds.add(leftMemberId);
        if(joinedMemberId) entry.joinedMemberIds.add(joinedMemberId);

        entry.timer=setTimeout(()=>{
            this.presenceTimers.delete(timerKey);

            const members=this.rrVoiceMembers(roomInstanceId,mode);
            const currentIds=new Set(
                members.map(member=>sessionOf(member).rrVoiceMemberId).filter(Boolean)
            );

            for(const leftId of entry.leftMemberIds) {
                if(currentIds.has(leftId)) continue;
                for(const member of members) sendSafe(member,"PEER_LEFT "+leftId);
            }

            const processedPairs=new Set();
            for(const joinedId of entry.joinedMemberIds) {
                const joined=this.rrVoiceMemberSocket(roomInstanceId,joinedId,mode);
                if(!joined) continue;

                for(const peer of members) {
                    if(peer===joined) continue;
                    const peerSession=sessionOf(peer);
                    if(!peerSession.rrVoiceMemberId) continue;

                    const pairKey=[joinedId,peerSession.rrVoiceMemberId].sort().join(":");
                    if(processedPairs.has(pairKey)) continue;
                    processedPairs.add(pairKey);

                    sendSafe(joined,rrPeerInfoMessage(peer));
                    sendSafe(peer,rrPeerInfoMessage(joined));
                    sendSafe(joined,"PEER_READY "+peerSession.rrVoiceMemberId);
                    sendSafe(peer,"PEER_READY "+joinedId);
                }
            }

            this.debug("rr_presence_flush",{
                mode,
                room:roomInstanceId.slice(0,12),
                members:members.length,
                joined:entry.joinedMemberIds.size,
                left:entry.leftMemberIds.size
            });
        },PRESENCE_DEBOUNCE_MS);

        this.presenceTimers.set(timerKey,entry);
    }

    async admitRrVoice(socket,requestedRoomInstanceId) {
        const current=sessionOf(socket);
        if(!validRrRoomInstanceId(requestedRoomInstanceId)) {
            sendSafe(socket,"RR_ADMIT_FAIL Invalid RR room instance");
            return;
        }
        if(current.roomCode!==null) {
            sendSafe(socket,"RR_ADMIT_FAIL Leave manual signaling room first");
            return;
        }
        if(!current.rrParticipantId ||
           !current.rrGameName ||
           Date.now()-current.rrVerifiedAt>RR_AUTH_TTL_MS) {
            this.leaveRrVoiceMembership(socket);
            sendSafe(socket,"RR_AUTH_REQUIRED");
            return;
        }

        const room=await this.rrRoomForParticipant(current.rrParticipantId);
        if(!room ||
           room.requestedRoomInstanceId!==requestedRoomInstanceId ||
           current.rrRoomInstanceId!==requestedRoomInstanceId) {
            this.leaveRrVoiceMembership(socket);
            sendSafe(socket,"RR_ADMIT_FAIL Authoritative RR room changed");
            return;
        }

        const player=room.roster.find(entry=>entry.pid===current.rrParticipantId);
        if(!player) {
            this.leaveRrVoiceMembership(socket);
            sendSafe(socket,"RR_ADMIT_FAIL Verified player is not in the RR room");
            return;
        }

        if(current.rrVoiceRoomInstanceId===requestedRoomInstanceId &&
           current.rrVoiceMemberId &&
           current.rrVoiceMode==="production") {
            updateSession(socket,{
                rrVoiceDisplayName:String(player.name??"Player").slice(0,32),
                rrVoiceFriendCode:String(player.friendCode??""),
                rrVoiceOpenHost:player.openHost===true
            });
            sendSafe(socket,await this.iceServersMessage());
            sendSafe(socket,
                "RR_ADMITTED "+current.rrVoiceMemberId+"\n"+requestedRoomInstanceId);
            this.syncVoicePresenceMetadata(
                current.rrVoiceParticipantId,
                player.name,
                player.friendCode,
                player.openHost===true);
            for(const peer of this.rrVoiceMembers(requestedRoomInstanceId,"production")) {
                if(peer!==socket) sendSafe(peer,rrPeerInfoMessage(socket));
            }
            this.broadcastRrVoiceOnlineCount();
            return;
        }

        const stale=this.rrVoiceMembers(requestedRoomInstanceId,"production")
            .find(member=>member!==socket &&
                sessionOf(member).rrVoiceParticipantId===current.rrParticipantId)??null;
        if(stale) {
            this.leaveRrVoiceMembership(stale);
            try {
                stale.close(1000,"Replaced by authenticated RR reconnect");
            } catch {}
        }

        const members=this.rrVoiceMembers(requestedRoomInstanceId,"production")
            .filter(member=>member!==socket);
        if(members.length>=MAX_ROOM_MEMBERS) {
            sendSafe(socket,"RR_ADMIT_FAIL RR voice room is full");
            return;
        }

        this.leaveRrVoiceMembership(socket);
        const memberId=(await sha256Hex(
            "mkwvc-rr-member\n"+
            current.rrParticipantId+"\n"+
            current.sessionId+"\n"+
            requestedRoomInstanceId)).slice(0,32);

        updateSession(socket,{
            rrVoiceRoomInstanceId:requestedRoomInstanceId,
            rrVoiceMemberId:memberId,
            rrVoiceParticipantId:current.rrParticipantId,
            rrVoiceDisplayName:String(player.name??"Player").slice(0,32),
            rrVoiceFriendCode:String(player.friendCode??""),
            rrVoiceOpenHost:player.openHost===true,
            rrVoiceMode:"production",
            signalsInRoom:0,
            lastHeartbeat:Date.now()
        });
        this.syncVoicePresenceMetadata(
            current.rrParticipantId,
            player.name,
            player.friendCode,
            player.openHost===true);
        await this.ensureLivenessAlarm();

        this.debug("rr_admit",{
            mode:"production",
            participant:current.rrParticipantId,
            room:requestedRoomInstanceId.slice(0,12),
            member:memberId.slice(0,8),
            session:current.sessionId?.slice(0,8)??null
        });

        sendSafe(socket,await this.iceServersMessage());
        sendSafe(socket,"RR_ADMITTED "+memberId+"\n"+requestedRoomInstanceId);
        this.scheduleRrVoicePresence(
            requestedRoomInstanceId,
            "production",
            null,
            memberId);
        this.broadcastRrVoiceOnlineCount();
    }

    sendRrDevelopmentAdmitted(socket,memberId,room) {
        const voiceIds=this.rrVoiceParticipantIds(room?.roomInstanceId??null);
        const lines=[
            memberId,
            room?.roomInstanceId??"",
            room?.roomId??"",
            room?.created??"",
            String(room?.roster?.length??0)
        ];

        for(const player of room?.roster??[]) {
            lines.push(
                player.pid+"\t"+
                (voiceIds.has(player.pid) ? "1" : "0")+"\t"+
                encodeHexText(player.name)+"\t"+
                String(player.friendCode??"")+"\t"+
                (player.openHost ? "1" : "0")
            );
        }

        sendSafe(socket,"RR_DEV_ADMITTED "+lines.join("\n"));
    }

    async admitRrDevelopmentVoice(
        socket,
        profileId,
        requestedRoomInstanceId=null,
        localProfileIds=[]) {
        if(!validRrProfileId(profileId) ||
           (requestedRoomInstanceId!==null && !validRrRoomInstanceId(requestedRoomInstanceId))) {
            sendSafe(socket,"RR_DEV_ADMIT_FAIL Invalid development admission request");
            return;
        }

        const current=sessionOf(socket);
        if(current.roomCode!==null) {
            sendSafe(socket,"RR_DEV_ADMIT_FAIL Leave manual signaling room first");
            return;
        }

        try {
            let room=await this.rrRoomForParticipant(profileId);
            let localFallback=false;
            if(!room) {
                room=this.developmentRoomFromLocalProfiles(
                    profileId,
                    localProfileIds);
                localFallback=room!==null;
            }
            if(!room) {
                this.leaveRrVoiceMembership(socket);
                sendSafe(socket,"RR_DEV_ADMIT_FAIL PID is absent from public RR roster and no matching local RKNet voice room exists");
                return;
            }

            const resolvedRoomInstanceId=room.roomInstanceId;
            if(requestedRoomInstanceId!==null &&
               requestedRoomInstanceId.toUpperCase()!==resolvedRoomInstanceId.toUpperCase()) {
                this.leaveRrVoiceMembership(socket);
                sendSafe(socket,"RR_DEV_ADMIT_FAIL Public RR room changed or PID is absent");
                return;
            }
            const roomInstanceId=resolvedRoomInstanceId;
            const player=room.roster.find(entry=>entry.pid===profileId);
            if(!player) {
                this.leaveRrVoiceMembership(socket);
                sendSafe(socket,"RR_DEV_ADMIT_FAIL PID is absent from public RR roster");
                return;
            }

            if(current.rrVoiceRoomInstanceId===roomInstanceId &&
               current.rrVoiceParticipantId===profileId &&
               current.rrVoiceMemberId &&
               current.rrVoiceMode==="development") {
                updateSession(socket,{
                    rrVoiceDisplayName:String(player.name??"Player").slice(0,32),
                    rrVoiceFriendCode:String(player.friendCode??""),
                    rrVoiceOpenHost:player.openHost===true
                });
                sendSafe(socket,await this.iceServersMessage());
                this.sendRrDevelopmentAdmitted(socket,current.rrVoiceMemberId,room);
                this.syncVoicePresenceMetadata(
                    profileId,
                    player.name,
                    player.friendCode,
                    player.openHost===true);
                for(const peer of this.rrVoiceMembers(roomInstanceId,"development")) {
                    if(peer!==socket) sendSafe(peer,rrPeerInfoMessage(socket));
                }
                this.broadcastRrVoiceOnlineCount();
                return;
            }

            const stale=this.rrVoiceMembers(roomInstanceId,"development")
                .find(member=>member!==socket &&
                    sessionOf(member).rrVoiceParticipantId===profileId)??null;
            if(stale) {
                this.leaveRrVoiceMembership(stale);
                try {
                    stale.close(1000,"Replaced by RR development reconnect");
                } catch {}
            }

            const members=this.rrVoiceMembers(roomInstanceId,"development")
                .filter(member=>member!==socket);
            if(members.length>=MAX_ROOM_MEMBERS) {
                sendSafe(socket,"RR_DEV_ADMIT_FAIL Development voice room is full");
                return;
            }

            this.leaveRrVoiceMembership(socket);
            const memberId=(await sha256Hex(
                "mkwvc-rr-dev-member\n"+
                profileId+"\n"+
                current.sessionId+"\n"+
                roomInstanceId)).slice(0,32);

            updateSession(socket,{
                rrParticipantId:profileId,
                rrGameName:"mariokartwii",
                rrVerifiedAt:0,
                rrRoomId:room.roomId,
                rrRoomInstanceId:roomInstanceId,
                rrRoomCreated:room.created,
                rrVoiceRoomInstanceId:roomInstanceId,
                rrVoiceMemberId:memberId,
                rrVoiceParticipantId:profileId,
                rrVoiceDisplayName:String(player.name??"Player").slice(0,32),
                rrVoiceFriendCode:String(player.friendCode??""),
                rrVoiceOpenHost:player.openHost===true,
                rrVoiceMode:"development",
                signalsInRoom:0,
                lastHeartbeat:Date.now()
            });
            this.syncVoicePresenceMetadata(
                profileId,
                player.name,
                player.friendCode,
                player.openHost===true);
            await this.ensureLivenessAlarm();

            this.debug("rr_dev_admit",{
                participant:profileId,
                room:roomInstanceId.slice(0,12),
                member:memberId.slice(0,8),
                session:current.sessionId?.slice(0,8)??null,
                source:localFallback ? "local-rknet-peer-match" : "public-rwfc-roster"
            });

            sendSafe(socket,await this.iceServersMessage());
            this.sendRrDevelopmentAdmitted(socket,memberId,room);
            this.scheduleRrVoicePresence(
                roomInstanceId,
                "development",
                null,
                memberId);
            this.broadcastRrVoiceOnlineCount();
        } catch(error) {
            this.leaveRrVoiceMembership(socket);
            this.debug("rr_dev_admit_error",{
                participant:profileId,
                error:String(error?.message??error),
                session:current.sessionId?.slice(0,8)??null
            });
            sendSafe(socket,"RR_DEV_ADMIT_FAIL Public RWFC room lookup unavailable");
        }
    }

    broadcastRrDebugPresence(roomInstanceId) {
        if(!roomInstanceId) return;

        const targets=this.sockets().filter(socket=>{
            const session=sessionOf(socket);
            return session.rrDebugPresence===true &&
                   session.rrParticipantId &&
                   session.rrRoomInstanceId===roomInstanceId;
        });

        if(targets.length===0) return;

        for(const socket of targets) {
            const session=sessionOf(socket);
            this.rrRoomForParticipant(session.rrParticipantId)
                .then(room=>{
                    const current=sessionOf(socket);
                    if(!current.rrDebugPresence ||
                       current.rrRoomInstanceId!==roomInstanceId) {
                        return;
                    }
                    this.sendRrDebugStatus(socket,current.rrParticipantId,room);
                })
                .catch(()=>{});
        }
    }

    async setDebugRrPresence(socket,profileId) {
        if(!validRrProfileId(profileId)) {
            sendSafe(socket,"RR_DEBUG_FAIL Invalid Retro Rewind profile ID");
            return;
        }

        const before=sessionOf(socket);
        const oldRoomInstanceId=before.rrRoomInstanceId;

        try {
            const room=await this.rrRoomForParticipant(profileId);
            this.leaveRrVoiceMembership(socket);
            updateSession(socket,{
                rrParticipantId:profileId,
                rrGameName:"mariokartwii",
                rrVerifiedAt:0,
                rrRoomId:room?.roomId??null,
                rrRoomInstanceId:room?.roomInstanceId??null,
                rrRoomCreated:room?.created??null,
                rrDebugPresence:true
            });

            this.debug("rr_debug_presence",{
                participant:profileId,
                room:room?.roomId??null,
                session:sessionOf(socket).sessionId?.slice(0,8)??null
            });

            this.sendRrDebugStatus(socket,profileId,room);

            if(oldRoomInstanceId && oldRoomInstanceId!==room?.roomInstanceId) {
                this.broadcastRrDebugPresence(oldRoomInstanceId);
            }
            if(room?.roomInstanceId) {
                this.broadcastRrDebugPresence(room.roomInstanceId);
            }
        } catch(error) {
            this.debug("rr_debug_presence_error",{
                participant:profileId,
                error:String(error?.message??error),
                session:sessionOf(socket).sessionId?.slice(0,8)??null
            });
            sendSafe(socket,"RR_DEBUG_FAIL RWFC room lookup unavailable");
        }
    }

    clearDebugRrPresence(socket) {
        const before=sessionOf(socket);
        const oldRoomInstanceId=before.rrRoomInstanceId;
        this.leaveRrVoiceMembership(socket);

        updateSession(socket,{
            rrParticipantId:null,
            rrGameName:null,
            rrVerifiedAt:0,
            rrRoomId:null,
            rrRoomInstanceId:null,
            rrRoomCreated:null,
            rrDebugPresence:false,
            rrVoiceRoomInstanceId:null,
            rrVoiceMemberId:null,
            rrVoiceParticipantId:null,
            rrVoiceDisplayName:null,
            rrVoiceFriendCode:null,
            rrVoiceOpenHost:false,
            rrVoiceMode:null
        });

        if(oldRoomInstanceId) {
            this.broadcastRrDebugPresence(oldRoomInstanceId);
        }
    }

    sendRrStatus(socket,room) {
        const current=sessionOf(socket);
        const lines=[
            current.rrParticipantId??"",
            room?.roomId??"",
            room?.roomInstanceId??"",
            room?.created??"",
            String(room?.roster?.length??0)
        ];

        const voiceIds=this.rrVoiceParticipantIds(room?.roomInstanceId??null);
        for(const player of room?.roster??[]) {
            lines.push(
                player.pid+"\t"+
                (voiceIds.has(player.pid) ? "1" : "0")+"\t"+
                encodeHexText(player.name)+"\t"+
                String(player.friendCode??"")+"\t"+
                (player.openHost ? "1" : "0")
            );
        }

        sendSafe(socket,"RR_STATUS\n"+lines.join("\n"));
    }

    async authenticateRr(socket,profileId,sessionKey,gameName) {
        if(!validRrProfileId(profileId) ||
           !validRrSessionKey(sessionKey) ||
           !validRrGameName(gameName)) {
            this.leaveRrVoiceMembership(socket);
            sendSafe(socket,"RR_AUTH_FAIL Invalid Retro Rewind credentials");
            return;
        }

        try {
            const verified=await verifyRrSession(this.env,profileId,sessionKey,gameName);
            if(!verified) {
                this.leaveRrVoiceMembership(socket);
                updateSession(socket,{
                    rrParticipantId:null,
                    rrGameName:null,
                    rrVerifiedAt:0,
                    rrRoomId:null,
                    rrRoomInstanceId:null,
                    rrRoomCreated:null
                });
                sendSafe(socket,"RR_AUTH_FAIL GPSP rejected this live session");
                return;
            }

            const room=await this.rrRoomForParticipant(profileId);
            updateSession(socket,{
                rrParticipantId:profileId,
                rrGameName:gameName,
                rrVerifiedAt:Date.now(),
                rrRoomId:room?.roomId??null,
                rrRoomInstanceId:room?.roomInstanceId??null,
                rrRoomCreated:room?.created??null
            });

            this.debug("rr_auth_ok",{
                participant:profileId,
                room:room?.roomId??null,
                session:sessionOf(socket).sessionId?.slice(0,8)??null
            });
            this.sendRrStatus(socket,room);
        } catch(error) {
            this.leaveRrVoiceMembership(socket);
            this.debug("rr_auth_error",{
                participant:profileId,
                error:String(error?.message??error),
                session:sessionOf(socket).sessionId?.slice(0,8)??null
            });
            const reason=String(error?.message??error??"RR verifier unavailable").replace(/[\r\n]/g," ").slice(0,160);
            sendSafe(socket,"RR_AUTH_FAIL "+reason);
        }
    }

    async syncRr(socket) {
        const current=sessionOf(socket);
        if(!current.rrParticipantId ||
           !current.rrGameName ||
           Date.now()-current.rrVerifiedAt>RR_AUTH_TTL_MS) {
            this.leaveRrVoiceMembership(socket);
            sendSafe(socket,"RR_AUTH_REQUIRED");
            return;
        }

        try {
            const room=await this.rrRoomForParticipant(current.rrParticipantId);
            if(current.rrVoiceRoomInstanceId &&
               (!room || room.roomInstanceId!==current.rrVoiceRoomInstanceId)) {
                this.leaveRrVoiceMembership(socket);
                sendSafe(socket,"RR_ADMIT_FAIL Authoritative RR room changed");
            }
            const player=room?.roster?.find(entry=>entry.pid===current.rrParticipantId)??null;
            updateSession(socket,{
                rrRoomId:room?.roomId??null,
                rrRoomInstanceId:room?.roomInstanceId??null,
                rrRoomCreated:room?.created??null,
                rrVoiceOpenHost:player ? player.openHost===true : current.rrVoiceOpenHost
            });
            if(player && current.rrVoiceRoomInstanceId===room?.roomInstanceId) {
                this.syncVoicePresenceMetadata(
                    current.rrParticipantId,
                    player.name,
                    player.friendCode,
                    player.openHost===true);
                this.broadcastRrVoiceOnlineCount();
            }
            this.sendRrStatus(socket,room);
        } catch(error) {
            this.leaveRrVoiceMembership(socket);
            this.debug("rr_sync_error",{
                participant:current.rrParticipantId,
                error:String(error?.message??error),
                session:current.sessionId?.slice(0,8)??null
            });
            sendSafe(socket,"RR_AUTH_FAIL RWFC room lookup unavailable");
        }
    }

    async ensureLivenessAlarm(deadline=Date.now()+LIVENESS_SWEEP_MS) {
        const current=await this.ctx.storage.getAlarm();
        if(current===null || current>deadline) {
            await this.ctx.storage.setAlarm(deadline);
        }
    }

    async alarm() {
        const now=Date.now();
        const roomSockets=this.sockets().filter(socket=>{
            const session=sessionOf(socket);
            return session.roomCode!==null ||
                   session.rrVoiceRoomInstanceId!==null ||
                   session.voiceOnline;
        });

        for(const socket of roomSockets) {
            const session=sessionOf(socket);
            if(now-session.lastHeartbeat<ROOM_HEARTBEAT_TIMEOUT_MS) continue;

            this.debug("liveness_timeout",{
                room:session.roomCode??session.rrVoiceRoomInstanceId,
                member:(session.memberId??session.rrVoiceMemberId)?.slice(0,8)??null,
                session:session.sessionId?.slice(0,8)??null
            });

            this.leaveMembership(socket);
            this.leaveRrVoiceMembership(socket);
            if(session.voiceOnline) this.setVoiceOnlinePresence(socket,false);
            try {
                socket.close(1001,"Liveness timeout");
            } catch {}
        }

        const remaining=this.sockets().filter(socket=>{
            const session=sessionOf(socket);
            return session.roomCode!==null ||
                   session.rrVoiceRoomInstanceId!==null ||
                   session.voiceOnline;
        });
        if(remaining.length===0) return;

        await this.ctx.storage.setAlarm(Date.now()+LIVENESS_SWEEP_MS);
    }


    members(roomCode) {
        return this.sockets().filter(socket=>sessionOf(socket).roomCode===roomCode);
    }

    socketBySessionId(sessionId) {
        if(!sessionId) return null;
        return this.sockets().find(socket=>sessionOf(socket).sessionId===sessionId)??null;
    }

    memberSocket(roomCode,memberId) {
        if(!roomCode || !memberId) return null;
        return this.members(roomCode).find(socket=>sessionOf(socket).memberId===memberId)??null;
    }

    makeCode() {
        const used=new Set();
        for(const socket of this.sockets()) {
            const session=sessionOf(socket);
            if(session.roomCode) used.add(session.roomCode);
        }

        for(let attempt=0;attempt<128;++attempt) {
            const code=randomCode();
            if(!used.has(code)) return code;
        }

        throw new Error("Unable to allocate room code");
    }

    schedulePresence(roomCode,leftMemberId=null,joinedMemberId=null) {
        const existing=this.presenceTimers.get(roomCode);

        if(existing) {
            if(leftMemberId) existing.leftMemberIds.add(leftMemberId);
            if(joinedMemberId) existing.joinedMemberIds.add(joinedMemberId);
            return;
        }

        const entry={leftMemberIds:new Set(),joinedMemberIds:new Set(),timer:null};
        if(leftMemberId) entry.leftMemberIds.add(leftMemberId);
        if(joinedMemberId) entry.joinedMemberIds.add(joinedMemberId);

        entry.timer=setTimeout(()=>{
            this.presenceTimers.delete(roomCode);

            const members=this.members(roomCode);
            const currentIds=new Set(
                members
                    .map(member=>sessionOf(member).memberId)
                    .filter(Boolean)
            );

            for(const leftId of entry.leftMemberIds) {
                if(currentIds.has(leftId)) continue;
                for(const member of members) sendSafe(member,"PEER_LEFT "+leftId);
            }

            const processedPairs=new Set();
            for(const joinedId of entry.joinedMemberIds) {
                const joined=this.memberSocket(roomCode,joinedId);
                if(!joined) continue;

                for(const peer of members) {
                    if(peer===joined) continue;
                    const peerSession=sessionOf(peer);
                    if(!peerSession.memberId) continue;

                    const pairKey=[joinedId,peerSession.memberId].sort().join(":");
                    if(processedPairs.has(pairKey)) continue;
                    processedPairs.add(pairKey);

                    sendSafe(joined,peerInfoMessage(peer));
                    sendSafe(peer,peerInfoMessage(joined));
                    sendSafe(joined,"PEER_READY "+peerSession.memberId);
                    sendSafe(peer,"PEER_READY "+joinedId);
                }
            }

            this.debug("presence_flush",{
                room:roomCode,
                members:members.length,
                joined:entry.joinedMemberIds.size,
                left:entry.leftMemberIds.size
            });
        },PRESENCE_DEBOUNCE_MS);

        this.presenceTimers.set(roomCode,entry);
    }

    async allow(binding,key) {
        const result=await binding.limit({key});
        return result.success;
    }

    consumeSessionBudget(socket,kind) {
        const session=sessionOf(socket);
        const now=Date.now();
        const startKey=
            kind==="room" ? "roomWindowStart" :
            kind==="signal" ? "signalWindowStart" :
            "messageWindowStart";
        const countKey=
            kind==="room" ? "roomActions" :
            kind==="signal" ? "signalActions" :
            "messageActions";
        let start=session[startKey];
        let count=session[countKey];

        if(start<=0 || now-start>=WINDOW_MS) {
            start=now;
            count=0;
        }

        count+=1;
        updateSession(socket,{[startKey]:start,[countKey]:count});
        return count;
    }

    rateLimited(socket,kind,closeSocket=false) {
        const session=sessionOf(socket);
        this.debug("rate_limited",{
            kind,
            session:session.sessionId?.slice(0,8)??null,
            room:session.roomCode
        });
        sendSafe(socket,"ERROR Rate limit exceeded");

        if(closeSocket) {
            try {
                socket.close(1008,"Rate limit exceeded");
            } catch {}
        }
    }

    async guardMessage(socket) {
        const count=this.consumeSessionBudget(socket,"message");
        if(count>SESSION_MESSAGE_HARD_LIMIT) {
            this.rateLimited(socket,"message_session_hard",true);
            return false;
        }

        const session=sessionOf(socket);
        const key=(session.ip??session.sessionId??"unknown")+":message";
        if(!await this.allow(this.env.MESSAGE_RATE_LIMITER,key)) {
            this.rateLimited(socket,"message_ip",true);
            return false;
        }

        return true;
    }

    async guardRoomAction(socket) {
        const count=this.consumeSessionBudget(socket,"room");
        if(count>SESSION_ROOM_HARD_LIMIT) {
            this.rateLimited(socket,"room_session_hard",true);
            return false;
        }
        if(count>SESSION_ROOM_SOFT_LIMIT) {
            this.rateLimited(socket,"room_session_soft",false);
            return false;
        }

        const session=sessionOf(socket);
        const authenticated=
            session.rrParticipantId &&
            Date.now()-session.rrVerifiedAt<=RR_AUTH_TTL_MS;
        const key=(authenticated ? "rr:"+session.rrParticipantId : (session.ip??session.sessionId??"unknown"))+":room";
        if(!await this.allow(this.env.ROOM_ACTION_RATE_LIMITER,key)) {
            this.rateLimited(socket,"room_ip",false);
            return false;
        }

        return true;
    }

    async guardSignal(socket) {
        const count=this.consumeSessionBudget(socket,"signal");
        if(count>SESSION_SIGNAL_HARD_LIMIT) {
            this.rateLimited(socket,"signal_session_hard",true);
            return false;
        }

        const session=sessionOf(socket);
        const authenticated=
            session.rrParticipantId &&
            Date.now()-session.rrVerifiedAt<=RR_AUTH_TTL_MS;
        const key=(authenticated ? "rr:"+session.rrParticipantId : (session.ip??session.sessionId??"unknown"))+":signal";
        if(!await this.allow(this.env.SIGNAL_RATE_LIMITER,key)) {
            this.rateLimited(socket,"signal_ip",false);
            return false;
        }

        return true;
    }

    leaveMembership(socket) {
        const session=sessionOf(socket);
        if(!session.roomCode) return;

        const roomCode=session.roomCode;
        const remaining=this.members(roomCode).filter(member=>member!==socket);

        this.debug("leave",{
            room:roomCode,
            session:session.sessionId?.slice(0,8)??null
        });

        updateSession(socket,{roomCode:null,role:null,memberId:null,signalsInRoom:0});

        if(remaining.length>0) this.schedulePresence(roomCode,session.memberId);
    }

    async iceServersMessage() {
        const servers=["stun:stun.l.google.com:19302"];
        const staticTurn=(this.env.MKWVC_TURN_URLS??"")
            .split(";")
            .map(value=>value.trim())
            .filter(Boolean);

        for(const url of staticTurn) {
            if(!url.startsWith("turn:") && !url.startsWith("turns:")) {
                throw new Error("Invalid MKWVC_TURN_URLS");
            }
            servers.push(url);
        }

        const host=(this.env.MKWVC_TURN_HOST??"").trim();
        const secret=(this.env.MKWVC_TURN_SECRET??"").trim();

        if(Boolean(host)!==Boolean(secret)) {
            throw new Error("TURN host and secret must be configured together");
        }

        if(host) {
            const port=parsePositiveInteger(this.env.MKWVC_TURN_PORT,3478,1,65535);
            const ttl=parsePositiveInteger(this.env.MKWVC_TURN_TTL,3600,60,86400);
            const expires=Math.floor(Date.now()/1000)+ttl;
            const username=`${expires}:mkwvc-${crypto.randomUUID().replaceAll("-","").slice(0,16)}`;
            const password=base64(await hmacSha1(secret,username));
            servers.push(`turn:${encodeURIComponent(username)}:${encodeURIComponent(password)}@${host}:${port}`);
        }

        return "ICE_SERVERS\n"+servers.join("\n");
    }

    async applyRoomState(socket,stateText,replaceExisting=false) {
        if(!await this.guardRoomAction(socket)) return;

        const session=sessionOf(socket);
        const normalized=stateText.trim().toUpperCase();
        const parts=normalized.split(/\s+/).filter(Boolean);
        let mode=parts[0]??"";
        let code=null;
        let memberId=null;
        let displayName="Player";

        if(mode==="JOIN") {
            code=parts[1]??"";
            memberId=parts[2]??null;
            displayName=decodeDisplayName(parts[3]??"");
            if(!validRoomCode(code)) {
                if(replaceExisting) this.leaveMembership(socket);
                sendSafe(socket,"ERROR Room not found");
                return;
            }
            if(memberId!==null && !validMemberId(memberId)) {
                sendSafe(socket,"ERROR Invalid member identity");
                return;
            }
        } else if(mode==="CREATE") {
            memberId=parts[1]??null;
            displayName=decodeDisplayName(parts[2]??"");
            if(memberId!==null && !validMemberId(memberId)) {
                sendSafe(socket,"ERROR Invalid member identity");
                return;
            }
        } else if(mode!=="NONE") {
            sendSafe(socket,"ERROR Unknown room state");
            return;
        }

        if((mode==="CREATE" || mode==="JOIN") && session.rrVoiceRoomInstanceId) {
            this.leaveRrVoiceMembership(socket);
        }

        this.debug("state_received",{
            state:mode,
            room:code,
            member:memberId?.slice(0,8)??null,
            session:session.sessionId?.slice(0,8)??null
        });

        if(mode==="NONE" && session.roomCode===null) {
            this.debug("state_noop",{state:"NONE",session:session.sessionId?.slice(0,8)??null});
            sendSafe(socket,"LEFT");
            return;
        }

        if(!replaceExisting && mode==="CREATE" && session.roomCode) {
            this.debug("state_noop",{state:"CREATE",room:session.roomCode,session:session.sessionId?.slice(0,8)??null});
            sendSafe(socket,"ROOM "+session.roomCode);
            return;
        }

        if(mode==="JOIN" && session.roomCode===code &&
           (memberId===null || session.memberId===memberId)) {
            this.debug("state_noop",{state:"JOIN",room:code,session:session.sessionId?.slice(0,8)??null});
            sendSafe(socket,"JOINED "+code);
            return;
        }

        let resumeReplaced=false;
        let targetMembers=[];
        if(mode==="JOIN") {
            targetMembers=this.members(code).filter(member=>member!==socket);

            if(memberId!==null) {
                const stale=targetMembers.find(member=>sessionOf(member).memberId===memberId)??null;
                if(stale) {
                    const staleSession=sessionOf(stale);
                    updateSession(stale,{roomCode:null,role:null,memberId:null,signalsInRoom:0});
                    this.debug("resume_replace",{
                        room:code,
                        member:memberId.slice(0,8),
                        oldSession:staleSession.sessionId?.slice(0,8)??null,
                        newSession:session.sessionId?.slice(0,8)??null
                    });
                    try {
                        stale.close(1000,"Replaced by reconnect");
                    } catch {}
                    resumeReplaced=true;
                    targetMembers=targetMembers.filter(member=>member!==stale);
                }
            }

            if(targetMembers.length===0 && !resumeReplaced) {
                if(replaceExisting) this.leaveMembership(socket);
                sendSafe(socket,"ERROR Room not found");
                return;
            }
            if(targetMembers.length>=MAX_ROOM_MEMBERS) {
                if(replaceExisting) this.leaveMembership(socket);
                sendSafe(socket,"ERROR Room is full");
                return;
            }
        }

        this.leaveMembership(socket);

        if(mode==="NONE") {
            sendSafe(socket,"LEFT");
            return;
        }

        if(mode==="CREATE") {
            const roomCode=this.makeCode();
            updateSession(socket,{
                roomCode,
                role:"member",
                memberId,
                displayName,
                signalsInRoom:0,
                lastHeartbeat:Date.now()
            });
            await this.ensureLivenessAlarm();
            this.debug("create",{room:roomCode,member:memberId?.slice(0,8)??null,country:session.country,session:session.sessionId?.slice(0,8)??null});
            sendSafe(socket,await this.iceServersMessage());
            sendSafe(socket,"ROOM "+roomCode);
            return;
        }

        const existing=this.members(code).filter(member=>member!==socket);
        if(existing.length===0 && !resumeReplaced) {
            sendSafe(socket,"ERROR Room not found");
            return;
        }
        if(existing.length>=MAX_ROOM_MEMBERS) {
            sendSafe(socket,"ERROR Room is full");
            return;
        }

        updateSession(socket,{
            roomCode:code,
            role:"member",
            memberId,
            displayName,
            signalsInRoom:0,
            lastHeartbeat:Date.now()
        });
        await this.ensureLivenessAlarm();
        this.debug("join",{room:code,member:memberId?.slice(0,8)??null,country:session.country,session:session.sessionId?.slice(0,8)??null});
        sendSafe(socket,await this.iceServersMessage());
        sendSafe(socket,"JOINED "+code);
        this.schedulePresence(code,null,memberId);
    }

    async webSocketMessage(socket,message) {
        if(!await this.guardMessage(socket)) return;

        const messageBytes=
            typeof message==="string" ? new TextEncoder().encode(message).byteLength :
            message instanceof ArrayBuffer ? message.byteLength :
            ArrayBuffer.isView(message) ? message.byteLength :
            MAX_SIGNAL_MESSAGE_BYTES+1;

        if(messageBytes>MAX_SIGNAL_MESSAGE_BYTES) {
            sendSafe(socket,"ERROR Signaling message is too large");
            try {
                socket.close(1009,"Message too large");
            } catch {}
            return;
        }

        if(typeof message!=="string") {
            sendSafe(socket,"ERROR Text signaling messages only");
            return;
        }

        try {
            if(message==="VOICE_ONLINE") {
                this.setVoiceOnlinePresence(socket,true);
                await this.ensureLivenessAlarm();
                return;
            }

            if(message==="VOICE_OFFLINE") {
                this.setVoiceOnlinePresence(socket,false);
                return;
            }

            if(message==="VOICE_IDENTITY_CLEAR") {
                this.setVoiceOnlineIdentity(socket,null);
                return;
            }

            if(message.startsWith("VOICE_IDENTITY ")) {
                const profileId=message.slice(15).trim();
                if(!validRrProfileId(profileId)) {
                    sendSafe(socket,"ERROR Invalid voice presence profile ID");
                    return;
                }
                this.setVoiceOnlineIdentity(socket,profileId);
                return;
            }

            if(message==="ALIVE") {
                const current=sessionOf(socket);
                if(current.rrVoiceMode==="production" &&
                   Date.now()-current.rrVerifiedAt>RR_AUTH_TTL_MS) {
                    this.leaveRrVoiceMembership(socket);
                    sendSafe(socket,"RR_AUTH_REQUIRED");
                    return;
                }
                if(current.roomCode!==null ||
                   current.rrVoiceRoomInstanceId!==null ||
                   current.voiceOnline) {
                    updateSession(socket,{lastHeartbeat:Date.now()});
                }
                return;
            }

            if(message.startsWith("RR_DEBUG_LOOKUP ")) {
                if(!await this.guardRoomAction(socket)) return;
                await this.debugLookupRr(socket,message.slice(16).trim());
                return;
            }

            if(message.startsWith("RR_DEBUG_PRESENCE ")) {
                if(!await this.guardRoomAction(socket)) return;
                await this.setDebugRrPresence(socket,message.slice(18).trim());
                return;
            }

            if(message==="RR_DEBUG_PRESENCE_CLEAR") {
                if(!await this.guardRoomAction(socket)) return;
                this.clearDebugRrPresence(socket);
                return;
            }

            if(message.startsWith("RR_AUTH ")) {
                if(!await this.guardRoomAction(socket)) return;
                const parts=message.slice(8).trim().split(/\s+/);
                if(parts.length!==3) {
                    sendSafe(socket,"RR_AUTH_FAIL Invalid RR_AUTH format");
                    return;
                }
                await this.authenticateRr(socket,parts[0],parts[1],parts[2]);
                return;
            }

            if(message==="RR_SYNC") {
                if(!await this.guardRoomAction(socket)) return;
                await this.syncRr(socket);
                return;
            }

            if(message.startsWith("RR_ADMIT ")) {
                if(!await this.guardRoomAction(socket)) return;
                await this.admitRrVoice(socket,message.slice(9).trim());
                return;
            }

            if(message.startsWith("RR_DEV_ADMIT ")) {
                if(!await this.guardRoomAction(socket)) return;
                const parts=message.slice(13).trim().split(/\s+/).filter(Boolean);
                if(parts.length<1 || parts.length>3) {
                    sendSafe(socket,"RR_DEV_ADMIT_FAIL Invalid development admission format");
                    return;
                }

                let requestedRoomInstanceId=null;
                let localProfileIds=[];
                for(const part of parts.slice(1)) {
                    if(part.startsWith("PIDS=")) {
                        if(localProfileIds.length>0) {
                            sendSafe(socket,"RR_DEV_ADMIT_FAIL Invalid development admission format");
                            return;
                        }
                        localProfileIds=part.slice(5)
                            .split(",")
                            .filter(Boolean);
                        if(localProfileIds.length>12 ||
                           localProfileIds.some(pid=>!validRrProfileId(pid))) {
                            sendSafe(socket,"RR_DEV_ADMIT_FAIL Invalid local RKNet PID list");
                            return;
                        }
                    } else if(requestedRoomInstanceId===null) {
                        requestedRoomInstanceId=part;
                    } else {
                        sendSafe(socket,"RR_DEV_ADMIT_FAIL Invalid development admission format");
                        return;
                    }
                }

                await this.admitRrDevelopmentVoice(
                    socket,
                    parts[0],
                    requestedRoomInstanceId,
                    localProfileIds);
                return;
            }

            if(message.startsWith("STATE ")) {
                await this.applyRoomState(socket,message.slice(6),true);
                return;
            }

            if(message==="CREATE" || message.startsWith("CREATE ")) {
                await this.applyRoomState(socket,message,true);
                return;
            }

            if(message.startsWith("JOIN ")) {
                await this.applyRoomState(socket,message,true);
                return;
            }

            if(message==="LEAVE") {
                await this.applyRoomState(socket,"NONE",true);
                return;
            }

            if(message.startsWith("SIGNAL\n") || message.startsWith("SIGNAL ")) {
                if(!await this.guardSignal(socket)) return;

                const current=sessionOf(socket);
                const manualMembership=current.roomCode!==null && Boolean(current.memberId);
                const rrMembership=
                    Boolean(current.rrVoiceRoomInstanceId) &&
                    Boolean(current.rrVoiceMemberId) &&
                    Boolean(current.rrVoiceMode) &&
                    (current.rrVoiceMode==="development" ||
                     (current.rrVoiceMode==="production" &&
                      current.rrRoomInstanceId===current.rrVoiceRoomInstanceId &&
                      Date.now()-current.rrVerifiedAt<=RR_AUTH_TTL_MS));

                if(!manualMembership && !rrMembership) {
                    sendSafe(socket,"ERROR Join an authorized room first");
                    return;
                }

                if(current.signalsInRoom>=ROOM_SIGNAL_LIMIT) {
                    this.rateLimited(socket,"signal_room",true);
                    return;
                }

                updateSession(socket,{signalsInRoom:current.signalsInRoom+1});

                const roomKey=manualMembership ? current.roomCode : current.rrVoiceRoomInstanceId;
                const sourceMemberId=manualMembership ? current.memberId : current.rrVoiceMemberId;
                const roomMembers=manualMembership
                    ? this.members(roomKey)
                    : this.rrVoiceMembers(roomKey,current.rrVoiceMode);

                let target=null;
                let outgoing=message;

                if(message.startsWith("SIGNAL\n")) {
                    const others=roomMembers.filter(member=>member!==socket);
                    if(others.length!==1) {
                        sendSafe(socket,"ERROR Target member required");
                        return;
                    }
                    target=others[0];
                } else {
                    const split=message.indexOf("\n");
                    const targetMemberId=(split<0 ? message.slice(7) : message.slice(7,split)).trim().toUpperCase();

                    if(!validMemberId(targetMemberId) || targetMemberId===sourceMemberId) {
                        sendSafe(socket,"ERROR Invalid signal target");
                        return;
                    }

                    target=manualMembership
                        ? this.memberSocket(roomKey,targetMemberId)
                        : this.rrVoiceMemberSocket(
                            roomKey,
                            targetMemberId,
                            current.rrVoiceMode);
                    if(split<0 || !target) {
                        sendSafe(socket,"ERROR Peer has not joined yet");
                        return;
                    }

                    outgoing="SIGNAL "+sourceMemberId+"\n"+message.slice(split+1);
                }

                if(!target) {
                    sendSafe(socket,"ERROR Peer has not joined yet");
                    return;
                }

                this.debug("signal_relay",{
                    room:roomKey,
                    from:sourceMemberId.slice(0,8),
                    to:(manualMembership
                        ? sessionOf(target).memberId
                        : sessionOf(target).rrVoiceMemberId)?.slice(0,8)??null
                });

                if(!sendSafe(target,outgoing)) sendSafe(socket,"ERROR Peer disconnected");
                return;
            }

            this.debug("unknown_command",{session:sessionOf(socket).sessionId?.slice(0,8)??null});
            sendSafe(socket,"ERROR Unknown command");
        } catch(error) {
            this.debug("handler_error",{message:String(error?.message??error)});
            sendSafe(socket,"ERROR Signaling server configuration error");
        }
    }

    webSocketClose(socket) {
        const session=sessionOf(socket);
        this.debug("ws_close",{session:session.sessionId?.slice(0,8)??null});
        if(session.voiceOnline) this.setVoiceOnlinePresence(socket,false);
        this.leaveRrVoiceMembership(socket);
        this.clearDebugRrPresence(socket);
        this.leaveMembership(socket);
    }

    webSocketError(socket) {
        const session=sessionOf(socket);
        this.debug("ws_error",{session:session.sessionId?.slice(0,8)??null});
        if(session.voiceOnline) this.setVoiceOnlinePresence(socket,false);
        this.leaveRrVoiceMembership(socket);
        this.clearDebugRrPresence(socket);
        this.leaveMembership(socket);

        try {
            socket.close(1011,"WebSocket error");
        } catch {}
    }
}

export default {
    async fetch(request,env) {
        const url=new URL(request.url);

        if(request.headers.get("Upgrade")?.toLowerCase()==="websocket") {
            const ip=request.headers.get("CF-Connecting-IP")??"unknown";
            const result=await env.CONNECTION_RATE_LIMITER.limit({key:ip});

            if(!result.success) {
                return new Response("Too many signaling connections",{status:429});
            }

            const id=env.SIGNALING_HUB.idFromName(HUB_INSTANCE);
            const headers=new Headers(request.headers);
            const country=typeof request.cf?.country==="string" ? request.cf.country.toUpperCase() : "??";
            headers.set("X-MKWVC-Country",country);
            return env.SIGNALING_HUB.get(id).fetch(new Request(request,{headers}));
        }

        if(url.pathname==="/health") {
            let rrVerificationMode="unavailable";
            try {
                if(rrVerifierConfig(env)!==null) rrVerificationMode="https";
            } catch {
                rrVerificationMode="invalid-config";
            }

            return Response.json({
                service:"mkw-voicechat-signaling",
                status:"ok",
                version:WORKER_VERSION,
                rrVerificationMode,
                rrDebugRosterLookup:true,
                rrDebugPresence:true
            },{
                headers:{
                    "Cache-Control":"no-store"
                }
            });
        }

        return new Response("MKW VoiceChat signaling",{status:200});
    }
};
