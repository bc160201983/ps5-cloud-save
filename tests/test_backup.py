import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]

class BackupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                        '-DPSCLOUD_HOST_TEST',str(ROOT/'ps5/backup.c'),
                        *[str(ROOT/'ps5/common'/n) for n in ['mount.c','restore.c','zip.c','log.c']],
                        '-o',str(ROOT/'backup-host'),'-lcrypto'],check=True)

    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        self.home=self.root/'home'
        source=self.home/'1eb70483/savedata_prospero/PPSA02433';source.mkdir(parents=True)
        self.image=source/'sdimg_PlayerSaveSlot0Save'
        self.original=b'\x02'+b'\0'*8191;self.image.write_bytes(self.original)
        self.spoolroot=self.root/'pscloud';self.spoolroot.mkdir()
        self.payload=self.root/'fixture';self.payload.mkdir()
        (self.payload/'ue4savegame.dpx.sav').write_bytes(b'progress')
        (self.payload/'sce_sys').mkdir();(self.payload/'sce_sys/param.sfo').write_bytes(b'metadata')
        self.config=self.root/'backup.conf'
        self.config.write_text('USER_ID=1eb70483\nTITLE=PPSA02433\nSAVE_NAME=PlayerSaveSlot0Save\nCONFIRM_GAME_CLOSED=yes\n')
        self.log=self.root/'log'

    def tearDown(self):self.tmp.cleanup()

    def run_backup(self,**flags):
        return subprocess.run([str(ROOT/'backup-host'),str(self.config),str(self.home),
                               str(self.spoolroot),str(self.log)],
                              env=dict(os.environ,PSCLOUD_TEST_PAYLOAD=str(self.payload),**flags),
                              capture_output=True,text=True,timeout=10)

    def test_complete_lifecycle_publishes_after_unmount_and_preserves_source(self):
        r=self.run_backup();self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertEqual(self.image.read_bytes(),self.original)
        files=list((self.spoolroot/'spool').glob('*.ready'));self.assertEqual(len(files),1)
        with zipfile.ZipFile(files[0]) as z:
            self.assertIsNone(z.testzip());self.assertEqual(z.namelist(),['ue4savegame.dpx.sav'])
        self.assertLess(r.stdout.index('Host simulated unmount'),r.stdout.index('Console backup ready'))
        self.assertFalse(list((self.spoolroot/'staging').iterdir()))
        self.assertFalse((self.spoolroot/'.mount-active').exists())
        meta=next((self.spoolroot/'spool').glob('*.identity')).read_text()
        self.assertIn('USER_ID=1eb70483',meta);self.assertIn('SAVE_NAME=PlayerSaveSlot0Save',meta)

    def test_confirmation_required_before_staging(self):
        self.config.write_text(self.config.read_text().replace('CLOSED=yes','CLOSED=no'))
        self.assertEqual(self.run_backup().returncode,2)
        self.assertFalse(list(self.spoolroot.iterdir()))

    def test_mount_failure_does_not_publish_or_change_original(self):
        self.assertEqual(self.run_backup(PSCLOUD_TEST_MOUNT_FAIL='1').returncode,1)
        self.assertFalse(list((self.spoolroot/'spool').glob('*.ready')))
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_unmount_failure_retains_image_without_publication(self):
        r=self.run_backup(PSCLOUD_TEST_UNMOUNT_FAIL='1');self.assertEqual(r.returncode,1)
        self.assertFalse(list((self.spoolroot/'spool').glob('*.ready')))
        self.assertTrue(list((self.spoolroot/'staging').glob('*/image')))
        self.assertTrue((self.spoolroot/'.mount-active').exists())
        self.assertEqual(self.image.read_bytes(),self.original)

    def test_export_failure_still_unmounts(self):
        (self.payload/'link').symlink_to(self.image)
        r=self.run_backup();self.assertEqual(r.returncode,1)
        self.assertIn('Host simulated unmount',r.stdout)
        self.assertFalse(list((self.spoolroot/'spool').glob('*.ready')))

    def test_foreign_mount_refused(self):
        self.assertEqual(self.run_backup(PSCLOUD_TEST_FOREIGN_MOUNT='1').returncode,1)
        self.assertFalse(list(self.spoolroot.iterdir()))

    def test_prior_active_marker_blocks_another_mount(self):
        (self.spoolroot/'.mount-active').write_text('/data/previous/mount')
        r=self.run_backup();self.assertEqual(r.returncode,1)
        self.assertNotIn('Host simulated mount',r.stdout)
        self.assertFalse((self.spoolroot/'staging').exists())

    def test_source_symlink_refused(self):
        other=self.root/'image';self.image.rename(other);self.image.symlink_to(other)
        self.assertEqual(self.run_backup().returncode,1)
        self.assertEqual(other.read_bytes(),self.original)

    def test_credential_restore_failure_blocks_publication(self):
        self.assertEqual(self.run_backup(PSCLOUD_TEST_CREDENTIAL_RESTORE_FAIL='1').returncode,1)
        self.assertFalse(list((self.spoolroot/'spool').glob('*.ready')))

    def test_selection_cannot_traverse_or_select_unrelated_game(self):
        original=self.config.read_text()
        for value in [original.replace('1eb70483','../private'),original.replace('PPSA02433','PPSA99999')]:
            self.config.write_text(value);self.assertEqual(self.run_backup().returncode,2)
