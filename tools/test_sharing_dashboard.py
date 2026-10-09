"""Export/import/check only. Never calls live restore. No saves written to PC.
Requires explicit confirmation that this game is closed on both consoles.
"""
import argparse
import hashlib
import http.cookiejar
import json
import urllib.error
import urllib.parse
import urllib.request


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',required=True);p.add_argument('--target',required=True)
    p.add_argument('--source-user',required=True);p.add_argument('--target-user',required=True)
    p.add_argument('--title',required=True)
    p.add_argument('--confirm-games-closed',action='store_true')
    args=p.parse_args()
    if not args.confirm_games_closed:p.error('Both games must be fully closed; confirm explicitly')
    clients={}
    for host in (args.source,args.target):
        base='http://'+host+':8082'
        op=urllib.request.build_opener(urllib.request.ProxyHandler({}),urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        op.open(base+'/',timeout=10).read()
        health=json.load(op.open(base+'/api/health',timeout=10))
        if health.get('busy'):raise RuntimeError('Console busy; no sharing test started')
        clients[host]=(base,op)

    def request(host,path,form=None,data=None):
        base,op=clients[host]
        if form is not None:data=urllib.parse.urlencode(form).encode()
        r=urllib.request.Request(base+path,data=data,headers={'X-PSCloud-Request':'1'})
        try:response=op.open(r,timeout=180)
        except urllib.error.HTTPError as e:raise RuntimeError(e.read().decode()) from e
        return json.load(response)

    selection={'user':args.source_user,'title':args.title,'slot':'WholeGame','closed':'yes'}
    print('Starting source export on staged copies:',args.title,flush=True)
    exported=request(args.source,'/api/share-export',selection)
    print('Export result:',exported,flush=True)
    base,op=clients[args.source]
    data=op.open(base+'/api/share-download?'+urllib.parse.urlencode({'file':exported['file']}),timeout=180).read()
    if hashlib.sha256(data).hexdigest()!=exported['sha256']:raise RuntimeError('Export download checksum mismatch')
    selection['user']=args.target_user
    print('Importing portable package into receiver local storage; no live changes',flush=True)
    imported=request(args.target,'/api/share-import?'+urllib.parse.urlencode(selection),data=data)
    if imported['sha256']!=exported['sha256']:raise RuntimeError('Imported checksum mismatch')
    selection.update(file=imported['file'],sha256=imported['sha256'])
    print('Starting recipient staged compatibility check',flush=True)
    checked=request(args.target,'/api/share-check',selection)
    print('Staged check result:',checked,flush=True)
    for host in (args.source,args.target):
        print(host,'health after check:',request(host,'/api/health'),flush=True)
    print('No live restore was requested. No save files were written to this PC.',flush=True)


if __name__=='__main__':main()
