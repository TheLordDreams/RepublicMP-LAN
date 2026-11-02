// SPDX-License-Identifier: GPL-3.0-only
// Optional integration check against an owned install. Writes only ignored local/.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { inspectGame } from './inspect-game.mjs';
if (!process.argv[2]) throw Error('Usage: node tools/check-install-mutations.mjs <game-directory>');
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const privateRoot=path.join(repo,'local'); fs.mkdirSync(privateRoot,{recursive:true});
const copy=fs.mkdtempSync(path.join(privateRoot,'fingerprint-test-'));
const source=path.resolve(process.argv[2]);
const before=inspectGame(source);
assert.equal(before.fingerprintMatch,true);
assert.equal(before.hooks.every(h=>h.matched),true);
console.log('PASS: owned install matches allowlist and both capture prologues');
for (const name of ['SOVIET64.exe','C3DDLL64.dll','steam_api64.dll']) fs.copyFileSync(path.join(source,name),path.join(copy,name),fs.constants.COPYFILE_EXCL);
assert.equal(inspectGame(copy).fingerprintMatch,true);
// A harmless DOS-stub byte mutation leaves version and PE timestamp intact.
const exe=path.join(copy,'SOVIET64.exe'), bytes=fs.readFileSync(exe);
bytes[0x40]^=1; fs.writeFileSync(exe,bytes);
assert.equal(inspectGame(copy).compatibility,'blocked-build-mismatch');
console.log('PASS: changed EXE refused despite matching version and timestamp');
bytes[0x40]^=1;
const hook=before.hooks[0], rva=Number(hook.rva);
const section=before.files['SOVIET64.exe'].sections.find(s=>rva>=s.rva && rva<s.rva+s.size);
bytes[section.offset+rva-section.rva]^=1; fs.writeFileSync(exe,bytes);
assert.equal(inspectGame(copy).hooks[0].matched,false);
console.log('PASS: changed hook prologue detected');
fs.copyFileSync(path.join(source,'SOVIET64.exe'),exe);
const engine=path.join(copy,'C3DDLL64.dll'), eb=fs.readFileSync(engine);
eb[0x40]^=1; fs.writeFileSync(engine,eb);
assert.equal(inspectGame(copy).compatibility,'blocked-build-mismatch');
console.log('PASS: changed engine refused independently of EXE');
const after=inspectGame(source);
for (const name of Object.keys(before.files)) assert.equal(before.files[name].sha256,after.files[name].sha256);
console.log('PASS: all three original binary hashes unchanged');
fs.writeFileSync(path.join(privateRoot,'mutation-results.json'),JSON.stringify({
  checkedAt:new Date().toISOString(),results:5,passed:5,copy,
  scope:'Static JS inspector only. Native build guard and multiplayer were not executed.'},null,2)+'\n');
