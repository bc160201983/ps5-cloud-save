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
import test_worker

ROOT=Path(__file__).resolve().parents[1]
FILE='ps5-11.40-PPSA02433-'+'b'*32+'.zip'
SLOT='PlayerSaveSlot0Save'
PREFIX='/backups/Crash%20Bandicoot%204%20-%20PPSA02433/User-1eb70483/'+SLOT+'/'

class DashboardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['python3',str(ROOT/'tools/embed_ui.py'),str(ROOT/'ps5/ui.html'),
                        str(ROOT/'ps5/ui.h'),str(ROOT/'VERSION')],check=True)
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                        '-DPSCLOUD_HOST_TEST','-DPSCLOUD_BACKUP_EMBEDDED','-DPSCLOUD_DOWNLOAD_EMBEDDED','-DPSCLOUD_EMBEDDED',
                        *[str(ROOT/p) for p in ['ps5/dashboard.c','ps5/backup.c','ps5/download.c','src/worker.c']],
                        *[str(ROOT/'ps5/common'/n) for n in ['managed.c','mount.c','restore.c','zip.c','log.c','cloud.c','snapshot.c','appmeta.c','bundle.c']],
                        '-o',str(ROOT/'dashboard-host'),'-lcurl','-lcrypto'],check=True)

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
        self.process=subprocess.Popen([str(ROOT/'dashboard-host'),str(self.root),str(self.home),'0'],
                                      env=dict(os.environ,PSCLOUD_TEST_PAYLOAD=str(self.payload)),
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

    def test_whole_game_missing_or_symlinked_profile_is_not_published(self):
        chosen=self.selected(closed='yes');chosen['slot']='WholeGame'
        self.assertEqual(self.request('/api/backup',chosen)[0],500)
        profile=self.image.with_name('sdimg_PlayerSaveProfileSaveData');profile.symlink_to(self.image)
        self.assertEqual(self.request('/api/backup',chosen)[0],500)
        self.assertFalse(list((self.root/'spool').glob('*.ready')))
        self.assertEqual(self.image.read_bytes(),self.original)

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
