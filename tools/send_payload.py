#!/usr/bin/env python3
"""Send a PS5 SDK ELF to elfldr. A successful send does not confirm execution."""
import argparse
from pathlib import Path
import socket
import sys

def send(host, port, path):
    with Path(path).open('rb') as f:
        header=f.read(20)
        # ELF64 little endian, x86-64. Host executables may also match; use only SDK builds.
        if len(header)<20 or header[:6]!=b'\x7fELF\x02\x01' or header[18:20]!=b'\x3e\x00':
            raise ValueError('Expected an ELF64 little-endian x86-64 payload')
        f.seek(0)
        with socket.create_connection((host,port),timeout=15) as s:
            while chunk:=f.read(65536):
                s.sendall(chunk)
            s.shutdown(socket.SHUT_WR)
    print('Payload sent. Check the console report to confirm execution.')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('host')
    p.add_argument('payload',type=Path)
    p.add_argument('--port',type=int,default=9021)
    a=p.parse_args()
    try: send(a.host,a.port,a.payload)
    except (OSError,ValueError) as e:
        print(f'Not sent: {e}',file=sys.stderr)
        return 1
    return 0
if __name__=='__main__': sys.exit(main())
