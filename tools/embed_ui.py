"""Embed the dashboard HTML and release version into a C header."""
from pathlib import Path
import json
import sys

source, destination, version = map(Path, sys.argv[1:4])
release=version.read_text().strip()
data = source.read_text(encoding='utf-8').replace('__VERSION__', release).encode('utf-8')
rows = [','.join(str(b) for b in data[i:i+24]) for i in range(0,len(data),24)]
destination.write_text('#include <stddef.h>\n#define PSCLOUD_VERSION '+json.dumps(release)+'\nstatic const unsigned char pscloud_ui[] = {\n'+
                       ',\n'.join(rows)+'\n};\nstatic const size_t pscloud_ui_size = sizeof pscloud_ui;\n')
