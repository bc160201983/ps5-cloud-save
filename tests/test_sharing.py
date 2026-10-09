"""Generic sharing fixtures: nested files, arbitrary slots, unsafe packages.
Host mounts are simulated; they are not evidence of game-level compatibility.
"""
import ctypes
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zipfile
import test_portable

ROOT=Path(__file__).resolve().parents[1]

def zip_bytes(files):
    b=io.BytesIO()
    with zipfile.ZipFile(b,'w',compression=zipfile.ZIP_STORED) as z:
        for name,data in files:z.writestr(name,data)
    data=bytearray(b.getvalue());pos=struct.unpack_from('<I',data,len(data)-6)[0]
    while data[pos:pos+4]==b'PK\x01\x02':
        struct.pack_into('<I',data,pos+38,0)
        name,extra,comment=struct.unpack_from('<HHH',data,pos+28);pos+=46+name+extra+comment
    return bytes(data)

def portable(inner=None,slots=('save1','profile'),title='PPSA10528',version='01.000.002'):
    inner=inner if inner is not None else zip_bytes([('folder/progress.bin',b'shared'),('settings.dat',b'config'),('empty',b'')])
    manifest=f'FORMAT=PSCLOUD_PORTABLE_GAME_V1\nTITLE={title}\nSOURCE_FIRMWARE=07000000\nGAME_VERSION={version}\nSLOTS={len(slots)}\n'
    for i,slot in enumerate(slots):manifest+=f'SLOT={i}:{slot}:8192:{hashlib.sha256(inner).hexdigest()}\n'
    return zip_bytes([('manifest.txt',manifest.encode())]+[(f'slot-{i}.zip',inner) for i in range(len(slots))])

class GenericSharingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sources=['ps5/backup.c']+[f'ps5/common/{n}.c' for n in ('sharing','mount','restore','zip','log','snapshot','savemeta','appmeta')]
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror','-fPIC','-shared','-DPSCLOUD_HOST_TEST','-DPSCLOUD_BACKUP_EMBEDDED',*[str(ROOT/p) for p in sources],'-o',str(ROOT/'sharing-host.so'),'-lcrypto'],check=True)
        cls.lib=ctypes.CDLL(str(ROOT/'sharing-host.so'))
        cls.lib.pscloud_share_validate.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_char_p]
        cls.lib.pscloud_share_game.argtypes=[ctypes.c_char_p]*5+[ctypes.c_int,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_void_p]

    def validate(self,data,title=b'PPSA10528'):
        return self.lib.pscloud_share_validate(ctypes.create_string_buffer(data),len(data),title)

    def test_general_nested_multiple_slot_package(self):
        self.assertEqual(self.validate(portable()),0)
        self.assertEqual(self.validate(portable(slots=('different_slot_0',))),0)

    def test_rejects_traversal_metadata_duplicates_and_conflicting_paths(self):
        for files in ([('../bad',b'x')],[('sce_sys/param.sfo',b'x')],[('a',b'x'),('a',b'y')],[('a',b'x'),('a/file',b'y')],[('/absolute',b'x')],[('a//b',b'x')],[('.pscloud-new',b'x')]):
            with self.subTest(files=files):self.assertNotEqual(self.validate(portable(zip_bytes(files))),0)

    def test_rejects_wrong_game_duplicate_slots_and_corruption(self):
        self.assertNotEqual(self.validate(portable(),b'PPSA99999'),0)
        self.assertNotEqual(self.validate(portable(slots=('save1','save1'))),0)
        self.assertNotEqual(self.validate(portable()[:-1]),0)
        data=bytearray(portable());data[50]^=1;self.assertNotEqual(self.validate(bytes(data)),0)

    def fixture(self,base,version='01.000.002',slots=('save1','profile'),capacity=8192):
        home=base/'home';root=base/'root';root.mkdir();meta=base/'meta/PPSA10528';meta.mkdir(parents=True)
        (meta/'param.json').write_text(json.dumps({'contentVersion':version}))
        live=home/'179a0cd8/savedata_prospero/PPSA10528';live.mkdir(parents=True);payload=base/'payload';metadata=[]
        for i,slot in enumerate(slots):
            (live/('sdimg_'+slot)).write_bytes(b'\x02'+b'\0'*(capacity-1))
            test_portable.PortableTests.sfo(self,payload/slot,slot,b'\x77'*8)
            sfo=payload/slot/'sce_sys/param.sfo';sfo.write_bytes(sfo.read_bytes().replace(b'PPSA02433',b'PPSA10528'));metadata.append(sfo.read_bytes())
            (payload/slot/'stale-file').write_bytes(b'not shared')
        return home,root,base/'meta',live,payload,metadata

    def run_share(self,fixture,mode,data=None):
        home,root,meta,live,payload,_=fixture
        old=os.environ.get('PSCLOUD_TEST_GENERIC_PAYLOADS');os.environ['PSCLOUD_TEST_GENERIC_PAYLOADS']=str(payload)
        try:
            published=ctypes.create_string_buffer(128);sha=ctypes.create_string_buffer(65);buf=ctypes.create_string_buffer(data) if data else None
            result=self.lib.pscloud_share_game(str(home).encode(),str(root).encode(),str(meta).encode(),b'179a0cd8',b'PPSA10528',mode,buf,len(data) if data else 0,published,sha)
            return result,published.value.decode(),sha.value.decode()
        finally:
            if old is None:os.environ.pop('PSCLOUD_TEST_GENERIC_PAYLOADS',None)
            else:os.environ['PSCLOUD_TEST_GENERIC_PAYLOADS']=old

    def test_stage_check_preserves_live_keys_metadata_and_imports_all_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            f=self.fixture(Path(tmp))
            for slot in ('save1','profile'):
                (f[4]/slot/'folder').mkdir();(f[4]/slot/'folder/progress.bin').write_bytes(b'old');(f[4]/slot/'folder/progress.bin').chmod(0o640)
            self.assertEqual(self.run_share(f,1,portable())[0],0)
            for i,slot in enumerate(('save1','profile')):
                self.assertEqual((f[3]/('sdimg_'+slot)).read_bytes(),b'\x02'+b'\0'*8191)
                self.assertEqual((f[4]/slot/'sce_sys/param.sfo').read_bytes(),f[5][i])
                self.assertEqual((f[4]/slot/'folder/progress.bin').read_bytes(),b'shared')
                self.assertEqual((f[4]/slot/'folder/progress.bin').stat().st_mode&0o777,0o640)
                self.assertFalse((f[4]/slot/'stale-file').exists())
            self.assertFalse((f[1]/'.mount-active').exists())
            self.assertFalse(list(f[1].glob('portable-stage-*')))

    def test_version_missing_slot_and_capacity_mismatch_refused_before_mount(self):
        for kwargs in ({'version':'02.000.000'},{'slots':('save1',)},{'capacity':4096}):
            with self.subTest(kwargs=kwargs),tempfile.TemporaryDirectory() as tmp:
                f=self.fixture(Path(tmp),**kwargs);self.assertNotEqual(self.run_share(f,1,portable())[0],0)
                self.assertFalse(list(f[1].glob('portable-stage-*')))

    def test_export_inventory_and_no_foreign_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            f=self.fixture(Path(tmp));result,file,sha=self.run_share(f,0);self.assertEqual(result,0)
            data=(f[1]/'share'/file).read_bytes();self.assertEqual(hashlib.sha256(data).hexdigest(),sha);self.assertEqual(self.validate(data),0)
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                self.assertEqual(z.namelist(),['manifest.txt','slot-0.zip','slot-1.zip'])
                for slot in z.namelist()[1:]:
                    with zipfile.ZipFile(io.BytesIO(z.read(slot))) as inner:self.assertFalse(any('sce_sys' in n for n in inner.namelist()))

    def test_failed_unmount_retains_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            f=self.fixture(Path(tmp));os.environ['PSCLOUD_TEST_UNMOUNT_FAIL']='1'
            try:self.assertNotEqual(self.run_share(f,1,portable())[0],0)
            finally:os.environ.pop('PSCLOUD_TEST_UNMOUNT_FAIL')
            self.assertTrue((f[1]/'.mount-active').exists())

    def test_system_memory_layout_without_identity_fields_is_supported(self):
        with tempfile.TemporaryDirectory() as tmp:
            f=self.fixture(Path(tmp),slots=('sce_sdmemory',))
            sfo=f[4]/'sce_sdmemory/sce_sys/param.sfo'
            # Valid SFO table with one non-identity field, not a truncated file.
            key=b'TITLE\0';value=b'Memory\0'
            sfo.write_bytes(struct.pack('<IIIII',0x46535000,0x101,36,36+len(key),1)+struct.pack('<HHIII',0,0x0204,len(value),len(value),0)+key+value)
            before=sfo.read_bytes()
            self.assertEqual(self.run_share(f,1,portable(slots=('sce_sdmemory',)))[0],0)
            self.assertEqual(sfo.read_bytes(),before)

    def test_normal_slot_missing_identity_still_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            f=self.fixture(Path(tmp),slots=('save1',));sfo=f[4]/'save1/sce_sys/param.sfo'
            sfo.write_bytes(struct.pack('<IIIII',0x46535000,0x101,20,20,0))
            self.assertNotEqual(self.run_share(f,1,portable(slots=('save1',)))[0],0)

    def test_system_memory_zero_placeholder_and_corrupt_metadata(self):
        for corrupt in (False,True):
            with self.subTest(corrupt=corrupt),tempfile.TemporaryDirectory() as tmp:
                f=self.fixture(Path(tmp),slots=('sce_sdmemory',));sfo=f[4]/'sce_sdmemory/sce_sys/param.sfo'
                data=b'\0'*3072
                if corrupt:data=data[:100]+b'\x01'+data[101:]
                sfo.write_bytes(data)
                self.assertEqual(self.run_share(f,1,portable(slots=('sce_sdmemory',)))[0]!=0,corrupt)
                self.assertEqual(sfo.read_bytes(),data)

    def test_restore_and_partial_rollback_retain_recovery_copies(self):
        for failure in (False,True):
            with self.subTest(failure=failure),tempfile.TemporaryDirectory() as tmp:
                f=self.fixture(Path(tmp))
                if failure:os.environ['PSCLOUD_TEST_GENERIC_COMMIT_FAIL']='1'
                try:result=self.run_share(f,2,portable())[0]
                finally:os.environ.pop('PSCLOUD_TEST_GENERIC_COMMIT_FAIL',None)
                self.assertEqual(result!=0,failure)
                self.assertEqual(len(list(f[1].glob('portable-stage-*/before-*.img'))),2)
                self.assertFalse((f[1]/'.restore-active').exists())
