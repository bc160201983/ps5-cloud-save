"""Portable Crash package and staged cross-profile checks; no real saves."""
import ctypes
import hashlib
import io
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]
SLOTS=('PlayerSaveSlot0Save','PlayerSaveProfileSaveData')

def package(progress=b'progress',profile=b'profile',extra=None,compression=zipfile.ZIP_STORED):
    manifest=('FORMAT=PSCLOUD_PORTABLE_CRASH_V1\nTITLE=PPSA02433\nSOURCE_FIRMWARE=11400000\nCREATED_UNIX=1700000000\n'
              'PROGRESS_SHA256='+hashlib.sha256(progress).hexdigest()+'\nPROFILE_SHA256='+hashlib.sha256(profile).hexdigest()+'\n')
    b=io.BytesIO()
    with zipfile.ZipFile(b,'w',compression=compression) as z:
        z.writestr('progress.dat',progress);z.writestr('profile.dat',profile);z.writestr('manifest.txt',manifest)
        if extra:z.writestr(extra,b'invalid')
    data=bytearray(b.getvalue());pos=struct.unpack_from('<I',data,len(data)-6)[0]
    while data[pos:pos+4]==b'PK\x01\x02':
        struct.pack_into('<I',data,pos+38,0)
        name,extra_len,comment=struct.unpack_from('<HHH',data,pos+28);pos+=46+name+extra_len+comment
    return bytes(data)

class Portable(ctypes.Structure):
    _fields_=[('payload',ctypes.c_void_p*2),('size',ctypes.c_size_t*2),('firmware',ctypes.c_uint),('created',ctypes.c_longlong)]

class PortableTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror','-fPIC','-shared','-DPSCLOUD_HOST_TEST',
                        *[str(ROOT/'ps5/common'/f) for f in ('portable.c','zip.c','snapshot.c','log.c')],
                        '-o',str(ROOT/'portable-host.so'),'-lcrypto'],check=True)
        cls.lib=ctypes.CDLL(str(ROOT/'portable-host.so'))
        cls.lib.pscloud_portable_parse.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(Portable)]
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror','-DPSCLOUD_HOST_TEST','-DPSCLOUD_BACKUP_EMBEDDED',
                        str(ROOT/'ps5/share.c'),str(ROOT/'ps5/backup.c'),
                        *[str(ROOT/'ps5/common'/f) for f in ('portable.c','mount.c','restore.c','zip.c','log.c','snapshot.c','savemeta.c')],
                        '-o',str(ROOT/'share-host'),'-lcrypto'],check=True)

    def parse(self,data):
        b=ctypes.create_string_buffer(data);p=Portable();result=self.lib.pscloud_portable_parse(b,len(data),ctypes.byref(p))
        return result,tuple(ctypes.string_at(p.payload[i],p.size[i]) for i in range(2)) if result==0 else None

    def test_valid_payload_only_package(self):
        self.assertEqual(self.parse(package()),(0,(b'progress',b'profile')))

    def test_rejects_unsafe_and_corrupt_packages(self):
        for data in (package(extra='sce_sys/param.sfo'),package(extra='../bad'),package(compression=zipfile.ZIP_DEFLATED),package()[:-1],package().replace(b'PPSA02433',b'PPSA99999')):
            self.assertNotEqual(self.parse(data)[0],0)
        data=bytearray(package());data[42]^=1;self.assertNotEqual(self.parse(bytes(data))[0],0)

    def sfo(self,path,slot,account):
        path.mkdir(parents=True);(path/'sce_sys').mkdir()
        fields=[('TITLE_ID',0x0204,b'PPSA02433\0'),('SAVEDATA_DIRECTORY',0x0204,slot.encode()+b'\0'),('ACCOUNT_ID',0x0004,account)]
        keys=b'';values=b'';entries=[]
        for key,fmt,value in fields:
            entries.append(struct.pack('<HHIII',len(keys),fmt,len(value),len(value),len(values)));keys+=key.encode()+b'\0';values+=value
        offset=20+16*len(fields)
        (path/'sce_sys/param.sfo').write_bytes(struct.pack('<IIIII',0x46535000,0x101,offset,offset+len(keys),len(fields))+b''.join(entries)+keys+values)
        (path/'ue4savegame.dpx.sav').write_bytes(b'old recipient payload')

    def test_failed_mount_preserves_marker_and_originals(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp);home=base/'home';root=base/'root';root.mkdir()
            live=home/'1eb70483/savedata_prospero/PPSA02433';live.mkdir(parents=True)
            original=b'\x02'+b'\0'*8191
            for slot in SLOTS:(live/('sdimg_'+slot)).write_bytes(original)
            config=base/'config';config.write_text('MODE=export\nUSER_ID=1eb70483\nCONFIRM_GAME_CLOSED=yes\n')
            r=subprocess.run([str(ROOT/'share-host'),str(config),str(home),str(root)],env=dict(os.environ,PSCLOUD_TEST_MOUNT_FAIL='1'),capture_output=True,timeout=10)
            self.assertNotEqual(r.returncode,0);self.assertTrue((root/'.mount-active').exists())
            for slot in SLOTS:self.assertEqual((live/('sdimg_'+slot)).read_bytes(),original)
            self.assertFalse(list((root/'share').glob('*.zip')))

    def test_export_and_recipient_check_preserve_live_images_and_account_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp);home=base/'home';root=base/'root';root.mkdir()
            live=home/'1eb70483/savedata_prospero/PPSA02433';live.mkdir(parents=True)
            originals=b'\x02'+b'\0'*8191
            source=base/'source';target=base/'target';metadata=[]
            for i,slot in enumerate(SLOTS):
                (live/('sdimg_'+slot)).write_bytes(originals)
                self.sfo(source/str(i),slot,b'\x01'*8);self.sfo(target/str(i),slot,b'\x02'*8)
                (source/str(i)/'ue4savegame.dpx.sav').write_bytes(b'shared '+slot.encode())
                metadata.append((target/str(i)/'sce_sys/param.sfo').read_bytes())
            config=base/'config';config.write_text('MODE=export\nUSER_ID=1eb70483\nCONFIRM_GAME_CLOSED=yes\n')
            env=dict(os.environ,PSCLOUD_TEST_SHARE_PAYLOADS=str(source))
            r=subprocess.run([str(ROOT/'share-host'),str(config),str(home),str(root)],env=env,capture_output=True,timeout=10)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            exported=next((root/'share').glob('*.zip'))
            with zipfile.ZipFile(exported) as z:self.assertEqual(z.namelist(),['progress.dat','profile.dat','manifest.txt'])
            config.write_text('MODE=check\nUSER_ID=1eb70483\nPACKAGE='+exported.name+'\nCONFIRM_GAME_CLOSED=yes\n')
            r=subprocess.run([str(ROOT/'share-host'),str(config),str(home),str(root)],env=dict(env,PSCLOUD_TEST_SHARE_PAYLOADS=str(target)),capture_output=True,timeout=10)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            for i,slot in enumerate(SLOTS):
                self.assertEqual((live/('sdimg_'+slot)).read_bytes(),originals)
                self.assertEqual((target/str(i)/'sce_sys/param.sfo').read_bytes(),metadata[i])
                self.assertEqual((target/str(i)/'ue4savegame.dpx.sav').read_bytes(),b'shared '+slot.encode())
            self.assertFalse((root/'.mount-active').exists());self.assertFalse((root/'.restore-active').exists())
