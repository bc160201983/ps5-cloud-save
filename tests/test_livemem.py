"""Host memory-file checks; these do not establish PS5 game compatibility."""
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LiveMemoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        binary = Path(cls.build.name) / 'live.so'
        subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-DPSCLOUD_HOST_TEST', str(ROOT/'ps5/common/livemem.c'),
                        str(ROOT/'ps5/common/log.c'), '-pthread', '-o', str(binary)], check=True)
        cls.lib = ctypes.CDLL(str(binary))
        msg = [ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.pscloud_live_scan.argtypes = [ctypes.c_char_p, ctypes.c_double, *msg]
        cls.lib.pscloud_live_pin.argtypes = [ctypes.c_char_p, ctypes.c_char_p, *msg]
        cls.lib.pscloud_live_edit.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_char_p, ctypes.c_double, *msg]
        for name in ('state', 'pins'):
            getattr(cls.lib, 'pscloud_live_'+name+'_json').argtypes = msg
        cls.lib.pscloud_live_list_json.argtypes = [*msg, ctypes.c_int]

    @classmethod
    def tearDownClass(cls):
        cls.lib.pscloud_live_shutdown()
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.memory = Path(self.tmp.name)/'memory'
        self.stamp = Path(self.tmp.name)/'session'
        self.stamp.write_text('100')
        data = bytearray(65536)
        for offset in (256, 1024):
            struct.pack_into('<i', data, offset, 1234)
        self.memory.write_bytes(data)
        self.old = {k: os.environ.get(k) for k in ('PSCLOUD_TEST_LIVE', 'PSCLOUD_TEST_LIVE_SESSION')}
        os.environ['PSCLOUD_TEST_LIVE'] = str(self.memory)
        os.environ['PSCLOUD_TEST_LIVE_SESSION'] = str(self.stamp)
        self.json('state')
        for group in self.json('pins')['groups']:
            self.edit('remove', id=group['id'], addresses=[])
        self.call('scan', b'int32', 1234)
        self.wait_for(lambda: not self.json('state')['busy'])
        self.addresses = [p['address'] for p in self.json('list')['places']]
        self.assertEqual(len(self.addresses), 2)

    def tearDown(self):
        self.stamp.write_text('101')
        self.json('state')
        for k, v in self.old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.tmp.cleanup()

    def call(self, name, *args, ok=True):
        message = ctypes.create_string_buffer(512)
        result = getattr(self.lib, 'pscloud_live_'+name)(*args, message, len(message))
        self.assertEqual(result, 0 if ok else -1, message.value.decode())
        return message.value.decode()

    def json(self, name):
        out = ctypes.create_string_buffer(20000)
        args = (out, len(out), 200) if name == 'list' else (out, len(out))
        getattr(self.lib, 'pscloud_live_'+name+'_json')(*args)
        return json.loads(out.value)

    def edit(self, action, value=0, id=0, addresses=None, ok=True):
        selected = ','.join(self.addresses if addresses is None else addresses).encode()
        return self.call('edit', selected, id, action.encode(), value, ok=ok)

    def pin(self, name=b'Money'):
        self.call('pin', ','.join(self.addresses).encode(), name)
        return self.json('pins')['groups'][-1]['id']

    def values(self):
        data = self.memory.read_bytes()
        return [struct.unpack_from('<i', data, o)[0] for o in (256, 1024)]

    def game_write(self, value):
        with self.memory.open('r+b') as f:
            for o in (256, 1024):
                f.seek(o)
                f.write(struct.pack('<i', value))

    def wait_for(self, condition):
        for _ in range(100):
            if condition():
                return
            time.sleep(0.02)
        self.fail('Timed out waiting for Live worker')

    def test_batch_prevalidates_every_address_and_range(self):
        self.edit('set', 9999, addresses=[self.addresses[0], '0x10004000'], ok=False)
        self.edit('set', 9999, addresses=[self.addresses[0]]*2, ok=False)
        self.edit('set', 2**40, ok=False)
        self.edit('set', 1.5, ok=False)
        self.assertEqual(self.values(), [1234, 1234])
        self.edit('set', 9999)
        self.assertEqual(self.values(), [9999, 9999])

    def test_saved_group_survives_new_search_and_reset(self):
        group = self.pin(b'Money "wallet"')
        self.lib.pscloud_live_reset()
        self.edit('add', 100, id=group)
        self.assertEqual(self.values(), [1334, 1334])
        self.call('scan', b'uint8', 99)
        self.wait_for(lambda: not self.json('state')['busy'])
        self.edit('add', -34, id=group)
        self.assertEqual(self.values(), [1300, 1300])
        self.assertEqual(self.json('pins')['groups'][0]['name'], 'Money "wallet"')
        self.edit('remove', id=group)
        self.edit('set', 5, id=group, ok=False)

    def test_freeze_adjust_unfreeze_and_remove(self):
        group = self.pin()
        self.edit('freeze', 2000, id=group)
        self.game_write(5)
        self.wait_for(lambda: self.values() == [2000, 2000])
        self.edit('add', 500, id=group)
        self.game_write(6)
        self.wait_for(lambda: self.values() == [2500, 2500])
        self.assertTrue(self.json('pins')['groups'][0]['frozen'])
        self.edit('unfreeze', id=group)
        self.game_write(7)
        time.sleep(0.35)
        self.assertEqual(self.values(), [7, 7])
        self.edit('remove', id=group)
        self.assertEqual(self.json('pins')['groups'], [])

    def test_restart_with_same_pid_expires_freeze_and_addresses(self):
        group = self.pin()
        self.edit('freeze', 3000, id=group)
        self.stamp.write_text('102')
        self.wait_for(lambda: self.json('pins')['groups'] == [])
        self.game_write(42)
        time.sleep(0.35)
        self.assertEqual(self.values(), [42, 42])
        self.edit('set', 9999, id=group, ok=False)
        self.edit('set', 9999, ok=False)

    def test_unreadable_place_stops_freeze_and_overlapping_pins_refused(self):
        group = self.pin()
        self.call('pin', self.addresses[0].encode(), b'Duplicate', ok=False)
        self.edit('freeze', 3000, id=group)
        with self.memory.open('r+b') as f:
            f.truncate(512)
        self.wait_for(lambda: not self.json('pins')['groups'][0]['frozen'])
        self.assertIn('Freeze stopped', self.json('pins')['groups'][0]['error'])

    def test_nonfinite_and_overflow_values_never_write(self):
        group = self.pin()
        for value in (float('nan'), float('inf'), 1e300):
            self.edit('set', value, id=group, ok=False)
        self.assertEqual(self.values(), [1234, 1234])

    def test_batch_updates_frozen_targets_and_blocks_different_type_overlap(self):
        group = self.pin()
        self.edit('freeze', 2000, id=group)
        self.call('scan', b'int16', 2000)
        self.wait_for(lambda: not self.json('state')['busy'])
        self.addresses = [p['address'] for p in self.json('list')['places']]
        self.edit('set', 300, ok=False)
        self.assertEqual(self.values(), [2000, 2000])
        self.call('scan', b'int32', 2000)
        self.wait_for(lambda: not self.json('state')['busy'])
        self.addresses = [p['address'] for p in self.json('list')['places']]
        self.edit('set', 2500)
        self.game_write(5)
        self.wait_for(lambda: self.values() == [2500, 2500])

    def test_decimal_freeze_and_float_overflow(self):
        with self.memory.open('r+b') as f:
            for offset in (256, 1024):
                f.seek(offset)
                f.write(struct.pack('<f', 12.5))
        self.call('scan', b'float', 12.5)
        self.wait_for(lambda: not self.json('state')['busy'])
        self.addresses = [p['address'] for p in self.json('list')['places']]
        group = self.pin(b'Health')
        before = self.memory.read_bytes()
        self.edit('freeze', 1e39, id=group, ok=False)
        self.assertEqual(self.memory.read_bytes(), before)
        self.edit('freeze', 25.25, id=group)
        with self.memory.open('r+b') as f:
            for offset in (256, 1024):
                f.seek(offset)
                f.write(struct.pack('<f', 2.5))
        def values():
            data = self.memory.read_bytes()
            return [struct.unpack_from('<f', data, o)[0] for o in (256, 1024)]
        self.wait_for(lambda: values() == [25.25, 25.25])
        self.edit('add', 0.25, id=group)
        self.assertEqual(values(), [25.5, 25.5])
