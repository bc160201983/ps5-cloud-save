from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]
class ExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                        '-DPSCLOUD_HOST_TEST',str(ROOT/'ps5/export.c'),
                        str(ROOT/'ps5/common/log.c'),str(ROOT/'ps5/common/zip.c'),
                        '-o',str(ROOT/'export-host')],check=True)
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.root=Path(self.tmp.name)
        self.source=self.root/'mounted'
        self.source.mkdir()
        self.spool=self.root/'spool'
        self.spool.mkdir()
        self.config=self.root/'export.conf'
        self.log=self.root/'pscloud.log'
        self.write_config()
    def tearDown(self): self.tmp.cleanup()
    def write_config(self,closed='yes',source=None):
        self.config.write_text(f'SOURCE={source or self.source}\nTITLE=PPSA02343\nCONFIRM_GAME_CLOSED={closed}\n')
    def run_export(self):
        return subprocess.run([str(ROOT/'export-host'),str(self.config),str(self.spool),str(self.log)],
                              capture_output=True,text=True,timeout=10)
    def test_valid_archive_payload_crc_metadata_exclusion_and_notifications(self):
        data=bytes(range(256))*513
        (self.source/'slot').mkdir()
        (self.source/'slot'/'progress.bin').write_bytes(data)
        (self.source/'empty.bin').write_bytes(b'')
        (self.source/'sce_sys').mkdir()
        (self.source/'sce_sys'/'param.sfo').write_bytes(b'destination metadata')
        before=(self.source/'slot'/'progress.bin').stat().st_mtime_ns
        r=self.run_export()
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        files=list(self.spool.glob('*.zip.ready'))
        self.assertEqual(len(files),1)
        with zipfile.ZipFile(files[0]) as z:
            self.assertIsNone(z.testzip())
            self.assertEqual(set(z.namelist()),{'slot/progress.bin','empty.bin'})
            self.assertEqual(z.read('slot/progress.bin'),data)
        self.assertEqual((self.source/'slot'/'progress.bin').stat().st_mtime_ns,before)
        self.assertIn('[PS5 notification] Save export payload started',r.stdout)
        self.assertIn('Export complete: 2 files',r.stdout)
        self.assertIn('Backup ready:',self.log.read_text())
        self.assertFalse(list(self.spool.glob('*.part')))
    def test_symlink_failure_never_publishes_partial_archive(self):
        (self.source/'valid').write_bytes(b'ok')
        external=self.root/'private'; external.write_bytes(b'secret')
        (self.source/'link').symlink_to(external)
        r=self.run_export()
        self.assertEqual(r.returncode,1)
        self.assertFalse(list(self.spool.iterdir()))
        self.assertIn('Export failed - partial backup removed',r.stdout)
        self.assertNotIn('Export complete',r.stdout)
    def test_game_closure_confirmation_required(self):
        self.write_config(closed='no')
        r=self.run_export()
        self.assertEqual(r.returncode,1)
        self.assertFalse(list(self.spool.iterdir()))
        self.assertIn('missing/invalid mounted-save config',r.stdout)
    def test_mount_path_symlink_refused(self):
        link=self.root/'link'; link.symlink_to(self.source,target_is_directory=True)
        self.write_config(source=link)
        r=self.run_export()
        self.assertEqual(r.returncode,1)
        self.assertFalse(list(self.spool.iterdir()))
        self.assertIn('mounted save unavailable',r.stdout)
    def test_empty_source_is_not_success(self):
        r=self.run_export()
        self.assertEqual(r.returncode,1)
        self.assertFalse(list(self.spool.iterdir()))
    def test_unique_backups_keep_previous_version(self):
        save=self.source/'save'; save.write_bytes(b'first')
        self.assertEqual(self.run_export().returncode,0)
        save.write_bytes(b'second')
        self.assertEqual(self.run_export().returncode,0)
        files=list(self.spool.glob('*.zip.ready'))
        self.assertEqual(len(files),2)
        contents=[]
        for p in files:
            with zipfile.ZipFile(p) as z: contents.append(z.read('save'))
        self.assertEqual(set(contents),{b'first',b'second'})
    def test_unsafe_archive_name_refused(self):
        (self.source/'..\\escape').write_bytes(b'bad')
        self.assertEqual(self.run_export().returncode,1)
        self.assertFalse(list(self.spool.iterdir()))
    def test_missing_config_is_visible_failure(self):
        self.config.unlink()
        r=self.run_export()
        self.assertEqual(r.returncode,1)
        self.assertIn('missing/invalid mounted-save config',r.stdout)
        self.assertTrue(self.log.exists())
