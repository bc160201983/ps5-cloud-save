"""Closed-game backup-only test. Never mounts, restores or deletes game saves.

Pass only a game confirmed closed by the user. No credentials or real saves
are written to disk. All original active images are hashed before and after.
"""
import ftplib
import hashlib
import http.cookiejar
import io
import json
import re
import sys
import urllib.parse
import urllib.request
import zipfile

title = sys.argv[1]
assert title in ('PPSA02433', 'PPSA10595')
user = '1eb70483'
base = 'http://192.168.0.193:8082'
folder = f'/user/home/{user}/savedata_prospero/{title}'
client = urllib.request.build_opener(urllib.request.ProxyHandler({}),
    urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
client.open(base+'/', timeout=10).close()

def api(path, data=None):
    body = None if data is None else urllib.parse.urlencode(data).encode()
    request = urllib.request.Request(base+path, body, {'X-PSCloud-Request':'1'})
    with client.open(request, timeout=600) as response:
        return json.load(response)

def originals():
    ftp = ftplib.FTP()
    ftp.connect('192.168.0.193', 2121, timeout=60)
    ftp.login();ftp.cwd(folder)
    rows=[];ftp.retrlines('LIST', rows.append)
    names=sorted(row.split()[-1] for row in rows if row.split()[-1].startswith('sdimg_')
        and not row.split()[-1].startswith('sdimg_sce_bu_'))
    assert names and all(re.fullmatch(r'sdimg_[A-Za-z0-9_-]{1,63}', name) for name in names)
    hashes={}
    for name in names:
        digest=hashlib.sha256();ftp.retrbinary('RETR '+name, digest.update)
        hashes[name]=digest.hexdigest()
    ftp.quit();return hashes

assert api('/api/state')['version']=='0.10.0'
automatic=api('/api/preferences')['auto_upload']
chosen={'user':user,'title':title,'slot':'WholeGame'}
query=urllib.parse.urlencode(chosen)
before=originals()
print(title, 'active original slots:', len(before), flush=True)
for attempt in range(2):
    result=api('/api/backup',dict(chosen,closed='yes'))
    print('Backup',attempt+1,result['message'],flush=True)
    if automatic:
        snapshots=api('/api/backups?'+query)['backups']
        keys={s['file'] for s in snapshots}
        if attempt==0:
            first=keys
            snapshot=max(snapshots,key=lambda s:s['created'])
            url='/api/download-pc?'+urllib.parse.urlencode(dict(chosen,file=snapshot['file']))
    else:
        snapshots=[s for s in api('/api/queue')['items'] if s['title']==title and s['user']==user and s['slot']=='WholeGame']
        keys={s['file'] for s in snapshots}
        if attempt==0:
            first=keys;snapshot=max(snapshots,key=lambda s:s['created'])
            url='/api/queue-download?'+urllib.parse.urlencode({'file':snapshot['file']})
    if attempt==1:assert first==keys, 'Unexpected duplicate version'
with client.open(base+url, timeout=600) as response:archive=response.read(256*1024*1024+1)
assert len(archive)<=256*1024*1024
with zipfile.ZipFile(io.BytesIO(archive)) as z:
    assert z.testzip() is None
    assert set(z.namelist())==set(before)|{'manifest.txt'}
    for name,digest in before.items():assert hashlib.sha256(z.read(name)).hexdigest()==digest,name
    manifest=z.read('manifest.txt')
    assert ('TITLE='+title+'\n').encode() in manifest
    assert ('SLOTS='+str(len(before))+'\n').encode() in manifest
if automatic:assert hashlib.sha256(archive).hexdigest()==snapshot['sha256']
assert originals()==before, 'Original save changed during test'
print('PASS:',title,'all',len(before),'slots included; archive CRC and image hashes valid; repeat deduplicated; originals unchanged',flush=True)
