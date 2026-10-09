from pathlib import Path
import io,tarfile,subprocess,hashlib,os
root=Path(__file__).resolve().parent
stream=io.BytesIO()
with tarfile.open(fileobj=stream,mode='w') as t:
 for name in ('start.s','rload.c','assets.h'):
  b=(root/name).read_bytes();i=tarfile.TarInfo(name);i.size=len(b);t.addfile(i,io.BytesIO(b))
cmd='''cd /tmp; tar xf -
vasm -Faout -quiet -m68000 -spaces -devpac start.s -o start.o >&2
m68k-atari-mint-gcc -m68000 -std=c99 -Os -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror -c rload.c -o rload.o >&2
vlink -s -bataritos start.o rload.o $(m68k-atari-mint-gcc -m68000 -print-libgcc-file-name) -o RLOAD4.TOS >&2
cat RLOAD4.TOS'''
r=subprocess.run(['docker','run','--rm','--pull=never','--network=none','--read-only','--tmpfs','/tmp:rw,exec','-i','--entrypoint','sh',os.environ.get('SIDETNFS_TOOLCHAIN_IMAGE','304fea9c3e8e'),'-ec',cmd],input=stream.getvalue(),capture_output=True)
print(r.stderr.decode());r.check_returncode();assert r.stdout[:2]==b'\x60\x1a';(root/'RLOAD4.TOS').write_bytes(r.stdout);print(len(r.stdout),hashlib.sha256(r.stdout).hexdigest())
