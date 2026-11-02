// SPDX-License-Identifier: GPL-3.0-only
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parsePE } from './inspect-game.mjs';

// Synthetic PE only. No game binary or asset is stored in the test suite.
function fixture() {
  const b=Buffer.alloc(0x600);
  b.write('MZ'); b.writeUInt32LE(0x80,0x3c); b.write('PE\0\0',0x80);
  b.writeUInt16LE(0x8664,0x84); b.writeUInt16LE(2,0x86);
  b.writeUInt32LE(0x6a3eb6ad,0x88); b.writeUInt16LE(0xf0,0x94); b.writeUInt16LE(0x20b,0x98);
  for (const [i,name,rva,off] of [[0,'.text',0x1000,0x200],[1,'.rdata',0x2000,0x400]]) {
    const at=0x188+i*40; b.write(name,at); b.writeUInt32LE(0x200,at+8);
    b.writeUInt32LE(rva,at+12); b.writeUInt32LE(0x200,at+16); b.writeUInt32LE(off,at+20);
  }
  b.write('v%d.%d.%d.%d (64 bit DX11.1)\0',0x400);
  // mov [rsp+20],9; mov edx,1; mov r9d,edx; mov r8d,edx; lea rcx,version
  Buffer.from('c744242009000000ba01000000448bca448bc2488d0d','hex').copy(b,0x240);
  b.writeInt32LE(0x2000-(0x1040+19+7),0x240+22);
  return b;
}
test('reads synthetic x64 identity and version arguments',()=>{
  const p=parsePE(fixture()); assert.equal(p.machine,'0x8664');
  assert.equal(p.timestamp,'0x6a3eb6ad'); assert.equal(p.versionCandidates.length,1);
  assert.equal(p.versionCandidates[0].value,'1.1.1.9');
});
test('corrupt/truncated headers fail rather than being classified as supported',()=>{
  for (const b of [Buffer.alloc(12),fixture().subarray(0,0x190)]) assert.throws(()=>parsePE(b));
  const b=fixture(); b.writeUInt32LE(0xfffffff0,0x3c); assert.throws(()=>parsePE(b));
});
test('missing version anchor stays unknown',()=>{
  const b=fixture(); b.fill(0,0x400,0x440); assert.deepEqual(parsePE(b).versionCandidates,[]);
});
test('unrecognised stack argument does not invent a version',()=>{
  const b=fixture(); b[0x243]=0x28; assert.equal(parsePE(b).versionCandidates[0].value,'unknown');
});
test('different build timestamp is retained without normalization',()=>{
  const b=fixture(); b.writeUInt32LE(0x12345678,0x88); assert.equal(parsePE(b).timestamp,'0x12345678');
});
test('invalid section raw range is rejected',()=>{
  const b=fixture(); b.writeUInt32LE(0xfffffff0,0x188+20); assert.throws(()=>parsePE(b));
});
