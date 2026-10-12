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
