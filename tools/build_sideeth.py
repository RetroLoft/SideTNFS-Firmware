from pathlib import Path
import os,io,tarfile,subprocess,hashlib,json
r=Path(__file__).resolve().parents[1];a=r/'atari/sideeth';h=Path(os.environ.get('STING_INCLUDE',r.parents[2].parent/'Atari/STinG/include'))
e={p.name:p.read_bytes() for p in a.iterdir() if p.suffix in ['.h','.c']};e.update({'include/'+p.name:p.read_bytes() for p in h.glob('*.h')});stream=io.BytesIO()
with tarfile.open(fileobj=stream,mode='w') as t:
 for n,b in e.items():
  i=tarfile.TarInfo(n);i.size=len(b);t.addfile(i,io.BytesIO(b))
cmd='cd /buildsrc && tar xf - && m68k-atari-mint-gcc -m68000 -std=gnu99 -O2 -fomit-frame-pointer -Wall -Wextra -Iinclude -I. -s sideeth.c cart.c arp.c tools.c -o SIDEETH.STX >&2 && cat SIDEETH.STX'
p=subprocess.run(['docker','run','--rm','--pull=never','--network=none','--read-only','--tmpfs','/buildsrc:rw,exec','--tmpfs','/tmp:rw,exec','-i','--entrypoint','sh',os.environ.get('SIDETNFS_TOOLCHAIN_IMAGE','304fea9c3e8e'),'-ec',cmd],input=stream.getvalue(),stdout=subprocess.PIPE,check=True)
out=r/'dist/sideeth';out.mkdir(parents=True,exist_ok=True);(out/'SIDEETH.STX').write_bytes(p.stdout);assert p.stdout[:2]==b'\x60\x1a';(out/'manifest.json').write_text(json.dumps({'bytes':len(p.stdout),'sha256':hashlib.sha256(p.stdout).hexdigest(),'sources':{k:hashlib.sha256(v).hexdigest() for k,v in e.items()},'headers_directory':str(h),'toolchain':'304fea9c3e8e'},indent=2)+'\n');print('STX bytes',len(p.stdout))
