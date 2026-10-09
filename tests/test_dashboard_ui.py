"""Execute browser request/control functions with isolated Node fixtures."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[1]


class DashboardUITest(unittest.TestCase):
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
