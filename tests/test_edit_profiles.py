"""Execute the real browser profile engine against synthetic saves; no console writes."""
from pathlib import Path
import shutil
import subprocess
import unittest
import base64
import hashlib
import io
import struct
import zipfile
import zlib

ROOT=Path(__file__).resolve().parents[1]


class EditProfileTests(unittest.TestCase):
    def test_profile_write_repairs_game_zip_and_manifest_checksums(self):
        import test_sharing
        node=shutil.which('node')
        if not node:self.skipTest('Node.js is required')
        payload=bytearray(244)
        payload[28:52]=b'PlayerMoneyContext00XXXX'
        struct.pack_into('<i',payload,52,500)
        payload[56:88]=b'PersistentMoneyFieldContextXXXXXX'
        file=struct.pack('<III',4,4,zlib.crc32(payload))+payload
        inner=test_sharing.zip_bytes([('progress.bin',file)])
        archive=test_sharing.portable(inner=inner,slots=('save1',),title='PPSA02433',version='01.000.000')
        script=r"""
const fs=require('fs'),vm=require('vm');const html=fs.readFileSync('ps5/ui.html','utf8');
const c={TextEncoder,TextDecoder,DataView,Uint8Array,Uint32Array};
vm.runInNewContext(html.slice(html.indexOf('const SaveLab='),html.indexOf('const lab={'))+';this.S=SaveLab;this.P=EditorProfiles;',c);
const pkg=c.S.parse(new Uint8Array(Buffer.from('__ARCHIVE__','base64')));
const p=c.P.capture(pkg,[{slot:'save1',file:'progress.bin',off:64,type:'int32',value:9999}],'Money');
const result=c.S.write(pkg,c.P.resolve(p,pkg,9999));process.stdout.write(Buffer.from(result.bytes).toString('base64'));
""".replace('__ARCHIVE__',base64.b64encode(archive).decode())
        result=subprocess.run([node,'-e',script],cwd=ROOT,check=True,capture_output=True,text=True,timeout=20)
        with zipfile.ZipFile(io.BytesIO(base64.b64decode(result.stdout))) as outer:
            changed=outer.read('slot-0.zip');manifest=outer.read('manifest.txt').decode()
        self.assertIn(hashlib.sha256(changed).hexdigest(),manifest)
        with zipfile.ZipFile(io.BytesIO(changed)) as nested:
            edited=nested.read('progress.bin')
        self.assertEqual(struct.unpack_from('<i',edited,64)[0],9999)
        self.assertEqual(struct.unpack_from('<I',edited,8)[0],zlib.crc32(edited[12:]))

    def test_portable_save_rules_relocate_and_fail_closed(self):
        node=shutil.which('node')
        if not node:self.skipTest('Node.js is required')
        script=r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync('ps5/ui.html','utf8');
const ctx={TextEncoder,TextDecoder,DataView,Uint8Array,Uint32Array};
vm.runInNewContext(html.slice(html.indexOf('const SaveLab='),html.indexOf('const lab={'))+';this.S=SaveLab;this.P=EditorProfiles;',ctx);
const {S,P}=ctx;
function pkg(){const b=new Uint8Array(256);b.set(new TextEncoder().encode('PersistentMoneyLabelABCDE'),40);new DataView(b.buffer).setInt32(64,500,true);b.set(new TextEncoder().encode('CurrencyDataStableContextABCDEFG'),68);return {bytes:b,title:'PPSA02433',version:'01.000.000',slots:[{slot:'save1',index:0,files:[{name:'progress.bin',start:0,size:256}]}]};}
const a=pkg(),edit={slot:'save1',file:'progress.bin',off:64,type:'int32',value:9999};
const profile=P.capture(a,[edit],'Money');assert.equal(profile.rules.length,1);assert.equal('off' in profile.rules[0],false);
const imported=P.validate(JSON.parse(JSON.stringify({...profile,script:'throw Error()',pid:99})));assert.equal('script' in imported,false);assert.equal('pid' in imported,false);
const b=pkg();b.bytes.fill(0);b.bytes.set(a.bytes.subarray(40,100),128);new DataView(b.bytes.buffer).setInt32(152,750,true);
const edits=P.resolve(imported,b,9999);assert.equal(edits[0].off,152);assert.equal(edits[0].value,9999);assert.equal(S.read(b,edits[0]),750); // resolution itself does not write
assert.equal(P.resolve(imported,b,50,'add')[0].value,800);
assert.throws(()=>P.resolve(imported,{...b,version:'02.000.000'},5),/version/);
assert.throws(()=>P.resolve(imported,{...b,title:'PPSA99999'},5),/game/);
assert.throws(()=>P.resolve(imported,b,1e300),/fit/);assert.throws(()=>P.resolve(imported,b,1.5),/fit/);
const duplicate=pkg();duplicate.bytes.set(a.bytes.subarray(40,100),128);assert.throws(()=>P.resolve(imported,duplicate,5),/more than once/);
const missing=pkg();missing.bytes.fill(0);assert.throws(()=>P.resolve(imported,missing,5),/not found/);
assert.throws(()=>P.validate({...profile,rules:[{...profile.rules[0],pattern:'??'.repeat(60)}]}),/weak/);
assert.throws(()=>P.validate({...profile,rules:[{...profile.rules[0],type:'constructor'}]}),/type/);
assert.throws(()=>P.validate({...profile,rules:[{...profile.rules[0],file:'../sce_sys/param.sfo'}]}),/identity/);
const newer=pkg();newer.bytes[43]^=1;new DataView(newer.bytes.buffer).setInt32(64,700,true);
const masked=P.capture(newer,[edit],'Money',a);assert.equal(masked.rules[0].pattern.slice(6,8),'??');assert.equal(P.resolve(masked,newer,900)[0].off,64);
const refined=P.refine(profile,P.capture(newer,[edit],'Money'));assert.equal(refined.rules[0].pattern.slice(6,8),'??');
const increased=S.changed(a,newer,'increased',['int32']);assert(increased.some(x=>x.off===64));
const narrowed=S.changed(a,newer,'unchanged',['int32'],increased);assert.equal(narrowed.length,0);
assert.throws(()=>S.write(a,[{...edit,value:2147483648}]),/range/);
const encrypted=pkg();encrypted.bytes.set(new TextEncoder().encode('DSSS'));assert.throws(()=>P.capture(encrypted,[edit],'Money'),/encrypt/);
console.log('Portable profile relocation, bounds, ambiguity, masking and discovery passed');
vm.runInNewContext(html.slice(html.indexOf('async function editorClosedCheck'),html.indexOf("$('labMake').addEventListener")),ctx);
(async()=>{ctx.api=async()=>({known:false,running:false});await assert.rejects(ctx.editorClosedCheck(),/cannot be verified/);
ctx.api=async()=>({known:true,running:true});await assert.rejects(ctx.editorClosedCheck(),/Close the game/);
ctx.api=async()=>({known:true,running:false});await ctx.editorClosedCheck();})().catch(e=>{console.error(e);process.exit(1);});
"""
        subprocess.run([node,'-e',script],cwd=ROOT,check=True,timeout=20)
