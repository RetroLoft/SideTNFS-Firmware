"""Encode a built cartridge as C data; never patch its instructions."""
from pathlib import Path
import struct,sys
rom=Path(sys.argv[1]).read_bytes()
assert rom[:4]==bytes.fromhex('abcdef42') and len(rom)%2==0 and len(rom)<=65536
words=struct.unpack(f'>{len(rom)//2}H',rom)
Path(sys.argv[2]).write_text('#include "include/firmware_gemdrvemul.h"\nconst uint16_t gemdrvemulROM[]={\n'+
    '\n'.join(','.join(f'0x{w:04x}' for w in words[i:i+16])+',' for i in range(0,len(words),16))+
    '\n};\nuint16_t gemdrvemulROM_length=sizeof(gemdrvemulROM)/sizeof(gemdrvemulROM[0]);\n')
