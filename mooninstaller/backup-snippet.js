// The "Backup MoonLight config" bookmarklet: a self-contained copy of the backup walker in
// src/ui/migrate.js, because it targets firmware that PREDATES the File Manager's
// Backup button, it runs same-origin on the old device's own page (no CORS, no mixed
// content), walks the filesystem over the file API served since v3.0.0 (July 2026), and
// downloads the same bundle the new Restore button accepts. The format field below keeps the
// OLD product name because this tool's own audience is old firmware, whose Restore accepts only
// that value, while the current Restore accepts both. Exported as a string so the
// installer page renders it (bookmarklet link + copyable console snippet) and a node test
// executes it against a mocked device.
export const BACKUP_SNIPPET = "javascript:" + `(async()=>{let ui;try{
const fd=async p=>{const r=await fetch('/api/dir?path='+encodeURIComponent(p)+'&hidden=1');if(!r.ok)throw Error('dir '+p+': HTTP '+r.status);return r.json()};
const ff=async p=>{const r=await fetch('/api/file?path='+encodeURIComponent(p));if(!r.ok)throw Error(p+': HTTP '+r.status);return new TextDecoder('utf-8',{ignoreBOM:true}).decode(await r.arrayBuffer())};
ui=document.createElement('div');ui.textContent='MoonLight backup: reading files…';ui.style.cssText='position:fixed;top:10px;right:10px;z-index:99999;background:#222;color:#fff;padding:10px 14px;border-radius:8px;font:14px sans-serif;box-shadow:0 2px 8px rgba(0,0,0,.4)';document.body.appendChild(ui);
const files={};
const skipped=[];const sz=t=>new TextEncoder().encode(t).length;const walk=async d=>{for(let e of await fd(d)){const p=(d==='/'?'':d)+'/'+e.name;if(p==='/.hls')continue;if(e.isDir)await walk(p);else{let t;try{t=await ff(p)}catch(_){skipped.push(p);continue}let n=sz(t);if(n!==e.size){const l=(await fd(d)).find(x=>x.name===e.name);if(l)e=l;try{t=await ff(p)}catch(_){skipped.push(p);continue}n=sz(t)}if(n<e.size)throw Error(p+': got '+n+' of '+e.size+' bytes (truncated read)');if(n>e.size)skipped.push(p);else{files[p]=t;ui.textContent='MoonLight backup: '+Object.keys(files).length+' files…'}}}};
await walk('/');
let device=location.hostname||'device',firmware='',build='';
try{const st=await(await fetch('/api/state')).json();const w=ms=>{for(const m of ms||[]){for(const c of m.controls||[]){if(c.name==='deviceName'&&c.value)device=c.value;if(c.name==='firmware')firmware=c.value||'';if(c.name==='build')build=c.value||''}w(m.children)}};w(st.modules)}catch(_){}
const b={format:'projectMM-config-backup',version:1,capturedAt:new Date().toISOString(),origin:location.origin,device:device,firmware:firmware,build:build,files:files};
ui.remove();const u=URL.createObjectURL(new Blob([JSON.stringify(b,null,1)],{type:'application/json'}));
const a=document.createElement('a');a.href=u;a.download='MoonLight-config-'+device+'-'+new Date().toISOString().slice(0,10)+'.json';document.body.appendChild(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(u),4000);
alert('Backup downloaded: '+Object.keys(files).length+' files'+(skipped.length?', skipped (not text): '+skipped.join(', '):'')+'. Keep the file private: it contains the WiFi password.');
}catch(e){if(ui)ui.remove();alert('Backup failed: '+e.message)}})()`.replace(/\n/g, "");
