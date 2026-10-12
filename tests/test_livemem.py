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
        cls.lib.pscloud_live_next.argtypes = [ctypes.c_double, *msg]
        cls.lib.pscloud_live_pin.argtypes = [ctypes.c_char_p, ctypes.c_char_p, *msg]
        cls.lib.pscloud_live_edit.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_char_p, ctypes.c_double, *msg]
        for name in ('state', 'pins'):
            getattr(cls.lib, 'pscloud_live_'+name+'_json').argtypes = msg
        cls.lib.pscloud_live_list_json.argtypes = [*msg, ctypes.c_int]
        cls.lib.pscloud_live_locator.argtypes = [ctypes.c_uint, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.pscloud_live_resolve.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, *msg]

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
        self.old = {k: os.environ.get(k) for k in ('PSCLOUD_TEST_LIVE', 'PSCLOUD_TEST_LIVE_SESSION', 'PSCLOUD_TEST_LIVE_UNREADABLE')}
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

    def teach_locator(self):
        with self.memory.open('r+b') as f:
            for index, offset in enumerate((256, 1024)):
                f.seek(offset-24)
                context=bytearray(('PlayerMoneyContext%02dXXXX' % index).encode())
                self.assertEqual(len(context),24)
                f.write(context)
                f.seek(offset+4)
                f.write(b'PersistentMoneyFieldContextXXXXXX')
        group=self.pin()
        out=ctypes.create_string_buffer(14000)
        self.assertEqual(self.lib.pscloud_live_locator(group,b'01.000.000',out,len(out)),0)
        return json.loads(out.value)

    def test_locator_relocates_after_restart_without_reusing_old_addresses(self):
        profile=self.teach_locator()
        old=self.memory.read_bytes()
        moved=bytearray(65536)
        for src,dst in ((256,512),(1024,2048)):
            moved[dst-24:dst+40]=old[src-24:src+40]
            struct.pack_into('<i',moved,dst,7654)
        self.memory.write_bytes(moved)
        self.stamp.write_text('103')
        patterns=','.join(r['pattern'].replace('??','xx') for r in profile['rules']).encode()
        self.call('resolve',b'PPSA02433',b'int32',patterns)
        self.wait_for(lambda: not self.json('state')['busy'])
        self.assertFalse(self.json('state')['error'])
        places=self.json('list')['places']
        self.assertEqual([p['address'] for p in places],['0x10000200','0x10000800'])
        self.assertEqual([p['value'] for p in places],[7654,7654])
        self.assertEqual(self.memory.read_bytes(),bytes(moved))  # relocation never writes
        self.addresses=[p['address'] for p in places]
        group=self.pin()
        self.edit('set',9999,id=group)
        data=self.memory.read_bytes()
        self.assertEqual(struct.unpack_from('<i',data,512)[0],9999)
        self.assertEqual(struct.unpack_from('<i',data,256)[0],0)

    def test_locator_ambiguity_wrong_title_and_weak_patterns_refused(self):
        profile=self.teach_locator()
        data=bytearray(self.memory.read_bytes())
        data[4096-24:4096+40]=data[256-24:256+40]
        self.memory.write_bytes(data)
        patterns=','.join(r['pattern'].replace('??','xx') for r in profile['rules']).encode()
        self.call('resolve',b'PPSA99999',b'int32',patterns,ok=False)
        self.call('resolve',b'PPSA02433',b'double',b'xx'*64,ok=False)
        self.assertEqual(self.json('state')['type'],'int32')  # failed import cannot change existing search type
        self.call('resolve',b'PPSA02433',b'int32',patterns)
        self.wait_for(lambda: not self.json('state')['busy'])
        state=self.json('state')
        # The copy at 4096 makes one place ambiguous: every match becomes a search result.
        self.assertEqual((state['error'],state['has'],state['candidates']),('',True,True))
        self.assertEqual([p['address'] for p in self.json('list')['places']],['0x10000100','0x10000400','0x10001000'])
        self.assertEqual(self.memory.read_bytes(),bytes(data))  # relocation never writes
        with self.memory.open('r+b') as f:
            f.seek(256);f.write(struct.pack('<i',4321))  # the game's real copy changes
        self.call('next',4321.0)
        self.wait_for(lambda: not self.json('state')['busy'])
        self.assertEqual([p['address'] for p in self.json('list')['places']],['0x10000100'])
        self.assertFalse(self.json('state')['candidates'])

    def test_relocation_skips_unreadable_pages(self):
        profile=self.teach_locator()
        old=self.memory.read_bytes()
        moved=bytearray(65536)
        for src,dst in ((256,512),(1024,2048)):
            moved[dst-24:dst+40]=old[src-24:src+40]
            struct.pack_into('<i',moved,dst,7654)
        moved[9000-24:9000+40]=moved[512-24:512+40]  # a copy inside memory that cannot be read
        self.memory.write_bytes(moved)
        self.stamp.write_text('104')
        os.environ['PSCLOUD_TEST_LIVE_UNREADABLE']='8192:12288'
        patterns=','.join(r['pattern'].replace('??','xx') for r in profile['rules']).encode()
        self.call('resolve',b'PPSA02433',b'int32',patterns)
        self.wait_for(lambda: not self.json('state')['busy'])
        self.assertFalse(self.json('state')['error'])
        self.assertEqual([p['address'] for p in self.json('list')['places']],['0x10000200','0x10000800'])
        self.assertEqual(self.memory.read_bytes(),bytes(moved))

    def locator(self, group):
        out=ctypes.create_string_buffer(16000)
        result=self.lib.pscloud_live_locator(group,b'01.000.000',out,len(out))
        return result,out.value.decode()

    def test_wide_window_retains_a_count_in_sparse_memory(self):
        # Nothing distinctive within 24 bytes of the count; an item table header 110 bytes before it.
        with self.memory.open('r+b') as f:
            for index,offset in enumerate((256,1024)):
                f.seek(offset-110);f.write(('YewWoodItemTableHeader%02d' % index).encode())
        result,text=self.locator(self.pin())
        self.assertEqual(result,0,text)
        rule=json.loads(text)['rules'][0]
        self.assertEqual((len(rule['pattern']),rule['offset']),(512,128))
        self.assertEqual(rule['pattern'][256:264],'????????')

    def test_weak_context_reports_the_reason(self):
        result,text=self.locator(self.pin())  # only zeros around both values
        self.assertEqual(result,-1)
        self.assertIn('Too little stable memory',text)

    def test_stop_search_ends_a_running_search(self):
        with self.memory.open('r+b') as f:
            f.truncate(256*1024*1024)  # large enough that the search is still running
        self.stamp.write_text('105')
        self.call('scan', b'int32', 1234)
        self.lib.pscloud_live_reset()
        self.wait_for(lambda: not self.json('state')['busy'])
        state = self.json('state')
        self.assertEqual(state['error'], 'Search stopped')
        self.assertFalse(state['has'])

    def test_search_trace_names_each_region_before_reading_it(self):
        trace = Path(self.tmp.name)/'live-trace.txt'
        self.lib.pscloud_live_set_trace.argtypes = [ctypes.c_char_p]
        self.lib.pscloud_live_set_trace(str(trace).encode())
        try:
            self.call('scan', b'int32', 1234)
            self.wait_for(lambda: not self.json('state')['busy'])
        finally:
            self.lib.pscloud_live_set_trace(b'')
        lines = trace.read_text().splitlines()
        self.assertTrue(lines[0].startswith('search in PPSA02433: 1 regions'), lines)
        self.assertTrue(lines[1].startswith('region 1/1 0x10000000-0x10010000 64 KiB'), lines)
        self.assertEqual(lines[-1], 'finished: ok')
