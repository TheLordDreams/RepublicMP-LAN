// SPDX-License-Identifier: GPL-3.0-only
// Export selected successful native runs without machine paths, PIDs or saves.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const [output,...ids]=process.argv.slice(2);
if(!output||!ids.length)throw Error('Usage: node tools/export-qa-summary.mjs OUTPUT.json RUN_ID ...');
const target=path.resolve(output),outputs=path.resolve(repo,'../..','outputs');
const relative=path.relative(outputs,target);
assert.ok(relative&&!relative.startsWith('..')&&!path.isAbsolute(relative),'Export must be under workspace outputs');
const read=file=>JSON.parse(fs.readFileSync(file,'utf8').replace(/^\uFEFF/,''));
let dllSha256;
const runs=ids.map(id=>{
  assert.match(id,/^\d{4}-\d\d-\d\dT[\d-]+Z$/,'Use a QA run directory name');
  const root=path.join(repo,'local','qa-runs',id),r=read(path.join(root,'result.json'));
  assert.equal(r.outcome,'PASS',id+' has not passed');
  if(dllSha256)assert.equal(r.dllSha256,dllSha256,'Selected runs use different DLLs');else dllSha256=r.dllSha256;
  const samples=read(path.join(root,'matched-samples.json')).sort((a,b)=>a.tick-b.tick);
  const finalStatus=read(path.join(root,'final-status.json'));
  assert.equal(finalStatus.length,2);
  assert.ok(samples.length>0);
  for(const sample of samples){
    for(const key of ['all','clock','buildingsHash','vehiclesHash','citizensHash'])assert.match(sample[key],/^[a-f0-9]{16}$/);
    assert.equal(sample.vehicleRecordsValid,true);
    assert.ok(sample.vehicleRecords.length>0&&sample.vehicleRecords.every(v=>v.pos.every(Number.isFinite)));
  }
  const construction=[];
  for(let slot=0;slot<2;slot++){
    const file=path.join(root,'construction-slot-'+slot+'.json');if(!fs.existsSync(file))continue;
    const c=read(file),actor=slot===0?'host':'client';
    assert.equal(c.built[0].pausedState.all,c.built[1].pausedState.all);
    assert.equal(c.removed[0].pausedState.all,c.removed[1].pausedState.all);
    for(let i=0;i<2;i++){
      assert.ok(c.built[i].probeSlots[slot].progress>=1);
      assert.equal(c.placed[i].pausedState.buildings,c.before[i].pausedState.buildings+1);
      assert.equal(c.removed[i].pausedState.buildings,c.before[i].pausedState.buildings);
      assert.ok(c.removed[i].probeSlots[slot].demolished&&!c.removed[i].probeSlots[slot].exists);
      assert.equal(c.demolition[i].commandsApplyEpoch,c.built[i].commandsApplyEpoch+1);
      for(const key of ['localRub','localUsd'])assert.equal(c.demolition[i].probeSlots[slot][key],c.before[i].probeSlots[slot][key]);
    }
    const log=fs.readFileSync(path.join(root,actor+'-tesmioloader.log'),'utf8');
    assert.equal(log.split('LOCKSTEP: building demolition captured').length-1,1,'Expected one actual native demolition capture by '+actor);
    const net=c.before[0].moneyRubles-c.built[0].moneyRubles;
    assert.equal(net,c.before[1].moneyRubles-c.built[1].moneyRubles);
    construction.push({actor,nativeBuildingCompleted:true,nativeBuildingRemoved:true,nativeDemolitionCaptureCount:1,
      demolitionReplayedExactlyOnce:true,localCurrencyUnchangedAfterReplay:true,equalNetRubleDecrease:net});
  }
  let longRun=r.longRun;
  if(longRun){
    assert.ok(longRun.elapsedMs>=longRun.requestedMinutes*60000);
    const active=samples.filter(s=>s.tick>=Math.max(...longRun.startTicks));let movingIntervals=0;
    for(let i=1;i<active.length;i++)if(active[i].vehicleRecords.some(v=>{
      const prev=active[i-1].vehicleRecords.find(p=>p.index===v.index);return prev&&v.pos.some((n,j)=>n!==prev.pos[j]);
    }))movingIntervals++;
    assert.ok(movingIntervals>0,'Long run did not demonstrate moving vehicles');
    longRun={...longRun,sampledIntervalsWithMovingVehicles:movingIntervals,exactTickSamplesDuringLongRun:active.length};
  }
  let proxy=null;
  if(r.chaos!=='none'){
    const p=read(path.join(root,'tcp-chaos-proxy.summary.json'));
    const used=p.connections.filter(c=>c.connected&&c.clientToHost.forwardedBytes>0&&c.hostToClient.forwardedBytes>0);
    assert.ok(used.length);assert.equal(p.profile,r.chaos);
    proxy={profile:p.profile,usedConnections:used.length,maxWriteChunk:256,
      hostToClientBytes:used.reduce((n,c)=>n+c.hostToClient.forwardedBytes,0),
      clientToHostBytes:used.reduce((n,c)=>n+c.clientToHost.forwardedBytes,0),
      injectedDisconnects:used.filter(c=>c.endReason==='injected-disconnect').length,
      scope:'TCP stream delay/jitter/fragmentation or explicit connection closure; not OS packet loss'};
    if(r.disconnectAfterMs){assert.equal(proxy.injectedDisconnects,1);assert.ok(used.length>=2);}
  }
  return {run:id,outcome:r.outcome,completedChecks:r.completedChecks,exactTickMatches:r.comparedTicks.length,
    lastComparedTick:Math.max(...r.comparedTicks),nativeHostLastMatchedTick:finalStatus[0].lastMatchedTick,construction,longRun,proxy};
});
const saves=read(path.join(repo,'local','original-saves-verification.json'));
assert.ok(saves.pass&&saves.changed.length===0&&saves.missing.length===0&&saves.added.length===0);
const guard=read(path.join(repo,'local','native-guard-final.json'));
const launch=read(path.join(repo,'local','package-launch-validation.json'));
const originalBinaries=read(path.join(repo,'local','original-binaries-final.json'));
assert.ok(originalBinaries.pass);
assert.ok(guard.pass&&launch.pass);assert.equal(guard.dllSha256,dllSha256);assert.equal(launch.dllSha256,dllSha256);
const scriptHash=name=>crypto.createHash('sha256').update(fs.readFileSync(path.join(repo,'packaging',name))).digest('hex');
assert.equal(launch.launchScriptSha256,scriptHash('Launch-Coop.ps1'));
assert.equal(launch.prepareScriptSha256,scriptHash('Prepare-Coop.ps1'));
const summary={schema:1,generatedAt:new Date().toISOString(),dllSha256,gameVersion:'1.1.1.9',steamBuild:23935965,
  scope:'Two licensed native clients on one Windows PC, physical copies and PID-isolated native events',runs,
  nativeUnsupportedBuildGuard:{pass:true,verifiedAt:guard.verifiedAt},packageLaunch:{pass:true,verifiedAt:launch.verifiedAt,
    shell:'Windows PowerShell 5.1, process RemoteSigned',gracefulExit:launch.cleanExit,
    cleanup:launch.cleanExit?'graceful close':'owned copied process stopped after the 10-second close timeout',
    launchScriptSha256:launch.launchScriptSha256,prepareScriptSha256:launch.prepareScriptSha256},
  originalSaves:{pass:true,files:saves.baselineFiles,verifiedAt:saves.verifiedAt,changed:0,missing:0,added:0},
  originalBinaries:{pass:true,verifiedAt:originalBinaries.verifiedAt,files:originalBinaries.files},
  unverified:['two separate PCs','friend network, Internet, NAT and VPN','Steam P2P/invitations','physical UI clicking','replay with different local currency selections on the two peers','all building types/tools, transport/office settings and long-term reliability beyond the measured run']};
fs.mkdirSync(outputs,{recursive:true});fs.writeFileSync(target,JSON.stringify(summary,null,2)+'\n',{flag:'wx'});
console.log('Exported '+runs.length+' validated runs; private paths, PIDs and game files excluded.');
