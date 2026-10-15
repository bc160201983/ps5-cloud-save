"""Embed a file into a C header as <name>[] and <name>_size (used for the trusted certificate bundle)."""
from pathlib import Path
import sys

source, destination, name = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
data = source.read_bytes()
if not data:
    raise SystemExit(f'{source} is empty')
rows = [','.join(str(b) for b in data[i:i+24]) for i in range(0, len(data), 24)]
destination.write_text('#include <stddef.h>\nstatic const unsigned char ' + name + '[] = {\n' + ',\n'.join(rows) +
                       '\n};\nstatic const size_t ' + name + '_size = sizeof ' + name + ';\n')
