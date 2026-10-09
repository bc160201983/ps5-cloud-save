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
                        *[str(ROOT/'ps5/common'/n) for n in ['managed.c','mount.c','restore.c','zip.c','log.c','cloud.c','snapshot.c','appmeta.c','bundle.c','savemeta.c']],
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

    def test_preferences_persist_and_unavailable_automation_rejected(self):
        self.assertTrue(self.request('/api/preferences')[1]['auto_upload'])
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'0','activity_refresh':'0'})[0],200)
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'yes','activity_refresh':'0'})[0],400)
        self.assertEqual(self.request('/api/preferences',{'auto_upload':'1','activity_refresh':'1','game_close_backup':'1'})[0],400)
        self.process.terminate();self.process.communicate(timeout=5);self.start_server()
        prefs=self.request('/api/preferences')[1]
        self.assertFalse(prefs['auto_upload']);self.assertFalse(prefs['activity_refresh'])
        self.assertFalse(prefs['game_close_available']);self.assertFalse(prefs['sharing_available'])

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
        chosen=self.selected(closed='yes',slot='WholeGame')
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

    def test_whole_game_missing_or_symlinked_profile_is_not_published(self):
        chosen=self.selected(closed='yes');chosen['slot']='WholeGame'
        self.assertEqual(self.request('/api/backup',chosen)[0],500)
        profile=self.image.with_name('sdimg_PlayerSaveProfileSaveData');profile.symlink_to(self.image)
        self.assertEqual(self.request('/api/backup',chosen)[0],500)
        self.assertFalse(list((self.root/'spool').glob('*.ready')))
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_whole_game_rejects_unvalidated_extra_slots(self):
        self.image.with_name('sdimg_PlayerSaveProfileSaveData').write_bytes(self.original)
        self.image.with_name('sdimg_PlayerSaveSlot1Save').write_bytes(self.original)
        chosen=self.selected(closed='yes');chosen['slot']='WholeGame'
        self.assertEqual(self.request('/api/backup',chosen)[0],500)
        self.assertEqual(self.image.read_bytes(),self.original)

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
