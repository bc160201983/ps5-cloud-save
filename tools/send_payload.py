#!/usr/bin/env python3
"""Send a PS5 SDK ELF to elfldr and display loader/payload output."""
import argparse
from pathlib import Path
import socket
import struct
import sys


def validate_elf(f):
    header = f.read(64)
    if (len(header) != 64 or header[:6] != b'\x7fELF\x02\x01'
            or header[18:20] != b'\x3e\x00'):
        raise ValueError('Expected an ELF64 little-endian x86-64 payload, not a ZIP')
    f.seek(0, 2)
    size = f.tell()
    shoff = struct.unpack_from('<Q', header, 40)[0]
    shentsize, shnum = struct.unpack_from('<HH', header, 58)
    # elfldr_read requires a complete 64-byte section header table.
    if not shnum or shentsize != 64 or shoff < 64 or shoff + shnum * 64 > size:
        raise ValueError('Incomplete/unsupported ELF section table; use the SDK-built artifact')
    f.seek(shoff)
    for _ in range(shnum):
        section = f.read(64)
        kind = struct.unpack_from('<I', section, 4)[0]
        offset, length = struct.unpack_from('<QQ', section, 24)
        if kind != 8 and offset + length > size:  # SHT_NOBITS has no file bytes
            raise ValueError('Truncated ELF section; extract the artifact ZIP again')
    f.seek(0)
    return size


def send(host, port, path, connect_timeout=15, transfer_timeout=60, reply_timeout=10):
    with Path(path).open('rb') as f:
        size = validate_elf(f)
        print(f'ELF validated: {size:,} bytes', flush=True)
        print(f'Connecting to {host}:{port} (timeout {connect_timeout:g}s)...', flush=True)
        try:
            s = socket.create_connection((host, port), timeout=connect_timeout)
        except TimeoutError as e:
            raise ConnectionError(f'CONNECT timed out to {host}:{port}; no payload bytes were sent') from e
        except OSError as e:
            raise ConnectionError(f'CONNECT failed to {host}:{port}: {e}; no payload bytes were sent') from e
        with s:
            print(f'Connected. Uploading (socket timeout {transfer_timeout:g}s)...', flush=True)
            s.settimeout(transfer_timeout)
            sent = 0
            try:
                while chunk := f.read(16384):
                    view = memoryview(chunk)
                    while view:
                        n = s.send(view)
                        if n == 0:
                            raise ConnectionError('Socket closed during upload')
                        sent += n
                        view = view[n:]
                s.shutdown(socket.SHUT_WR)
            except OSError as e:
                raise ConnectionError(f'UPLOAD failed after {sent:,}/{size:,} bytes: {e}') from e
            print(f'Sent {sent:,}/{size:,} bytes. Reading loader output...', flush=True)
            s.settimeout(reply_timeout)
            reply = bytearray()
            try:
                while len(reply) < 65536:
                    chunk = s.recv(min(4096, 65536 - len(reply)))
                    if not chunk:
                        break
                    reply.extend(chunk)
            except TimeoutError:
                print('Reply wait ended; verify the report on the PS5.', flush=True)
            except OSError as e:
                print(f'Reply connection ended: {e}. Verify the PS5 report.', flush=True)
            if reply:
                text = reply.decode('utf-8', errors='replace').replace('\x00', '')
                print('Loader/payload output:\n' + text, flush=True)
                if '[elfldr.elf] Error' in text or '[elfldr.elf] Unknown' in text:
                    raise RuntimeError('elfldr reported an error; see output above')
            else:
                print('No loader output received.', flush=True)
    print('Transmission finished. Confirm execution using the PS5 notification, '
          '/data/pscloud.log, or the diagnostic report for a probe payload.')


def positive(value):
    result = float(value)
    if not 0 < result <= 3600:
        raise argparse.ArgumentTypeError('Timeout must be greater than 0 and at most 3600 seconds')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('host')
    p.add_argument('payload', type=Path)
    p.add_argument('--port', type=int, default=9021)
    p.add_argument('--connect-timeout', type=positive, default=15)
    p.add_argument('--timeout', type=positive, default=60, help='Upload socket timeout in seconds')
    p.add_argument('--reply-timeout', type=positive, default=10)
    a = p.parse_args()
    if not 1 <= a.port <= 65535:
        p.error('Port must be 1..65535')
    try:
        send(a.host, a.port, a.payload, a.connect_timeout, a.timeout, a.reply_timeout)
    except (OSError, ValueError, RuntimeError) as e:
        print(f'Failed: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
