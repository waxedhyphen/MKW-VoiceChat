const args=process.argv.slice(2);

function option(name,fallback) {
    const index=args.indexOf(name);
    return index>=0 && index+1<args.length ? args[index+1] : fallback;
}

const base=option("--url",process.env.MKWVC_RR_VERIFY_URL || "http://127.0.0.1:8788/verify");
const secret=option("--secret",process.env.MKWVC_RR_VERIFY_SECRET || "");

if(!secret) {
    console.error("Pass --secret or set MKWVC_RR_VERIFY_SECRET.");
    process.exit(1);
}

const response=await fetch(base,{
    method:"POST",
    headers:{
        "Accept":"application/json",
        "Content-Type":"application/json",
        "Authorization":"Bearer "+secret
    },
    body:JSON.stringify({
        profileId:"1",
        sessionKey:"1",
        gameName:"mariokartwii"
    })
});

let body;
try {
    body=await response.json();
} catch {
    body=null;
}

const pass=response.status===200 && body?.valid===false;
console.log(JSON.stringify({
    url:base,
    status:response.status,
    body,
    expected:"HTTP 200 with valid=false for deliberately invalid RR credentials",
    result:pass ? "PASS" : "FAIL"
},null,2));

if(!pass) process.exitCode=1;
