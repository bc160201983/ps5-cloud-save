from pathlib import Path
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[1]


class TransferBudgetTests(unittest.TestCase):
    def test_size_budget_is_bounded_and_preserves_small_transfer_limit(self):
        code=r'''
#include <assert.h>
#include "ps5/common/transfer.h"
int main(void) {
    assert(pscloud_transfer_timeout(9ULL*1024*1024)==300);
    assert(pscloud_transfer_timeout(342ULL*1024*1024)>2700);
    assert(pscloud_transfer_timeout(512ULL*1024*1024)<7200);
    assert(pscloud_transfer_timeout(~0ULL)==7200);
    return 0;
}
'''
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                        '-DPSCLOUD_EMBEDDED','-DPSCLOUD_HOST_TEST','-I',str(ROOT),
                        '-x','c','-','-x','none',str(ROOT/'src/worker.c'),
                        str(ROOT/'ps5/common/snapshot.c'),str(ROOT/'ps5/common/log.c'),
                        '-o',str(ROOT/'transfer-host'),'-lcurl','-lcrypto'],
                       input=code,text=True,check=True)
        subprocess.run([str(ROOT/'transfer-host')],check=True)
