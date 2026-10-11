"""Local design preview. No PS5, credentials or cloud connections are used."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import sys
from urllib.parse import urlsplit

ROOT=Path(__file__).resolve().parents[1]
TOKEN='preview'
# --demo: fictional games with generated cover art, for README screenshots (no real games or data).
DEMO='--demo' in sys.argv
DEMO_GAMES=[('PPSA90001','Starfall Odyssey',('#5b3df5','#ff6fb1')),('PPSA90002','Neon Drift',('#00c2ff','#7a2cff')),('PPSA90003','Ember Knights',('#ff7a1a','#9b1d3a')),
            ('PPSA90004','Tidebound',('#00b39f','#0a3d91')),('PPSA90005','Skyforge Racing',('#ffd166','#ef476f')),('PPSA90006','Hollow Lanterns',('#3a0ca3','#f72585')),
            ('PPSA90007','Glacier Rescue',('#90e0ef','#0077b6')),('PPSA90008','Pixel Paladins',('#80ed99','#22577a'))]
def demo_cover(title):
    name,(a,b)=next(((n,c) for t,n,c in DEMO_GAMES if t==title),('Game',('#444','#222')))
    words=name.split();line2=' '.join(words[1:])
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512"><defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{a}"/><stop offset="1" stop-color="{b}"/></linearGradient></defs>
<rect width="512" height="512" fill="url(#g)"/><circle cx="400" cy="120" r="150" fill="#fff" opacity=".12"/><circle cx="90" cy="430" r="120" fill="#000" opacity=".15"/>
<path d="M0 380 L140 250 L230 330 L330 210 L512 360 L512 512 L0 512 Z" fill="#000" opacity=".25"/>
<text x="40" y="200" font-family="Segoe UI,Arial,sans-serif" font-size="64" font-weight="800" fill="#fff">{words[0].upper()}</text>
<text x="40" y="262" font-family="Segoe UI,Arial,sans-serif" font-size="40" font-weight="600" fill="#fff" opacity=".85">{line2.upper()}</text></svg>'''.encode()

class Handler(BaseHTTPRequestHandler):
    def send(self,data,mime='application/json',status=200):
        if isinstance(data,dict):data=json.dumps(data).encode()
        self.send_response(status);self.send_header('Content-Type',mime)
        self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store')
        self.end_headers();self.wfile.write(data)

    def log_message(self,*args):pass

    def do_GET(self):
        path=urlsplit(self.path).path
        if path=='/':
            html=(ROOT/'ps5/ui.html').read_text(encoding='utf-8').replace('__VERSION__',(ROOT/'VERSION').read_text(encoding='utf-8').strip())
            if not DEMO:html=html.replace('<header class="topbar">','<div class="info" style="margin:0 0 22px;color:#d6caff">Design preview · PS5 offline. Sample versions only; no actions affect your saves or cloud account.</div><header class="topbar">')
            shot=dict(p.split('=',1) for p in urlsplit(self.path).query.split('&') if '=' in p).get('shot','')
            if DEMO and shot:  # open the page to capture, then stay still
                act={'game':"document.querySelector('#gamegrid article button').click();setTimeout(()=>{showDetailTab('share');window.scrollTo(0,0);},900);",
                     'storage':"view('storage');",'settings':"view('settings');setTimeout(()=>{const card=document.getElementById('peerRows').closest('.settingspage');let n=card.previousElementSibling;while(n){if(!n.classList.contains('sectionhead'))n.style.display='none';n=n.previousElementSibling;}},1200);",'home':''}.get(shot,'')
                html=html.replace('</body>','<script>setTimeout(()=>{'+act+'},1500);</script></body>')
            self.send(html.encode(),'text/html; charset=utf-8');return
        if DEMO and self.demo(path):return
        if path=='/api/state':
            self.send({'version':(ROOT/'VERSION').read_text().strip(),'firmware':'11.40','firmware_tested':True,'pid':42,'configured':True,'connected':True,
                       'url':'https://your-nextcloud.example/remote.php/dav/files/you/PS5Backups','username':'Your Nextcloud account'})
        elif path=='/api/preferences':
            self.send({'auto_upload':True,'activity_refresh':True,'game_close_available':True,'game_close_backup':False,'sharing_available':True,'compact_backups':True,'compact_required':False})
        elif path.startswith('/docs/') and '..' not in path and (ROOT/path.lstrip('/')).is_file():
            f=ROOT/path.lstrip('/');self.send(f.read_bytes(),'image/svg+xml' if f.suffix=='.svg' else 'image/png' if f.suffix=='.png' else 'text/plain')
        elif path=='/api/health':self.send({'busy':False})
        elif path=='/api/sync-status':self.send({'state':'cloud_newer','local_time':1791700000,'base':0,'cloud':{'file':'x','user':'1eb70484','name':'bilal','created':1791720000,'sha256':''},'links':1})
        elif path=='/api/cloud-profiles':self.send({'console':'a','profiles':[{'user':'1eb70484','name':'bilal','console':'b','firmware':'7.00','this_console':False},{'user':'00000001','name':'Player One','console':'a','firmware':'11.40','this_console':True}],'links':{'00000001':'1eb70484'}})
        elif path=='/api/backup-all':self.send({'running':False,'total':24,'done':24,'backed':3,'unchanged':20,'skipped':1,'failed':0,'current':'','started':1791700000,'finished':int(__import__('time').time())-60,'schedule_hour':3,'schedule_last':'2026-10-11'})
        elif path=='/api/game-settings':self.send({'title':'PPSA02433','close_backup':True,'backup_all':True})
        elif path=='/api/storage':self.send({'root':'/data/pscloud','free':412*2**30,'total':825*2**30,'places':[
            {'key':'backups','label':'Local backups','purpose':'Every backup made on this PS5.','path':'/data/pscloud/spool','exists':True,'files':14,'bytes':38*2**20},
            {'key':'shared','label':'Shared saves','purpose':'ZIPs exported for sharing and ones you received.','path':'/data/pscloud/share','exists':True,'files':3,'bytes':9*2**20},
            {'key':'rollback','label':'Restore safety copies','purpose':'A copy of your save taken just before each restore.','path':'/data/pscloud/rollback','exists':False,'files':0,'bytes':0},
            {'key':'saves','label':'Your PS5 saves','purpose':"The console's own save files.",'path':'/user/home','exists':True,'files':0,'bytes':0}]})
        elif path=='/api/usb':self.send({'drives':[{'id':'usb0','path':'/mnt/usb0','free':58*2**30,'total':64*2**30}]})
        elif path=='/api/usb-files':self.send({'files':[{'path':'PSCloud/Crash Bandicoot 4 (PPSA02433)/portable-PPSA02433-'+c*32+'.zip','size':5300,'modified':t} for c,t in (('c',1791712908),('d',1791713192))]})
        elif path=='/api/running':self.send({'known':True,'running':True,'title':'PPSA02433','name':'Crash Bandicoot 4','icon':'/api/icon?title=PPSA02433','backup_on_close':True})
        elif path=='/api/transfer':self.send({'sequence':3,'phase':7,'active':False,'result':0,'cancelled':False,'name':'Crash Bandicoot 4','file':'portable-PPSA02433-x.zip','total':0,'done':0,'transport':0,'http':200})
        elif path=='/api/google/status':self.send({'configured':False,'authorized':False,'pending':False,'transfers_available':False})
        elif path=='/api/queue':
            self.send({'items':[{'file':'portable-PPSA02433-a.zip','name':'Crash Bandicoot 4','user':'00000001','title':'PPSA02433','slot':'WholeGame','created':1791536400,'valid':True}],'count':1,'truncated':False})
        elif path=='/api/users':
            self.send({'users':[{'user':'00000001','name':'Player One'},{'user':'00000002','name':'Player Two'}]})
        elif path=='/api/games':
            self.send({'games':[{'user':'00000001','title':'PPSA02433','slot':slot,
                                 'name':'Crash Bandicoot 4','supported':True,'installed':True,'label':label,'sub':'','detail':detail}
                                for slot,label,detail in [('PlayerSaveSlot0Save','Slot 1 · N. Sanity Beach','42% complete'),('PlayerSaveProfileSaveData','Crash Bandicoot 4','Settings')]]+
                               [{'user':'00000002','title':'PPSA01341','slot':'SAVEDATA00','name':'Astro Bot','supported':True}]})
        elif path=='/api/backups':
            self.send({'backups':[{'file':'ps5-11.40-PPSA02433-'+char*32+'.zip',
                                   'sha256':char*64,'slot':'PlayerSaveSlot0Save','created':stamp}
                                  for char,stamp in [('a',1791536400),('b',1791450000)]]})
        elif path=='/api/log':
            self.send({'log':'2026-10-09 14:00:00 EVENT Preview: backup saved in local queue\n2026-10-09 14:01:00 INFO Preview: upload checksum verified\n2026-10-09 14:02:00 WARN Preview: network unavailable; backup retained\n'})
        else:self.send({'message':'Preview endpoint unavailable'},status=404)

    def demo(self,path):
        q=dict(p.split('=',1) for p in urlsplit(self.path).query.split('&') if '=' in p)
        if path=='/api/icon' or path=='/api/slot-icon':self.send(demo_cover(q.get('title','')),'image/svg+xml');return True
        if path=='/api/users':self.send({'users':[{'user':'00000001','name':'Alex'},{'user':'00000002','name':'Sam'}]});return True
        if path=='/api/games':
            slots=[('slot-1','Chapter 4 · The Sunken Gate','12h 40m'),('slot-2','Chapter 2 · Into the Wilds','5h 12m'),('settings','Settings','')]
            self.send({'games':[{'user':'00000001','title':t,'slot':sl,'name':n,'supported':True,'installed':i!=7,'label':lab,'sub':'','detail':det,'icon':'/api/icon?title='+t}
                                for i,(t,n,_) in enumerate(DEMO_GAMES) for sl,lab,det in (slots if i==0 else slots[:1])]});return True
        if path=='/api/running':self.send({'known':True,'running':True,'title':'PPSA90002','name':'Neon Drift','icon':'/api/icon?title=PPSA90002','backup_on_close':True});return True
        if path=='/api/sync-status':self.send({'state':'cloud_newer','local_time':1791700000,'base':0,'cloud':{'file':'x','user':'00000009','name':'Alex','created':1791720000,'sha256':'','source':'network','peer':'192.168.0.198:8082','firmware':'7.00'},'links':1,'other_found':True,'peers':1,'has_cloud':True});return True
        if path=='/api/queue':self.send({'items':[{'file':'portable-PPSA90001-'+'a'*32+'.zip','name':'Starfall Odyssey','user':'00000001','title':'PPSA90001','slot':'WholeGame','created':1791716400,'valid':True}],'count':1,'truncated':False});return True
        if path=='/api/peers':self.send({'peers':[{'address':'192.168.0.198:8082','reachable':True,'info':{'console':'b','firmware':'7.00','profiles':[{'user':'00000009','name':'Alex'},{'user':'00000010','name':'Sam'}],'sync':['PPSA90001']}}]});return True
        if path=='/api/cloud-profiles':self.send({'console':'a','profiles':[],'links':{'00000001':'00000009'}});return True
        if path=='/api/remote':self.send({'allow_replace':True});return True
        if path=='/api/transfer':self.send({'sequence':0,'phase':0,'active':False,'result':0,'cancelled':False,'name':'','file':'','total':0,'done':0,'transport':0,'http':0});return True
        if path=='/api/encryption':self.send({'on':True});return True
        if path=='/api/cloud-housekeeping':self.send({'configured':True,'keep':5,'known':True,'used':1288490188,'available':98*2**30});return True
        if path=='/api/cleanup':self.send({'blocked':False,'leftovers':{'bytes':0,'files':0},'safety':{'bytes':41*2**20,'files':6},'shared':{'bytes':12*2**20,'files':4},'history':{'bytes':3*2**20,'files':5}});return True
        if path=='/api/usb-files':self.send({'files':[{'path':'PSCloud/Starfall Odyssey (PPSA90001)/portable-PPSA90001-'+c*32+'.zip','size':5300000,'modified':t} for c,t in (('c',1791712908),('d',1791713192))]});return True
        if path=='/api/storage':
            self.send({'root':'/data/pscloud','free':412*2**30,'total':825*2**30,'places':[
                {'key':'backups','label':'Local backups','purpose':'Every backup made on this PS5. Ones waiting are uploaded next.','path':'/data/pscloud/spool','exists':True,'files':38,'bytes':164*2**20},
                {'key':'shared','label':'Shared saves','purpose':'ZIPs exported for sharing and ones you received. The 2 newest per game are kept.','path':'/data/pscloud/share','exists':True,'files':9,'bytes':27*2**20},
                {'key':'rollback','label':'Restore safety copies','purpose':'Your save as it was before each restore or replace. The 3 newest per game are kept.','path':'/data/pscloud/rollback','exists':True,'files':6,'bytes':41*2**20},
                {'key':'downloads','label':'Cloud downloads','purpose':'Backups downloaded from your cloud to this PS5.','path':'/data/pscloud/downloads','exists':False,'files':0,'bytes':0},
                {'key':'staging','label':'Leftover working files','purpose':'Normally removed automatically.','path':'/data/pscloud/staging','exists':False,'files':0,'bytes':0},
                {'key':'log','label':'Activity log','purpose':'Everything PSCloud did, shown on the Activity page.','path':'/data/pscloud/pscloud.log','exists':True,'files':1,'bytes':612*1024},
                {'key':'saves','label':'Your PS5 saves','purpose':'The console\'s own save files. PSCloud only reads them, except when you choose Restore or Replace.','path':'/user/home','exists':True,'files':0,'bytes':0}]});return True
        return False

    def do_POST(self):
        # Deliberately discard input. Do not persist or echo passwords/settings.
        n=int(self.headers.get('Content-Length','0'))
        if n>8192:self.send({'message':'Request too large'},status=400);return
        self.rfile.read(n)
        if self.path.startswith('/api/google/'):
            self.send({'message':'Design preview only. Google sign-in requires the native payload and a registered app.'},status=501);return
        self.send({'ok':True,'message':'Preview only — no saves, settings or cloud files were changed.'})

if __name__=='__main__':
    port=int(sys.argv[1]) if len(sys.argv)>1 else 8085
    print(f'Dashboard preview: http://127.0.0.1:{port}/#token=preview',flush=True)
    ThreadingHTTPServer(('127.0.0.1',port),Handler).serve_forever()
