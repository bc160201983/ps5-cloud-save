import contextlib
import importlib.util
import io
from pathlib import Path
import socket
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch, MagicMock
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sender', ROOT/'tools/send_payload.py')
sender = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sender)


def elf_bytes():
    data = bytearray(128)
    data[:6] = b'\x7fELF\x02\x01'
    data[18:20] = b'\x3e\x00'
    struct.pack_into('<Q', data, 40, 64)
    struct.pack_into('<HH', data, 58, 64, 1)
    data.extend(b'payload'*20000)
    return bytes(data)


class SenderTests(unittest.TestCase):
    def test_rejects_non_elf_before_connect(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)/'invalid'
            p.write_bytes(b'not an ELF')
            with patch.object(sender.socket, 'create_connection') as connect:
                with self.assertRaises(ValueError): sender.send('127.0.0.1', 1, p)
                connect.assert_not_called()

    def test_rejects_truncated_section_table(self):
        data = elf_bytes()[:100]
        with self.assertRaisesRegex(ValueError, 'section table'):
            sender.validate_elf(io.BytesIO(data))

    def test_connect_timeout_has_stage_and_no_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)/'test.elf'; p.write_bytes(elf_bytes())
            with patch.object(sender.socket, 'create_connection', side_effect=TimeoutError()):
                with self.assertRaisesRegex(ConnectionError, 'CONNECT timed out.*no payload bytes'):
                    sender.send('127.0.0.1', 9021, p)

    def test_upload_timeout_reports_stage(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)/'test.elf'; p.write_bytes(elf_bytes())
            sock = MagicMock()
            sock.__enter__.return_value = sock
            sock.send.side_effect = TimeoutError('timed out')
            with patch.object(sender.socket, 'create_connection', return_value=sock):
                with self.assertRaisesRegex(ConnectionError, 'UPLOAD failed after 0/140,128 bytes'):
                    sender.send('127.0.0.1', 9021, p)

    def exchange(self, response):
        with tempfile.TemporaryDirectory() as tmp, socket.socket() as srv:
            srv.bind(('127.0.0.1', 0)); srv.listen(1); srv.settimeout(5)
            received = bytearray()
            errors = []
            def receive():
                try:
                    cl, _ = srv.accept()
                    with cl:
                        cl.settimeout(5)
                        while chunk := cl.recv(4096): received.extend(chunk)
                        cl.sendall(response)
                except Exception as e: errors.append(e)
            t = threading.Thread(target=receive); t.start()
            data = elf_bytes()
            p = Path(tmp)/'test.elf'; p.write_bytes(data)
            output = io.StringIO()
            caught = None
            try:
                with contextlib.redirect_stdout(output):
                    sender.send('127.0.0.1', srv.getsockname()[1], p)
            except RuntimeError as e:
                caught = e
            finally:
                t.join(6)
            self.assertFalse(t.is_alive()); self.assertFalse(errors)
            self.assertEqual(received, data)
            return output.getvalue(), caught

    def test_sends_exact_bytes_and_reads_payload_reply(self):
        output, error = self.exchange(b'PSCloud diagnostic written: /data/pscloud-probe.txt\n')
        self.assertIsNone(error)
        self.assertIn('PSCloud diagnostic written', output)

    def test_loader_error_is_not_success(self):
        output, error = self.exchange(b'[elfldr.elf] Error spawning payload\n\r\x00')
        self.assertIsInstance(error, RuntimeError)
        self.assertIn('Error spawning payload', output)
