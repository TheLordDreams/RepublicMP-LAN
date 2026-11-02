// SPDX-License-Identifier: GPL-3.0-only
// Static, read-only PE inspection. Does not load or execute game code.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

export function parsePE(d) {
  const need = (o, n) => { if (!Number.isInteger(o) || o < 0 || o + n > d.length) throw Error('Truncated PE'); };
  const u16 = o => { need(o, 2); return d.readUInt16LE(o); };
  const u32 = o => { need(o, 4); return d.readUInt32LE(o); };
  if (d.toString('ascii', 0, 2) !== 'MZ') throw Error('Not MZ');
  const pe = u32(0x3c); need(pe, 24);
  if (d.toString('ascii', pe, pe + 4) !== 'PE\0\0') throw Error('Not PE');
  const count = u16(pe + 6), opt = pe + 24, optSize = u16(pe + 20);
  if (!count || count > 96) throw Error('Invalid section count');
  need(opt, optSize);
  const magic = u16(opt);
  if (![0x20b, 0x10b].includes(magic)) throw Error('Unknown optional header');
  const sections = [];
  for (let i = 0; i < count; i++) {
    const s = opt + optSize + i * 40; need(s, 40);
    const section = { name: d.toString('ascii', s, s + 8).replace(/\0.*$/, ''),
      virtualSize: u32(s + 8), rva: u32(s + 12), size: u32(s + 16), offset: u32(s + 20) };
    need(section.offset, section.size); sections.push(section);
  }
  const offset = rva => {
    const s = sections.find(s => rva >= s.rva && rva < s.rva + s.size);
    if (!s) throw Error('RVA has no file bytes: ' + rva.toString(16));
    return s.offset + rva - s.rva;
  };
  const cstr = o => { need(o, 1); const end = d.indexOf(0, o); if (end < 0) throw Error('Unterminated string'); return d.toString('ascii', o, end); };
  const directories = opt + (magic === 0x20b ? 112 : 96);
  const imports = [];
  if (u32(directories + 8)) {
    let descriptor = offset(u32(directories + 8));
    for (let i = 0; i < 4096; i++, descriptor += 20) {
      need(descriptor, 20); if (!u32(descriptor + 12)) break;
      const dll = cstr(offset(u32(descriptor + 12)));
      imports.push(dll);
    }
  }
  const exports = [];
  if (u32(directories)) {
    const e = offset(u32(directories)), n = u32(e + 24), table = offset(u32(e + 32));
    if (n > 100000) throw Error('Invalid export count');
    for (let i = 0; i < n; i++) exports.push(cstr(offset(u32(table + i * 4))));
  }
  // Equivalent narrow version-call pattern used in TesmioLoader's launcher.
  // Unknown encodings remain unknown; this is not a general x64 decoder.
  const anchor = Buffer.from('v%d.%d.%d.%d (64 bit DX11.1)\0');
  const str = d.indexOf(anchor), text = sections.find(s => s.name === '.text');
  const strings = sections.find(s => str >= s.offset && str < s.offset + s.size);
  const versions = [];
  if (str >= 0 && strings && text && magic === 0x20b) {
    const target = strings.rva + str - strings.offset;
    for (let i = text.offset; i + 7 <= text.offset + text.size; i++) {
      if (![0x48, 0x4c].includes(d[i]) || d[i+1] !== 0x8d || (d[i+2] & 0xc7) !== 5) continue;
      if (text.rva + i - text.offset + 7 + d.readInt32LE(i+3) !== target) continue;
      let edx = null, r8 = null, r9 = null, stack = null;
      for (let p = Math.max(text.offset, i - 64); p + 2 < i; p++) {
        if (d[p] === 0xba && p + 5 <= i && u32(p+1) < 256) edx = u32(p+1);
        else if (d[p] === 0x41 && d[p+1] === 0xb8 && p+6 <= i && u32(p+2) < 256) r8 = u32(p+2);
        else if (d[p] === 0x41 && d[p+1] === 0xb9 && p+6 <= i && u32(p+2) < 256) r9 = u32(p+2);
        else if (d[p] === 0x44 && d[p+1] === 0x8b && d[p+2] === 0xc2) r8 = edx;
        else if (d[p] === 0x44 && d[p+1] === 0x8b && d[p+2] === 0xca) r9 = edx;
        else if (d[p] === 0xc7 && d[p+1] === 0x44 && d[p+2] === 0x24 && d[p+3] === 0x20 && p+8 <= i && u32(p+4) < 256) stack = u32(p+4);
      }
      versions.push({ value: [edx,r8,r9,stack].every(v => v !== null) ? [edx,r8,r9,stack].join('.') : 'unknown',
        leaRva: '0x' + (text.rva + i - text.offset).toString(16),
        argumentBytes: d.subarray(Math.max(text.offset,i-64),i+7).toString('hex') });
    }
  }
  return { machine: '0x'+u16(pe+4).toString(16), timestamp: '0x'+u32(pe+8).toString(16),
    pe32plus: magic === 0x20b, sections, imports, exports, versionCandidates: versions };
}

export function inspectGame(directory) {
  const files = {};
  for (const name of ['SOVIET64.exe', 'C3DDLL64.dll', 'steam_api64.dll']) {
    const bytes = fs.readFileSync(path.join(directory, name));
    files[name] = { bytes: bytes.length, sha256: crypto.createHash('sha256').update(bytes).digest('hex'), ...parsePE(bytes) };
  }
  const manifestPath = path.resolve(directory, '..', '..', 'appmanifest_784150.acf');
  let steamBuild = 'unknown';
  if (fs.existsSync(manifestPath)) steamBuild = /"buildid"\s+"(\d+)"/.exec(fs.readFileSync(manifestPath,'utf8'))?.[1] ?? 'unknown';
  // Version/stamp are a compatibility lead, not proof that every hook is safe.
  const exe = files['SOVIET64.exe'];
  const candidateMatch = exe.machine === '0x8664' && exe.timestamp === '0x6a3eb6ad' &&
    exe.versionCandidates.length === 1 && exe.versionCandidates[0].value === '1.1.1.9';
  const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
  const allowlist = fs.readFileSync(path.join(repo,'core','supported_build.h'),'utf8');
  const expectedExe = /kSupportedExeSha256\s*=\s*"([a-f0-9]{64})"/.exec(allowlist)?.[1];
  const expectedEngine = /kSupportedEngineSha256\s*=\s*"([a-f0-9]{64})"/.exec(allowlist)?.[1];
  if (!expectedExe || !expectedEngine) throw Error('Cannot read fingerprint allowlist');
  const fingerprintMatch = exe.sha256 === expectedExe && files['C3DDLL64.dll'].sha256 === expectedEngine;
  const hooks = [];
  const header = fs.readFileSync(path.join(repo,'world','local_builds.h'),'utf8');
  const source = fs.readFileSync(path.join(repo,'world','local_builds.cpp'),'utf8');
  const executable = fs.readFileSync(path.join(directory,'SOVIET64.exe'));
  for (const [define,array] of [['HOOK_UNIVERSAL_RVA','kUniversalExpect'],['HOOK_ROAD_COMMIT_RVA','kRoadCommitExpect']]) {
    const rva = Number(new RegExp('#define\\s+'+define+'\\s+(0x[a-fA-F0-9]+)').exec(header)?.[1]);
    const literals = new RegExp(array+'\\[\\d+\\]\\s*=\\s*\\{([^}]+)\\}').exec(source)?.[1];
    if (!Number.isInteger(rva) || !literals) throw Error('Hook metadata unreadable');
    const expected = Buffer.from([...literals.matchAll(/0x([a-fA-F0-9]{2})/g)].map(m=>parseInt(m[1],16)));
    const section = exe.sections.find(s=>rva>=s.rva && rva+expected.length<=s.rva+s.size);
    const at = section ? section.offset+rva-section.rva : -1;
    hooks.push({ name: define, rva: '0x'+rva.toString(16), bytes:expected.length,
      matched: at>=0 && expected.equals(executable.subarray(at,at+expected.length)) });
  }
  return { inspectedAt: new Date().toISOString(), directory: path.resolve(directory), steamBuild,
    expected: { version: '1.1.1.9', timestamp: '0x6a3eb6ad' },
    compatibility: candidateMatch && fingerprintMatch && hooks.every(h=>h.matched)
      ? 'fingerprints-and-two-prologues-match-runtime-unvalidated' : 'blocked-build-mismatch',
    fingerprintMatch, hooks, files };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  if (!process.argv[2]) throw Error('Usage: node tools/inspect-game.mjs <game-directory> [private-report.json]');
  const report = inspectGame(process.argv[2]);
  if (process.argv[3]) {
    const output = path.resolve(process.argv[3]);
    const privateRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', 'local') + path.sep;
    if (!output.startsWith(privateRoot)) throw Error('Reports must stay in ignored local/');
    fs.mkdirSync(path.dirname(output), {recursive:true}); fs.writeFileSync(output, JSON.stringify(report,null,2)+'\n');
  }
  console.log(JSON.stringify({ steamBuild: report.steamBuild, compatibility: report.compatibility,
    exe: { sha256: report.files['SOVIET64.exe'].sha256, timestamp: report.files['SOVIET64.exe'].timestamp,
      versions: report.files['SOVIET64.exe'].versionCandidates.map(v=>v.value) },
    engineExports: report.files['C3DDLL64.dll'].exports.length, hooks: report.hooks },null,2));
  if (report.compatibility.startsWith('blocked')) process.exitCode = 2;
}
