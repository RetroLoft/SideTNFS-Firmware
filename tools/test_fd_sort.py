"""Compile the real upstream descriptor allocator and test out-of-order gaps."""
from pathlib import Path
import tempfile,subprocess
r=Path(__file__).resolve().parents[1]
s=(r/'romemul/gemdrvemul.c').read_text();a=s.index('static int __not_in_flash_func(compare_fd)');b=s.index('// payloadPtr, dpath_string_table',a)
code='''#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#define __not_in_flash_func(x) x
#define DPRINTF(...) ((void)0)
#define FIRST_FILE_DESCRIPTOR 80
typedef struct F { uint16_t fd; struct F *next; } FileDescriptors;
'''+s[a:b]+'''
int main(void){
 assert(get_first_available_fd(NULL)==80);
 FileDescriptors c={81,NULL},b={83,&c},a={80,&b};
 assert(get_first_available_fd(&a)==82);
 b.fd=82;assert(get_first_available_fd(&a)==83);
 uint16_t hi=65535,lo=1;assert(compare_fd(&hi,&lo)>0 && compare_fd(&lo,&hi)<0 && compare_fd(&hi,&hi)==0);
 puts("Descriptor allocator: unsigned 16-bit ordering, unsorted gap and contiguous allocation pass.");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'t.c').write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',str(p/'t.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
