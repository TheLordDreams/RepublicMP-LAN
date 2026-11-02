// SPDX-License-Identifier: GPL-3.0-only
// Offline analysis of private opt-in QA snapshots, never touches game memory.
import fs from 'node:fs';
import path from 'node:path';
const [left,right,indexText='9']=process.argv.slice(2);
if(!left||!right)throw Error('Usage: node tools/analyze-vehicle-bytes.mjs LEFT.bin RIGHT.bin [vehicle-index]');
function read(file){
 const b=fs.readFileSync(file),count=b.readUInt32LE(0),size=b.readUInt32LE(4);
 if(size!==0x1ad8||count>65536||b.length!==8+count*size)throw Error('Incomplete/unsupported snapshot');
 const records=new Map();for(let i=0;i<count;i++){const r=b.subarray(8+i*size,8+(i+1)*size);records.set(r.readInt32LE(4),r);}return records;
}
const a=read(left).get(Number(indexText)),b=read(right).get(Number(indexText));
if(!a||!b)throw Error('Vehicle not found');
const pointer=v=>v>=0x100000000n&&v<0x800000000000n&&v%8n===0n;
let skipped=0,differences=[];
for(let i=0;i<a.length;i+=8){
 if(pointer(a.readBigUInt64LE(i))||pointer(b.readBigUInt64LE(i))){skipped++;continue;}
 for(let j=i;j<i+8;j+=4)if(a.readUInt32LE(j)!==b.readUInt32LE(j)) differences.push({offset:'0x'+j.toString(16),leftHex:a.subarray(j,j+4).toString('hex'),rightHex:b.subarray(j,j+4).toString('hex'),leftFloat:a.readFloatLE(j),rightFloat:b.readFloatLE(j)});
}
console.log(JSON.stringify({left:path.basename(left),right:path.basename(right),vehicle:Number(indexText),pointerPairsSkipped:skipped,differences},null,2));
