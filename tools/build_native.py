#!/usr/bin/env python3
"""Offline direct-source native firmware build. Only ROM data is generated."""
import argparse,os,hashlib,json,struct,subprocess
from pathlib import Path
levels={'OFF':0,'ERROR':1,'WARN':2,'INFO':3,'DEBUG':4,'TRACE':5}
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--log-level',type=str.upper,choices=levels,default='INFO',help='Initial level for all components (default INFO)')
parser.add_argument('--log-ceiling',type=str.upper,choices=levels,default='TRACE',help='Highest compiled level (default TRACE)')
args=parser.parse_args()
if levels[args.log_level]>levels[args.log_ceiling]:parser.error('--log-level cannot exceed --log-ceiling')
ROOT=Path(__file__).resolve().parents[1]
GUEST=Path(os.environ.get('SIDETNFS_GUEST_DIR', ROOT.parent/'SideTNFS-Gemdrive-integration'))
BASE=ROOT.parents[2]
SDK_SOURCE=Path(os.environ.get('SIDETNFS_SDK_SOURCE', ROOT))
out=ROOT/'build/falcon';out.mkdir(parents=True,exist_ok=True)
rom_path=GUEST/'dist/native/native.bin'
rom=rom_path.read_bytes()
def verify_uf2(data):
    assert data and len(data) % 512 == 0, "Invalid UF2 block size"
    for off in range(0, len(data), 512):
        block = data[off:off+512]
        magic0, magic1, flags, address, size, number, count, family = struct.unpack_from("<8I", block)
        assert (magic0, magic1) == (0x0a324655, 0x9e5d5157)
        assert struct.unpack_from("<I", block, 508)[0] == 0x0ab16f30
        assert flags == 0x2000 and family == 0xe48bff56, "Not an RP2040 UF2"
        assert number == off//512 and count == len(data)//512
        assert 0 < size <= 476
        assert 0x10000000 <= address and address+size <= 0x100de000, "UF2 overlaps reserved configuration/ROM flash"

env=os.environ.copy()
env.update({'PICO_SDK_PATH':os.environ.get('PICO_SDK_PATH',str(SDK_SOURCE/'pico-sdk')),'PICO_EXTRAS_PATH':os.environ.get('PICO_EXTRAS_PATH',str(SDK_SOURCE/'pico-extras')),
    'FATFS_SDK_PATH':os.environ.get('FATFS_SDK_PATH',str(SDK_SOURCE/'fatfs-sdk')),'BOARD_TYPE':'pico_w','DEBUG_MODE':'0',
    'SIDETNFS_ENABLE_DEBUG':'0','SIDETNFS_ENABLE_DIAG':'0','SIDETNFS_FORCE_CONFIG_RECOVERY':'0',
    'SIDETNFS_ENABLE_SD_SLOT_DUMP':'0','RELEASE_VERSION':'1.1.0-f37',
    'RELEASE_DATE':'2026-10-08 00:00:00','RELEASE_TYPE':'recovery','CMAKE_POLICY_VERSION_MINIMUM':'3.5'})
b=out
subprocess.run(['cmake','-S',str(ROOT/'romemul'),'-B',str(b),'-DCMAKE_BUILD_TYPE=RelWithDebInfo',
 '-DCMAKE_POLICY_VERSION_MINIMUM=3.5','-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
 '-DPICO_SDK_FETCH_FROM_GIT=OFF','-DPICO_EXTRAS_FETCH_FROM_GIT=OFF','-DPICO_BOARD=pico_w','-DSIDETNFS_NATIVE_FALCON=ON',
 f'-DSIDETNFS_LOG_COMPILED_LEVEL={levels[args.log_ceiling]}',f'-DSIDETNFS_LOG_DEFAULT_LEVEL={levels[args.log_level]}',f'-DSIDETNFS_NATIVE_ROM={rom_path}'],env=env,check=True)
subprocess.run(['cmake','--build',str(b),'--parallel','4'],env=env,check=True)
uf2=(b/'romemul.uf2').read_bytes();verify_uf2(uf2)
payload=b''.join(uf2[i+32:i+32+struct.unpack_from('<I',uf2,i+16)[0]] for i in range(0,len(uf2),512))
swapped=b''.join(rom[i:i+2][::-1] for i in range(0,len(rom),2))
assert payload.count(swapped)==1
config=Path(os.environ.get('SIDETNFS_CONFIG_PRG', ROOT.parent/'SideTNFS-Config/SIDETNFS.PRG')).read_bytes()
assert payload.count(config)==1
if levels[args.log_ceiling]>=levels['INFO']:
    assert b'F37 standalone GEMDRIVE + SIDEETH' in payload
artifact=ROOT/'dist/falcon';artifact.mkdir(parents=True,exist_ok=True)
(artifact/'sidetnfs_f37_falcon.uf2').write_bytes(uf2)
def digest(b):return hashlib.sha256(b).hexdigest()
def rev(root):return subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()
sources=[p for folder in ['romemul','tools','tests/native','docs','atari/sideeth'] for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.s','.py','.txt','.json','.md','.ld','.pio') and '__pycache__' not in p.parts]
manifest={'firmware_revision':rev(ROOT),'guest_revision':rev(GUEST),'hardware_tested':False,'logging':{'default':args.log_level,'compiled_ceiling':args.log_ceiling},
 'sdk_submodules':subprocess.check_output(['git','-C',str(SDK_SOURCE),'submodule','status','--recursive'],text=True).splitlines(),
 'uf2_sha256':digest(uf2),'rom_sha256':digest(rom),'config_sha256':digest(config),
 'source_sha256':{str(p.relative_to(ROOT)):digest(p.read_bytes()) for p in sources}}
(artifact/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Native UF2 verified:',len(uf2),'bytes',digest(uf2))
