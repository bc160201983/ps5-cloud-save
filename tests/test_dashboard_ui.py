"""Execute browser request/control functions with isolated Node fixtures."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[1]


class DashboardUITest(unittest.TestCase):
    def test_live_batch_and_saved_value_controls_send_one_write(self):
        node=shutil.which('node')
        if not node:self.skipTest('Node.js is required for the UI fixture')
        script=r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync('ps5/ui.html','utf8');
const nodes=new Map(),writes=[],selected=[{dataset:{address:'0x10000100'}},{dataset:{address:'0x10000400'}}];
function element(){return {children:[],dataset:{},style:{},classList:{toggle(){},add(){},contains(){return false;}},append(...n){this.children.push(...n);},replaceChildren(){this.children=[];},remove(){},setAttribute(){},addEventListener(event,fn){this[event]=fn;},querySelectorAll(){return selected;}};}
function $(id){if(!nodes.has(id))nodes.set(id,element());return nodes.get(id);}
let group={id:7,name:'Money',count:2,type:'int32',value:250,target:250,frozen:false,error:''};
const ctx={$ ,selected:{title:'PPSA02225'},activeView:'detail',document:{querySelector(){return element();}},el:()=>element(),notify(){},confirm:()=>true,setTimeout(){},clearTimeout(){},api:async(path,body)=>{
 if(body){writes.push({path,body});if(body.id){if(body.action==='add')group.target+=Number(body.value);if(body.action==='freeze')group.frozen=true;if(body.action==='unfreeze')group.frozen=false;}return {message:'done'};}
 if(path==='/api/live/state')return {running:true,title:'PPSA02433',busy:false,has:true,same_game:true,count:2};
 if(path==='/api/live/pins')return {groups:[group]};
 return {places:selected.map(x=>({address:x.dataset.address,value:250})),count:2,type:'int32'};
}};
vm.runInNewContext(html.slice(html.indexOf('const live={'),html.indexOf('function detailHeader')),ctx);
(async()=>{
 await ctx.liveState();assert.equal($('liveScan').disabled,false);$('liveSet').value='500';await $('liveApply').click();
 assert.equal(writes.length,1);assert.equal(writes[0].path,'/api/live/edit');assert.equal(writes[0].body.addresses,'0x10000100,0x10000400');assert.equal(writes[0].body.action,'set');
 $('liveName').value='Money';await $('livePin').click();assert.equal(writes[1].path,'/api/live/pin');
 const actions=$('liveSaved').children[0].children[2].children;
 actions[2].value='100';await actions[3].onclick();assert.equal(writes[2].body.action,'add');assert.equal(writes[2].body.value,100);assert.equal(writes[2].body.id,7);
 await actions[5].onclick();assert.equal(writes[3].body.action,'freeze');
 await actions[5].onclick();assert.equal(writes[4].body.action,'unfreeze');assert.equal('value' in writes[4].body,false);
})().catch(e=>{console.error(e);process.exit(1);});
"""
        subprocess.run([node,'-e',script],cwd=ROOT,check=True,timeout=15)

    def test_safe_get_retries_but_write_requests_are_not_replayed(self):
        node=shutil.which('node')
        if not node:self.skipTest('Node.js is required for the UI fixture')
        script=r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync('ps5/ui.html','utf8');
const source=html.slice(html.indexOf('async function api('),html.indexOf('\nfunction setControls'));
(async()=>{
 let calls=0;
 const c={URLSearchParams,setTimeout:fn=>fn(),fetch:async()=>{
  calls++;const status=calls<3?503:200;
  return {status,ok:status===200,json:async()=>({busy:status===503,message:'busy'})};
 }};
 vm.runInNewContext(source,c);await c.api('/api/queue');assert.equal(calls,3);
 calls=0;c.fetch=async()=>{calls++;return {status:503,ok:false,json:async()=>({busy:true,message:'busy'})};};
 await assert.rejects(c.api('/api/backup',{closed:'yes'}));assert.equal(calls,1);
 calls=0;c.fetch=async()=>{calls++;throw Error('network failure');};
 await assert.rejects(c.api('/api/restore',{confirm:'yes'}));assert.equal(calls,1);
 calls=0;c.fetch=async()=>{calls++;if(calls===1)throw Error('network failure');return {status:200,ok:true,json:async()=>({})};};
 await c.api('/api/games');assert.equal(calls,2);
 calls=0;c.fetch=async()=>{calls++;return {status:503,ok:false,json:async()=>({busy:true,message:'busy'})};};
 await assert.rejects(c.api('/api/download-pc?file=example'));assert.equal(calls,1);
 const start=html.indexOf('function setControls()'),end=html.indexOf('\nasync function operation',start);
 const buttons=[{id:'backup',classList:{contains:()=>false},dataset:{}},{id:'back',classList:{contains:()=>false},dataset:{}}];
 const confirm={checked:true},restore={},bar={classList:{toggle:()=>{}}};
 const controls={busy:false,versionLoading:true,backgroundRunning:false,document:{querySelectorAll:()=>buttons},$:id=>id==='busybar'?bar:id==='confirmRestore'?restore:confirm};
 vm.runInNewContext(html.slice(start,end),controls);controls.setControls();
 assert.equal(buttons[0].disabled,true);assert.equal(buttons[1].disabled,false);assert.equal(restore.disabled,true);
 controls.versionLoading=false;controls.setControls();assert.equal(buttons[0].disabled,false);
 controls.backgroundRunning=true;controls.setControls();assert.equal(buttons[0].disabled,false);assert.equal(buttons[1].disabled,false);
 buttons[0].dataset.upload='true';controls.setControls();assert.equal(buttons[0].disabled,true);
 console.log('Read retry/write non-replay/control fixtures passed');
})().catch(e=>{console.error(e);process.exit(1);});
"""
        subprocess.run([node,'-e',script],cwd=ROOT,check=True,timeout=15)

    def test_embedded_script_has_valid_syntax(self):
        """A syntax error anywhere leaves the whole dashboard blank, so check the full script."""
        import re,tempfile
        node=shutil.which('node')
        if not node:self.skipTest('Node.js is required for the UI fixture')
        html=(ROOT/'ps5/ui.html').read_text(encoding='utf-8')
        script=max(re.findall(r'<script[^>]*>(.*?)</script>',html,re.S),key=len)
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'ui.js';path.write_text(script,encoding='utf-8')
            result=subprocess.run([node,'--check',str(path)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)


class ProfileLibraryTest(unittest.TestCase):
    def test_failed_library_load_is_retried_before_retain_saves(self):
        """A library load that failed once (for example against an older dashboard) must not
        block every later Retain profile; the save retries the load first."""
        node=shutil.which('node')
        if not node:self.skipTest('Node.js is required for the UI fixture')
        script=r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync('ps5/ui.html','utf8');
const block=n=>{const s=html.indexOf('const '+n+'=(()=>{');return html.slice(s,html.indexOf('})();',s)+5);};
const flow=html.slice(html.indexOf('const profileBook='),html.indexOf('function editorProfilesDownload'));
let gets=0;const posts=[];
const status={textContent:''};
const ctx={TextDecoder,TextEncoder,URLSearchParams,JSON,confirm:()=>true,editorProfilesRender(){},$:()=>status,
 api:async path=>{assert.equal(path,'/api/editor-profiles');gets++;if(gets===1)throw Error('Profiles could not be read');return {text:posts.length?posts.at(-1):'[]',revision:'r'+gets};},
 fetch:async(url,options)=>{posts.push(options.body);return {ok:true,status:200,json:async()=>({message:'Profiles stored'})};}};
vm.runInNewContext(block('SaveLab')+'\n'+block('EditorProfiles')+'\n'+flow,ctx);
(async()=>{
 await vm.runInContext('editorProfilesLoad()',ctx);
 assert.equal(vm.runInContext('profileBook.revision',ctx),null);
 const parts=[];for(let i=0;i<256;i++)parts.push(i>=128&&i<132?'??':(i%3?'00':'4c'));
 const profile={format:'PSCLOUD_EDIT_PROFILE_V1',kind:'live',name:'LINEN',title:'PPSA03210',version:'02.024.000',rules:[{type:'int32',offset:128,pattern:parts.join('')}]};
 ctx.profile=profile;await vm.runInContext('editorProfilesRemember(profile)',ctx);
 assert.equal(posts.length,1);assert.equal(JSON.parse(posts[0])[0].name,'LINEN');
 console.log('Retain after a failed library load saved the profile');
})().catch(e=>{console.error(e);process.exit(1);});
"""
        subprocess.run([node,'-e',script],cwd=ROOT,check=True,timeout=15)

    def test_live_profiles_keep_valid_pointer_paths(self):
        node=shutil.which('node')
        if not node:self.skipTest('Node.js is required for the UI fixture')
        script=r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync('ps5/ui.html','utf8');
const block=n=>{const s=html.indexOf('const '+n+'=(()=>{');return html.slice(s,html.indexOf('})();',s)+5);};
const ctx={TextDecoder,TextEncoder};vm.runInNewContext(block('SaveLab')+'\n'+block('EditorProfiles')+';this.P=EditorProfiles;',ctx);
const parts=[];for(let i=0;i<256;i++)parts.push(i>=128&&i<132?'??':(i%3?'00':'4c'));
const live=chains=>({format:'PSCLOUD_EDIT_PROFILE_V1',kind:'live',name:'Health',title:'PPSA03210',version:'01.000.000',rules:[{type:'int32',offset:128,pattern:parts.join(''),...(chains?{chains}:{})}]});
assert.deepEqual(ctx.P.validate(live(['800/38/1c4'])).rules[0].chains,['800/38/1c4']);
assert.equal(ctx.P.validate(live()).rules[0].chains,undefined);
for(const bad of [['xyz'],['800//1c4'],['1/2/3/4/5/6'],'123456789'.split(''),'800'])assert.throws(()=>ctx.P.validate(live(bad)),/pointer paths/);
assert.equal(ctx.P.validate(live('abcdef09'.split(''))).rules[0].chains.length,8);
// Refining: paths found in both sessions first, then the new ones, then the older ones; eight at most.
assert.deepEqual(ctx.P.refine(live(['a/1','b/2']),live(['c/3','a/1'])).rules[0].chains,['a/1','c/3','b/2']);
assert.equal(ctx.P.refine(live('abcdef09'.split('')),live('12345678'.split(''))).rules[0].chains.join(''),'12345678');
assert.deepEqual(ctx.P.refine(live(['a/1']),live()).rules[0].chains,['a/1']);
console.log('Live profiles keep valid pointer paths');
"""
        subprocess.run([node,'-e',script],cwd=ROOT,check=True,timeout=15)

    def test_shared_save_slot_choices(self):
        """Issue #3: which saves of a file this profile lacks, and which of its own saves can take them."""
        node=shutil.which('node')
        if not node:self.skipTest('Node.js is required for the UI fixture')
        script=r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync('ps5/ui.html','utf8');
const pick=n=>{const s=html.indexOf('function '+n+'(');return html.slice(s,html.indexOf('\n',s));};
const ctx={};vm.runInNewContext(pick('shareIntoChoices')+'\n'+pick('shareIntoText')+';this.C=shareIntoChoices;this.T=shareIntoText;',ctx);
// The file holds the options save and a character in slot 3; this profile has options and a new game in slot 0.
const r=ctx.C(['SAVEDATA0OptionsProfile0','SAVEDATA0PlayerProfile3'],['SAVEDATA0OptionsProfile0','SAVEDATA0PlayerProfile0']);
assert.deepEqual([...r.missing],['SAVEDATA0PlayerProfile3']);
assert.deepEqual([...r.targets],['SAVEDATA0PlayerProfile0']);  // never the options save the file writes itself
assert.equal(ctx.T([['SAVEDATA0PlayerProfile3','SAVEDATA0PlayerProfile0']]),'SAVEDATA0PlayerProfile3=SAVEDATA0PlayerProfile0');
assert.equal(ctx.T([['a','x'],['b','']]),'a=x');  // empty: create a new slot as before
assert.equal(ctx.T([]),'');
assert.throws(()=>ctx.T([['a','x'],['b','x']]),/Two saves cannot go into x/);
console.log('Shared save slot choices');
"""
        subprocess.run([node,'-e',script],cwd=ROOT,check=True,timeout=15)

    def test_retained_live_profile_is_offered_for_the_running_game(self):
        node=shutil.which('node')
        if not node:self.skipTest('Node.js is required for the UI fixture')
        script=r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync('ps5/ui.html','utf8');
const start=html.indexOf('function liveResumeRender('),end=html.indexOf('async function editorProfilesResume(',start);
function element(tag,cls,text){return {tag,cls,textContent:text||'',children:[],dataset:{},append(...n){this.children.push(...n);},replaceChildren(){this.children=[];}};}
const box=element('box'),resumed=[];
const ctx={$:id=>id==='liveResume'?box:null,el:element,editorProfilesResume:p=>resumed.push(p.name),
 profileBook:{items:[{kind:'live',name:'LINEN',title:'PPSA03210',version:'02.024.000'},{kind:'save',name:'Steel',title:'PPSA03210',version:'02.024.000'},{kind:'live',name:'Gold',title:'PPSA99999',version:'01.000.000'}]}};
vm.runInNewContext(html.slice(start,end),ctx);
ctx.liveResumeRender({running:true,busy:false,pinned:0,title:'PPSA03210'});
assert.equal(box.children.length,1);assert.equal(box.children[0].children[1].textContent,'Relocate & bind LINEN');
box.children[0].children[1].onclick();assert.deepEqual(resumed,['LINEN']);assert.equal(box.children.length,0);
ctx.liveResumeRender({running:true,busy:false,pinned:1,title:'PPSA03210'});assert.equal(box.children.length,0);
ctx.liveResumeRender({running:true,busy:false,pinned:0,title:'PPSA11111'});assert.equal(box.children.length,0);
console.log('Retained profile prompt shown only for the running game');
"""
        subprocess.run([node,'-e',script],cwd=ROOT,check=True,timeout=15)

    def test_pc_save_replaces_matching_files_and_keeps_the_rest(self):
        node=shutil.which('node')
        if not node:self.skipTest('Node.js is required for the UI fixture')
        script=r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync('ps5/ui.html','utf8');
const start=html.indexOf('const pcSave='),end=html.indexOf('async function pcSaveRead(',start);
const ctx={TextDecoder};vm.runInNewContext(html.slice(start,end),ctx);
const b=(t,n)=>new Uint8Array(n||t.length).fill(t.charCodeAt(0));
const slots=[{slot:'other',files:[{path:'unrelated.dat',data:b('u',10)}],sys:[]},
 {slot:'SaveSlot1',files:[{path:'SaveGames/Slot1.sav',data:b('a',100)},{path:'settings.ini',data:b('s',20)}],sys:[{path:'param.sfo',data:b('p',8)},{path:'keystone',data:b('k',96)}]}];
const pc=[{path:'MyGame/Saved/SaveGames/Slot1.sav',data:b('P',120)},{path:'MyGame/readme.txt',data:b('r',5)}];
const plan=ctx.pcSavePlan(slots,pc);
assert.equal(plan.slot,'SaveSlot1');assert.equal(plan.matched,1);assert.equal(plan.unused.length,1);assert.equal(plan.rows[0].warn,'');
const files=ctx.pcSaveLayout(plan);
assert.deepEqual(files.map(f=>f.path),['SaveSlot1/sce_sys/param.sfo','SaveSlot1/sce_sys/keystone','SaveSlot1/SaveGames/Slot1.sav','SaveSlot1/settings.ini']);
assert.equal(files[2].data[0],'P'.charCodeAt(0));assert.equal(files[3].data[0],'s'.charCodeAt(0));
assert.equal(ctx.pcSavePlan(slots,[{path:'Slot1.sav',data:b('P',20)}]).rows[0].warn,'Size differs a lot');
assert.throws(()=>ctx.pcSavePlan(slots,[{path:'Slot1.sav',data:new TextEncoder().encode('DSSS....')}]),/encrypted/);
assert.throws(()=>ctx.pcSavePlan(slots,[{path:'profile.bin',data:b('x',4)}]),/None of these file names match/);
console.log('PC save plan replaces matching files and keeps the rest');
"""
        subprocess.run([node,'-e',script],cwd=ROOT,check=True,timeout=15)


