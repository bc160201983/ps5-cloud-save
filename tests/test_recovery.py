import hashlib
import io
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zipfile
import test_worker

ROOT=Path(__file__).resolve().parents[1]
NAME='ps5-11.40-PPSA02433-'+'a'*32+'.zip'
SAVE='ue4savegame.dpx.sav'

def archive(content=b'cloud progress',name=SAVE,compression=zipfile.ZIP_STORED):
    buffer=io.BytesIO()
    with zipfile.ZipFile(buffer,'w',compression=compression) as z:
        z.writestr(name,content)
    return buffer.getvalue()

class RecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        common=[str(ROOT/'ps5/common/restore.c'),str(ROOT/'ps5/common/log.c')]
        for name,extra,libs in [('download',['cloud.c'],['-lcurl','-lcrypto']),
                                ('restore',['zip.c'],['-lcrypto'])]:
            subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                            '-DPSCLOUD_HOST_TEST',str(ROOT/f'ps5/{name}.c'),*common,
                            *[str(ROOT/'ps5/common'/x) for x in extra],'-o',
                            str(ROOT/f'{name}-host'),*libs],check=True)

    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.root=Path(self.tmp.name)
        self.downloads=self.root/'downloads';self.downloads.mkdir()
        self.target=self.root/'mounted';self.target.mkdir()
        self.rollback=self.root/'rollback';self.rollback.mkdir()
        (self.target/SAVE).write_bytes(b'original progress')
        (self.target/'sce_sys').mkdir()
        (self.target/'sce_sys/param.sfo').write_bytes(b'console metadata')
        self.config=self.root/'restore.conf';self.log=self.root/'log'
        self.install_archive(archive())

    def tearDown(self):self.tmp.cleanup()

    def install_archive(self,data):
        (self.downloads/NAME).write_bytes(data)
        self.selection=f'BACKUP={NAME}\nSHA256={hashlib.sha256(data).hexdigest()}\nTITLE=PPSA02433\n'
        self.config.write_text(self.selection+f'TARGET={self.target}\nCONFIRM_GAME_CLOSED=yes\nCONFIRM_RESTORE=yes\n')

    def run_restore(self):
        return subprocess.run([str(ROOT/'restore-host'),str(self.config),str(self.downloads),
                               str(self.rollback),str(self.log)],capture_output=True,text=True,timeout=10)

    def assert_unchanged(self):
        self.assertEqual((self.target/SAVE).read_bytes(),b'original progress')
        self.assertEqual((self.target/'sce_sys/param.sfo').read_bytes(),b'console metadata')

    def test_restore_retains_destination_backup_and_metadata(self):
        r=self.run_restore();self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertEqual((self.target/SAVE).read_bytes(),b'cloud progress')
        self.assertEqual((self.target/'sce_sys/param.sfo').read_bytes(),b'console metadata')
        backups=list(self.rollback.glob('*.zip'));self.assertEqual(len(backups),1)
        with zipfile.ZipFile(backups[0]) as z:
            self.assertEqual(z.read(SAVE),b'original progress');self.assertNotIn('sce_sys/param.sfo',z.namelist())
        self.assertFalse(list(self.target.glob('*.part')))

    def test_confirmation_required(self):
        self.config.write_text(self.config.read_text().replace('CONFIRM_RESTORE=yes','CONFIRM_RESTORE=no'))
        self.assertEqual(self.run_restore().returncode,2);self.assert_unchanged()

    def test_wrong_title_rejected(self):
        self.config.write_text(self.config.read_text().replace('TITLE=PPSA02433','TITLE=PPSA99999'))
        self.assertEqual(self.run_restore().returncode,2);self.assert_unchanged()

    def test_archive_hash_mismatch(self):
        (self.downloads/NAME).write_bytes(archive(b'tampered'))
        self.assertEqual(self.run_restore().returncode,1);self.assert_unchanged()

    def test_unsafe_unsupported_and_corrupt_archives_rejected(self):
        damaged=bytearray(archive());n=struct.unpack_from('<H',damaged,26)[0];damaged[30+n]^=1
        cases=[archive(name='../'+SAVE),archive(name='sce_sys/param.sfo'),
               archive(compression=zipfile.ZIP_DEFLATED),bytes(damaged),archive()[:-1]]
        multi=io.BytesIO()
        with zipfile.ZipFile(multi,'w') as z:z.writestr(SAVE,b'one');z.writestr('other',b'two')
        cases.append(multi.getvalue())
        for data in cases:
            with self.subTest(size=len(data)):
                self.install_archive(data)
                self.assertEqual(self.run_restore().returncode,1);self.assert_unchanged()
        self.assertFalse(list(self.rollback.iterdir()))

    def test_destination_symlink_rejected(self):
        (self.target/SAVE).unlink();private=self.root/'private';private.write_bytes(b'private')
        (self.target/SAVE).symlink_to(private)
        self.assertEqual(self.run_restore().returncode,1)
        self.assertEqual(private.read_bytes(),b'private');self.assertFalse(list(self.rollback.iterdir()))

    def test_target_ancestor_symlink_rejected(self):
        link=self.root/'link';link.symlink_to(self.target,target_is_directory=True)
        self.config.write_text(self.config.read_text().replace(str(self.target),str(link)))
        self.assertEqual(self.run_restore().returncode,1);self.assert_unchanged()

    def test_rollback_failure_does_not_replace_save(self):
        self.rollback.rmdir()
        self.assertEqual(self.run_restore().returncode,1);self.assert_unchanged()

    def test_archive_symlink_rejected(self):
        data=self.root/'archive';(self.downloads/NAME).rename(data);(self.downloads/NAME).symlink_to(data)
        self.assertEqual(self.run_restore().returncode,1);self.assert_unchanged()

    def test_download_roundtrip_and_rejections(self):
        fixture=test_worker.WorkerTest();fixture.setUp()
        try:
            cloud=self.root/'cloud.conf'
            cloud.write_text('URL='+fixture.env['PSCLOUD_URL']+'\nUSER=user\nPASSWORD=pass\nCA_BUNDLE='+str(fixture.cert)+'\nMODE=once\n')
            selection=self.root/'selection.conf';selection.write_text(self.selection)
            original=(self.downloads/NAME).read_bytes();(self.downloads/NAME).unlink()
            fixture.objects['/backups/'+NAME]=original
            def run():
                return subprocess.run([str(ROOT/'download-host'),str(cloud),str(selection),
                                       str(self.downloads),str(self.log)],capture_output=True,text=True,timeout=15)
            r=run();self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            self.assertEqual((self.downloads/NAME).read_bytes(),original)
            (self.downloads/NAME).unlink()
            fixture.objects['/backups/'+NAME]=archive(b'wrong hash')
            self.assertEqual(run().returncode,1);self.assertFalse(list(self.downloads.iterdir()))
            fixture.status=404
            self.assertEqual(run().returncode,1);self.assertFalse(list(self.downloads.iterdir()))
            fixture.status=201;cloud.write_text(cloud.read_text().replace('PASSWORD=pass','PASSWORD=bad'))
            self.assertEqual(run().returncode,1);self.assertFalse(list(self.downloads.iterdir()))
        finally:fixture.tearDown()
