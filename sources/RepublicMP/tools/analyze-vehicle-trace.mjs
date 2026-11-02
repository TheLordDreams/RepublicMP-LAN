// SPDX-License-Identifier: GPL-3.0-only
import fs from 'node:fs';
const [file,indexText='9']=process.argv.slice(2);
if(!file)throw Error('Usage: node tools/analyze-vehicle-trace.mjs PRIVATE_TRACE.jsonl [vehicle-index]');
const records=fs.readFileSync(file,'utf8').trim().split(/\r?\n/).map(JSON.parse).filter(x=>x.before?.index===Number(indexText));
const groups=new Map(),movement=[];
for(const r of records){
 const key=JSON.stringify([r.completedTick,r.callerRva,r.isStepFrame,r.inStepFrame]);
 const g=groups.get(key)??{tick:r.completedTick,caller:r.callerRva,step:r.isStepFrame,inStep:r.inStepFrame,calls:0,moves:0};
 g.calls++;
 if(r.after&&r.before.pos.some((x,i)=>x!==r.after.pos[i])){g.moves++;movement.push(r);}
 groups.set(key,g);
}
console.log(JSON.stringify({vehicle:Number(indexText),records:records.length,groups:[...groups.values()],movement:movement.slice(0,50)},null,2));
