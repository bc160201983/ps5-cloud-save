import importlib.util
from pathlib import Path
import socket
import tempfile
import threading
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('sender',ROOT/'tools/send_payload.py')
sender=importlib.util.module_from_spec(spec)
spec.loader.exec_module(sender)
class SenderTests(unittest.TestCase):
    def test_rejects_non_elf_before_connect(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'invalid'
            p.write_bytes(b'not an ELF')
            with self.assertRaises(ValueError): sender.send('127.0.0.1',1,p)
    def test_sends_exact_bytes(self):
        with tempfile.TemporaryDirectory() as tmp, socket.socket() as srv:
            srv.bind(('127.0.0.1',0)); srv.listen(1); srv.settimeout(5)
            received=bytearray()
            def receive():
                cl,_=srv.accept()
                with cl:
                    cl.settimeout(5)
                    while chunk:=cl.recv(4096): received.extend(chunk)
            t=threading.Thread(target=receive); t.start()
            data=bytearray(20); data[:6]=b'\x7fELF\x02\x01'; data[18:20]=b'\x3e\x00'
            data.extend(b'payload'*20000)
            p=Path(tmp)/'test.elf'; p.write_bytes(data)
            sender.send('127.0.0.1',srv.getsockname()[1],p)
            t.join(6)
            self.assertFalse(t.is_alive()); self.assertEqual(received,data)
