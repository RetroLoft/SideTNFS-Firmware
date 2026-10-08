#!/usr/bin/env python3
"""Run native guest, SD, NTP and TNFS host regressions."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
g=Path(os.environ.get('SIDETNFS_GUEST_DIR',root.parent/'SideTNFS-Gemdrive-integration'))
r=root/'romemul/native'
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'hardware').mkdir();(p/'hardware/rtc.h').write_text('');(p/'lwip').mkdir()
 for h in ['udp.h','dns.h']:(p/'lwip'/h).write_text('')
 s=(r/'sd_protocol.h').read_text();(p/'pattern.inc').write_text(s[s.index('static unsigned r03_up'):s.index('static const char *r03_shortname')])
 for src in [*sorted((g/'tests/native').glob('*.c')),*sorted((root/'tests/native').glob('*.c'))]:
  flags=['-DR13A=1','-DR13B=1','-DR13C=1','-DR13D=1','-DR13E=1'] if src.name.endswith('wire.c') else []
  subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',*flags,'-I',str(p),'-I',str(r),'-I',str(g/'src/native'),'-I',str(root/'romemul'),*([str(r/'config_drive.c')] if src.name=='test_settings.c' else []),str(src),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
  if src.name=='test_logging.c':
   subprocess.run(['cc','-Os','-std=c99','-Wall','-Wextra','-Werror','-DSIDETNFS_LOG_COMPILED_LEVEL=3','-I',str(root/'romemul'),str(src),'-o',str(p/'test-info')],check=True)
   subprocess.run([str(p/'test-info')],check=True)
   assert b'compiled out %u' not in (p/'test-info').read_bytes()
   subprocess.run(['cc','-Os','-std=c99','-Wall','-Wextra','-Werror','-DSIDETNFS_LOG_COMPILED_LEVEL=0','-DSIDETNFS_LOG_DEFAULT_LEVEL=0','-I',str(root/'romemul'),str(src),'-o',str(p/'test-off')],check=True)
   subprocess.run([str(p/'test-off')],check=True)
   assert b'compiled off %u' not in (p/'test-off').read_bytes()

for name in ["test_rom_dma.py","test_fd_sort.py","test_config_protocol.py"]:
 subprocess.run(["python3",str(root/"tools"/name)],check=True)

subprocess.run(["python3",str(root/"tools/test_sideeth20.py")],check=True)

subprocess.run(["python3",str(root/"tools/test_sideeth_client.py")],check=True)

subprocess.run(["python3",str(root/"tools/test_sideeth_routing.py")],check=True)

subprocess.run(["python3",str(root/"tools/test_sideeth21_nat.py")],check=True)
