import http from "node:http";
import net from "node:net";
import crypto from "node:crypto";

const HOST=process.env.HOST?.trim() || "0.0.0.0";
const PORT=parseInteger(process.env.PORT,8788,1,65535);
const GPSP_HOST=process.env.MKWVC_RR_GPSP_HOST?.trim() || "gpsp.gs.play.rwfc.net";
const GPSP_PORT=parseInteger(process.env.MKWVC_RR_GPSP_PORT,29901,1,65535);
const VERIFY_SECRET=process.env.MKWVC_RR_VERIFY_SECRET?.trim() || "";
const REQUEST_TIMEOUT_MS=parseInteger(process.env.MKWVC_RR_VERIFY_TIMEOUT_MS,5000,1000,15000);
const MAX_BODY_BYTES=2048;
const MAX_GPSP_RESPONSE_BYTES=16*1024;

if(!VERIFY_SECRET) {
    console.error("MKWVC_RR_VERIFY_SECRET is required.");
    process.exit(1);
}

function parseInteger(value,fallback,min,max) {
    const parsed=Number.parseInt(value??"",10);
    if(!Number.isFinite(parsed)) return fallback;
    return Math.max(min,Math.min(max,parsed));
}

function sendJson(res,status,payload) {
    const body=JSON.stringify(payload);
    res.writeHead(status,{
        "Content-Type":"application/json; charset=utf-8",
        "Content-Length":Buffer.byteLength(body),
        "Cache-Control":"no-store",
        "X-Content-Type-Options":"nosniff"
    });
    res.end(body);
}

function secretMatches(header) {
    if(typeof header!=="string" || !header.startsWith("Bearer ")) return false;
    const supplied=Buffer.from(header.slice(7),"utf8");
    const expected=Buffer.from(VERIFY_SECRET,"utf8");
    if(supplied.length!==expected.length) return false;
    return crypto.timingSafeEqual(supplied,expected);
}

function validProfileId(value) {
    return typeof value==="string" && /^[0-9]{1,10}$/.test(value);
}

function validSessionKey(value) {
    return typeof value==="string" && /^-?[0-9]{1,10}$/.test(value);
}

function validGameName(value) {
    return typeof value==="string" && /^[A-Za-z0-9_-]{1,32}$/.test(value);
}

function readJsonBody(req) {
    return new Promise((resolve,reject)=>{
        const chunks=[];
        let size=0;

        req.on("data",chunk=>{
            size+=chunk.length;
            if(size>MAX_BODY_BYTES) {
                reject(Object.assign(new Error("request body too large"),{statusCode:413}));
                req.destroy();
                return;
            }
            chunks.push(chunk);
        });

        req.on("end",()=>{
            try {
                resolve(JSON.parse(Buffer.concat(chunks).toString("utf8")));
            } catch {
                reject(Object.assign(new Error("invalid JSON"),{statusCode:400}));
            }
        });

        req.on("error",reject);
    });
}

function verifyGpspSession(profileId,sessionKey,gameName) {
    return new Promise((resolve,reject)=>{
        const socket=net.createConnection({
            host:GPSP_HOST,
            port:GPSP_PORT
        });

        let response="";
        let settled=false;

        const finish=(error,result)=>{
            if(settled) return;
            settled=true;
            socket.destroy();
            if(error) reject(error);
            else resolve(result);
        };

        socket.setTimeout(REQUEST_TIMEOUT_MS,()=>{
            finish(new Error("GPSP request timed out"));
        });

        socket.on("connect",()=>{
            const message=
                "\\otherslist\\"+
                "\\profileid\\"+profileId+
                "\\sesskey\\"+sessionKey+
                "\\numopids\\0"+
                "\\opids\\0"+
                "\\gamename\\"+gameName+
                "\\final\\";
            socket.write(message);
        });

        socket.on("data",chunk=>{
            response+=chunk.toString("utf8");
            if(response.length>MAX_GPSP_RESPONSE_BYTES) {
                finish(new Error("GPSP response exceeded limit"));
                return;
            }

            if(!response.includes("\\final\\")) return;

            const valid=
                response.startsWith("\\otherslist\\") &&
                response.includes("\\oldone\\") &&
                response.includes("\\final\\");
            finish(null,valid);
        });

        socket.on("end",()=>{
            if(settled) return;
            if(!response.includes("\\final\\")) {
                finish(new Error("GPSP connection ended before final response"));
                return;
            }
            const valid=
                response.startsWith("\\otherslist\\") &&
                response.includes("\\oldone\\") &&
                response.includes("\\final\\");
            finish(null,valid);
        });

        socket.on("error",error=>finish(error));
    });
}

const server=http.createServer(async(req,res)=>{
    const url=new URL(req.url??"/","http://localhost");

    if(req.method==="GET" && url.pathname==="/health") {
        sendJson(res,200,{
            service:"mkw-voicechat-rr-verifier",
            status:"ok",
            gpspHost:GPSP_HOST,
            gpspPort:GPSP_PORT
        });
        return;
    }

    if(req.method!=="POST" || url.pathname!=="/verify") {
        sendJson(res,404,{error:"not found"});
        return;
    }

    if(!secretMatches(req.headers.authorization)) {
        sendJson(res,401,{error:"unauthorized"});
        return;
    }

    try {
        const body=await readJsonBody(req);
        const profileId=body?.profileId;
        const sessionKey=body?.sessionKey;
        const gameName=body?.gameName;

        if(!validProfileId(profileId) ||
           !validSessionKey(sessionKey) ||
           !validGameName(gameName)) {
            sendJson(res,400,{error:"invalid request"});
            return;
        }

        const valid=await verifyGpspSession(profileId,sessionKey,gameName);
        sendJson(res,200,{
            valid,
            profileId:valid ? profileId : null
        });
    } catch(error) {
        const status=Number.isInteger(error?.statusCode) ? error.statusCode : 502;
        console.error(JSON.stringify({
            event:"verify_error",
            message:String(error?.message??error)
        }));
        if(!res.headersSent) {
            sendJson(res,status,{error:status===502 ? "gpsp unavailable" : String(error?.message??"request failed")});
        } else {
            res.destroy();
        }
    }
});

server.requestTimeout=10_000;
server.headersTimeout=10_000;
server.keepAliveTimeout=5_000;

server.listen(PORT,HOST,()=>{
    console.log(JSON.stringify({
        event:"listening",
        host:HOST,
        port:PORT,
        gpspHost:GPSP_HOST,
        gpspPort:GPSP_PORT
    }));
});

for(const signal of ["SIGINT","SIGTERM"]) {
    process.on(signal,()=>{
        server.close(()=>process.exit(0));
    });
}
