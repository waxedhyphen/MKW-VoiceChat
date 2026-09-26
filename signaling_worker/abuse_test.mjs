const PROD_URL="wss://mkw-voicechat-signaling.mkwvoicechat.workers.dev";
const LOCAL_URL="ws://127.0.0.1:8787";
const MAX_COUNT=200;

if(typeof WebSocket==="undefined") {
    console.error("This test requires Node.js with the built-in WebSocket API.");
    process.exit(1);
}

const args=process.argv.slice(2);
const mode=args[0]??"";
const local=args.includes("--local");
const url=local ? LOCAL_URL : PROD_URL;

function option(name,fallback) {
    const index=args.indexOf(name);
    if(index<0 || index+1>=args.length) return fallback;
    return args[index+1];
}

function integerOption(name,fallback,min,max) {
    const value=Number.parseInt(option(name,String(fallback)),10);
    if(!Number.isFinite(value)) return fallback;
    return Math.max(min,Math.min(max,value));
}

const count=integerOption("--count",40,1,MAX_COUNT);
const interval=integerOption("--interval",20,0,1000);

function sleep(ms) {
    return new Promise(resolve=>setTimeout(resolve,ms));
}

function timeout(ms,message) {
    return new Promise((_,reject)=>setTimeout(()=>reject(new Error(message)),ms));
}

function openSocket(target=url) {
    return new Promise((resolve,reject)=>{
        const ws=new WebSocket(target);
        let settled=false;

        const timer=setTimeout(()=>{
            if(settled) return;
            settled=true;
            try { ws.close(); } catch {}
            reject(new Error("WebSocket open timed out"));
        },5000);

        ws.addEventListener("open",()=>{
            if(settled) return;
            settled=true;
            clearTimeout(timer);
            resolve(ws);
        },{once:true});

        ws.addEventListener("error",event=>{
            if(settled) return;
            settled=true;
            clearTimeout(timer);
            reject(new Error(event?.message||"WebSocket connection failed"));
        },{once:true});

        ws.addEventListener("close",event=>{
            if(settled) return;
            settled=true;
            clearTimeout(timer);
            reject(new Error(`WebSocket closed during connect: ${event.code} ${event.reason}`));
        },{once:true});
    });
}

function waitForMessage(ws,predicate,timeoutMs=5000) {
    return new Promise((resolve,reject)=>{
        const timer=setTimeout(()=>{
            ws.removeEventListener("message",onMessage);
            reject(new Error("Timed out waiting for signaling response"));
        },timeoutMs);

        function onMessage(event) {
            const value=String(event.data);
            if(!predicate(value)) return;
            clearTimeout(timer);
            ws.removeEventListener("message",onMessage);
            resolve(value);
        }

        ws.addEventListener("message",onMessage);
    });
}

function waitForClose(ws,timeoutMs=4000) {
    if(ws.readyState===WebSocket.CLOSED) return Promise.resolve({code:1006,reason:"already closed"});

    return new Promise(resolve=>{
        const timer=setTimeout(()=>{
            ws.removeEventListener("close",onClose);
            resolve(null);
        },timeoutMs);

        function onClose(event) {
            clearTimeout(timer);
            ws.removeEventListener("close",onClose);
            resolve({code:event.code,reason:event.reason});
        }

        ws.addEventListener("close",onClose);
    });
}

async function sendBurst(ws,makeMessage,total=count) {
    let sent=0;

    for(let i=0;i<total;++i) {
        if(ws.readyState!==WebSocket.OPEN) break;
        ws.send(makeMessage(i));
        ++sent;
        if(interval>0) await sleep(interval);
    }

    return sent;
}

function closeQuietly(ws) {
    if(!ws) return;
    try {
        if(ws.readyState===WebSocket.OPEN || ws.readyState===WebSocket.CONNECTING) ws.close(1000,"test complete");
    } catch {}
}

function printResult(name,details,pass) {
    console.log(JSON.stringify({mode:name,url,...details,result:pass?"PASS":"FAIL"},null,2));
    if(!pass) process.exitCode=1;
}

async function roomSpam() {
    const ws=await openSocket();
    const sent=await sendBurst(ws,i=>i%2===0 ? "CREATE" : "LEAVE");
    const closed=await waitForClose(ws);
    const pass=Boolean(closed && closed.code===1008);
    printResult("room-spam",{requested:count,sent,close:closed,expected:"server hard-closes abusive room-command session"},pass);
    closeQuietly(ws);
}

async function unknownSpam() {
    const ws=await openSocket();
    const sent=await sendBurst(ws,i=>`UNKNOWN_${i}`);
    const closed=await waitForClose(ws);
    const pass=Boolean(closed && closed.code===1008);
    printResult("unknown-spam",{requested:count,sent,close:closed,expected:"generic inbound-message guard closes the session"},pass);
    closeQuietly(ws);
}

async function binarySpam() {
    const ws=await openSocket();
    const sent=await sendBurst(ws,i=>new Uint8Array([i&255]));
    const closed=await waitForClose(ws);
    const pass=Boolean(closed && closed.code===1008);
    printResult("binary-spam",{requested:count,sent,close:closed,expected:"binary frames count toward the generic inbound-message guard"},pass);
    closeQuietly(ws);
}

async function signalSpam() {
    let creator;
    let guest;

    try {
        const requested=integerOption("--count",80,1,MAX_COUNT);
        creator=await openSocket();
        const roomMessage=waitForMessage(creator,message=>message.startsWith("ROOM "));
        creator.send("CREATE");
        const roomCode=(await roomMessage).slice(5);

        guest=await openSocket();
        const joined=waitForMessage(guest,message=>message===`JOINED ${roomCode}`);
        guest.send(`JOIN ${roomCode}`);
        await joined;

        const sent=await sendBurst(guest,i=>`SIGNAL\nABUSE_TEST_${i}`,requested);
        const closed=await waitForClose(guest);
        const pass=Boolean(closed && closed.code===1008);
        printResult("signal-spam",{roomCode,requested,sent,close:closed,expected:"room-membership SIGNAL budget eventually hard-closes abusive signaling"},pass);
    } finally {
        closeQuietly(guest);
        closeQuietly(creator);
    }
}

function memberId(index) {
    return index.toString(16).toUpperCase().padStart(32,"0");
}

function encodeName(name) {
    return [...new TextEncoder().encode(name)]
        .map(value=>value.toString(16).toUpperCase().padStart(2,"0"))
        .join("");
}

function trackMessages(ws) {
    const messages=[];
    ws.addEventListener("message",event=>messages.push(String(event.data)));
    return messages;
}

async function multiPeerRoom() {
    const peerCount=integerOption("--peers",3,3,12);
    const sockets=[];
    const messages=[];

    try {
        const creator=await openSocket();
        sockets.push(creator);
        messages.push(trackMessages(creator));

        const roomMessage=waitForMessage(creator,message=>message.startsWith("ROOM "));
        creator.send(`CREATE ${memberId(1)} ${encodeName("Peer1")}`);
        const roomCode=(await roomMessage).slice(5);

        for(let i=2;i<=peerCount;++i) {
            const ws=await openSocket();
            sockets.push(ws);
            messages.push(trackMessages(ws));
            const joined=waitForMessage(ws,message=>message===`JOINED ${roomCode}`);
            ws.send(`JOIN ${roomCode} ${memberId(i)} ${encodeName(`Peer${i}`)}`);
            await joined;
        }

        await sleep(1800);

        const membershipChecks=[];
        for(let i=0;i<peerCount;++i) {
            const expected=[];
            for(let j=0;j<peerCount;++j) {
                if(i===j) continue;
                const id=memberId(j+1);
                expected.push({
                    memberId:id,
                    info:messages[i].some(message=>message.startsWith(`PEER_INFO\n${id}\n`)),
                    ready:messages[i].includes(`PEER_READY ${id}`)
                });
            }
            membershipChecks.push(expected);
        }

        const marker=`TARGET_TEST_${Date.now()}`;
        const targetPromise=waitForMessage(
            sockets[1],
            message=>message===`SIGNAL ${memberId(1)}\n${marker}`
        );
        sockets[0].send(`SIGNAL ${memberId(2)}\n${marker}`);
        await targetPromise;
        await sleep(150);

        const leaked=messages.slice(2).some(list=>list.some(message=>message.includes(marker)));
        const complete=membershipChecks.every(list=>list.every(check=>check.info && check.ready));
        const pass=complete && !leaked;

        printResult("multi-peer",{
            roomCode,
            peerCount,
            membershipChecks,
            targetedSignalLeakedToBystander:leaked,
            expected:"all peers discover each other and targeted signaling reaches only the selected member"
        },pass);
    } finally {
        for(const ws of sockets) closeQuietly(ws);
    }
}

async function livenessSteady() {
    const seconds=integerOption("--seconds",105,95,300);
    let creator;
    let guest;
    let creatorHeartbeat;
    let guestHeartbeat;

    try {
        creator=await openSocket();
        const creatorMessages=trackMessages(creator);
        const roomMessage=waitForMessage(creator,message=>message.startsWith("ROOM "));
        creator.send(`CREATE ${memberId(1)} ${encodeName("SteadyA")}`);
        const roomCode=(await roomMessage).slice(5);

        guest=await openSocket();
        const guestMessages=trackMessages(guest);
        const joined=waitForMessage(guest,message=>message===`JOINED ${roomCode}`);
        guest.send(`JOIN ${roomCode} ${memberId(2)} ${encodeName("SteadyB")}`);
        await joined;
        await sleep(1500);

        const sendAlive=ws=>{
            if(ws?.readyState===WebSocket.OPEN) ws.send("ALIVE");
        };

        sendAlive(creator);
        sendAlive(guest);
        creatorHeartbeat=setInterval(()=>sendAlive(creator),60000);
        guestHeartbeat=setInterval(()=>sendAlive(guest),60000);

        const deadline=Date.now()+seconds*1000;
        while(Date.now()<deadline) {
            if(creator.readyState!==WebSocket.OPEN || guest.readyState!==WebSocket.OPEN) break;
            await sleep(1000);
        }

        const badCreatorMessage=creatorMessages.find(message=>
            message.startsWith("PEER_LEFT ") || message.startsWith("ERROR ")
        )??null;
        const badGuestMessage=guestMessages.find(message=>
            message.startsWith("PEER_LEFT ") || message.startsWith("ERROR ")
        )??null;

        const creatorOpen=creator.readyState===WebSocket.OPEN;
        const guestOpen=guest.readyState===WebSocket.OPEN;
        const pass=creatorOpen && guestOpen && !badCreatorMessage && !badGuestMessage;

        printResult("liveness-steady",{
            roomCode,
            seconds,
            creatorOpen,
            guestOpen,
            badCreatorMessage,
            badGuestMessage,
            expected:"both healthy room members remain connected beyond the 90-second stale timeout"
        },pass);
    } finally {
        if(creatorHeartbeat) clearInterval(creatorHeartbeat);
        if(guestHeartbeat) clearInterval(guestHeartbeat);
        closeQuietly(guest);
        closeQuietly(creator);
    }
}

async function livenessTimeout() {
    let survivor;
    let stale;
    let heartbeatTimer;

    try {
        survivor=await openSocket();
        const survivorMessages=trackMessages(survivor);
        const roomMessage=waitForMessage(survivor,message=>message.startsWith("ROOM "));
        survivor.send(`CREATE ${memberId(1)} ${encodeName("Survivor")}`);
        const roomCode=(await roomMessage).slice(5);

        stale=await openSocket();
        const joined=waitForMessage(stale,message=>message===`JOINED ${roomCode}`);
        stale.send(`JOIN ${roomCode} ${memberId(2)} ${encodeName("Stale")}`);
        await joined;

        await sleep(1500);

        heartbeatTimer=setInterval(()=>{
            if(survivor?.readyState===WebSocket.OPEN) survivor.send("ALIVE");
        },30000);

        survivor.send("ALIVE");

        const staleClosed=waitForClose(stale,130000);
        const peerLeft=waitForMessage(
            survivor,
            message=>message===`PEER_LEFT ${memberId(2)}`,
            130000
        );

        const [closeResult,leftMessage]=await Promise.all([staleClosed,peerLeft]);
        const survivorStayedOpen=survivor.readyState===WebSocket.OPEN;
        const pass=
            Boolean(closeResult && closeResult.code===1001) &&
            leftMessage===`PEER_LEFT ${memberId(2)}` &&
            survivorStayedOpen;

        printResult("liveness-timeout",{
            roomCode,
            staleClose:closeResult,
            peerLeft:leftMessage,
            survivorStayedOpen,
            survivorMessages:survivorMessages.slice(-8),
            expected:"stale member is closed by room liveness timeout while the healthy member remains connected"
        },pass);
    } finally {
        if(heartbeatTimer) clearInterval(heartbeatTimer);
        closeQuietly(stale);
        closeQuietly(survivor);
    }
}

async function reconnectSpam() {
    const attempts=Math.min(count,30);
    let opened=0;
    let rejected=0;
    const errors=[];

    for(let i=0;i<attempts;++i) {
        try {
            const ws=await openSocket();
            ++opened;
            ws.close(1000,"reconnect abuse test");
            await Promise.race([waitForClose(ws,1500),sleep(1500)]);
        } catch(error) {
            ++rejected;
            errors.push(String(error.message||error));
        }

        if(interval>0) await sleep(interval);
    }

    const observedProtection=rejected>0;
    console.log(JSON.stringify({
        mode:"reconnect-spam",
        url,
        attempts,
        opened,
        rejected,
        result:observedProtection ? "PASS_OBSERVED_REJECTION" : "NO_REJECTION_OBSERVED",
        note:"Cloudflare rate-limit bindings are intentionally permissive/eventually consistent. This mode records whether any rejection was observed; it does not assert an exact cutoff.",
        errors:errors.slice(0,5)
    },null,2));
}

async function mixedSpam() {
    const ws=await openSocket();
    const sent=await sendBurst(ws,i=>{
        switch(i%4) {
            case 0: return "CREATE";
            case 1: return "LEAVE";
            case 2: return `UNKNOWN_${i}`;
            default: return new Uint8Array([i&255]);
        }
    });
    const closed=await waitForClose(ws);
    const pass=Boolean(closed && closed.code===1008);
    printResult("mixed-spam",{requested:count,sent,close:closed,expected:"generic or command-specific server guard hard-closes the session"},pass);
    closeQuietly(ws);
}

async function rrInvalidAuth() {
    let ws;
    try {
        ws=await openSocket();
        const response=waitForMessage(
            ws,
            message=>message.startsWith("RR_AUTH_FAIL "),
            8000
        );
        ws.send("RR_AUTH 1 1 mariokartwii");
        const message=await response;
        const pass=message==="RR_AUTH_FAIL GPSP rejected this live session";
        printResult("rr-invalid-auth",{
            response:message,
            expected:"RR_AUTH_FAIL GPSP rejected this live session"
        },pass);
    } finally {
        closeQuietly(ws);
    }
}

async function rrRosterDebug() {
    const pid=option("--pid","");
    if(!/^[0-9]{1,10}$/.test(pid)) {
        throw new Error("Pass an active Retro Rewind PID with --pid <profileId>");
    }

    let ws;
    try {
        ws=await openSocket();
        const response=waitForMessage(
            ws,
            message=>message.startsWith("RR_DEBUG_STATUS\n") || message.startsWith("RR_DEBUG_FAIL "),
            8000
        );
        ws.send("RR_DEBUG_LOOKUP "+pid);
        const message=await response;
        const pass=message.startsWith("RR_DEBUG_STATUS\n");
        printResult("rr-roster-debug",{
            pid,
            response:message,
            expected:"RR_DEBUG_STATUS with the public room/roster snapshot; this does NOT authenticate the PID"
        },pass);
    } finally {
        closeQuietly(ws);
    }
}

async function health() {
    const healthUrl=url.replace(/^wss:/,"https:").replace(/^ws:/,"http:").replace(/\/$/,"")+"/health?ts="+Date.now();
    const response=await Promise.race([fetch(healthUrl,{cache:"no-store"}),timeout(5000,"Health request timed out")]);
    const text=await response.text();
    console.log(JSON.stringify({mode:"health",url:healthUrl,status:response.status,body:text},null,2));
    if(!response.ok) process.exitCode=1;
}

function usage() {
    console.log(`Usage:
  npm.cmd run abuse-test -- health
  npm.cmd run abuse-test -- room-spam [--count 40] [--interval 20]
  npm.cmd run abuse-test -- signal-spam [--count 80] [--interval 20]
  npm.cmd run abuse-test -- multi-peer [--peers 3]
  npm.cmd run abuse-test -- liveness-timeout
  npm.cmd run abuse-test -- liveness-steady [--seconds 105]
  npm.cmd run abuse-test -- rr-invalid-auth
  npm.cmd run abuse-test -- rr-roster-debug --pid <profileId>
  npm.cmd run abuse-test -- unknown-spam [--count 40] [--interval 20]
  npm.cmd run abuse-test -- binary-spam [--count 40] [--interval 20]
  npm.cmd run abuse-test -- mixed-spam [--count 40] [--interval 20]
  npm.cmd run abuse-test -- reconnect-spam [--count 12] [--interval 100]
  Add --local to target ws://127.0.0.1:8787 instead of the project workers.dev endpoint.

Production runs are intentionally capped at ${MAX_COUNT} messages and 30 reconnect attempts per invocation.
Wait at least 60 seconds between production abuse modes so IP-level windows do not contaminate the next result.`);
}

try {
    switch(mode) {
        case "health": await health(); break;
        case "room-spam": await roomSpam(); break;
        case "signal-spam": await signalSpam(); break;
        case "multi-peer": await multiPeerRoom(); break;
        case "liveness-timeout": await livenessTimeout(); break;
        case "liveness-steady": await livenessSteady(); break;
        case "rr-invalid-auth": await rrInvalidAuth(); break;
        case "rr-roster-debug": await rrRosterDebug(); break;
        case "unknown-spam": await unknownSpam(); break;
        case "binary-spam": await binarySpam(); break;
        case "mixed-spam": await mixedSpam(); break;
        case "reconnect-spam": await reconnectSpam(); break;
        default:
            usage();
            process.exitCode=mode ? 1 : 0;
            break;
    }
} catch(error) {
    console.error(JSON.stringify({mode,url,result:"ERROR",error:String(error?.message??error)},null,2));
    process.exitCode=1;
}
