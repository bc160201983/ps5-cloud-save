from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class ProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                        '-DPSCLOUD_HOST_TEST',str(ROOT/'ps5/probe.c'),str(ROOT/'ps5/common/log.c'),'-o',str(ROOT/'probe-host')],check=True)
    def test_enumerates_without_reading_or_changing_save(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'users'
            title=root/'1a2b'/'savedata_prospero'/'PPSA12345'
            title.mkdir(parents=True)
            save=title/'save.dat'
            save.write_bytes(b'original save bytes')
            before=save.stat().st_mtime_ns
            (root/'1a2b'/'savedata_prospero'/'PPSA99999').symlink_to(title,target_is_directory=True)
            (root/'not-a-user').mkdir()
            report=Path(tmp)/'report'
            result=subprocess.run([str(ROOT/'probe-host'),str(root),str(report)],check=True,capture_output=True,text=True)
            self.assertIn('[PS5 notification] Diagnostic started',result.stdout)
            self.assertIn('Diagnostic complete - save folders scanned',result.stdout)
            self.assertIn('Scanning save folders for user 1',Path(str(report)+'.log').read_text())
            text=report.read_text()
            self.assertIn('title=PPSA12345',text)
            self.assertNotIn('PPSA99999',text)
            self.assertIn('users=1',text)
            self.assertIn('module.fs=not-probed-on-host',text)
            self.assertIn('no probed function called',text)
            self.assertNotIn('1a2b',text)
            self.assertEqual(save.read_bytes(),b'original save bytes')
            self.assertEqual(save.stat().st_mtime_ns,before)
    def test_missing_root_reports_unavailable(self):
        with tempfile.TemporaryDirectory() as tmp:
            report=Path(tmp)/'report'
            subprocess.run([str(ROOT/'probe-host'),str(Path(tmp)/'missing'),str(report)],capture_output=True)
            self.assertIn('user_scan=unavailable',report.read_text())
    def test_report_symlink_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            target=Path(tmp)/'original'
            target.write_text('unchanged')
            report=Path(tmp)/'report'
            report.symlink_to(target)
            result=subprocess.run([str(ROOT/'probe-host'),tmp,str(report)],capture_output=True)
            self.assertNotEqual(result.returncode,0)
            self.assertEqual(target.read_text(),'unchanged')
