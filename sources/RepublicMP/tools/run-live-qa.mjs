// SPDX-License-Identifier: GPL-3.0-only
// Real-client QA. Requires already prepared copies and a locally built LAN DLL.
// No original installation writes, installer execution, Steam login or mock world.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const local=path.join(repo,'local');
const testRoot=process.env.COOP_QA_TEST_ROOT ? path.resolve(process.env.COOP_QA_TEST_ROOT) : path.join(local,'test-installs');
if(!process.env.COOP_QA_OWNER||!process.env.COOP_PYTHON)throw Error('Use tools/Run-LiveQA.ps1 to acquire the game slots first');
const slotTool=path.join(repo,'third_party','coop-anything','gameslot','gameslot.py');
const dll=path.join(local,'build','lan','RepublicMP.dll');
if (!fs.existsSync(dll)) throw Error('BLOCKED: local LAN DLL has not been built');
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const expectedHash=hash(dll);
const longMinutes=Number(process.env.COOP_QA_LONG_MINUTES??0);
assert.ok(Number.isInteger(longMinutes)&&longMinutes>=0&&longMinutes<=60);
const construction=process.env.COOP_QA_CONSTRUCTION==='1',rejoin=process.env.COOP_QA_REJOIN==='1';
const chaos=process.env.COOP_QA_CHAOS??'none';
assert.ok(['none','clean','lag-jitter'].includes(chaos));
const disconnectAfterMs=Number(process.env.COOP_QA_DISCONNECT_MS??0);
assert.ok(Number.isInteger(disconnectAfterMs)&&disconnectAfterMs>=0&&disconnectAfterMs<=600000);
assert.ok(!disconnectAfterMs||disconnectAfterMs>=10000,'Injected disconnect needs >=10000ms for a useful post-rejoin sample');
assert.ok(!disconnectAfterMs||(chaos!=='none'&&!construction&&!longMinutes),'Fault test requires proxy and its own scenario');
const stamp=new Date().toISOString().replace(/[:.]/g,'-');
const run=path.join(local,'qa-runs',stamp); fs.mkdirSync(run,{recursive:true});
const seats=['host','client'].map(name=>{
  const root=path.join(testRoot,name);
  const build=fs.existsSync(path.join(root,'coop-runtime')) ? path.join(root,'coop-runtime') : path.join(root,'tesmioloader','build');
  const plugins=path.join(build,'plugins');
  if (!fs.existsSync(path.join(root,'TEST-COPY.txt'))) throw Error('Missing isolated-copy marker: '+name);
  return {name,root,build,plugins,sequence:0,gamePid:0,startedAt:Date.now()};
});
fs.writeFileSync(path.join(run,'run-config.json'),JSON.stringify(seats.map(s=>({name:s.name,
  profile:fs.existsSync(path.join(s.plugins,'RepublicMP.profile.ini'))?fs.readFileSync(path.join(s.plugins,'RepublicMP.profile.ini'),'utf8'):null,
  isolateNativeEvents:fs.existsSync(path.join(s.plugins,'RepublicMP.qa.isolate-events.enabled')),
  vehicleTrace:fs.existsSync(path.join(s.plugins,'RepublicMP.qa.vehicle-trace.enabled')),
  vehicleBytes:fs.existsSync(path.join(s.plugins,'RepublicMP.qa.vehicle-bytes.enabled'))})),null,2));
const delay=ms=>new Promise(r=>setTimeout(r,ms));
const log=line=>{console.log(line);fs.appendFileSync(path.join(run,'runner.log'),line+'\n');};
let leaseError=null;
const leaseTimer=setInterval(()=>{
  const renewed=spawnSync(process.env.COOP_PYTHON,['-I',slotTool,'acquire','--owner',process.env.COOP_QA_OWNER,'--slots','2','--ttl','3600','--wait','0'],{encoding:'utf8',windowsHide:true});
  if(renewed.status!==0)leaseError=Error('Could not renew owned game slots: '+renewed.stderr+renewed.stdout);
  else log('LEASE RENEWED for owned test clients');
},300000);
leaseTimer.unref();
const read=s=>{
  if(leaseError)throw leaseError;
  if(s.launchError)throw s.launchError;
  if(!s.gamePid) {
    try {
      const launch=fs.readFileSync(path.join(run,s.name+'-launcher.stdout'),'utf8');
      const pid=/pid (\d+) created suspended/.exec(launch);
      if(pid)s.gamePid=Number(pid[1]);
    } catch {}
  }
  try {
    const file=path.join(s.plugins,'RepublicMP.qa.status.json');
    if(Date.now()-fs.statSync(file).mtimeMs>10000)return null;
    const value=JSON.parse(fs.readFileSync(file,'utf8'));
    if (s.gamePid && value.pid!==s.gamePid) return null;
    return value;
  } catch {return null;}
};
async function wait(label,predicate,timeout=120000) {
  const end=Date.now()+timeout;
  while (Date.now()<end) { const answer=predicate(); if(answer) return answer; await delay(250); }
  throw Error('TIMEOUT: '+label);
}
async function command(s,verb,arg='') {
  const sequence=++s.sequence;
  const command=path.join(s.plugins,'RepublicMP.qa.command');
  fs.writeFileSync(command+'.tmp',`${sequence}|${verb}|${arg}\n`);
  fs.renameSync(command+'.tmp',command);
  const status=await wait(s.name+' ack '+verb,()=>{const v=read(s);return v?.lastSequence===sequence&&!v.probeCommandPending&&v;},20000);
  if(status.commandError||((verb==='probe-prime'||verb==='build-test'||verb==='demolish-test')&&status.probeError)) throw Error(s.name+' '+verb+': '+(status.commandError||status.probeError));
  return status;
}
let sessionWorlds=null;
function checkedPair() {
  const a=read(seats[0]), b=read(seats[1]);
  if(!a||!b) return null;
  for (const v of [a,b]) {
    if(v.commandError||v.sessionError||v.desync) throw Error('Game reported error: '+JSON.stringify({state:v.sessionState,world:v.worldLoad,tick:v.tick,commandError:v.commandError,sessionError:v.sessionError,desync:v.desync}));
  }
  if(sessionWorlds) {
    assert.equal(a.sessionState,3,'Host left active session');
    assert.equal(b.sessionState,5,'Client left active session');
    assert.equal(a.worldLoad,sessionWorlds[0],'Unexpected host reload/resync');
    assert.equal(b.worldLoad,sessionWorlds[1],'Unexpected client reload/resync');
  }
  return [a,b];
}
const compared=new Map();
function compare(a,b) {
  for(const x of a.samples) {
    const y=b.samples.find(y=>y.tick===x.tick); if(!y) continue;
    // The shared session is established before this is ever called.
    for(const key of ['all','clock','buildingsHash','vehiclesHash','citizensHash','buildings','vehicles','citizens'])
      assert.equal(x[key],y[key],`DESYNC at tick ${x.tick}, ${key}`);
    compared.set(x.tick,x);
  }
}
let outcome='FAIL';
let longRun=null,proxy=null;
const completedChecks=[];
try {
  if(chaos!=='none'){
    const out=fs.openSync(path.join(run,'proxy.stdout'),'w'),err=fs.openSync(path.join(run,'proxy.stderr'),'w');
    proxy=spawn(process.execPath,[path.join(repo,'tools','tcp-chaos-proxy.mjs'),'--run-directory',run,'--profile',chaos,'--disconnect-after-ms',String(disconnectAfterMs)],
      {cwd:repo,windowsHide:true,stdio:['ignore',out,err,'ipc']});
    fs.closeSync(out);fs.closeSync(err);
    proxy.on('error',error=>log('PROXY ERROR: '+error.message));
    await wait('TCP proxy ready',()=>fs.existsSync(path.join(run,'tcp-chaos-proxy.ready.json')),10000);
    const ready=JSON.parse(fs.readFileSync(path.join(run,'tcp-chaos-proxy.ready.json'),'utf8'));
    assert.equal(ready.pid,proxy.pid);assert.equal(ready.profile,chaos);
    assert.deepEqual(ready.listen,{host:'127.0.0.1',port:5561});
    assert.deepEqual(ready.target,{host:'127.0.0.1',port:5560});
    fs.writeFileSync(path.join(seats[1].plugins,'RepublicMP.qa.proxy.enabled'),'private loopback proxy\n');
  } else {
    const marker=path.join(seats[1].plugins,'RepublicMP.qa.proxy.enabled');if(fs.existsSync(marker))fs.unlinkSync(marker);
  }
  for(const s of seats) {
    for(const file of fs.readdirSync(s.plugins).filter(f=>/^RepublicMP\.qa\.vehicles\.(?:(?:pre|post)\.)?\d+\.bin$/.test(f)))
      fs.unlinkSync(path.join(s.plugins,file));
    for(const file of ['RepublicMP.qa.status.json','RepublicMP.qa.command']) {
      const p=path.join(s.plugins,file); if(fs.existsSync(p)) fs.renameSync(p,p+'.before-'+stamp);
    }
    fs.copyFileSync(dll,path.join(s.plugins,'RepublicMP.dll'));
    assert.equal(hash(path.join(s.plugins,'RepublicMP.dll')),expectedHash);
    fs.writeFileSync(path.join(s.plugins,'RepublicMP.qa.enabled'),'isolated local QA\n');
    const stdout=fs.openSync(path.join(run,s.name+'-launcher.stdout'),'w');
    const stderr=fs.openSync(path.join(run,s.name+'-launcher.stderr'),'w');
    const child=spawn(path.join(s.build,'tesmiolauncher.exe'),['--nogui','--game',path.join(s.root,'SOVIET64.exe')],
      {cwd:s.build,windowsHide:true,stdio:['ignore',stdout,stderr]});
    child.on('error',error=>{s.launchError=error;});
    child.on('exit',code=>{if(code)s.launchError=Error(s.name+' launcher exit '+code);});
    fs.closeSync(stdout);fs.closeSync(stderr);
    const status=await wait(s.name+' native bridge startup',()=>read(s));
    assert.equal(status.version,'0.7.2-lan2-rc1','Wrong or upstream DLL loaded');
    s.gamePid=status.pid;
    const registered=spawnSync(process.env.COOP_PYTHON,['-I',slotTool,'register-pid','--owner',process.env.COOP_QA_OWNER,'--pid',String(s.gamePid)],{encoding:'utf8',windowsHide:true});
    if(registered.status!==0)throw Error('Could not register owned game PID: '+registered.stderr+registered.stdout);
    const launch=fs.readFileSync(path.join(run,s.name+'-launcher.stdout'),'utf8');
    assert.match(launch,new RegExp('pid '+s.gamePid+' created suspended'));
    log(`STARTED ${s.name} PID ${s.gamePid}`);
  }
  await wait('both root menus initialized',()=>seats.every(s=>read(s)?.menuBuildCount>=2));
  for(const s of seats){
    const startup=fs.readFileSync(path.join(s.build,'tesmioloader.log'),'utf8');
    assert.ok(startup.includes('QA INSTANCE EVENTS: CreateEventW isolation installed=1'),s.name+' early native-event hook missing');
  }
  await command(seats[0],'load','coopqa_fixture');
  await wait('host fixture really loaded',()=>read(seats[0])?.loaded,180000);
  log('LOADED host fixture');
  await command(seats[0],'host');
  await wait('host TCP listener',()=>read(seats[0])?.sessionState===1);
  await command(seats[1],'join','127.0.0.1');
  await wait('two real clients in shared Co-op',()=>{
    const pair=checkedPair(); if(!pair)return false;
    const [a,b]=pair;
    return a.sessionState===3&&b.sessionState===5&&a.sessionRunning&&b.sessionRunning&&
      a.loaded&&b.loaded&&a.peers===1&&a.players===2&&b.players===2;
  },240000);
  log('CONNECTED two real clients; shared city loaded');
  // Filling/renderer constructors may run only when the first world loads.
  for(const s of seats){
    const startup=fs.readFileSync(path.join(s.build,'tesmioloader.log'),'utf8');
    for(const event of ['FillingrStart','RenderStart','RenderFinished'])
      assert.ok(startup.includes(`QA INSTANCE EVENTS: ${event} isolated by PID; created=1; error=0`),s.name+' native event isolation missing: '+event);
  }
  log('ISOLATED Filling and renderer native events for both test processes');
  sessionWorlds=checkedPair().map(s=>s.worldLoad);
  if(disconnectAfterMs){
    await command(seats[0],'resume');
    await wait('injected TCP disconnect detected by actual clients',()=>{
      const p=seats.map(read);if(p.some(s=>!s))return false;
      assert.ok(p.every((s,i)=>s.worldLoad===sessionWorlds[i]&&!s.desync),'Unexpected reload/desync during fault injection');
      if(p[0].sessionState===3&&p[1].sessionState===5&&p.every(s=>!s.sessionError)){compare(...checkedPair());return false;}
      return p[0].sessionState===1&&p[1].sessionState===0&&p.every(s=>s.loaded)&&/lockstep|connection|host/i.test(p[1].sessionError)&&p;
    },disconnectAfterMs+30000);
    fs.writeFileSync(path.join(run,'fault-detected-status.json'),JSON.stringify(seats.map(read),null,2));
    log('DETECTED injected TCP stream closure; host listening, client disconnected, both processes/worlds alive');
    const worlds=sessionWorlds;sessionWorlds=null;
    await command(seats[1],'join','127.0.0.1');
    await wait('rejoin after injected disconnect',()=>{
      const p=seats.map(read);if(p.some(s=>!s)||p[0].sessionState!==3||p[1].sessionState!==5)return false;
      return p.every((s,i)=>s.sessionRunning&&s.loaded&&s.worldLoad>worlds[i])&&checkedPair();
    },240000);
    sessionWorlds=checkedPair().map(s=>s.worldLoad);compared.clear();
    await command(seats[0],'resume');
    await wait('post-fault rejoin strict hashes',()=>{const p=checkedPair();if(!p)return false;compare(...p);return p[0].lastMatchedTick>=60;},Math.min(disconnectAfterMs-1000,30000));
    log('RECOVERED from injected TCP disconnect; client rejoined with >=60 strict matching ticks');
    completedChecks.push('injected TCP closure detected and client rejoined with strict matching state');
  }else{
  await command(seats[0],'resume');
  await wait('five exact-tick shared hash samples',()=>{
    const pair=checkedPair();if(!pair)return false;
    compare(...pair);
    return compared.size>=5&&pair[0].lastMatchedTick>=300;
  },180000);
  log('MATCHED '+compared.size+' exact-tick samples and upstream lastMatchedTick >= 300');
  for(const sample of compared.values()) {
    assert.ok(sample.buildings>0,'Fixture has no buildings');
    assert.ok(sample.citizens>0,'Fixture has no citizens');
  }
  await command(seats[1],'pause');
  const paused=await wait('client pause propagated and both caught up',()=>{
    const pair=checkedPair();return pair&&pair.every(v=>v.paused)&&pair[0].tick===pair[1].tick&&pair;
  },15000);
  await delay(1200);
  const still=checkedPair();
  for(let i=0;i<2;i++)assert.equal(still[i].tick,paused[i].tick,'Paused simulation advanced');
  log('PAUSED from client; both stable at tick '+paused[0].tick);
  for(const s of still)assert.ok(Number.isFinite(s.moneyRubles)&&Number.isFinite(s.moneyDollars),'Paused money snapshot missing');
  assert.equal(still[0].moneyRubles,still[1].moneyRubles,'Shared paused balances differ');
  await command(seats[1],'credit-test');
  const credited=await wait('client economy command applied exactly once on both games',()=>{
    const pair=checkedPair();if(!pair)return false;
    return pair.every((s,i)=>s.tick===still[i].tick&&s.commandsApplyEpoch>still[i].commandsApplyEpoch&&
      Math.abs(s.moneyRubles-(still[i].moneyRubles+100))<1e-6&&s.moneyDollars===still[i].moneyDollars)&&pair;
  },15000);
  await delay(1500);
  const settled=checkedPair();
  for(let i=0;i<2;i++)for(const key of ['tick','commandsApplyEpoch','moneyRubles','moneyDollars'])
    assert.equal(settled[i][key],credited[i][key],'Paused command changed again after acknowledgement: '+key);
  log('APPLIED client +100-ruble native lockstep command exactly once on both games');
  if(construction){
    for(const s of seats)await command(s,'probe-prime');
    const prime=checkedPair();
    assert.ok(prime.every(s=>s.probeValid&&s.pausedState),'Probe/paused state missing');
    for(let slot=0;slot<2;slot++)for(const key of ['x','y','z'])
      assert.equal(prime[0].probeSlots[slot][key],prime[1].probeSlots[slot][key],'Construction targets differ');
    for(let slot=0;slot<2;slot++){
      const before=checkedPair();
      await command(seats[slot],'build-test',String(slot));
      const placed=await wait('native construction site by '+seats[slot].name,()=>{
        const p=checkedPair(); if(!p)return false;
        if(p.some(s=>s.probeError))throw Error('Construction probe: '+p.map(s=>s.probeError).join(';'));
        return p.every((s,i)=>s.probeValid&&s.probeSlots[slot].nearby===1&&s.probeSlots[slot].exists&&
          s.pausedState.buildings===before[i].pausedState.buildings+1)&&p;
      },30000);
      assert.equal(placed[0].pausedState.all,placed[1].pausedState.all,'Placement paused hashes differ');
      let built=placed;
      for(let attempt=0;attempt<10&&!built.every(s=>s.probeSlots[slot].progress>=1);attempt++){
        const from=built.map(s=>s.tick);
        await command(seats[slot],'resume');
        await wait('paid construction simulation advances',()=>{const p=checkedPair();if(!p)return false;compare(...p);return p.every((s,i)=>s.tick>=from[i]+120);},60000);
        await command(seats[slot],'pause');
        built=await wait('paid construction paused observation',()=>{const p=checkedPair();return p&&p.every(s=>s.paused&&s.pausedState&&s.probeValid)&&p[0].tick===p[1].tick&&p;},15000);
        log('BUILD progress slot '+slot+' = '+built.map(s=>s.probeSlots[slot].progress).join('/')+' at tick '+built[0].tick);
      }
      assert.ok(built.every(s=>s.probeSlots[slot].progress>=1),'Paid construction did not finish in 1200 simulation ticks');
      assert.ok(built.every((s,i)=>s.moneyRubles<before[i].moneyRubles),'No paid-construction debit observed');
      assert.equal(built[0].pausedState.all,built[1].pausedState.all,'Construction paused hashes differ');
      const cost=before[0].moneyRubles-built[0].moneyRubles;
      assert.equal(cost,before[1].moneyRubles-built[1].moneyRubles,'Construction payment differs');
      log('BUILT from '+seats[slot].name+' slot '+slot+', native completed building, equal net ruble decrease '+cost);
      const captureCount=()=>fs.readFileSync(path.join(seats[slot].build,'tesmioloader.log'),'utf8').split('LOCKSTEP: building demolition captured').length-1;
      const capturesBefore=captureCount();
      await command(seats[slot],'demolish-test',String(slot));
      const demolition=await wait('sender payment mode used for native demolition',()=>{
        const p=checkedPair();return p&&p.every((s,i)=>s.probeValid&&s.commandsApplyEpoch>built[i].commandsApplyEpoch&&
          (!s.probeSlots[slot].exists||s.probeSlots[slot].demolishing===1))&&p;
      },15000);
      assert.equal(captureCount(),capturesBefore+1,'Native demolition entry did not capture exactly one command');
      for(let i=0;i<2;i++)assert.equal(demolition[i].commandsApplyEpoch,built[i].commandsApplyEpoch+1,'Demolition replay was not exactly once');
      for(let i=0;i<2;i++)for(const key of ['localRub','localUsd'])
        assert.equal(demolition[i].probeSlots[slot][key],before[i].probeSlots[slot][key],'Demolition changed local build currency');
      let removed=demolition;
      for(let attempt=0;attempt<15&&!removed.every(s=>s.probeValid&&s.probeSlots[slot].demolished&&!s.probeSlots[slot].exists);attempt++){
        const from=removed.map(s=>s.tick);
        await command(seats[slot],'resume');
        await wait('demolition simulation advances',()=>{const p=checkedPair();if(!p)return false;compare(...p);return p.every((s,i)=>s.tick>=from[i]+120);},60000);
        await command(seats[slot],'pause');
        removed=await wait('demolition paused observation',()=>{const p=checkedPair();return p&&p.every(s=>s.paused&&s.pausedState&&s.probeValid)&&p[0].tick===p[1].tick&&p;},15000);
      }
      assert.ok(removed.every((s,i)=>s.probeSlots[slot].demolished&&!s.probeSlots[slot].exists&&s.pausedState.buildings===before[i].pausedState.buildings),'Demolition did not remove the native building within 1800 ticks');
      assert.equal(removed[0].pausedState.all,removed[1].pausedState.all,'Demolition paused hashes differ');
      fs.writeFileSync(path.join(run,'construction-slot-'+slot+'.json'),JSON.stringify({before,placed,built,demolition,removed},null,2));
      log('DEMOLISHED from '+seats[slot].name+' slot '+slot+'; both building counts restored');
    }
    completedChecks.push('host/client paid native construction and demolition');
  }
  const saveName='coopqa_shared_'+Date.now();
  await command(seats[1],'save',saveName);
  await wait('shared native save completed on both games',()=>seats.every(s=>{
    const header=path.join(s.root,'media_soviet','save',saveName,'header.bin');
    if(!fs.existsSync(header)||fs.statSync(header).size===0)return false;
    const logFile=path.join(s.build,'tesmioloader.log');
    const text=fs.readFileSync(logFile,'utf8');
    return text.includes(`SAVE: [save/${saveName}] saved after tick`);
  }),90000);
  log('SAVED shared game on both clients: '+saveName);
  for(const s of seats) {
    const requested=Date.now();
    fs.writeFileSync(path.join(s.plugins,'RepublicMP.qa.screenshot.request'),'capture own framebuffer\n');
    await wait(s.name+' game-owned framebuffer capture',()=>{
      const file=path.join(s.plugins,'RepublicMP.qa.frame.bmp');
      return fs.existsSync(file)&&fs.statSync(file).mtimeMs>=requested&&file;
    },15000);
    fs.copyFileSync(path.join(s.plugins,'RepublicMP.qa.frame.bmp'),path.join(run,s.name+'-game.bmp'));
  }
  await command(seats[1],'resume');
  await wait('client resume propagated',()=>{
    const pair=checkedPair();if(!pair)return false;compare(...pair);
    return pair.every(v=>!v.paused&&v.tick>paused[0].tick+60);
  },30000);
  log('RESUMED from client; both simulations advanced');
  completedChecks.push('city transfer, strict baseline, pause/resume, exactly-once economy and shared save');
  if(rejoin){
    fs.writeFileSync(path.join(run,'matched-before-rejoin.json'),JSON.stringify([...compared.values()],null,2));
    sessionWorlds=null;
    const worlds=seats.map(s=>read(s).worldLoad);
    await command(seats[1],'disconnect');
    await wait('client left; host listening again',()=>read(seats[1])?.sessionState===0&&read(seats[0])?.sessionState===1,30000);
    await command(seats[1],'join','127.0.0.1');
    await wait('client rejoined and both reloaded shared city',()=>{
      // "every player left" is expected on the host until the next join is
      // accepted; enforce strict errors only after both new sessions are active.
      const p=seats.map(read);if(p.some(s=>!s)||p[0].sessionState!==3||p[1].sessionState!==5)return false;
      return p.every((s,i)=>s.sessionRunning&&s.loaded&&s.worldLoad>worlds[i]&&s.players===2)&&checkedPair();
    },240000);
    sessionWorlds=checkedPair().map(s=>s.worldLoad);compared.clear();
    await command(seats[0],'resume');
    await wait('rejoin strict hash baseline',()=>{const p=checkedPair();if(!p)return false;compare(...p);return p[0].lastMatchedTick>=300;},180000);
    log('REJOINED client; fresh city transfer and >=300 strict matching ticks');
    completedChecks.push('client disconnect/rejoin with fresh shared world and strict hashes');
  }
  if(longMinutes){
    const start=performance.now(),startPair=checkedPair();let lastHealthy=start,lastProgress=start,lastMatched=startPair[0].lastMatchedTick,nextBeat=start;
    longRun={startedAt:new Date().toISOString(),requestedMinutes:longMinutes,startTicks:startPair.map(s=>s.tick),startWorlds:sessionWorlds};
    log('LONG RUN started: '+longMinutes+' real minutes; strict hashes, no reload/resync allowed');
    while(performance.now()-start<longMinutes*60000){
      const now=performance.now(),p=checkedPair();
      if(p){
        compare(...p);lastHealthy=now;
        assert.ok(p.every(s=>!s.paused),'Unexpected pause during long run');
        if(p[0].lastMatchedTick>lastMatched){lastProgress=now;lastMatched=p[0].lastMatchedTick;}
        assert.ok(now-lastProgress<60000,'No matched-tick progress for 60 seconds');
        if(now>=nextBeat){log(`LONG RUN ${Math.floor((now-start)/1000)}s hostTick=${p[0].tick} clientTick=${p[1].tick} matched=${lastMatched} samples=${compared.size}`);nextBeat=now+30000;}
      }
      assert.ok(now-lastHealthy<15000,'Game status unavailable for 15 seconds');
      await delay(250);
    }
    const end=checkedPair();compare(...end);
    longRun={...longRun,finishedAt:new Date().toISOString(),elapsedMs:performance.now()-start,endTicks:end.map(s=>s.tick),lastMatchedTick:end[0].lastMatchedTick,matchedSamples:compared.size};
    assert.ok(end.every((s,i)=>s.tick>longRun.startTicks[i]+300),'Insufficient simulation progress during long run');
    fs.writeFileSync(path.join(run,'long-run.json'),JSON.stringify(longRun,null,2));
    log('LONG RUN PASSED '+Math.floor(longRun.elapsedMs/1000)+' real seconds');
    completedChecks.push(longMinutes+' real-minute strict hash run without resync');
  }
  }
  const final=checkedPair(); compare(...final);
  fs.writeFileSync(path.join(run,'final-status.json'),JSON.stringify(final,null,2));
  fs.writeFileSync(path.join(run,'matched-samples.json'),JSON.stringify([...compared.values()],null,2));
  outcome='PASS';
} catch(error) {
  log(String(error.stack??error));
  fs.writeFileSync(path.join(run,'failure-status.json'),JSON.stringify(seats.map(s=>read(s)),null,2));
  process.exitCode=1;
} finally {
  clearInterval(leaseTimer);
  for(const s of seats) {
    if(s.gamePid) { try {await command(s,'disconnect');} catch(error) {log('Cleanup disconnect: '+error.message);} }
    for(const file of ['tesmioloader.log']) {
      const p=path.join(s.build,file);if(fs.existsSync(p))fs.copyFileSync(p,path.join(run,s.name+'-'+file));
    }
    const marker=path.join(s.plugins,'RepublicMP.qa.enabled');if(fs.existsSync(marker))fs.unlinkSync(marker);
    for(const file of fs.readdirSync(s.plugins).filter(f=>/^RepublicMP\.qa\.vehicles\.(?:(?:pre|post)\.)?\d+\.bin$/.test(f)))
      fs.copyFileSync(path.join(s.plugins,file),path.join(run,s.name+'-'+file));
    const traceFile=`RepublicMP.qa.vehicle-trace.${s.gamePid}.jsonl`;
    if(fs.existsSync(path.join(s.plugins,traceFile)))
      fs.copyFileSync(path.join(s.plugins,traceFile),path.join(run,s.name+'-vehicle-trace.jsonl'));
  }
  if(proxy){
    if(proxy.connected)proxy.send({type:'stop'});
    const until=Date.now()+5000;while(proxy.exitCode===null&&proxy.signalCode===null&&Date.now()<until)await delay(100);
    if(proxy.exitCode===null&&proxy.signalCode===null)proxy.kill();
    if(fs.existsSync(path.join(run,'tcp-chaos-proxy.summary.json'))){
      const summary=JSON.parse(fs.readFileSync(path.join(run,'tcp-chaos-proxy.summary.json'),'utf8'));
      const used=(summary.connections??[]).filter(c=>c.connected&&c.clientToHost?.forwardedBytes>0&&c.hostToClient?.forwardedBytes>0);
      if(summary.serverError||summary.summaryWriteError||summary.profile!==chaos||!used.length||
          (disconnectAfterMs&&(!used.some(c=>c.endReason==='injected-disconnect')||used.length<2))){
        outcome='FAIL';process.exitCode=1;log('TCP proxy evidence failed: missing bidirectional traffic/profile/injected disconnect recovery');
      }
    }else{outcome='FAIL';process.exitCode=1;log('Missing TCP proxy shutdown summary');}
  }
  // Keep positively identified copied game PIDs for the caller's explicit cleanup.
  // No blanket process kill and no stopping any original/user-owned session.
  fs.writeFileSync(path.join(run,'processes.json'),JSON.stringify(seats.map(s=>({pid:s.gamePid,exe:path.join(s.root,'SOVIET64.exe')})),null,2));
  fs.writeFileSync(path.join(run,'result.json'),JSON.stringify({outcome,dllSha256:expectedHash,comparedTicks:[...compared.keys()],completedChecks,longRun,chaos,disconnectAfterMs,
    scope:'Two actual clients on one Windows PC with isolated native events. Separate PCs, friend network, Steam P2P and OS packet loss are not tested.'},null,2));
  log('RESULT: '+outcome);
}
