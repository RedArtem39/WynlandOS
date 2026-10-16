#!/bin/sh
# Node.js on WynlandOS (boot.cfg "wlrun=sh+/usr/share/wynland/qs-smoke/node.sh"
# in a session of an administrator with the password "wyn"): installs
# Ubuntu's nodejs with leaf and runs small checks; its log on the serial
# console, every line "[node] ...".
exec 2>&1
say() { echo "[node] $*"; }
if ! command -v node >/dev/null; then
    say "installing"
    echo wyn | ary -S leaf update 2>&1 | tail -2 | sed "s/^/[node] leaf: /"
    echo wyn | ary -S leaf install nodejs 2>&1 | tail -5 | sed 's/^/[node] leaf: /'
fi
say "version: $(node --version 2>&1)"
say "eval: $(node -e 'console.log(6 * 7)' 2>&1)"
say "jit loop: $(node -e 'let s=0; for (let i=0;i<5e7;i++) s+=i%7; console.log(s)' 2>&1)"
say "fs: $(node -e 'const fs=require("fs"); fs.writeFileSync("/tmp/n.txt","hi"); console.log(fs.readFileSync("/tmp/n.txt","utf8"))' 2>&1)"
say "timers+promises: $(node -e 'setTimeout(()=>Promise.resolve(5).then(v=>console.log(v)),50)' 2>&1)"
say "child_process: $(node -e 'console.log(require("child_process").execSync("echo kid").toString().trim())' 2>&1)"
say "worker_threads: $(node -e 'const {Worker}=require("worker_threads"); new Worker("require(\"worker_threads\").parentPort.postMessage(9)",{eval:true}).on("message",m=>console.log(m))' 2>&1)"
say "http: $(node -e 'const h=require("http"); const s=h.createServer((q,r)=>r.end("pong")).listen(8123,()=>h.get("http://127.0.0.1:8123",r=>{let d="";r.on("data",c=>d+=c);r.on("end",()=>{console.log(d);s.close()})}))' 2>&1)"
say "https: $(node -e 'require("https").get("https://example.com",r=>{console.log(r.statusCode);r.resume()}).on("error",e=>console.log("err "+e.message))' 2>&1)"
say "crypto: $(node -e 'console.log(require("crypto").createHash("sha256").update("wyn").digest("hex").slice(0,16))' 2>&1)"
if ! command -v npm >/dev/null; then
    say "installing npm"
    echo wyn | ary -S leaf install npm 2>&1 | tail -3 | sed 's/^/[node] leaf: /'
fi
say "npm: $(npm --version 2>&1 | tail -1)"
mkdir -p /tmp/np && cd /tmp/np
say "npm init: $(npm init -y 2>&1 | grep -c '"name"')"
say "npm install: $(timeout 300 npm install --no-audit --no-fund is-odd 2>&1 | tail -2 | tr '\n' ' ')"
say "require: $(node -e 'console.log(require("is-odd")(3))' 2>&1)"
say "done"
