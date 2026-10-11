"""Local design preview. No PS5, credentials or cloud connections are used."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import sys
from urllib.parse import urlsplit

ROOT=Path(__file__).resolve().parents[1]
TOKEN='preview'

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
            html=html.replace('<header class="topbar">','<div class="info" style="margin:0 0 22px;color:#d6caff">Design preview · PS5 offline. Sample versions only; no actions affect your saves or cloud account.</div><header class="topbar">')
            self.send(html.encode(),'text/html; charset=utf-8');return
        if path=='/api/state':
            self.send({'version':(ROOT/'VERSION').read_text().strip(),'firmware':'11.40','firmware_tested':True,'pid':42,'configured':True,'connected':True,
                       'url':'https://your-nextcloud.example/remote.php/dav/files/you/PS5Backups','username':'Your Nextcloud account'})
        elif path=='/api/preferences':
            self.send({'auto_upload':True,'activity_refresh':True,'game_close_available':True,'game_close_backup':False,'sharing_available':True,'compact_backups':True,'compact_required':False})
        elif path=='/api/health':self.send({'busy':False})
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
                                 'name':'Crash Bandicoot 4','supported':True}
                                for slot in ['PlayerSaveSlot0Save','PlayerSaveProfileSaveData']]+
                               [{'user':'00000002','title':'PPSA01341','slot':'SAVEDATA00','name':'Astro Bot','supported':True}]})
        elif path=='/api/backups':
            self.send({'backups':[{'file':'ps5-11.40-PPSA02433-'+char*32+'.zip',
                                   'sha256':char*64,'slot':'PlayerSaveSlot0Save','created':stamp}
                                  for char,stamp in [('a',1791536400),('b',1791450000)]]})
        elif path=='/api/log':
            self.send({'log':'2026-10-09 14:00:00 EVENT Preview: backup saved in local queue\n2026-10-09 14:01:00 INFO Preview: upload checksum verified\n2026-10-09 14:02:00 WARN Preview: network unavailable; backup retained\n'})
        else:self.send({'message':'Preview endpoint unavailable'},status=404)

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
