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
            self.send({'version':(ROOT/'VERSION').read_text().strip(),'configured':True,'connected':True,
                       'url':'https://your-nextcloud.example/remote.php/dav/files/you/PS5Backups','username':'Your Nextcloud account'})
        elif path=='/api/preferences':
            self.send({'auto_upload':True,'activity_refresh':True,'game_close_available':False,'sharing_available':False})
        elif path=='/api/health':self.send({'busy':False})
        elif path=='/api/google/status':self.send({'configured':False,'authorized':False,'pending':False,'transfers_available':False})
        elif path=='/api/queue':
            self.send({'items':[],'count':0,'truncated':False})
        elif path=='/api/games':
            self.send({'games':[{'user':'00000001','title':'PPSA02433','slot':slot,
                                 'name':'Crash Bandicoot 4','supported':True}
                                for slot in ['PlayerSaveSlot0Save','PlayerSaveProfileSaveData']]})
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
