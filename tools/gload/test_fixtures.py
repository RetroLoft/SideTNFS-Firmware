"""Check generated files against the manifest and the actual Atari CRC routine."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from generate_fixtures import ROOT, fixtures, metadata

assert metadata() == json.loads((ROOT / 'assets.json').read_text())
source = (ROOT / 'rload.c').read_text()
crc = source[source.index('static uint32_t crc('):source.index('static int stopped(')]
code = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
struct asset {const char *path; uint32_t size,crc;};
#include "assets.h"
static uint32_t table[256];
''' + crc + r'''
int main(int argc,char **argv){
 assert(argc==7);
 for(unsigned i=0;i<256;i++){
  uint32_t c=i;
  for(unsigned j=0;j<8;j++)c=c>>1^((c&1)?0xedb88320UL:0);
  table[i]=c;
 }
 for(unsigned i=0;i<6;i++){
  FILE *f=fopen(argv[i+1],"rb");assert(f);
  uint8_t *data=malloc(assets[i].size);assert(data);
  assert(fread(data,1,assets[i].size,f)==assets[i].size);
  assert(fgetc(f)==EOF);assert(!ferror(f));fclose(f);
  assert(crc(data,assets[i].size)==assets[i].crc);
  data[assets[i].size/2]^=1;
  assert(crc(data,assets[i].size)!=assets[i].crc);
  free(data);
 }
}
'''
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    dest = root / 'data'
    command = [sys.executable, str(ROOT / 'generate_fixtures.py'), str(dest)]
    subprocess.run(command, check=True)
    subprocess.run(command, check=True)  # Identical files can be regenerated.
    paths = []
    for name, data in fixtures():
        path = dest / name
        assert path.read_bytes() == data
        paths.append(str(path))
    (root / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT), str(root / 'test.c'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test'), *paths], check=True)
    Path(paths[0]).write_bytes(b'keep existing data')
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode != 0 and 'Refusing to overwrite' in result.stderr
    assert Path(paths[0]).read_bytes() == b'keep existing data'
print('PASS: all six fixture sizes/checksums, Atari CRCs, corruption detection and overwrite protection')
