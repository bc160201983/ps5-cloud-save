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
                        *[str(ROOT/'ps5/common'/n) for n in ['managed.c','mount.c','restore.c','zip.c','log.c','cloud.c','snapshot.c','appmeta.c','bundle.c','savemeta.c','google.c','sharing.c','slotcreate.c']],
                        '-o',str(ROOT/'dashboard-host'),'-lcurl','-lcrypto','-lsqlite3','-pthread'],check=True)

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

    def selected(self,**extra):return dict(dict(user='1eb70483',title='PPSA02433',slot=SLOT),**extra)

    def test_users_and_save_status_are_read_only_and_cover_profiles_without_saves(self):
        empty=self.home/'1eb70485';empty.mkdir();(empty/'username.dat').write_bytes(b'Guest\0\0\0')
        (self.home/'1eb70483/username.dat').write_bytes(b'Main')
        (self.root/'appmeta/PPSA02433').mkdir(parents=True);(self.root/'appmeta/PPSA02433/param.json').write_text('{"contentVersion":"01.000.000"}')
        before=self.image.read_bytes()
        status,data=self.request('/api/users');self.assertEqual(status,200)
        self.assertEqual(sorted((u['user'],u['name']) for u in data['users']),[('1eb70483','Main'),('1eb70485','Guest')])
        status,data=self.request('/api/save-status?user=1eb70483&title=PPSA02433');self.assertEqual(status,200)
        self.assertEqual((data['installed'],data['mounted'],data['slots']),(True,False,['PlayerSaveSlot0Save']))
        status,data=self.request('/api/save-status?user=1eb70485&title=PPSA02433');self.assertEqual(status,200)
        self.assertEqual(data['slots'],[])
        status,data=self.request('/api/save-status?user=1eb70485&title=PPSA99999');self.assertFalse(data['installed'])
        for bad in ('user=../x&title=PPSA02433','user=1eb70483&title=CUSA00001','title=PPSA02433'):
            with self.subTest(bad=bad):self.assertEqual(self.request('/api/save-status?'+bad)[0],400)
        self.assertEqual(self.image.read_bytes(),before)

    def test_any_firmware_starts_untested_ones_are_flagged_and_always_use_compact(self):
        status,data=self.request('/api/state');self.assertEqual((data['firmware'],data['firmware_tested']),('11.40',True))
        self.assertFalse(self.request('/api/preferences')[1]['compact_required'])
        self.process.terminate();self.process.communicate(timeout=5)
        self.start_server(PSCLOUD_TEST_FIRMWARE='09600000')
        status,data=self.request('/api/state');self.assertEqual(status,200)
        self.assertEqual((data['firmware'],data['firmware_tested']),('9.60',False))
        status,data=self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1','compact_backups':'0'});self.assertEqual(status,200)
        self.assertEqual((data['compact_backups'],data['compact_required']),(True,True))

    def test_activity_log_waits_for_changes_and_answers_when_one_happens(self):
        import threading
        status,data=self.request('/api/log');self.assertEqual(status,200)
        size=data['size'];log=self.root/'pscloud.log'
        self.assertEqual(log.stat().st_size if log.exists() else 0,size)
        status,data=self.request('/api/log?wait=1&since=-1');self.assertEqual(status,200)  # no size known yet: immediate
        def append():
            time.sleep(2)
            with open(log,'a') as f:f.write('2026-10-11 10:00:00 EVENT Test line\n')
        threading.Thread(target=append).start()
        started=time.monotonic();status,data=self.request('/api/log?wait=1&since=%d'%size);took=time.monotonic()-started
        self.assertEqual(status,200);self.assertIn('Test line',data['log']);self.assertGreater(data['size'],size)
        self.assertGreater(took,1.5);self.assertLess(took,8)
        # Other requests are not blocked while a client waits.
        threading.Thread(target=lambda:self.request('/api/log?wait=1&since=%d'%data['size'])).start()
        time.sleep(0.5);started=time.monotonic();self.assertEqual(self.request('/api/state')[0],200);self.assertLess(time.monotonic()-started,3)

    def test_saves_kept_under_a_shared_title_id_show_the_installed_game(self):
        # Ghost of Tsushima (PPSA03210) keeps its saves under PPSA02225 via savedata.titleIdForSharing.
        meta=self.root/'appmeta/PPSA03210';meta.mkdir(parents=True)
        (meta/'param.json').write_text(json.dumps({'titleId':'PPSA03210','contentVersion':'02.024.000',
            'savedata':{'titleIdForSharing':'PPSA02433'},'localizedParameters':{'en-US':{'titleName':'Ghost of Tsushima'}}}))
        (meta/'icon0.png').write_bytes(b'\x89PNG\r\n\x1a\n'+b'\0'*16)
        status,data=self.request('/api/save-status?user=1eb70483&title=PPSA02433');self.assertEqual(status,200)
        self.assertEqual((data['name'],data['installed']),('Ghost of Tsushima',True))
        connection=http.client.HTTPConnection('127.0.0.1',self.port,timeout=10)
        connection.request('GET','/api/icon?title=PPSA02433',headers={'Cookie':self.cookie,'X-PSCloud-Request':'1'})
        response=connection.getresponse();self.assertEqual(response.status,200);self.assertTrue(response.read().startswith(b'\x89PNG'));connection.close()
        # A folder named by content ID on external storage is found by its titleId.
        external=self.root/'appmeta/external/UP0000-PPSA12345_00-EXAMPLE0000000';external.mkdir(parents=True)
        (external/'param.json').write_text(json.dumps({'titleId':'PPSA12345','localizedParameters':{'en-US':{'titleName':'External Game'}}}))
        status,data=self.request('/api/save-status?user=1eb70483&title=PPSA12345');self.assertEqual(data['name'],'External Game')
        # An installed game with its own ID still wins over a sharing match.
        own=self.root/'appmeta/PPSA02433';own.mkdir(parents=True)
        (own/'param.json').write_text(json.dumps({'titleId':'PPSA02433','contentVersion':'01.000.000','localizedParameters':{'en-US':{'titleName':'Crash Bandicoot 4'}}}))
        self.assertEqual(self.request('/api/save-status?user=1eb70483&title=PPSA02433')[1]['name'],'Crash Bandicoot 4')

    @staticmethod
    def save_sfo(fields):
        import struct
        keys=b'';values=b'';entries=b''
        for k,v in fields.items():
            raw=v.encode()+b'\0';entries+=struct.pack('<HHIII',len(keys),0x0204,len(raw),len(raw),len(values))
            keys+=k.encode()+b'\0';values+=raw
        key_offset=20+len(entries);value_offset=key_offset+len(keys)
        return b'\0PSF'+struct.pack('<IIII',0x101,key_offset,value_offset,len(fields))+entries+keys+values

    def test_uninstalled_game_is_named_by_the_title_its_saves_agree_on(self):
        def add(title,slots):
            live=self.home/'1eb70483/savedata_prospero'/title;live.mkdir(parents=True)
            meta=self.home/'1eb70483/savedata_prospero_meta/user'/title;meta.mkdir(parents=True)
            for slot,label in slots:
                (live/('sdimg_'+slot)).write_bytes(b'\0'*64)
                (meta/('sce_bu_'+slot+'.sfo')).write_bytes(self.save_sfo({'MAINTITLE':label,'SAVEDATA_DIRECTORY':slot}))
        add('PPSA03355',[('HadesSaveDirectory','Hades')])
        add('PPSA02100',[('Global','Settings'),('SlotsSlot1Data','Slot 1')])  # labels disagree: keep the ID
        add('PPSA02343',[('ProfileData','ProfileData')])  # label is only the slot name: keep the ID
        add('PPSA17525',[('SLOT0','Game Data')])  # generic label: keep the ID
        status,data=self.request('/api/games');self.assertEqual(status,200)
        names={g['title']:(g['name'],g['installed']) for g in data['games']}
        self.assertEqual(names['PPSA03355'],('Hades',False))
        self.assertEqual(names['PPSA02100'],('PPSA02100',False))
        self.assertEqual(names['PPSA02343'],('PPSA02343',False))
        self.assertEqual(names['PPSA17525'],('PPSA17525',False))
        self.assertEqual(self.request('/api/save-status?user=1eb70483&title=PPSA03355')[1]['name'],'Hades')

    def test_storage_locations_and_verified_usb_copies(self):
        self.process.terminate();self.process.communicate(timeout=5)
        usb=self.fixture.root/'usb';(usb/'usb0').mkdir(parents=True)
        self.start_server(PSCLOUD_TEST_USB_ROOT=str(usb))
        backup='portable-PPSA02433-'+'a'*32+'.zip';shared='portable-PPSA02433-'+'b'*32+'.zip'
        (self.root/'spool').mkdir(exist_ok=True);(self.root/'spool'/(backup+'.ready')).write_bytes(b'backup-bytes')
        (self.root/'share').mkdir(exist_ok=True);(self.root/'share'/shared).write_bytes(b'shared-bytes')
        status,data=self.request('/api/storage');self.assertEqual(status,200)
        places={p['key']:p for p in data['places']}
        self.assertTrue(places['backups']['path'].endswith('/spool'));self.assertEqual(places['backups']['files'],1)
        self.assertTrue(places['shared']['path'].endswith('/share'));self.assertTrue(places['log']['path'].endswith('pscloud.log'))
        self.assertEqual([d['id'] for d in self.request('/api/usb')[1]['drives']],['usb0'])
        status,data=self.request('/api/usb-copy',{'drive':'usb0','kind':'backup','file':backup});self.assertEqual(status,200,data)
        copied=usb/'usb0/PSCloud/Crash Bandicoot 4 (PPSA02433)'/backup
        self.assertEqual(copied.read_bytes(),b'backup-bytes');self.assertIn('Copied to USB',data['message'])
        self.assertIn('Already on USB',self.request('/api/usb-copy',{'drive':'usb0','kind':'backup','file':backup})[1]['message'])
        other='portable-PPSA01341-'+'c'*32+'.zip';(self.root/'share'/other).write_bytes(b'other-game')
        status,data=self.request('/api/usb-copy-all',{'drive':'usb0','title':'PPSA02433'});self.assertEqual(status,200,data)
        self.assertFalse(any(p.name==other for p in usb.rglob('*.zip')))  # only the chosen game
        self.assertEqual((usb/'usb0/PSCloud/Crash Bandicoot 4 (PPSA02433)'/shared).read_bytes(),b'shared-bytes')
        self.assertIn('1 copied, 1 already there',data['message'])
        files=self.request('/api/usb-files?drive=usb0')[1]['files'];self.assertEqual(len(files),2)
        self.assertEqual(self.request('/api/usb-copy-all',{'drive':'usb0','title':'bad'})[0],400)
        path=next(f['path'] for f in files if f['path'].endswith(shared))
        status,_,body=self.binary('/api/usb-file?'+urllib.parse.urlencode({'drive':'usb0','path':path}));self.assertEqual((status,body),(200,b'shared-bytes'))
        for query in ({'drive':'usb0','path':'PSCloud/../secret.zip'},{'drive':'usb0','path':'/etc/passwd'},{'drive':'usb9','path':path}):
            with self.subTest(query=query):self.assertNotEqual(self.binary('/api/usb-file?'+urllib.parse.urlencode(query))[0],200)
        self.assertEqual(self.request('/api/usb-copy',{'drive':'usb0','kind':'backup','file':'../x.zip'})[0],400)
        self.assertEqual(list((usb/'usb0').iterdir()),[usb/'usb0/PSCloud'])  # nothing written outside PSCloud
        # The same save exported again (new name, new export time) is recognised and not copied twice.
        import test_sharing
        first='portable-PPSA02433-'+'d'*32+'.zip';again='portable-PPSA02433-'+'e'*32+'.zip'
        (self.root/'share'/first).write_bytes(test_sharing.portable(title='PPSA02433',firmware='07000000'))
        (self.root/'share'/again).write_bytes(test_sharing.portable(title='PPSA02433',firmware='11400000'))
        self.assertIn('Copied to USB',self.request('/api/usb-copy',{'drive':'usb0','kind':'shared','file':first})[1]['message'])
        status,data=self.request('/api/usb-copy',{'drive':'usb0','kind':'shared','file':again})
        self.assertEqual(status,200);self.assertIn('Already on USB',data['message']);self.assertIn(first,data['message'])
        self.assertFalse(any(p.name==again for p in usb.rglob('*.zip')))
        # Delete stays inside PSCloud.
        target='PSCloud/Crash Bandicoot 4 (PPSA02433)/'+first
        self.assertEqual(self.request('/api/usb-delete',{'drive':'usb0','path':'PSCloud/../x.zip'})[0],400)
        self.assertEqual(self.request('/api/usb-delete',{'drive':'usb0','path':target})[0],200)
        self.assertFalse((usb/'usb0'/target).exists())

    def test_free_up_space_removes_only_what_was_chosen(self):
        r=self.root
        stage=r/'portable-stage-aaaa';(stage/'mount-0').mkdir(parents=True)
        (stage/'before-0.img').write_bytes(b'o'*10);(stage/'image-0.img').write_bytes(b'i'*20)
        (r/'staging/run1').mkdir(parents=True);(r/'staging/run1/copy.img').write_bytes(b's'*5)
        (r/'rollback/restore-PPSA02433-1eb70483-x').mkdir(parents=True)
        (r/'rollback/restore-PPSA02433-1eb70483-x/before-0.img').write_bytes(b'r'*9);(r/'rollback/pre-restore-a.img').write_bytes(b'p'*7)
        share=r/'share';share.mkdir(exist_ok=True)
        for i,c in enumerate('abc'):
            f=share/('portable-PPSA02433-'+c*32+'.zip');f.write_bytes(b'z'*(100+i));os.utime(f,(1000+i,1000+i))
        other=share/('portable-PPSA01341-'+'d'*32+'.zip');other.write_bytes(b'q'*50)
        status,data=self.request('/api/cleanup');self.assertEqual(status,200)
        self.assertEqual((data['leftovers']['bytes'],data['leftovers']['files']),(25,2))
        self.assertEqual((data['safety']['bytes'],data['safety']['files']),(26,3))
        self.assertEqual((data['shared']['bytes'],data['shared']['files']),(201,2))
        places={p['key']:p for p in self.request('/api/storage')[1]['places']}
        self.assertEqual((places['staging']['bytes'],places['rollback']['bytes']),(25,26))
        self.assertEqual(self.request('/api/cleanup',{})[0],400)
        status,data=self.request('/api/cleanup',{'leftovers':'1','shared':'1'});self.assertEqual(status,200,data);self.assertIn('Freed',data['message'])
        self.assertEqual(sorted(p.name for p in stage.iterdir()),['before-0.img'])  # the original stays
        self.assertFalse((r/'staging/run1').exists())
        self.assertEqual(sorted(p.name for p in share.glob('*.zip')),['portable-PPSA01341-'+'d'*32+'.zip','portable-PPSA02433-'+'c'*32+'.zip'])
        self.assertEqual((r/'rollback/pre-restore-a.img').read_bytes(),b'p'*7)
        (r/'.restore-active').write_text('interrupted')
        self.assertEqual(self.request('/api/cleanup',{'safety':'1'})[0],409)
        self.assertTrue(self.request('/api/cleanup')[1]['blocked'])
        (r/'.restore-active').unlink()
        status,data=self.request('/api/cleanup',{'safety':'1'});self.assertEqual(status,200,data)
        self.assertFalse(stage.exists());self.assertEqual(list((r/'rollback').iterdir()),[])
        self.assertEqual(self.image.read_bytes(),self.original)

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

    def test_preferences_persist_and_invalid_values_rejected(self):
        self.assertTrue(self.request('/api/preferences')[1]['auto_upload'])
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'0'})[0],200)
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'yes','activity_refresh':'0'})[0],400)
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'0','game_close_backup':'yes'})[0],400)
        self.process.terminate();self.process.communicate(timeout=5);self.start_server()
        prefs=self.request('/api/preferences')[1]
        self.assertFalse(prefs['auto_upload']);self.assertFalse(prefs['activity_refresh'])
        self.assertTrue(prefs['game_close_available']);self.assertFalse(prefs['game_close_backup']);self.assertTrue(prefs['sharing_available'])
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'0','game_close_backup':'1'})[0],200)
        self.process.terminate();self.process.communicate(timeout=5);self.start_server()
        self.assertTrue(self.request('/api/preferences')[1]['game_close_backup'])

    def test_sharing_routes_require_closure_and_staged_proof(self):
        self.assertEqual(self.request('/api/share-export',self.selected())[0],400)
        self.assertEqual(self.request('/api/share-restore',self.selected(closed='yes',confirm='yes',file='portable-missing.zip',sha256='0'*64))[0],400)
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_compact_backup_queue_dedup_upload_and_staged_cloud_restore(self):
        import test_portable
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'compact';test_portable.PortableTests.sfo(self,fixture/SLOT,SLOT,b'\x44'*8)
        metadata=self.root/'appmeta/PPSA02433';metadata.mkdir(parents=True);(metadata/'param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture))
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1','compact_backups':'1'})[0],200)
        selected=self.selected(slot='WholeGame',closed='yes')
        status,backup=self.request('/api/backup',selected);self.assertEqual(status,200,backup)
        queue=self.request('/api/queue')[1]['items'];self.assertEqual(len(queue),1);self.assertTrue(queue[0]['file'].startswith('portable-'))
        self.assertEqual(self.request('/api/backup',selected)[0],200);self.assertEqual(len(self.request('/api/queue')[1]['items']),1)
        status,result=self.request('/api/sync-one',{'file':queue[0]['file']});self.assertEqual(status,200,result)
        status,result=self.request('/api/restore-check',dict(selected,file=queue[0]['file'],confirm='yes'));self.assertEqual(status,200,result)
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_game_close_backup_only_when_enabled_and_after_save_changes(self):
        import test_portable
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'compact';test_portable.PortableTests.sfo(self,fixture/SLOT,SLOT,b'\x44'*8)
        metadata=self.root/'appmeta/PPSA02433';metadata.mkdir(parents=True);(metadata/'param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture),PSCLOUD_TEST_WATCH_SECONDS='1')
        def queue():
            # While the watcher's backup holds the operation lock the API answers "busy".
            for _ in range(60):
                data=self.request('/api/queue')[1]
                if 'items' in data:return data['items']
                time.sleep(0.5)
            self.fail('dashboard stayed busy')
        def wait_for(check,seconds):
            deadline=time.monotonic()+seconds
            while time.monotonic()<deadline:
                if check():return True
                time.sleep(0.5)
            return check()
        def touch(offset):t=time.time()+offset;os.utime(self.image,(t,t))
        status,prefs=self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1','compact_backups':'1'})
        self.assertEqual(status,200);self.assertTrue(prefs['game_close_available']);self.assertFalse(prefs['game_close_backup'])
        touch(100);time.sleep(4);self.assertEqual(queue(),[])
        status,prefs=self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1','compact_backups':'1','game_close_backup':'1'})
        self.assertTrue(prefs['game_close_backup']);self.assertIn('GAME_CLOSE_BACKUP=1',(self.root/'preferences.conf').read_text())
        time.sleep(3);self.assertEqual(queue(),[])  # enabling records a baseline; earlier changes are not backed up
        touch(200)
        self.assertTrue(wait_for(lambda:len(queue())==1,15),queue())
        self.assertTrue(queue()[0]['file'].startswith('portable-PPSA02433-'))
        self.assertEqual(self.image.read_bytes(),self.original)
        self.assertIn('Game closed: backing up',(self.root/'pscloud.log').read_text())

    def test_compact_backup_waits_for_a_running_upload_instead_of_failing(self):
        import fcntl,threading,test_portable
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'compact';test_portable.PortableTests.sfo(self,fixture/SLOT,SLOT,b'\x44'*8)
        metadata=self.root/'appmeta/PPSA02433';metadata.mkdir(parents=True);(metadata/'param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture))
        self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1','compact_backups':'1'})
        spool=self.root/'spool';spool.mkdir(exist_ok=True)
        lock=open(spool/'.worker.lock','a+');fcntl.flock(lock,fcntl.LOCK_EX)  # what a running upload holds
        threading.Timer(3,lambda:(fcntl.flock(lock,fcntl.LOCK_UN),lock.close())).start()
        started=time.monotonic();status,data=self.request('/api/backup',self.selected(slot='WholeGame',closed='yes'))
        self.assertEqual(status,200,data);self.assertGreater(time.monotonic()-started,2)
        self.assertIn('Waiting for the current upload to finish',(self.root/'pscloud.log').read_text())
        self.assertEqual(len(list(spool.glob('portable-PPSA02433-*.zip.ready'))),1)

    def test_save_game_to_usb_skips_an_unchanged_save_and_leaves_no_export_behind(self):
        import test_portable
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'compact';test_portable.PortableTests.sfo(self,fixture/SLOT,SLOT,b'\x44'*8)
        metadata=self.root/'appmeta/PPSA02433';metadata.mkdir(parents=True);(metadata/'param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        usb=self.fixture.root/'usb';(usb/'usb0').mkdir(parents=True)
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture),PSCLOUD_TEST_USB_ROOT=str(usb))
        form=self.selected(slot='WholeGame',drive='usb0')
        self.assertEqual(self.request('/api/usb-save-game',form)[0],400)  # game not confirmed closed
        status,data=self.request('/api/usb-save-game',dict(form,closed='yes'));self.assertEqual(status,200,data);self.assertIn('Saved to USB',data['message'])
        status,data=self.request('/api/usb-save-game',dict(form,closed='yes'));self.assertEqual(status,200,data);self.assertIn('Already on USB, unchanged',data['message'])
        self.assertEqual(len(list(usb.rglob('portable-PPSA02433-*.zip'))),1)
        self.assertEqual(list((self.root/'share').glob('portable-*.zip')),[])
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_slot_labels_and_pictures_come_from_the_consoles_save_info(self):
        meta=self.home/'1eb70483/savedata_prospero_meta/user/PPSA02433';meta.mkdir(parents=True)
        (meta/('sce_bu_'+SLOT+'.sfo')).write_bytes(self.save_sfo({'MAINTITLE':'Slot 1 [Auto] A Debt Repaid','SUBTITLE':'Chapter 3','DETAIL':'2h 14m','SAVEDATA_DIRECTORY':SLOT}))
        (meta/(SLOT+'_icon0.png')).write_bytes(b'\x89PNG\r\n\x1a\n'+b'\0'*16)
        slot=next(g for g in self.request('/api/games')[1]['games'] if g['slot']==SLOT)
        self.assertEqual((slot['label'],slot['sub'],slot['detail']),('Slot 1 [Auto] A Debt Repaid','Chapter 3','2h 14m'))
        query=urllib.parse.urlencode({'user':'1eb70483','title':'PPSA02433','slot':SLOT})
        self.assertEqual(self.binary('/api/slot-icon?'+query)[0],200)
        self.assertEqual(self.binary('/api/slot-icon?'+urllib.parse.urlencode({'user':'1eb70483','title':'PPSA02433','slot':'../x'}))[0],400)

    def test_per_game_settings_are_saved_and_default_to_on(self):
        self.assertEqual(self.request('/api/game-settings?title=PPSA02433')[1],{'title':'PPSA02433','close_backup':True,'backup_all':True,'auto_sync':False,'auto_apply':False})
        status,_=self.request('/api/game-settings',{'title':'PPSA02433','close_backup':'0','backup_all':'1'});self.assertEqual(status,200)
        self.assertEqual(self.request('/api/game-settings?title=PPSA02433')[1]['close_backup'],False)
        self.assertIn('PPSA02433 CLOSE=0 ALL=1',(self.root/'games.conf').read_text())
        self.assertEqual(self.request('/api/game-settings',{'title':'../x','close_backup':'0','backup_all':'1'})[0],400)

    def batch_fixture(self,**extra):
        import test_portable
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'compact';test_portable.PortableTests.sfo(self,fixture/SLOT,SLOT,b'\x44'*8)
        metadata=self.root/'appmeta/PPSA02433';metadata.mkdir(parents=True);(metadata/'param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture),**extra)
        self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1','compact_backups':'1'})

    def wait_batch(self):
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            data=self.request('/api/backup-all')[1]
            if not data['running'] and data['finished']:return data
            time.sleep(0.3)
        self.fail('Back up all games did not finish')

    def test_back_up_all_games_counts_new_unchanged_and_switched_off_games(self):
        self.batch_fixture()
        self.assertEqual(self.request('/api/backup-all-start',{'user':'1eb70483'})[0],200)
        data=self.wait_batch();self.assertEqual((data['total'],data['backed'],data['unchanged'],data['failed']),(1,1,0,0))
        ready=list((self.root/'spool').glob('*.ready'));self.assertEqual(len(ready),1)
        self.assertEqual(self.request('/api/backup-all-start',{})[0],200)
        data=self.wait_batch();self.assertEqual((data['backed'],data['unchanged']),(0,1))
        self.assertEqual(list((self.root/'spool').glob('*.ready')),ready)  # unchanged: not queued again
        self.request('/api/game-settings',{'title':'PPSA02433','close_backup':'1','backup_all':'0'})
        self.request('/api/backup-all-start',{});self.assertEqual(self.wait_batch()['skipped'],1)
        self.assertEqual(self.request('/api/backup-all-start',{'user':'../x'})[0],400)
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_nightly_schedule_runs_once_a_day(self):
        self.batch_fixture(PSCLOUD_TEST_SCHEDULE_NOW='1')
        self.assertEqual(self.request('/api/schedule',{'hour':'25'})[0],400)
        self.assertEqual(self.request('/api/schedule',{'hour':'3'})[0],200)
        self.assertIn('HOUR=3',(self.root/'schedule.conf').read_text())
        data=self.wait_batch();self.assertEqual(data['backed'],1);self.assertTrue(data['schedule_last'])
        time.sleep(2);self.assertEqual(self.request('/api/backup-all')[1]['started'],data['started'])  # not again today
        self.assertEqual(self.request('/api/schedule',{'hour':'-1'})[0],200)

    def test_game_close_backup_waits_while_the_game_is_still_running(self):
        import test_portable
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'compact';test_portable.PortableTests.sfo(self,fixture/SLOT,SLOT,b'D'*8)
        metadata=self.root/'appmeta/PPSA02433';metadata.mkdir(parents=True);(metadata/'param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        running=self.fixture.root/'running.txt';running.write_text('PPSA02433')
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture),PSCLOUD_TEST_WATCH_SECONDS='1',PSCLOUD_TEST_RUNNING_TITLES=str(running))
        def queue():
            for _ in range(60):
                data=self.request('/api/queue')[1]
                if 'items' in data:return data['items']
                time.sleep(0.5)
            self.fail('dashboard stayed busy')
        status,data=self.request('/api/save-status?user=1eb70483&title=PPSA02433');self.assertTrue(data['running'])
        status,data=self.request('/api/running');self.assertEqual((status,data['running'],data['title'],data['icon']),(200,True,'PPSA02433','/api/icon?title=PPSA02433'))
        self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1','compact_backups':'1','game_close_backup':'1'})
        time.sleep(3)
        # The game autosaves while running and its save is not mounted between saves.
        for offset in (100,200):
            t=time.time()+offset;os.utime(self.image,(t,t));time.sleep(5)
        self.assertEqual(queue(),[])
        self.assertIn('still running; backup waits',(self.root/'pscloud.log').read_text())
        running.write_text('')
        deadline=time.monotonic()+15
        while time.monotonic()<deadline and len(queue())!=1:time.sleep(0.5)
        self.assertEqual(len(queue()),1)
        self.assertFalse(self.request('/api/save-status?user=1eb70483&title=PPSA02433')[1]['running'])
        self.assertEqual(self.request('/api/running')[1],{'known':True,'running':False,'backup_on_close':True})

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

    def test_sync_with_a_linked_profile_from_another_console(self):
        import test_sharing,test_portable
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'generic';slot='save1'
        test_portable.PortableTests.sfo(self,fixture/slot,slot,b'\x44'*8)
        second=self.image.parent/('sdimg_'+slot);second.write_bytes(self.original)
        metadata=self.root/'appmeta/PPSA02433';metadata.mkdir(parents=True)
        (metadata/'param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture))
        status_url='/api/sync-status?user=1eb70483&title=PPSA02433'
        self.assertEqual(self.request(status_url)[1]['state'],'not_in_cloud')
        # The other console's profile uploaded a newer save of this game.
        old=time.time()-5000
        for f in (self.image,second):os.utime(f,(old,old))
        package=test_sharing.portable(slots=(slot,),title='PPSA02433');sha=hashlib.sha256(package).hexdigest()
        folder='/backups/Crash%20Bandicoot%204%20-%20PPSA02433/User-1eb70484/WholeGame/'
        def publish(name,created):
            self.fixture.objects[folder+name]=package
            self.fixture.objects[folder+'.pscloud/'+name+'.identity']=('USER_ID=1eb70484\nTITLE=PPSA02433\nSAVE_NAME=WholeGame\nSHA256='+sha+'\nCREATED_UNIX=%d\n'%created).encode()
        publish('portable-PPSA02433-'+'e'*32+'.zip',int(time.time()))
        self.assertEqual(self.request(status_url)[1]['state'],'not_in_cloud')  # not linked yet
        self.assertEqual(self.request('/api/links',{'user':'1eb70483','linked':'1eb70484'})[0],200)
        self.assertEqual(self.request('/api/links',{'user':'1eb70483','linked':'../x'})[0],400)
        data=self.request(status_url)[1];self.assertEqual((data['state'],data['cloud']['user'],data['links']),('cloud_newer','1eb70484',1))
        context=self.selected(slot='WholeGame')
        self.assertEqual(self.request('/api/sync-get',context)[0],400)  # game not confirmed closed
        status,got=self.request('/api/sync-get',dict(context,closed='yes'));self.assertEqual(status,200,got)
        self.assertEqual(self.image.read_bytes(),self.original)  # checked only, nothing replaced
        status,done=self.request('/api/share-restore',self.selected(closed='yes',confirm='yes',file=got['file'],sha256=got['sha256']));self.assertEqual(status,200,done)
        self.assertEqual(self.request(status_url)[1]['state'],'in_sync')
        # Both sides change after the sync: a conflict, not a silent overwrite.
        future=time.time()+5000
        for f in (self.image,second):os.utime(f,(future,future))
        publish('portable-PPSA02433-'+'f'*32+'.zip',int(time.time())+100)
        self.assertEqual(self.request(status_url)[1]['state'],'conflict')
        # Profiles are published for the other console to find.
        status,profiles=self.request('/api/cloud-profiles');self.assertEqual(status,200)
        self.assertIn('/backups/.pscloud-profiles/1eb70483.txt',self.fixture.objects)
        self.assertEqual(profiles['links'],{'1eb70483':'1eb70484'})

    def test_restore_creates_slots_this_profile_lacks_without_the_game(self):
        import test_sharing,test_portable,sqlite3
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'generic'
        test_portable.PortableTests.sfo(self,fixture/SLOT,SLOT,b'\x44'*8)
        (fixture/SLOT/'sce_sys/keystone').write_bytes(b'K'*96)
        # The profile has one slot of the game (made by the game earlier); the game is not installed.
        self.save_db(self.root,'1eb70483',[(SLOT,'2026-10-01T10:00:00.00Z')])
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture))
        package=test_sharing.portable(slots=(SLOT,'save2'),title='PPSA02433');sha=hashlib.sha256(package).hexdigest()
        name='portable-PPSA02433-'+'a'*32+'.zip';(self.root/'share').mkdir(exist_ok=True);(self.root/'share'/name).write_bytes(package)
        base=self.selected(slot='WholeGame',closed='yes',file=name,sha256=sha)
        status,refused=self.request('/api/share-check',base);self.assertEqual(status,400);self.assertIn('Install this game',refused['message'])
        options=dict(base,not_installed='yes',create_missing='yes')
        status,checked=self.request('/api/share-check',options);self.assertEqual(status,200,checked)
        created=self.image.parent/'sdimg_save2';self.assertFalse(created.exists())  # a check creates nothing
        status,done=self.request('/api/share-restore',dict(options,confirm='yes'));self.assertEqual(status,200,done)
        self.assertTrue(created.exists());self.assertEqual(created.stat().st_size,1024*1024)
        db=sqlite3.connect(str(self.root/'system/system_data/savedata_prospero/1eb70483/db/user/savedata.db'))
        row=db.execute("select sub_title,blocks,size_kib from savedata where title_id='PPSA02433' and dir_name='save2'").fetchone();db.close()
        self.assertEqual(row,('Added by PSCloud',16,1024))
        # The new slot carries the game's identity from the other slot, under its own name, and the shared files.
        self.assertIn(b'save2\0',(fixture/'save2/sce_sys/param.sfo').read_bytes())
        self.assertEqual((fixture/'save2/sce_sys/keystone').read_bytes(),b'K'*96)
        self.assertEqual((fixture/'save2/folder/progress.bin').read_bytes(),b'shared')
        # A second import finds the slot and just replaces it.
        status,again=self.request('/api/share-check',options);self.assertEqual(status,200,again)

    def test_creating_a_slot_needs_another_slot_of_the_game(self):
        import test_sharing,test_portable
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'generic';test_portable.PortableTests.sfo(self,fixture/SLOT,SLOT,b'\x44'*8)
        self.save_db(self.root,'1eb70483',[('other','2026-10-01T10:00:00.00Z')])  # no database row for the existing slot
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture))
        package=test_sharing.portable(slots=(SLOT,'save2'),title='PPSA02433');sha=hashlib.sha256(package).hexdigest()
        name='portable-PPSA02433-'+'b'*32+'.zip';(self.root/'share').mkdir(exist_ok=True);(self.root/'share'/name).write_bytes(package)
        options=self.selected(slot='WholeGame',closed='yes',file=name,sha256=sha,not_installed='yes',create_missing='yes')
        self.assertEqual(self.request('/api/share-check',options)[0],200)
        before=self.image.read_bytes()
        status,refused=self.request('/api/share-restore',dict(options,confirm='yes'));self.assertEqual(status,400)
        self.assertIn('start the game once',refused['message'])
        self.assertFalse((self.image.parent/'sdimg_save2').exists());self.assertEqual(self.image.read_bytes(),before)

    @staticmethod
    def sfo_bytes(fields):
        keys=b'';values=b'';entries=[]
        for key,fmt,value,size in fields:
            length=len(value)+1 if fmt==0x0204 else len(value)
            entries.append(struct.pack('<HHIII',len(keys),fmt,length,size,len(values)));keys+=key.encode()+b'\0';values+=value.ljust(size,b'\0')
        offset=20+16*len(fields);keys=keys.ljust((len(keys)+3)//4*4,b'\0')
        return struct.pack('<IIIII',0x46535000,0x101,offset,offset+len(keys),len(fields))+b''.join(entries)+keys+values

    def test_writes_a_save_for_a_game_this_profile_never_had(self):
        import test_sharing,test_portable,sqlite3
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'generic'
        # This profile only has a save of another game (PPSA02433), with its own account.
        test_portable.PortableTests.sfo(self,fixture/SLOT,SLOT,b'\x44'*8)
        self.save_db(self.root,'1eb70483',[(SLOT,'2026-10-01T10:00:00.00Z')])
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture))
        # A V2 package of PPSA09999 from the other PS5: game files plus the slot's sce_sys.
        params=bytearray(1024);params[4:8]=b'\x99\x99\x99\x99';params[8:40]=b'C'*32;params[0x2c:0x35]=b'PPSA09998'
        source_sfo=self.sfo_bytes([('ACCOUNT_ID',0x0004,b'\x77'*8,8),('MAINTITLE',0x0204,b'Never Played',128),('PARAMS',0x0004,bytes(params),1024),
            ('SAVEDATA_DIRECTORY',0x0204,b'slotA',32),('SAVEDATA_REVISION',0x0004,b'R'*16,16),('SUBTITLE',0x0204,b'Chapter 9',128),('TITLE_ID',0x0204,b'PPSA09999',12)])
        inner=test_sharing.zip_bytes([('folder/progress.bin',b'shared'),('settings.dat',b'config')])
        sys_zip=test_sharing.zip_bytes([('keystone',b'S'*96),('param.sfo',source_sfo)])
        manifest=('FORMAT=PSCLOUD_PORTABLE_GAME_V2\nTITLE=PPSA09999\nSOURCE_FIRMWARE=11400000\nGAME_VERSION=01.000.000\nSLOTS=1\n'
                  'SLOT=0:slotA:3145728:%s\nSYS=0:%s\n'%(hashlib.sha256(inner).hexdigest(),hashlib.sha256(sys_zip).hexdigest()))
        package=test_sharing.zip_bytes([('manifest.txt',manifest.encode()),('slot-0.zip',inner),('sys-0.zip',sys_zip)]);sha=hashlib.sha256(package).hexdigest()
        name='portable-PPSA09999-'+'c'*32+'.zip';(self.root/'share').mkdir(exist_ok=True);(self.root/'share'/name).write_bytes(package)
        options=dict(user='1eb70483',title='PPSA09999',slot='WholeGame',closed='yes',file=name,sha256=sha,not_installed='yes',create_missing='yes')
        status,checked=self.request('/api/share-check',options);self.assertEqual(status,200,checked)
        folder=self.home/'1eb70483/savedata_prospero/PPSA09999';self.assertFalse(folder.exists())  # a check writes nothing
        status,done=self.request('/api/share-restore',dict(options,confirm='yes'));self.assertEqual(status,200,done)
        self.assertEqual((folder/'sdimg_slotA').stat().st_size,3145728)
        db=sqlite3.connect(str(self.root/'system/system_data/savedata_prospero/1eb70483/db/user/savedata.db'))
        row=db.execute("select main_title,sub_title,blocks,size_kib,game_title_id from savedata where title_id='PPSA09999' and dir_name='slotA'").fetchone();db.close()
        self.assertEqual(row,('Never Played','Chapter 9',48,3072,'PPSA09998'))
        written=(fixture/'slotA/sce_sys/param.sfo').read_bytes()
        self.assertIn(b'\x44'*8,written);self.assertNotIn(b'\x77'*8,written)  # this profile's account, not the source's
        self.assertIn(b'C'*32,written);self.assertNotIn(b'\x99\x99\x99\x99',written)  # source origin value kept, profile number replaced
        self.assertEqual((fixture/'slotA/sce_sys/keystone').read_bytes(),b'S'*96)
        self.assertEqual((fixture/'slotA/folder/progress.bin').read_bytes(),b'shared')

    def test_export_carries_each_slots_metadata(self):
        import zipfile,io
        status,data=self.request('/api/share-export',self.selected(slot='WholeGame',closed='yes'))
        if status!=200:self.skipTest(data.get('message'))
        package=(self.root/'share'/data['file']).read_bytes();z=zipfile.ZipFile(io.BytesIO(package))
        manifest=z.read('manifest.txt').decode();self.assertTrue(manifest.startswith('FORMAT=PSCLOUD_PORTABLE_GAME_V2\n'),manifest)
        self.assertIn('sys-0.zip',z.namelist());self.assertIn('param.sfo',zipfile.ZipFile(io.BytesIO(z.read('sys-0.zip'))).namelist())

    def test_sync_with_another_ps5_on_the_local_network(self):
        import test_sharing,test_portable,socket
        def free_udp():
            u=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);u.bind(('127.0.0.1',0));port=u.getsockname()[1];u.close();return port
        udp_b=free_udp()
        # Console B: profile 1eb70484 with a newer backup of the game in its queue.
        other=Path(self.fixture.root)/'consoleB';(other/'home/1eb70484/savedata_prospero').mkdir(parents=True);(other/'root/spool').mkdir(parents=True)
        (other/'home/1eb70484/username.dat').write_bytes(b'bilal')
        package=test_sharing.portable(slots=('save1',),title='PPSA02433');sha=hashlib.sha256(package).hexdigest()
        name='portable-PPSA02433-'+'9'*32+'.zip'
        (other/'root/spool'/(name+'.sent')).write_bytes(package)
        (other/'root/spool'/(name+'.identity')).write_text('USER_ID=1eb70484\nTITLE=PPSA02433\nSAVE_NAME=WholeGame\nSHA256=%s\nCREATED_UNIX=%d\n'%(sha,int(time.time())))
        b=subprocess.Popen([str(ROOT/'dashboard-host'),str(other/'root'),str(other/'home'),'0'],env=dict(os.environ,PSCLOUD_TEST_UDP_PORT=str(udp_b)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        try:
            port_b=int(b.stdout.readline().strip().split('=',1)[1]);b.stdout.readline()
            # Console A (this test's dashboard) with the game's save and the share fixture.
            self.process.terminate();self.process.communicate(timeout=5)
            fixture=self.fixture.root/'generic';test_portable.PortableTests.sfo(self,fixture/'save1','save1',b'\x44'*8)
            second=self.image.parent/'sdimg_save1';second.write_bytes(self.original)
            metadata=self.root/'appmeta/PPSA02433';metadata.mkdir(parents=True);(metadata/'param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
            self.cloud.unlink()  # no cloud at all: network only
            self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture),PSCLOUD_TEST_DISCOVER_PORT=str(udp_b),PSCLOUD_TEST_UDP_PORT=str(free_udp()))
            old=time.time()-5000
            for f in (self.image,second):os.utime(f,(old,old))
            found=self.request('/api/peers/discover')[1]['found'];self.assertEqual([f['address'] for f in found],['127.0.0.1:%d'%port_b])
            self.assertEqual(self.request('/api/peers',{'peers':'not-an-ip'})[0],400)
            self.assertEqual(self.request('/api/peers',{'peers':'127.0.0.1:%d'%port_b})[0],200)
            peers=self.request('/api/peers')[1]['peers'];self.assertTrue(peers[0]['reachable']);self.assertEqual(peers[0]['info']['profiles'][0]['name'],'bilal')
            self.request('/api/links',{'user':'1eb70483','linked':'1eb70484'})
            data=self.request('/api/sync-status?user=1eb70483&title=PPSA02433')[1]
            self.assertEqual((data['state'],data['cloud']['source'],data['cloud']['name']),('cloud_newer','network','bilal'))
            status,got=self.request('/api/sync-get',self.selected(slot='WholeGame',closed='yes'));self.assertEqual(status,200,got)
            self.assertEqual(self.image.read_bytes(),self.original)
            status,done=self.request('/api/share-restore',self.selected(closed='yes',confirm='yes',file=got['file'],sha256=got['sha256']));self.assertEqual(status,200,done)
            self.assertEqual(self.request('/api/sync-status?user=1eb70483&title=PPSA02433')[1]['state'],'in_sync')
            # Played here afterwards: this PS5 is newer, never "in sync" while the other holds the older save.
            future=time.time()+5000
            for f in (self.image,second):os.utime(f,(future,future))
            data=self.request('/api/sync-status?user=1eb70483&title=PPSA02433')[1]
            self.assertEqual((data['state'],data['cloud']['firmware']),('local_newer',data['cloud']['firmware']))
            # Peer endpoints only serve backups, by valid name.
            c=http.client.HTTPConnection('127.0.0.1',port_b,timeout=10);c.request('GET','/api/peer/file?file=../../etc/passwd');self.assertEqual(c.getresponse().status,400);c.close()
            # The other PS5 switched off: say so, rather than "only on this PS5".
            b.terminate();b.communicate(timeout=5)
            self.assertEqual(self.request('/api/sync-status?user=1eb70483&title=PPSA02433')[1]['state'],'other_offline')
        finally:
            if b.poll() is None:b.terminate();b.communicate(timeout=5)

    def test_send_a_newer_save_to_the_other_ps5(self):
        import test_portable,socket
        def free_udp():
            u=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);u.bind(('127.0.0.1',0));port=u.getsockname()[1];u.close();return port
        # This PS5 (A): the test dashboard, with compact backups and the share fixture.
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'generic';test_portable.PortableTests.sfo(self,fixture/'save1','save1',b'\x44'*8)
        test_portable.PortableTests.sfo(self,fixture/SLOT,SLOT,b'\x44'*8)
        (self.image.parent/'sdimg_save1').write_bytes(self.original)
        metadata=self.root/'appmeta/PPSA02433';metadata.mkdir(parents=True);(metadata/'param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        self.cloud.unlink()
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture),PSCLOUD_TEST_UDP_PORT=str(free_udp()))
        self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1','compact_backups':'1'})
        # The other PS5 (B): profile 1eb70484 with an older save of the same game.
        other=Path(self.fixture.root)/'consoleB';live=other/'home/1eb70484/savedata_prospero/PPSA02433';live.mkdir(parents=True)
        (live/'sdimg_save1').write_bytes(self.original);old=time.time()-5000;os.utime(live/'sdimg_save1',(old,old))
        (other/'root/appmeta/PPSA02433').mkdir(parents=True);(other/'root/appmeta/PPSA02433/param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        (other/'root/peers.conf').write_text('127.0.0.1:%d\n'%self.port);(other/'root/links.conf').write_text('1eb70484 1eb70483\n')
        b=subprocess.Popen([str(ROOT/'dashboard-host'),str(other/'root'),str(other/'home'),'0'],env=dict(os.environ,PSCLOUD_TEST_UDP_PORT=str(free_udp()),PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        try:
            port_b=int(b.stdout.readline().strip().split('=',1)[1]);b.stdout.readline()
            context=self.selected(slot='WholeGame')
            self.assertEqual(self.request('/api/sync-push',dict(context,closed='yes'))[0],400)  # not set up yet
            self.request('/api/peers',{'peers':'127.0.0.1:%d'%port_b});self.request('/api/links',{'user':'1eb70483','linked':'1eb70484'})
            self.assertEqual(self.request('/api/sync-push',context)[0],400)  # game not confirmed closed
            # B does not allow remote replacing: the save is sent, checked and left ready there.
            status,data=self.request('/api/sync-push',dict(context,closed='yes',replace='1'));self.assertEqual(status,200,data);self.assertIn('waiting there',data['message'])
            self.assertFalse(list((other/'root/rollback').glob('restore-*')) if (other/'root/rollback').exists() else [])
            self.assertTrue(list((other/'root/sync').glob('1eb70484-PPSA02433.pending')))
            # Once B allows it, the same button replaces there, keeping a rollback copy.
            (other/'root/remote.conf').write_text('ALLOW_REPLACE=1\n')
            status,data=self.request('/api/sync-push',dict(context,closed='yes',replace='1'));self.assertEqual(status,200,data);self.assertIn('Replaced on the other PS5',data['message'])
            self.assertEqual(len(list((other/'root/rollback').glob('restore-PPSA02433-1eb70484-*'))),1)
            # A console that is not in B's list is refused.
            (other/'root/peers.conf').write_text('')
            self.assertEqual(self.request('/api/sync-push',dict(context,closed='yes'))[0],409)
        finally:
            b.terminate();b.communicate(timeout=5)

    @staticmethod
    def save_db(root,user,rows):
        import sqlite3
        folder=Path(root)/'system/system_data/savedata_prospero'/user/'db/user';folder.mkdir(parents=True,exist_ok=True)
        db=sqlite3.connect(str(folder/'savedata.db'))
        db.execute('create table if not exists savedata (id INTEGER PRIMARY KEY, title_id TEXT, dir_name TEXT, main_title TEXT, sub_title TEXT, detail TEXT, tmp_dir_name TEXT, is_broken INTEGER, user_param INTEGER, blocks INTEGER, free_blocks INTEGER, size_kib INTEGER, mtime TEXT, fake_broken INTEGER)')
        db.execute('delete from savedata')
        for slot,when in rows:db.execute('insert into savedata (title_id,dir_name,main_title,detail,mtime) values (?,?,?,?,?)',('PPSA02433',slot,'Label '+slot,'x'*3000,when))
        db.commit();db.close()

    def test_sync_compares_when_the_game_saved_each_slot(self):
        import test_portable,socket
        def free_udp():
            u=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);u.bind(('127.0.0.1',0));port=u.getsockname()[1];u.close();return port
        self.process.terminate();self.process.communicate(timeout=5)
        fixture=self.fixture.root/'generic'
        for slot in ('save1',SLOT):test_portable.PortableTests.sfo(self,fixture/slot,slot,b'\x44'*8)
        (self.image.parent/'sdimg_save1').write_bytes(self.original)
        for r in (self.root,):(Path(r)/'appmeta/PPSA02433').mkdir(parents=True);(Path(r)/'appmeta/PPSA02433/param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        # This PS5 saved both slots on 1 October (long rows test the overflow pages too).
        self.save_db(self.root,'1eb70483',[('save1','2026-10-01T10:00:00.00Z'),(SLOT,'2026-10-01T10:00:00.00Z')])
        self.cloud.unlink()
        self.start_server(PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture),PSCLOUD_TEST_UDP_PORT=str(free_udp()))
        self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1','compact_backups':'1'})
        # The other PS5 saved slot save1 later (5 October), the other slot at the same time.
        other=Path(self.fixture.root)/'consoleB';live=other/'home/1eb70484/savedata_prospero/PPSA02433';live.mkdir(parents=True)
        for slot in ('save1',SLOT):(live/('sdimg_'+slot)).write_bytes(self.original)
        (other/'root/appmeta/PPSA02433').mkdir(parents=True);(other/'root/appmeta/PPSA02433/param.json').write_text(json.dumps({'contentVersion':'01.000.002'}))
        self.save_db(other/'root','1eb70484',[('save1','2026-10-05T18:30:00.00Z'),(SLOT,'2026-10-01T10:00:00.00Z')])
        (other/'root/peers.conf').write_text('127.0.0.1:%d\n'%self.port);(other/'root/links.conf').write_text('1eb70484 1eb70483\n')
        (other/'root/preferences.conf').write_text('AUTO_UPLOAD=0\nACTIVITY_REFRESH=1\nLOCAL_KEEP_LATEST=0\nCOMPACT_BACKUPS=1\nGAME_CLOSE_BACKUP=0\n')
        b=subprocess.Popen([str(ROOT/'dashboard-host'),str(other/'root'),str(other/'home'),'0'],env=dict(os.environ,PSCLOUD_TEST_UDP_PORT=str(free_udp()),PSCLOUD_TEST_GENERIC_PAYLOADS=str(fixture)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        try:
            port_b=int(b.stdout.readline().strip().split('=',1)[1]);b.stdout.readline()
            self.request('/api/peers',{'peers':'127.0.0.1:%d'%port_b});self.request('/api/links',{'user':'1eb70483','linked':'1eb70484'})
            status_url='/api/sync-status?user=1eb70483&title=PPSA02433'
            data=self.request(status_url)[1];self.assertEqual(data['state'],'cloud_newer',data)
            slots=self.request('/api/sync-slots?user=1eb70483&title=PPSA02433')[1]
            there={x['slot']:x['mtime'] for x in slots['there']};here={x['slot']:x['mtime'] for x in slots['here']}
            self.assertGreater(there['save1'],here['save1']);self.assertEqual(there[SLOT],here[SLOT])
            self.assertEqual(next(x['label'] for x in slots['here'] if x['slot']=='save1'),'')  # no sfo copy here: label empty
            # Get the newer save: the other PS5 backs up its current save first, then it is checked here.
            status,got=self.request('/api/sync-get',self.selected(slot='WholeGame',closed='yes'));self.assertEqual(status,200,got)
            self.assertTrue(list((other/'root/spool').glob('portable-PPSA02433-*.identity')))
            status,done=self.request('/api/share-restore',self.selected(closed='yes',confirm='yes',file=got['file'],sha256=got['sha256']));self.assertEqual(status,200,done)
            self.assertEqual(self.request(status_url)[1]['state'],'in_sync')
            # The game saves here afterwards: this PS5 is newer.
            self.save_db(self.root,'1eb70483',[('save1','2026-10-20T09:00:00.00Z'),(SLOT,'2026-10-01T10:00:00.00Z')])
            self.assertEqual(self.request(status_url)[1]['state'],'local_newer')
            # And the other PS5 saves a different slot: both changed.
            self.save_db(other/'root','1eb70484',[('save1','2026-10-05T18:30:00.00Z'),(SLOT,'2026-10-21T09:00:00.00Z')])
            self.assertEqual(self.request(status_url)[1]['state'],'conflict')
        finally:
            if b.poll() is None:b.terminate();b.communicate(timeout=5)

    def test_per_game_sync_settings_round_trip(self):
        self.request('/api/game-settings',{'title':'PPSA02433','close_backup':'1','backup_all':'1','auto_sync':'1','auto_apply':'1'})
        data=self.request('/api/game-settings?title=PPSA02433')[1];self.assertEqual((data['auto_sync'],data['auto_apply']),(True,True))
        self.request('/api/game-settings',{'title':'PPSA02433','close_backup':'1','backup_all':'1','auto_sync':'0','auto_apply':'1'})
        data=self.request('/api/game-settings?title=PPSA02433')[1];self.assertEqual((data['auto_sync'],data['auto_apply']),(False,False))  # replace needs download

    def test_cloud_keeps_only_the_newest_versions_when_asked(self):
        folder='/backups/Crash%20Bandicoot%204%20-%20PPSA02433/User-1eb70483/WholeGame/'
        names=['portable-PPSA02433-'+c*32+'.zip' for c in 'abcd']
        for i,name in enumerate(names):
            self.fixture.objects[folder+name]=b'zip'
            self.fixture.objects[folder+'.pscloud/'+name+'.identity']=('USER_ID=1eb70483\nTITLE=PPSA02433\nSAVE_NAME=WholeGame\nSHA256='+'0'*64+'\nCREATED_UNIX=%d\n'%(1000+i)).encode()
        broken='portable-PPSA02433-'+'e'*32+'.zip';self.fixture.objects[folder+'.pscloud/'+broken+'.identity']=b'not an identity'
        data=self.request('/api/cloud-housekeeping')[1];self.assertEqual((data['configured'],data['keep']),(True,0))
        self.assertEqual(self.request('/api/cloud-prune',self.selected(slot='WholeGame'))[0],400)  # nothing chosen yet
        self.assertEqual(self.request('/api/cloud-keep',{'keep':'x'})[0],400)
        self.assertEqual(self.request('/api/cloud-keep',{'keep':'2'})[0],200)
        self.assertEqual(self.request('/api/cloud-housekeeping')[1]['keep'],2)
        status,data=self.request('/api/cloud-prune',self.selected(slot='WholeGame'));self.assertEqual(status,200,data);self.assertIn('Removed 2',data['message'])
        left=sorted(p.rsplit('/',1)[1] for p in self.fixture.objects if p.startswith(folder) and p.endswith('.zip'))
        self.assertEqual(left,names[2:])
        self.assertIn(folder+'.pscloud/'+broken+'.identity',self.fixture.objects)  # unreadable: never deleted
        self.assertIn('KEEP=2',(self.root/'cloud-keep.conf').read_text())

    def test_encrypted_upload_round_trip_and_wrong_password(self):
        self.assertFalse(self.request('/api/encryption')[1]['on'])
        self.assertEqual(self.request('/api/encryption',{'password':'short','again':'short'})[0],400)
        self.assertEqual(self.request('/api/encryption',{'password':'correct horse','again':'different'})[0],400)
        self.assertEqual(self.request('/api/encryption',{'password':'correct horse','again':'correct horse'})[0],200)
        self.assertTrue(self.request('/api/encryption')[1]['on'])
        self.assertNotIn('correct horse',json.dumps(self.request('/api/encryption')[1]))
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'1'})[0],200)
        self.assertEqual(self.request('/api/backup',self.selected(closed='yes'))[0],200)
        item=self.request('/api/queue')[1]['items'][0];plain=next((self.root/'spool').glob(item['file']+'.ready')).read_bytes()
        self.assertEqual(self.request('/api/sync-one',{'file':item['file']})[0],200)
        stored=next(v for k,v in self.fixture.objects.items() if k.endswith('/'+item['file']))
        self.assertTrue(stored.startswith(b'PSCENC1\0'));self.assertEqual(len(stored),len(plain)+56);self.assertNotIn(plain[:64],stored)
        self.assertEqual(list((self.root/'spool').glob('.*.enc')),[])  # no temporary copy left
        query='/api/download-pc?'+urllib.parse.urlencode(self.selected(file=item['file']))
        status,_,body=self.binary(query);self.assertEqual((status,body),(200,plain))
        self.assertEqual(self.request('/api/encryption',{'password':'wrong password','again':'wrong password'})[0],200)
        self.assertEqual(self.binary(query)[0],400)
        self.assertEqual(self.request('/api/encryption',{'password':''})[0],200)
        status,_,body=self.binary(query);self.assertEqual(status,400);self.assertIn(b'encrypted',body)
        # a plain upload made afterwards is plain again
        self.assertFalse((self.root/'encrypt.conf').exists())

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
