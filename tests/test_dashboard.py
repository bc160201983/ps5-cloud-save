import hashlib
import http.client
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import urllib.parse
import zipfile
import struct
import threading
import time
import test_worker

ROOT=Path(__file__).resolve().parents[1]
FILE='ps5-11.40-PPSA02433-'+'b'*32+'.zip'
SLOT='PlayerSaveSlot0Save'
PREFIX='/backups/Crash%20Bandicoot%204%20-%20PPSA02433/User-1eb70483/'+SLOT+'/'

class DashboardTests(unittest.TestCase):
    def test_ui_ids_unique_and_settings_activity_sections_present(self):
        import re
        html=(ROOT/'ps5/ui.html').read_text()
        ids=re.findall(r'\bid="([^"]+)"',html)
        self.assertEqual(len(ids),len(set(ids)))
        self.assertIn('<section id="settings"',html)
        self.assertIn('<section id="activity"',html)

    @classmethod
    def setUpClass(cls):
        subprocess.run(['python3',str(ROOT/'tools/embed_ui.py'),str(ROOT/'ps5/ui.html'),
                        str(ROOT/'ps5/ui.h'),str(ROOT/'VERSION')],check=True)
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                        '-DPSCLOUD_HOST_TEST','-DPSCLOUD_BACKUP_EMBEDDED','-DPSCLOUD_DOWNLOAD_EMBEDDED','-DPSCLOUD_EMBEDDED',
                        *[str(ROOT/p) for p in ['ps5/dashboard.c','ps5/backup.c','ps5/download.c','src/worker.c']],
                        *[str(ROOT/'ps5/common'/n) for n in ['managed.c','mount.c','restore.c','zip.c','log.c','cloud.c','snapshot.c','appmeta.c','bundle.c','savemeta.c','google.c','sharing.c']],
                        '-o',str(ROOT/'dashboard-host'),'-lcurl','-lcrypto','-pthread'],check=True)

    def setUp(self):
        self.fixture=test_worker.WorkerTest();self.fixture.setUp()
        self.root=self.fixture.root/'pscloud';self.root.mkdir()
        self.home=self.fixture.root/'home';source=self.home/'1eb70483/savedata_prospero/PPSA02433';source.mkdir(parents=True)
        self.original=b'\x02'+b'\0'*8191
        self.image=source/'sdimg_PlayerSaveSlot0Save';self.image.write_bytes(self.original)
        self.payload=self.fixture.root/'mounted-fixture';self.payload.mkdir()
        (self.payload/'ue4savegame.dpx.sav').write_bytes(b'local progress')
        (self.payload/'sce_sys').mkdir();(self.payload/'sce_sys/param.sfo').write_bytes(b'original metadata')
        self.cloud=self.root/'upload.conf'
        self.cloud.write_text('URL='+self.fixture.env['PSCLOUD_URL']+'\nUSER=user\nPASSWORD=pass\nCA_BUNDLE='+str(self.fixture.cert)+'\nMODE=once\n')
        b=io.BytesIO()
        with zipfile.ZipFile(b,'w') as z:z.writestr('ue4savegame.dpx.sav',b'cloud progress')
        self.archive=b.getvalue();self.sha=hashlib.sha256(self.archive).hexdigest()
        self.fixture.objects[PREFIX+FILE]=self.archive
        self.fixture.objects[PREFIX+'.pscloud/'+FILE+'.identity']=('USER_ID=1eb70483\nTITLE=PPSA02433\nSAVE_NAME='+SLOT+'\nSHA256='+self.sha+'\nCREATED_UNIX=1700000000\n').encode()
        self.start_server()

    def start_server(self,**extra):
        self.process=subprocess.Popen([str(ROOT/'dashboard-host'),str(self.root),str(self.home),'0'],
                                      env=dict(os.environ,PSCLOUD_TEST_PAYLOAD=str(self.payload),**extra),
                                      stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        self.port=int(self.process.stdout.readline().strip().split('=',1)[1])
        self.process.stdout.readline()
        connection=http.client.HTTPConnection('127.0.0.1',self.port,timeout=10)
        connection.request('GET','/');response=connection.getresponse();self.cookie=response.getheader('Set-Cookie').split(';')[0];response.read();connection.close()

    def tearDown(self):
        self.process.terminate()
        try:self.process.communicate(timeout=5)
        except subprocess.TimeoutExpired:self.process.kill();self.process.communicate()
        self.fixture.tearDown()

    def request(self,path,data=None,token=True,raw=False):
        connection=http.client.HTTPConnection('127.0.0.1',self.port,timeout=15)
        headers={'Cookie':self.cookie,'X-PSCloud-Request':'1'} if token else {}
        method='POST' if data is not None else 'GET'
        body=urllib.parse.urlencode(data) if data is not None else None
        connection.request(method,path,body,headers)
        r=connection.getresponse();text=r.read().decode();status=r.status;connection.close()
        return status,text if raw else json.loads(text)

    def selected(self,**extra):return dict(user='1eb70483',title='PPSA02433',slot=SLOT,**extra)

    def test_idle_browser_connection_does_not_block_dashboard(self):
        idle=http.client.HTTPConnection('127.0.0.1',self.port,timeout=5);idle.connect()
        try:
            started=time.monotonic()
            self.assertEqual(self.request('/api/health')[1],{'busy':False})
            self.assertLess(time.monotonic()-started,2)
        finally:idle.close()

    def test_background_upload_returns_immediately_and_keeps_views_readable(self):
        self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1'})
        self.assertEqual(self.request('/api/backup',self.selected(closed='yes'))[0],200)
        queued=self.request('/api/queue')[1]['items'][0]['file']
        entered=threading.Event();release=threading.Event()
        handler=self.fixture.server.RequestHandlerClass;original=handler.do_PUT
        def slow(request):
            if not request.path.endswith('.identity'):entered.set();release.wait(8)
            original(request)
        handler.do_PUT=slow
        try:
            started=time.monotonic();status,data=self.request('/api/sync-start',{'file':queued})
            self.assertEqual(status,202,data);self.assertTrue(data['background'])
            self.assertLess(time.monotonic()-started,2);self.assertTrue(entered.wait(4))
            status,data=self.request('/api/transfer');self.assertEqual(status,200)
            self.assertTrue(data['active']);self.assertEqual(data['file'],queued+'.ready')
            self.assertEqual(self.request('/api/games')[0],200)
            self.assertEqual(self.request('/api/queue')[1]['count'],1)
            self.assertTrue(self.request('/api/health')[1]['background_upload'])
            self.assertEqual(self.request('/api/sync-start',{})[0],409)
            self.assertEqual(self.request('/api/backup',self.selected(closed='yes'))[0],200)
            self.assertEqual(self.binary('/api/download-pc?'+urllib.parse.urlencode(self.selected(file=FILE)))[0],200)
            self.assertEqual(self.request('/api/restore',self.selected(file=FILE,closed='yes',confirm='yes'))[0],200)
        finally:release.set()
        for _ in range(100):
            data=self.request('/api/transfer')[1]
            if not data['active']:break
            time.sleep(.05)
        self.assertFalse(data['active']);self.assertEqual(data['result'],0)
        self.assertEqual(self.request('/api/queue')[1]['count'],0)

    def test_local_first_unchanged_restore_works_without_cloud_or_mount(self):
        self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1'})
        folder=self.home/'1eb70483/savedata_prospero/PPSA10595';folder.mkdir()
        image=folder/'sdimg_global1';image.write_bytes(self.original)
        chosen={'user':'1eb70483','title':'PPSA10595','slot':'WholeGame','closed':'yes','confirm':'yes','skip_identical':'yes'}
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        archive=next((self.root/'spool').glob('*.ready'));sha=hashlib.sha256(archive.read_bytes()).hexdigest()
        self.fixture.status=503
        status,result=self.request('/api/restore',dict(chosen,file=archive.name[:-6],sha256=sha))
        self.assertEqual(status,200,result);self.assertIn('already matches',result['message'])
        self.assertFalse((self.root/'.mount-active').exists());self.assertFalse((self.root/'.restore-active').exists())
        self.assertEqual(image.read_bytes(),self.original)
        self.assertIn('checksum-verified local archive',(self.root/'pscloud.log').read_text())
        status,_=self.request('/api/restore',dict(chosen,file=archive.name[:-6],sha256='f'*64))
        self.assertEqual(status,400);self.assertEqual(image.read_bytes(),self.original)

    def test_cloud_delete_requires_confirmation_and_preserves_console_save(self):
        chosen=self.selected(file=FILE)
        self.assertEqual(self.request('/api/delete-cloud',chosen)[0],400)
        self.assertIn(PREFIX+FILE,self.fixture.objects)
        self.assertEqual(self.request('/api/delete-cloud',dict(chosen,confirm='yes'))[0],200)
        self.assertNotIn(PREFIX+FILE,self.fixture.objects)
        self.assertNotIn(PREFIX+'.pscloud/'+FILE+'.identity',self.fixture.objects)
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_latest_local_retention_preserves_pending_and_rollback(self):
        files=[]
        for value in (b'one',b'two'):
            (self.payload/'ue4savegame.dpx.sav').write_bytes(value)
            self.assertEqual(self.request('/api/backup',self.selected(closed='yes'))[0],200)
            files.append(next(p for p in (self.root/'spool').glob('*.sent') if p not in files))
        for i,file in enumerate(files):
            meta=file.with_name(file.name[:-5]+'.identity');text=meta.read_text()
            import re
            meta.write_text(re.sub(r'CREATED_UNIX=\d+',f'CREATED_UNIX={1000+i}',text))
        self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1'})
        (self.payload/'ue4savegame.dpx.sav').write_bytes(b'pending')
        self.request('/api/backup',self.selected(closed='yes'))
        rollback=self.root/'rollback';rollback.mkdir(exist_ok=True);(rollback/'original.img').write_bytes(self.original)
        status,prefs=self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1','local_keep_latest':'1'})
        self.assertEqual(status,200);self.assertTrue(prefs['local_keep_latest'])
        self.assertFalse(files[0].exists());self.assertTrue(files[1].exists())
        self.assertEqual(len(list((self.root/'spool').glob('*.ready'))),1)
        self.assertEqual((rollback/'original.img').read_bytes(),self.original)

    def test_manual_backup_uploads_only_its_game_not_another_pending_game(self):
        self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1'})
        self.request('/api/backup',self.selected(closed='yes'))
        old=self.request('/api/queue')[1]['items'][0]['file']
        folder=self.home/'1eb70483/savedata_prospero/PPSA10595';folder.mkdir()
        (folder/'sdimg_global1').write_bytes(self.original)
        self.request('/api/preferences',{'auto_upload':'1','activity_refresh':'1'})
        status,result=self.request('/api/backup-start',{'user':'1eb70483','title':'PPSA10595','slot':'WholeGame','closed':'yes'})
        self.assertEqual(status,202,result)
        for _ in range(100):
            data=self.request('/api/transfer')[1]
            if not data['active']:break
            time.sleep(.05)
        self.assertFalse(data['active']);self.assertEqual(data['result'],0)
        self.assertTrue((self.root/'spool'/(old+'.ready')).exists())
        self.assertEqual(len(list((self.root/'spool').glob('*PPSA10595*.sent'))),1)
        self.assertFalse(any(old in path for path in self.fixture.puts))

    def test_background_cancel_retains_local_queue(self):
        self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1'})
        self.request('/api/backup',self.selected(closed='yes'))
        entered=threading.Event();release=threading.Event()
        handler=self.fixture.server.RequestHandlerClass;original=handler.do_PUT
        def slow(request):
            if not request.path.endswith('.identity'):entered.set();release.wait(8)
            original(request)
        handler.do_PUT=slow
        try:
            self.assertEqual(self.request('/api/sync-start',{})[0],202)
            self.assertTrue(entered.wait(4))
            self.assertEqual(self.request('/api/sync-cancel',{},token=False)[0],401)
            self.assertEqual(self.request('/api/sync-cancel',{})[0],200)
        finally:release.set()
        for _ in range(100):
            data=self.request('/api/transfer')[1]
            if not data['active']:break
            time.sleep(.05)
        self.assertFalse(data['active']);self.assertTrue(data['cancelled'])
        self.assertNotEqual(data['result'],0)
        self.assertEqual(len(list((self.root/'spool').glob('*.ready'))),1)
        self.assertFalse(list((self.root/'spool').glob('*.sent')))

    def test_transfer_progress_available_while_upload_waits_for_server(self):
        self.assertEqual(self.request('/api/transfer',token=False)[0],401)
        self.assertEqual(self.request('/api/transfer')[1]['phase'],0)
        self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1'})
        self.assertEqual(self.request('/api/backup',self.selected(closed='yes'))[0],200)
        queued=self.request('/api/queue')[1]['items'][0]['file']
        entered=threading.Event();release=threading.Event();result=[]
        handler=self.fixture.server.RequestHandlerClass;original=handler.do_PUT
        def slow(request):
            if not request.path.endswith('.identity'):entered.set();release.wait(8)
            original(request)
        handler.do_PUT=slow
        task=threading.Thread(target=lambda:result.append(self.request('/api/sync-one',{'file':queued})))
        task.start()
        try:
            self.assertTrue(entered.wait(4));status,data=self.request('/api/transfer')
            self.assertEqual(status,200);self.assertEqual(data['phase'],4)
            self.assertGreater(data['total'],0);self.assertGreater(data['sequence'],0)
            self.assertFalse(any(key in data for key in ('url','password','username')))
        finally:release.set();task.join(10)
        self.assertEqual(result[0][0],200)
        self.assertEqual(self.request('/api/transfer')[1]['phase'],7)

    def test_slow_cloud_operation_keeps_page_and_health_responsive(self):
        entered=threading.Event();release=threading.Event();results=[]
        icons=self.root/'appmeta/PPSA02433';icons.mkdir(parents=True)
        png=b'\x89PNG\r\n\x1a\nfixture';(icons/'icon0.png').write_bytes(png)
        handler=self.fixture.server.RequestHandlerClass
        original=handler.do_PROPFIND
        def slow(request):
            entered.set();release.wait(8);original(request)
        handler.do_PROPFIND=slow
        task=threading.Thread(target=lambda:results.append(self.request('/api/backups?'+urllib.parse.urlencode(self.selected()))))
        task.start()
        try:
            self.assertTrue(entered.wait(3));started=time.monotonic()
            self.assertEqual(self.request('/api/health')[1],{'busy':True})
            self.assertEqual(self.request('/api/state')[0],200)
            self.assertEqual(self.request('/api/preferences')[0],200)
            self.assertEqual(self.request('/api/log')[0],200)
            status,_,body=self.binary('/api/icon?title=PPSA02433')
            self.assertEqual((status,body),(200,png))
            self.assertEqual(self.request('/',raw=True)[0],200)
            self.assertLess(time.monotonic()-started,2)
            c=http.client.HTTPConnection('127.0.0.1',self.port,timeout=5)
            c.request('POST','/api/backup',urllib.parse.urlencode(self.selected(closed='yes')),{'Cookie':self.cookie,'X-PSCloud-Request':'1'})
            self.assertEqual(c.getresponse().status,503);c.close()
            self.assertEqual(self.image.read_bytes(),self.original)
        finally:release.set();task.join(10)
        self.assertEqual(results[0][0],200)
        self.assertEqual(self.request('/api/health')[1],{'busy':False})

    def google_fixture(self):
        self.process.terminate();self.process.communicate(timeout=5)
        self.google_calls=[];self.google_reply={'error':'authorization_pending'}
        parent=self
        def post(request):
            form=urllib.parse.parse_qs(request.rfile.read(int(request.headers['Content-Length'])).decode())
            parent.google_calls.append((request.path,form))
            if request.path=='/device/code':
                status=200;reply={'device_code':'fixture-device','user_code':'TEST-CODE','verification_url':'https://www.google.com/device','expires_in':30,'interval':1}
            else:status=200 if 'access_token' in parent.google_reply else 428;reply=parent.google_reply
            data=json.dumps(reply).encode();request.send_response(status);request.send_header('Content-Length',str(len(data)));request.end_headers();request.wfile.write(data)
        self.fixture.server.RequestHandlerClass.do_POST=post
        self.start_server(PSCLOUD_TEST_GOOGLE_URL=self.fixture.env['PSCLOUD_URL'].replace('/backups',''))

    def test_google_device_approval_is_private_and_does_not_switch_webdav(self):
        self.google_fixture();before=self.cloud.read_bytes()
        status,result=self.request('/api/google/begin',{'client_id':'fixture.apps.googleusercontent.com','client_secret':'fixture-secret'})
        self.assertEqual(status,200);self.assertEqual(result['user_code'],'TEST-CODE')
        self.assertNotIn('device_code',result);self.assertNotIn('client_secret',result)
        self.assertEqual(self.request('/api/google/poll',{})[1]['pending'],True)
        self.assertEqual(len(self.google_calls),1) # interval respected; no immediate token call
        self.google_reply={'access_token':'fixture-access','refresh_token':'fixture-refresh','token_type':'Bearer','expires_in':3600,'scope':'https://www.googleapis.com/auth/drive.file'}
        time.sleep(1.1)
        self.assertTrue(self.request('/api/google/poll',{})[1]['authorized'])
        path=self.root/'google.conf';self.assertEqual(path.stat().st_mode&0o777,0o600)
        self.assertIn('fixture-refresh',path.read_text());self.assertNotIn('fixture-access',path.read_text())
        result=self.request('/api/google/status')[1]
        self.assertTrue(result['authorized']);self.assertFalse(result['transfers_available'])
        self.assertNotIn('fixture',json.dumps(result));self.assertEqual(self.cloud.read_bytes(),before)
        self.process.terminate();self.process.communicate(timeout=5);self.start_server()
        self.assertTrue(self.request('/api/google/status')[1]['authorized'])

    def test_google_denial_preserves_existing_authorization(self):
        self.google_fixture()
        path=self.root/'google.conf';path.write_text('CLIENT_ID=old-client\nCLIENT_SECRET=old-secret\nREFRESH_TOKEN=old-refresh\n');path.chmod(0o600)
        before=path.read_bytes()
        self.request('/api/google/begin',{'client_id':'new-client','client_secret':'new-secret'})
        self.google_reply={'error':'access_denied'};time.sleep(1.1)
        self.assertTrue(self.request('/api/google/poll',{})[1]['expired'])
        self.assertEqual(path.read_bytes(),before)

    def test_google_rejects_malformed_and_missing_scope_tokens(self):
        self.google_fixture();self.request('/api/google/begin',{'client_id':'fixture-client'})
        self.google_reply={'access_token':'fixture-access','refresh_token':'fixture-refresh','token_type':'Bearer','expires_in':3600,'scope':'https://www.googleapis.com/auth/drive'}
        time.sleep(1.1)
        self.assertEqual(self.request('/api/google/poll',{})[0],502)
        self.assertFalse((self.root/'google.conf').exists())
        self.assertFalse(self.request('/api/google/status')[1]['authorized'])

    def test_google_authentication_required_and_private_config_rejected(self):
        self.assertEqual(self.request('/api/google/status',token=False)[0],401)
        path=self.root/'google.conf';path.write_text('CLIENT_ID=fixture\nCLIENT_SECRET=\nREFRESH_TOKEN=fixture\n');path.chmod(0o644)
        self.process.terminate();self.process.communicate(timeout=5);self.start_server()
        self.assertFalse(self.request('/api/google/status')[1]['configured'])

    def test_preferences_persist_and_unavailable_automation_rejected(self):
        self.assertTrue(self.request('/api/preferences')[1]['auto_upload'])
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'0'})[0],200)
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'yes','activity_refresh':'0'})[0],400)
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'1','activity_refresh':'1','game_close_backup':'1'})[0],400)
        self.process.terminate();self.process.communicate(timeout=5);self.start_server()
        prefs=self.request('/api/preferences')[1]
        self.assertFalse(prefs['auto_upload']);self.assertFalse(prefs['activity_refresh'])
        self.assertFalse(prefs['game_close_available']);self.assertTrue(prefs['sharing_available'])

    def test_sharing_routes_require_closure_and_staged_proof(self):
        self.assertEqual(self.request('/api/share-export',self.selected())[0],400)
        self.assertEqual(self.request('/api/share-restore',self.selected(closed='yes',confirm='yes',file='portable-missing.zip',sha256='0'*64))[0],400)
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_generic_dashboard_import_check_and_download(self):
        import test_sharing
        import test_portable
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'generic';slot='save1'
        test_portable.PortableTests.sfo(self,fixture/slot,slot,b'\x44'*8)
        (self.image.parent/('sdimg_'+slot)).write_bytes(self.original)
        metadata=self.root/'appmeta/PPSA02433';metadata.mkdir(parents=True)
        (metadata/'param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture))
        data=test_sharing.portable(slots=(slot,),title='PPSA02433')
        c=http.client.HTTPConnection('127.0.0.1',self.port,timeout=15)
        c.request('POST','/api/share-import?'+urllib.parse.urlencode(self.selected()),data,{'Cookie':self.cookie,'X-PSCloud-Request':'1','Content-Type':'application/zip'})
        response=c.getresponse();result=json.loads(response.read());self.assertEqual(response.status,200,result);c.close()
        context=self.selected(closed='yes',file=result['file'],sha256=result['sha256'])
        self.assertEqual(self.request('/api/share-restore',dict(context,confirm='yes'))[0],400)
        status,check=self.request('/api/share-check',context);self.assertEqual(status,200,check)
        self.assertEqual(self.image.read_bytes(),self.original)
        self.assertEqual((fixture/slot/'folder/progress.bin').read_bytes(),b'shared')
        c=http.client.HTTPConnection('127.0.0.1',self.port,timeout=15)
        c.request('GET','/api/share-download?'+urllib.parse.urlencode({'file':result['file']}),headers={'Cookie':self.cookie})
        response=c.getresponse();self.assertEqual(response.status,200);self.assertEqual(response.read(),data);c.close()

    def test_local_only_backup_then_explicit_upload(self):
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1'})[0],200)
        chosen=self.selected(closed='yes')
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        self.assertEqual(self.fixture.puts,[])
        queue=self.request('/api/queue')[1];self.assertEqual(queue['count'],1)
        self.assertEqual(self.request('/api/sync-one',{'file':queue['items'][0]['file']})[0],200)
        self.assertEqual(self.request('/api/queue')[1]['count'],0)

    def test_whole_game_local_only_and_preference_authentication(self):
        self.assertEqual(self.request('/api/preferences',token=False)[0],401)
        # Rejected POST bodies are deliberately not consumed; inspect the status
        # without expecting a graceful body read after the socket closes.
        c=http.client.HTTPConnection('127.0.0.1',self.port,timeout=10)
        c.request('POST','/api/preferences','auto_upload=0&activity_refresh=1')
        self.assertEqual(c.getresponse().status,401);c.close()
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1'})[0],200)
        self.image.with_name('sdimg_PlayerSaveProfileSaveData').write_bytes(self.original)
        chosen=self.selected(closed='yes');chosen['slot']='WholeGame'
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        self.assertEqual(self.fixture.puts,[])
        self.assertEqual(self.request('/api/queue')[1]['count'],1)

    def binary(self,path,data=None):
        c=http.client.HTTPConnection('127.0.0.1',self.port,timeout=15)
        c.request('POST' if data is not None else 'GET',path,data,{'Cookie':self.cookie,'X-PSCloud-Request':'1'})
        r=c.getresponse();status=r.status;headers=dict(r.getheaders());body=r.read();c.close();return status,headers,body

    def game_fixture(self):
        self.profile=self.image.with_name('sdimg_PlayerSaveProfileSaveData');self.profile.write_bytes(self.original)
        chosen=self.selected(closed='yes');chosen['slot']='WholeGame'
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        file=next((self.root/'spool').glob('*.sent'))
        return chosen,file.name[:-5],file.read_bytes()

    def import_fixture(self,chosen,data):
        return self.binary('/api/import?'+urllib.parse.urlencode(dict(chosen,policy='new')),data)

    def test_pc_duplicate_choices_keep_replace_or_new(self):
        chosen,file,data=self.game_fixture()
        def choice(policy):
            status,_,body=self.binary('/api/import?'+urllib.parse.urlencode(dict(chosen,policy=policy)),data)
            return status,json.loads(body)
        status,check=choice('check');self.assertEqual(status,200);self.assertTrue(check['duplicate']);self.assertEqual(check['file'],file)
        self.assertEqual(self.request('/api/queue')[1]['count'],0)
        self.assertEqual(choice('skip')[0],200);self.assertEqual(self.request('/api/queue')[1]['count'],0)
        self.assertEqual(self.binary('/api/import?'+urllib.parse.urlencode(chosen),data)[0],409)
        self.assertEqual(choice('replace')[0],200);self.assertEqual(self.request('/api/queue')[1]['count'],1)
        self.fixture.puts.clear();self.assertEqual(self.request('/api/sync-one',{'file':file})[0],200)
        prefix='/backups/Crash%20Bandicoot%204%20-%20PPSA02433/User-1eb70483/WholeGame/'
        self.assertIn(prefix+file,self.fixture.puts);self.assertFalse((self.root/'spool'/(file+'.replace')).exists())
        self.assertEqual(len(list((self.root/'spool').glob('*.sent'))),1)
        self.assertEqual(choice('new')[0],200);self.assertEqual(self.request('/api/queue')[1]['count'],1)
        self.assertEqual(len(list((self.root/'spool').glob('*.identity'))),2)

    def test_duplicate_check_detects_cloud_copy_without_local_archive(self):
        chosen,file,data=self.game_fixture()
        for p in (self.root/'spool').glob('*.sent'):p.unlink()
        status,_,body=self.binary('/api/import?'+urllib.parse.urlencode(dict(chosen,policy='check')),data)
        self.assertEqual(status,200);self.assertTrue(json.loads(body)['duplicate'])
        self.assertEqual(self.binary('/api/import?'+urllib.parse.urlencode(dict(chosen,policy='replace')),data)[0],200)
        self.fixture.puts.clear();self.assertEqual(self.request('/api/sync',{})[0],200)
        self.assertTrue(self.fixture.puts);self.assertEqual(len(list((self.root/'spool').glob('*.sent'))),1)

    def test_pc_download_import_queue_and_individual_upload(self):
        chosen,file,data=self.game_fixture()
        status,headers,download=self.binary('/api/download-pc?'+urllib.parse.urlencode(dict(chosen,file=file)))
        self.assertEqual(status,200);self.assertIn('attachment',headers['Content-Disposition']);self.assertEqual(download,data)
        self.assertEqual(self.import_fixture(chosen,data)[0],200)
        self.assertEqual(self.import_fixture(chosen,data)[0],200)
        queue=self.request('/api/queue')[1];self.assertEqual(queue['count'],2)
        selected=queue['items'][0]['file']
        self.assertEqual(self.binary('/api/queue-download?'+urllib.parse.urlencode({'file':selected}))[2],data)
        self.assertEqual(self.request('/api/sync-one',{'file':selected})[0],200)
        self.assertEqual(self.request('/api/queue')[1]['count'],1)
        self.assertEqual(self.request('/api/sync',{})[0],200)
        self.assertEqual(self.request('/api/queue')[1]['count'],0)
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_pc_import_rejects_wrong_identity_compressed_and_corrupt_zip(self):
        chosen,_,data=self.game_fixture();wrong=dict(chosen,user='deadbeef')
        self.assertEqual(self.import_fixture(wrong,data)[0],400)
        corrupt=bytearray(data);corrupt[60]^=1
        self.assertEqual(self.import_fixture(chosen,corrupt)[0],400)
        b=io.BytesIO()
        with zipfile.ZipFile(io.BytesIO(data)) as source,zipfile.ZipFile(b,'w',compression=zipfile.ZIP_DEFLATED) as z:
            for name in source.namelist():z.writestr(name,source.read(name))
        self.assertEqual(self.import_fixture(chosen,b.getvalue())[0],400)
        self.assertEqual(self.request('/api/queue')[1]['count'],0)

    def test_pc_import_restore_pair_requires_confirmation_and_preserves_rollback(self):
        chosen,_,data=self.game_fixture();self.assertEqual(self.import_fixture(chosen,data)[0],200)
        item=self.request('/api/queue')[1]['items'][0]
        changed=b'\x02'+b'\0'*4095+b'changed'+b'\0'*(8192-4103)
        self.image.write_bytes(changed);self.profile.write_bytes(changed)
        request=dict(chosen,file=item['file'],confirm='no')
        self.assertEqual(self.request('/api/restore-local',request)[0],400)
        self.assertEqual(self.image.read_bytes(),changed)
        request['confirm']='yes'
        status,result=self.request('/api/restore-local',request);self.assertEqual(status,200,result)
        self.assertEqual(self.image.read_bytes(),self.original);self.assertEqual(self.profile.read_bytes(),self.original)
        rollback=next((self.root/'rollback').glob('whole-restore-*'))
        self.assertEqual((rollback/'before-0.img').read_bytes(),changed)
        self.assertEqual((rollback/'before-1.img').read_bytes(),changed)
        self.assertFalse((self.root/'.restore-active').exists())

    def test_whole_restore_wrong_keys_and_partial_commit_failure(self):
        chosen,_,data=self.game_fixture();self.assertEqual(self.import_fixture(chosen,data)[0],200)
        item=self.request('/api/queue')[1]['items'][0];request=dict(chosen,file=item['file'],confirm='yes')
        wrong=bytearray(self.original);wrong[0x800]=1;self.image.write_bytes(wrong)
        self.assertEqual(self.request('/api/restore-local',request)[0],500)
        self.assertEqual(self.image.read_bytes(),wrong)
        changed=bytearray(self.original);changed[4096]=1;self.image.write_bytes(changed);self.profile.write_bytes(changed)
        self.process.terminate();self.process.communicate(timeout=5)
        self.start_server(PSCLOUD_TEST_BUNDLE_COMMIT_FAIL='1')
        self.assertEqual(self.request('/api/restore-local',request)[0],500)
        self.assertEqual(self.image.read_bytes(),changed);self.assertEqual(self.profile.read_bytes(),changed)
        self.assertFalse((self.root/'.restore-active').exists())

    def recovery_roots(self,account=b'\x01'*8,wrong_account=False,wrong_title=False):
        source=self.fixture.root/'recovery-source';target=self.fixture.root/'recovery-target'
        for role,root in [('source',source),('target',target)]:
            for i,slot in enumerate([SLOT,'PlayerSaveProfileSaveData']):
                folder=root/str(i);(folder/'sce_sys').mkdir(parents=True)
                (folder/'ue4savegame.dpx.sav').write_bytes((b'archived-' if role=='source' else b'fresh-')+str(i).encode())
                title='PPSA99999' if wrong_title and role=='source' else 'PPSA02433'
                aid=b'\x02'*8 if wrong_account and role=='source' else account
                fields=[('TITLE_ID',0x0204,title.encode()+b'\0'),('SAVEDATA_DIRECTORY',0x0204,slot.encode()+b'\0'),('ACCOUNT_ID',0x0004,aid)]
                keys=b'';values=b'';entries=[]
                for key,fmt,value in fields:
                    entries.append(struct.pack('<HHIII',len(keys),fmt,len(value),len(value),len(values)))
                    keys+=key.encode()+b'\0';values+=value
                key_offset=20+16*len(fields);value_offset=key_offset+len(keys)
                blob=struct.pack('<IIIII',0x46535000,0x101,key_offset,value_offset,len(fields))+b''.join(entries)+keys+values
                (folder/'sce_sys/param.sfo').write_bytes(blob)
        self.process.terminate();self.process.communicate(timeout=5)
        self.start_server(PSCLOUD_TEST_SOURCE_ROOT=str(source),PSCLOUD_TEST_TARGET_ROOT=str(target))
        return source,target

    def test_recreated_save_recovery_preserves_new_keys_and_metadata(self):
        chosen,file,_=self.game_fixture();source,target=self.recovery_roots()
        fresh=bytearray(self.original);fresh[0x800:0x860]=b'N'*0x60
        self.image.write_bytes(fresh);self.profile.write_bytes(fresh)
        metadata=[(target/str(i)/'sce_sys/param.sfo').read_bytes() for i in range(2)]
        request=dict(chosen,file=file,confirm='yes')
        status,result=self.request('/api/restore',request);self.assertEqual(status,200,result)
        self.assertEqual(self.image.read_bytes()[0x800:0x860],b'N'*0x60)
        self.assertEqual(self.profile.read_bytes()[0x800:0x860],b'N'*0x60)
        for i in range(2):
            self.assertEqual((target/str(i)/'ue4savegame.dpx.sav').read_bytes(),b'archived-'+str(i).encode())
            self.assertEqual((target/str(i)/'sce_sys/param.sfo').read_bytes(),metadata[i])
        self.assertFalse((self.root/'.mount-active').exists());self.assertFalse((self.root/'.restore-active').exists())

    def test_recovery_wrong_account_rejects_before_any_live_replacement(self):
        chosen,file,_=self.game_fixture();self.recovery_roots(wrong_account=True)
        fresh=bytearray(self.original);fresh[0x800]=1;self.image.write_bytes(fresh);self.profile.write_bytes(fresh)
        self.assertEqual(self.request('/api/restore',dict(chosen,file=file,confirm='yes'))[0],500)
        self.assertEqual(self.image.read_bytes(),fresh);self.assertEqual(self.profile.read_bytes(),fresh)

    def test_recovery_staged_check_never_replaces_live_images(self):
        chosen,file,_=self.game_fixture();self.recovery_roots()
        fresh=bytearray(self.original);fresh[0x800]=1;self.image.write_bytes(fresh);self.profile.write_bytes(fresh)
        self.assertEqual(self.request('/api/restore-check',dict(chosen,file=file,confirm='yes'))[0],200)
        self.assertEqual(self.image.read_bytes(),fresh);self.assertEqual(self.profile.read_bytes(),fresh)

    def test_automatic_session_protects_apis_and_state_never_returns_password(self):
        status,page=self.request('/',token=False,raw=True);self.assertEqual(status,200);self.assertIn('PSCloud',page)
        self.assertEqual(self.request('/api/state',token=False)[0],401)
        status,state=self.request('/api/state');self.assertEqual(status,200)
        self.assertEqual(state['version'],(ROOT/'VERSION').read_text().strip())
        self.assertNotIn('password',state)

    def test_cross_site_mutation_is_rejected(self):
        connection=http.client.HTTPConnection('127.0.0.1',self.port,timeout=10)
        connection.request('POST','/api/stop','',{'Cookie':self.cookie,'X-PSCloud-Request':'1','Origin':'https://untrusted.example'})
        response=connection.getresponse();self.assertEqual(response.status,403);response.read();connection.close()
        self.assertIsNone(self.process.poll())

    def test_unrelated_host_cannot_start_an_automatic_session(self):
        connection=http.client.HTTPConnection('127.0.0.1',self.port,timeout=10)
        connection.request('GET','/',headers={'Host':'untrusted.example'})
        response=connection.getresponse();self.assertEqual(response.status,403)
        self.assertIsNone(response.getheader('Set-Cookie'));response.read();connection.close()

    def test_installed_game_name_unicode_and_icon(self):
        folder=self.root/'appmeta/PPSA02433';folder.mkdir(parents=True)
        (folder/'param.json').write_text(json.dumps({'localizedParameters':{'defaultLanguage':'en-US','en-US':{'titleName':'Crash Bandicoot 4: It\'s About Time™'}}}))
        png=b'\x89PNG\r\n\x1a\n'+b'fixture'
        (folder/'icon0.png').write_bytes(png)
        status,data=self.request('/api/games');self.assertEqual(status,200)
        self.assertEqual(data['games'][0]['name'],"Crash Bandicoot 4: It's About Time™")
        connection=http.client.HTTPConnection('127.0.0.1',self.port,timeout=10)
        connection.request('GET',data['games'][0]['icon'],headers={'Cookie':self.cookie})
        response=connection.getresponse();self.assertEqual(response.status,200);self.assertEqual(response.read(),png);connection.close()
        self.assertEqual(self.request('/api/icon?title=../private')[0],400)

    def test_cloud_login_checks_credentials_before_saving(self):
        original=self.cloud.read_text()
        self.assertEqual(self.request('/api/connect',dict(url=self.fixture.env['PSCLOUD_URL'],username='user',password='bad'))[0],502)
        self.assertEqual(self.cloud.read_text(),original)
        self.assertEqual(self.request('/api/connect',dict(url=self.fixture.env['PSCLOUD_URL'],username='user',password='pass'))[0],200)
        self.assertTrue(self.request('/api/state')[1]['connected'])

    def test_game_discovery_excludes_backup_images(self):
        self.image.with_name('sdimg_sce_bu_PlayerSaveSlot0Save').write_bytes(self.original)
        status,data=self.request('/api/games');self.assertEqual(status,200)
        self.assertEqual(len(data['games']),1);self.assertEqual(data['games'][0]['slot'],SLOT)

    def test_large_game_library_keeps_supported_saves_visible(self):
        another=self.home/'1eb70483/savedata_prospero/PPSA10595';another.mkdir()
        for i in range(300):(another/f'sdimg_replay{i}').write_bytes(b'file')
        status,data=self.request('/api/games');self.assertEqual(status,200)
        self.assertEqual(data['games'][0]['title'],'PPSA02433')
        self.assertTrue(data['games'][0]['supported']);self.assertTrue(data['truncated'])

    def test_stop_requires_pairing_and_shuts_down_cleanly(self):
        self.assertEqual(self.request('/api/stop',{},token=False)[0],401)
        self.assertIsNone(self.process.poll())
        self.assertEqual(self.request('/api/stop',{})[0],200)
        self.assertEqual(self.process.wait(timeout=5),0)

    def test_grouped_committed_cloud_listing_and_download(self):
        status,data=self.request('/api/backups?'+urllib.parse.urlencode(self.selected()))
        self.assertEqual(status,200);self.assertEqual(data['backups'][0]['sha256'],self.sha)
        self.assertEqual(data['backups'][0]['created'],1700000000)
        status,_=self.request('/api/download',self.selected(file=FILE));self.assertEqual(status,200)
        self.assertEqual((self.root/'downloads'/FILE).read_bytes(),self.archive)

    def test_backup_requires_closure_and_skips_duplicate(self):
        self.assertEqual(self.request('/api/backup',self.selected(closed='no'))[0],400)
        self.assertEqual(self.request('/api/backup',self.selected(closed='yes'))[0],200)
        self.assertEqual(self.request('/api/backup',self.selected(closed='yes'))[0],200)
        self.assertEqual(len(list((self.root/'spool').glob('*.sent'))),1)
        self.assertEqual(self.image.read_bytes(),self.original)
        self.assertTrue(any(k.startswith(PREFIX) and k.endswith('.zip') for k in self.fixture.objects))

    def test_whole_game_zip_contains_both_images_and_skips_unchanged(self):
        profile=self.image.with_name('sdimg_PlayerSaveProfileSaveData')
        profile.write_bytes(b'\x02'+b'P'*8191)
        chosen=self.selected(closed='yes');chosen['slot']='WholeGame'
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        archives=list((self.root/'spool').glob('*.sent'));self.assertEqual(len(archives),1)
        with zipfile.ZipFile(archives[0]) as z:
            self.assertIsNone(z.testzip())
            self.assertEqual(set(z.namelist()),{'sdimg_PlayerSaveSlot0Save','sdimg_PlayerSaveProfileSaveData','manifest.txt'})
            self.assertEqual(z.read('sdimg_PlayerSaveSlot0Save'),self.original)
            self.assertEqual(z.read('sdimg_PlayerSaveProfileSaveData'),profile.read_bytes())
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        self.assertEqual(len(list((self.root/'spool').glob('*.sent'))),1)
        self.assertEqual(self.image.read_bytes(),self.original)
        self.assertEqual(self.request('/api/restore',chosen)[0],400)
        chosen.pop('closed')
        status,listing=self.request('/api/backups?'+urllib.parse.urlencode(chosen))
        self.assertEqual(status,200);self.assertEqual(len(listing['backups']),1)
        self.assertGreater(listing['backups'][0]['created'],0)

    def test_whole_game_single_slot_valid_and_symlink_rejected(self):
        chosen=self.selected(closed='yes');chosen['slot']='WholeGame'
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        archive=next((self.root/'spool').glob('*.sent'))
        with zipfile.ZipFile(archive) as z:
            self.assertEqual(z.namelist(),['sdimg_'+SLOT,'manifest.txt'])
            self.assertIn(b'SLOTS=1\n',z.read('manifest.txt'))
        profile=self.image.with_name('sdimg_PlayerSaveProfileSaveData');profile.symlink_to(self.image)
        self.assertEqual(self.request('/api/backup',chosen)[0],500)
        self.assertFalse(list((self.root/'spool').glob('*.ready')))
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_whole_game_includes_every_crash_slot(self):
        self.image.with_name('sdimg_PlayerSaveProfileSaveData').write_bytes(self.original)
        self.image.with_name('sdimg_PlayerSaveSlot1Save').write_bytes(self.original)
        chosen=self.selected(closed='yes');chosen['slot']='WholeGame'
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        archive=next((self.root/'spool').glob('*.sent'));data=archive.read_bytes()
        with zipfile.ZipFile(archive) as z:
            self.assertEqual(z.namelist(),['sdimg_PlayerSaveProfileSaveData','sdimg_PlayerSaveSlot0Save','sdimg_PlayerSaveSlot1Save','manifest.txt'])
            self.assertIsNone(z.testzip());self.assertIn(b'SLOTS=3\n',z.read('manifest.txt'))
        self.assertEqual(self.import_fixture(chosen,data)[0],200)
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        self.assertEqual(len(list((self.root/'spool').glob('*.sent'))),1)
        self.assertEqual(len(list((self.root/'spool').glob('*.ready'))),1) # separate PC import still awaits explicit upload
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_general_game_many_slots_backup_import_and_dedup(self):
        folder=self.home/'1eb70483/savedata_prospero/PPSA10595';folder.mkdir()
        slots=[f'sdimg_replay{i:03}' for i in range(50)]
        for name in reversed(slots):(folder/name).write_bytes(self.original)
        chosen={'user':'1eb70483','title':'PPSA10595','slot':'WholeGame','closed':'yes'}
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        archive=next((self.root/'spool').glob('*.sent'));data=archive.read_bytes()
        self.assertTrue(archive.name.startswith('ps5-11.40-PPSA10595-'))
        with zipfile.ZipFile(archive) as z:
            self.assertEqual(z.namelist(),slots+['manifest.txt']);self.assertIsNone(z.testzip())
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        self.assertEqual(len(list((self.root/'spool').glob('*.sent'))),1)
        status,_,body=self.binary('/api/import?'+urllib.parse.urlencode(dict(chosen,policy='check')),data)
        self.assertEqual(status,200);self.assertTrue(json.loads(body)['duplicate'])
        self.assertEqual(self.import_fixture(chosen,data)[0],200)
        item=self.request('/api/queue')[1]['items'][0];self.assertEqual(item['title'],'PPSA10595')
        self.assertEqual(self.binary('/api/queue-download?file='+item['file'])[2],data)
        self.assertEqual(self.request('/api/sync-one',{'file':item['file']})[0],200)

    def test_general_game_changed_keys_refused_and_slot_inventory_guard(self):
        folder=self.home/'1eb70483/savedata_prospero/PPSA10595';folder.mkdir()
        image=folder/'sdimg_global1';image.write_bytes(self.original)
        chosen={'user':'1eb70483','title':'PPSA10595','slot':'WholeGame','closed':'yes','confirm':'yes'}
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        archive=next((self.root/'spool').glob('*.sent'));file=archive.name[:-5]
        changed=bytearray(self.original);changed[0x800]=99;image.write_bytes(changed)
        status,result=self.request('/api/restore',dict(chosen,file=file));self.assertEqual(status,500)
        self.assertIn('keys changed',result['message']);self.assertEqual(image.read_bytes(),changed)
        extra=folder/'sdimg_extra';extra.write_bytes(self.original)
        status,result=self.request('/api/restore',dict(chosen,file=file));self.assertEqual(status,500)
        self.assertIn('slots differ',result['message']);self.assertEqual(extra.read_bytes(),self.original)

    def test_general_game_slot_limit_refuses_incomplete_backup(self):
        folder=self.home/'1eb70483/savedata_prospero/PPSA10595';folder.mkdir()
        for i in range(129):(folder/f'sdimg_slot{i}').write_bytes(self.original)
        chosen={'user':'1eb70483','title':'PPSA10595','slot':'WholeGame','closed':'yes'}
        self.assertEqual(self.request('/api/backup',chosen)[0],500)
        self.assertFalse(list((self.root/'spool').glob('*.sent')))

    def sfo_fixture(self,folder,title,slot,account=b'\x01'*8):
        (folder/'sce_sys').mkdir(parents=True)
        fields=[('TITLE_ID',0x0204,title.encode()+b'\0'),('SAVEDATA_DIRECTORY',0x0204,slot.encode()+b'\0'),('ACCOUNT_ID',0x0004,account)]
        keys=b'';values=b'';entries=[]
        for key,fmt,value in fields:
            entries.append(struct.pack('<HHIII',len(keys),fmt,len(value),len(value),len(values)))
            keys+=key.encode()+b'\0';values+=value
        key_offset=20+16*len(fields);value_offset=key_offset+len(keys)
        (folder/'sce_sys/param.sfo').write_bytes(struct.pack('<IIIII',0x46535000,0x101,key_offset,value_offset,len(fields))+b''.join(entries)+keys+values)
        (folder/'arbitrary-game-file.dat').write_bytes(b'not a UE4 save')

    def test_general_three_slot_restore_and_rollback_without_ue4_assumption(self):
        folder=self.home/'1eb70483/savedata_prospero/PPSA10595';folder.mkdir()
        slots=['global1','progress','replay1']
        for slot in slots:(folder/('sdimg_'+slot)).write_bytes(self.original)
        chosen={'user':'1eb70483','title':'PPSA10595','slot':'WholeGame','closed':'yes','confirm':'yes'}
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        archive=next((self.root/'spool').glob('*.sent'));file=archive.name[:-5]
        source=self.fixture.root/'general-source';target=self.fixture.root/'general-target'
        for i,slot in enumerate(slots):
            self.sfo_fixture(source/str(i),'PPSA10595',slot)
            self.sfo_fixture(target/str(i),'PPSA10595',slot)
        changed=bytearray(self.original);changed[4096]=7
        for slot in slots:(folder/('sdimg_'+slot)).write_bytes(changed)
        self.process.terminate();self.process.communicate(timeout=5)
        self.start_server(PSCLOUD_TEST_SOURCE_ROOT=str(source),PSCLOUD_TEST_TARGET_ROOT=str(target),PSCLOUD_TEST_BUNDLE_COMMIT_FAIL='1')
        request=dict(chosen,file=file)
        self.assertEqual(self.request('/api/restore',request)[0],500)
        for slot in slots:self.assertEqual((folder/('sdimg_'+slot)).read_bytes(),changed)
        self.process.terminate();self.process.communicate(timeout=5)
        self.start_server(PSCLOUD_TEST_SOURCE_ROOT=str(source),PSCLOUD_TEST_TARGET_ROOT=str(target))
        status,result=self.request('/api/restore',request);self.assertEqual(status,200,result)
        for slot in slots:self.assertEqual((folder/('sdimg_'+slot)).read_bytes(),self.original)
        for i in range(3):self.assertEqual((target/str(i)/'arbitrary-game-file.dat').read_bytes(),b'not a UE4 save')
        self.assertFalse((self.root/'.restore-active').exists());self.assertFalse((self.root/'.mount-active').exists())

    def test_general_archive_rejects_wrong_game_and_traversal(self):
        folder=self.home/'1eb70483/savedata_prospero/PPSA10595';folder.mkdir()
        (folder/'sdimg_global1').write_bytes(self.original)
        chosen={'user':'1eb70483','title':'PPSA10595','slot':'WholeGame','closed':'yes'}
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        data=next((self.root/'spool').glob('*.sent')).read_bytes()
        wrong=dict(chosen,title='PPSA99999');self.assertEqual(self.import_fixture(wrong,data)[0],400)
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            manifest=z.read('manifest.txt').replace(b'sdimg_global1=',b'sdimg_../bad=')
            b=io.BytesIO()
            with zipfile.ZipFile(b,'w') as out:
                out.writestr('sdimg_../bad',self.original);out.writestr('manifest.txt',manifest)
        self.assertEqual(self.import_fixture(chosen,b.getvalue())[0],400)

    def test_deleted_cloud_game_copy_reuploads_same_archive_without_new_version(self):
        self.image.with_name('sdimg_PlayerSaveProfileSaveData').write_bytes(self.original)
        chosen=self.selected(closed='yes');chosen['slot']='WholeGame'
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        archive=next((self.root/'spool').glob('*.sent'));name=archive.name[:-5]
        self.fixture.puts.clear()
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        self.assertEqual(self.fixture.puts,[])
        prefix='/backups/Crash%20Bandicoot%204%20-%20PPSA02433/User-1eb70483/WholeGame/'
        del self.fixture.objects[prefix+name]
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        self.assertIn(prefix+name,self.fixture.objects)
        self.assertEqual(len(list((self.root/'spool').glob('*.sent'))),1)
        self.fixture.objects.pop(prefix+'.pscloud/'+name+'.identity')
        self.assertEqual(self.request('/api/backup',chosen)[0],200)
        self.assertIn(prefix+'.pscloud/'+name+'.identity',self.fixture.objects)
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_cloud_error_during_unchanged_backup_retains_retry_not_false_success(self):
        self.assertEqual(self.request('/api/backup',self.selected(closed='yes'))[0],200)
        self.fixture.status=503
        self.assertEqual(self.request('/api/backup',self.selected(closed='yes'))[0],502)
        self.assertEqual(len(list((self.root/'spool').glob('*.ready'))),1)
        self.fixture.status=201
        self.fixture.puts.clear()
        self.assertEqual(self.request('/api/sync',{})[0],200)
        self.assertEqual(self.fixture.puts,[])

    def test_restore_rejects_unconfirmed_and_wrong_identity(self):
        self.assertEqual(self.request('/api/restore',self.selected(file=FILE,closed='no',confirm='yes'))[0],400)
        self.assertEqual(self.image.read_bytes(),self.original)
        self.fixture.objects[PREFIX+'.pscloud/'+FILE+'.identity']=self.fixture.objects[PREFIX+'.pscloud/'+FILE+'.identity'].replace(SLOT.encode(),b'PlayerSaveProfileSaveData')
        self.assertEqual(self.request('/api/restore',self.selected(file=FILE,closed='yes',confirm='yes'))[0],400)
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_managed_restore_preserves_encrypted_rollback_and_metadata(self):
        status,response=self.request('/api/restore',self.selected(file=FILE,closed='yes',confirm='yes'))
        self.assertEqual(status,200,response)
        self.assertEqual((self.payload/'ue4savegame.dpx.sav').read_bytes(),b'cloud progress')
        self.assertEqual((self.payload/'sce_sys/param.sfo').read_bytes(),b'original metadata')
        self.assertEqual(next((self.root/'rollback').glob('*.img')).read_bytes(),self.original)
        self.assertFalse((self.root/'.mount-active').exists())

    def test_body_injection_and_traversal_are_rejected(self):
        self.assertEqual(self.request('/api/backup',self.selected(closed='yes',dummy='x'))[0],200)
        bad=self.selected(closed='yes');bad['user']='../private'
        self.assertEqual(self.request('/api/backup',bad)[0],400)
        self.assertEqual(self.request('/api/connect',dict(url=self.fixture.env['PSCLOUD_URL'],username='user\nURL=https://other',password='pass'))[0],400)
