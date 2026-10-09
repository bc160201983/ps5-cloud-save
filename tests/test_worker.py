import http.server
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import unittest
import hashlib

ROOT = Path(__file__).resolve().parents[1]

class WorkerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.spool = self.root / 'spool'
        self.spool.mkdir()
        self.cert = self.root / 'cert.pem'
        key = self.root / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(key), '-out', str(self.cert), '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.objects = {}
        self.status = 201
        self.manifest_status = 201
        self.collections=set()
        self.puts=[]
        self.corrupt_upload=False
        parent = self
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_MKCOL(self):
                if self.headers.get('Authorization') != 'Basic dXNlcjpwYXNz':
                    self.send_response(401)
                elif self.path in parent.collections:
                    self.send_response(405)
                else:
                    parent.collections.add(self.path);self.send_response(201)
                self.end_headers()
            def do_PROPFIND(self):
                if self.headers.get('Authorization') != 'Basic dXNlcjpwYXNz':
                    self.send_response(401);self.end_headers();return
                prefix=self.path.rstrip('/')+'/'
                matches=[p for p in parent.objects if p.startswith(prefix)]
                if not matches and self.path.rstrip('/')!='/backups' and self.path.rstrip('/') not in parent.collections:
                    self.send_response(404);self.end_headers();return
                xml='<d:multistatus xmlns:d="DAV:">'+''.join('<d:response><d:href>'+p+'</d:href></d:response>' for p in matches)+'</d:multistatus>'
                data=xml.encode();self.send_response(207);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
            def do_GET(self):
                if self.headers.get('Authorization') != 'Basic dXNlcjpwYXNz':
                    self.send_response(401); self.end_headers(); return
                if parent.status != 201:
                    self.send_response(parent.status); self.end_headers(); return
                body=parent.objects.get(self.path)
                if body is None:
                    self.send_response(404); self.end_headers(); return
                self.send_response(200)
                self.send_header('Content-Length',str(len(body)))
                self.end_headers(); self.wfile.write(body)
            def do_HEAD(self):
                if self.headers.get('Authorization') != 'Basic dXNlcjpwYXNz':
                    self.send_response(401);self.end_headers();return
                if parent.status != 201:
                    self.send_response(parent.status);self.end_headers();return
                body=parent.objects.get(self.path)
                self.send_response(200 if body is not None else 404)
                if body is not None:self.send_header('Content-Length',str(len(body)))
                self.end_headers()
            def do_PUT(self):
                body = self.rfile.read(int(self.headers['Content-Length']))
                if self.headers.get('Authorization') != 'Basic dXNlcjpwYXNz':
                    self.send_response(401)
                else:
                    status=parent.manifest_status if self.path.endswith('.identity') else parent.status
                    if status == 201:
                        parent.puts.append(self.path)
                        parent.objects[self.path] = (bytes([body[0]^1])+body[1:]) if parent.corrupt_upload and body and not self.path.endswith('.identity') else body
                    self.send_response(status)
                self.end_headers()
            def log_message(self, *args): pass
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(self.cert, key)
        self.server.socket = ctx.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.env = dict(os.environ, PSCLOUD_URL=f'https://localhost:{self.server.server_port}/backups',
                        PSCLOUD_USER='user', PSCLOUD_PASSWORD='pass', PSCLOUD_CA_BUNDLE=str(self.cert))
    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.tmp.cleanup()
    def run_worker(self, **overrides):
        return subprocess.run([str(ROOT/'cloud-worker'), str(self.spool), '--once'],
                              env=dict(self.env, **overrides), capture_output=True, timeout=15)
    def job(self):
        p = self.spool/'console-user-game-unique.zip.ready'
        p.write_bytes(b'opaque archive payload\x00\xff')
        return p
    def test_upload_and_no_repeat(self):
        p = self.job()
        body = p.read_bytes()
        self.assertEqual(self.run_worker().returncode, 0)
        self.assertFalse(p.exists())
        self.assertTrue(p.with_name(p.name[:-6]+'.sent').exists())
        self.assertEqual(next(iter(self.objects.values())), body)
        self.objects.clear()
        self.assertEqual(self.run_worker().returncode, 0)
        self.assertFalse(self.objects)
    def test_failure_survives_restart(self):
        p = self.job()
        self.status = 503
        self.assertEqual(self.run_worker().returncode, 1)
        self.assertTrue(p.exists())
        self.status = 201
        self.assertEqual(self.run_worker().returncode, 0)
        self.assertFalse(p.exists())
    def test_auth_failure_preserves_job(self):
        p = self.job()
        self.assertEqual(self.run_worker(PSCLOUD_PASSWORD='wrong').returncode, 1)
        self.assertTrue(p.exists())
    def test_successful_http_upload_with_corrupt_content_keeps_local_backup(self):
        p=self.job();original=p.read_bytes();self.corrupt_upload=True
        self.assertEqual(self.run_worker().returncode,1)
        self.assertEqual(p.read_bytes(),original)
        self.assertFalse(p.with_name(p.name[:-6]+'.sent').exists())
        self.corrupt_upload=False
        self.assertEqual(self.run_worker().returncode,0)
        self.assertEqual(next(iter(self.objects.values())),original)
    def test_untrusted_tls_rejected(self):
        p = self.job()
        self.assertEqual(self.run_worker(PSCLOUD_CA_BUNDLE='').returncode, 1)
        self.assertTrue(p.exists())
    def test_incomplete_and_symlink_not_uploaded(self):
        (self.spool/'unfinished.part').write_bytes(b'partial')
        target = self.root/'private'
        target.write_bytes(b'secret')
        (self.spool/'link.zip.ready').symlink_to(target)
        self.assertEqual(self.run_worker().returncode, 1)
        self.assertFalse(self.objects)
    def test_plain_http_refused(self):
        self.assertEqual(self.run_worker(PSCLOUD_URL='http://localhost/backups').returncode, 2)

    def structured_job(self):
        p=self.job();body=p.read_bytes()
        (self.spool/(p.name[:-6]+'.identity')).write_text('USER_ID=1eb70483\nTITLE=PPSA02433\nSAVE_NAME=PlayerSaveSlot0Save\nSHA256='+hashlib.sha256(body).hexdigest()+'\n')
        return p

    def test_game_user_slot_folders_and_commit(self):
        p=self.structured_job();self.assertEqual(self.run_worker().returncode,0)
        prefix='/backups/Crash%20Bandicoot%204%20-%20PPSA02433/User-1eb70483/PlayerSaveSlot0Save/'
        self.assertIn(prefix+p.name[:-6],self.objects)
        self.assertIn(prefix+'.pscloud/'+p.name[:-6]+'.identity',self.objects)

    def test_failed_manifest_commit_retains_ready_job(self):
        p=self.structured_job();self.manifest_status=503
        self.assertEqual(self.run_worker().returncode,1);self.assertTrue(p.exists())
        self.manifest_status=201;self.assertEqual(self.run_worker().returncode,0)

    def test_corrupt_remote_copy_is_reuploaded_instead_of_skipped(self):
        p=self.structured_job();original=p.read_bytes()
        self.assertEqual(self.run_worker().returncode,0)
        sent=p.with_name(p.name[:-6]+'.sent');sent.rename(p)
        remote=next(k for k in self.objects if not k.endswith('.identity'))
        self.objects[remote]=bytes([original[0]^1])+original[1:]
        self.puts.clear()
        self.assertEqual(self.run_worker().returncode,0)
        self.assertIn(remote,self.puts);self.assertEqual(self.objects[remote],original)

    def test_corrupt_upload_does_not_publish_identity_commit(self):
        p=self.structured_job();self.corrupt_upload=True
        self.assertEqual(self.run_worker().returncode,1);self.assertTrue(p.exists())
        self.assertFalse(any(k.endswith('.identity') for k in self.objects))

    def test_identity_traversal_rejected(self):
        p=self.structured_job();meta=self.spool/(p.name[:-6]+'.identity')
        meta.write_text(meta.read_text().replace('PlayerSaveSlot0Save','../private'))
        self.assertEqual(self.run_worker().returncode,1);self.assertFalse(self.objects)

if __name__ == '__main__': unittest.main()
