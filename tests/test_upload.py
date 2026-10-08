from pathlib import Path
import subprocess
import unittest
import test_worker

ROOT=Path(__file__).resolve().parents[1]

class UploadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                        '-DPSCLOUD_HOST_TEST','-DPSCLOUD_EMBEDDED',
                        str(ROOT/'ps5/upload.c'),str(ROOT/'src/worker.c'),
                        str(ROOT/'ps5/common/log.c'),str(ROOT/'ps5/common/cloud.c'),'-o',str(ROOT/'upload-host'),
                        '-lcurl'],check=True)

    def setUp(self):
        self.fixture=test_worker.WorkerTest()
        self.fixture.setUp()
        self.config=self.fixture.root/'upload.conf'
        self.log=self.fixture.root/'upload.log'
        self.config.write_text('URL='+self.fixture.env['PSCLOUD_URL']+
                               '\nUSER=user\nPASSWORD=pass\nCA_BUNDLE='+
                               str(self.fixture.cert)+'\nMODE=once\n')

    def tearDown(self):
        self.fixture.tearDown()

    def run_upload(self):
        return subprocess.run([str(ROOT/'upload-host'),str(self.config),
                               str(self.fixture.spool),str(self.log)],
                              capture_output=True,text=True,timeout=15)

    def test_configured_payload_uploads_and_retains_archive(self):
        job=self.fixture.job(); body=job.read_bytes()
        result=self.run_upload()
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(next(iter(self.fixture.objects.values())),body)
        self.assertTrue(job.with_name(job.name[:-6]+'.sent').exists())
        self.assertNotIn('PASSWORD=',result.stdout+self.log.read_text())

    def test_auth_failure_retains_backup(self):
        self.config.write_text(self.config.read_text().replace('PASSWORD=pass','PASSWORD=wrong'))
        job=self.fixture.job()
        result=self.run_upload()
        self.assertEqual(result.returncode,1,result.stdout+result.stderr)
        self.assertTrue(job.exists())
        self.assertNotIn('wrong',result.stdout+result.stderr+self.log.read_text())

    def test_duplicate_config_rejected_before_upload(self):
        self.config.write_text(self.config.read_text()+'MODE=watch\n')
        self.fixture.job()
        self.assertEqual(self.run_upload().returncode,2)
        self.assertFalse(self.fixture.objects)

    def test_missing_ca_rejected_before_upload(self):
        self.config.write_text(self.config.read_text().replace(str(self.fixture.cert),'/missing-ca.pem'))
        self.fixture.job()
        self.assertEqual(self.run_upload().returncode,2)
        self.assertFalse(self.fixture.objects)

    def test_plain_http_rejected_before_upload(self):
        self.config.write_text(self.config.read_text().replace('https://','http://'))
        self.fixture.job()
        self.assertEqual(self.run_upload().returncode,2)
        self.assertFalse(self.fixture.objects)
