#!/usr/bin/env python3
"""Build the RAM-only ARM32 screen test using project-local compiler settings."""
from pathlib import Path
import json, subprocess
root=Path(__file__).resolve().parents[1]
out=root/'work/display/live'
cfg=json.loads((out/'build-settings.json').read_text())
cmd=[cfg['compiler'],*cfg['base_arguments'],'-std=gnu11','-Wall','-Wextra','-Werror',
     str(root/'tools/screenapi_live_test.c'),str(root/'work/rootfs/lib/libdl.so.2'),
     *cfg['tail_arguments'],'-o',str(out/'screenapi-live-test.arm')]
subprocess.run(cmd,check=True)
print(out/'screenapi-live-test.arm')
