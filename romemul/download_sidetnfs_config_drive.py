"""Fase 10A: generate sidetnfs_config_drive.c, the byte-exact C embedding of
the SideTNFS configuration drive's contents (SIDETNFS.PRG and README.TXT).

Follows the style of download_firmware.py, but deliberately does NOT do any
endian conversion or word-packing: firmwareROM/gemdrvemulROM etc. hold
16-bit Atari ROM images and are intentionally packed into uint16_t words,
but SIDETNFS.PRG and README.TXT must stay byte-for-byte identical to their
source files, so both are emitted as plain const uint8_t arrays.

Not run automatically by the build (see romemul/CMakeLists.txt) -- like
firmware.c, the generated sidetnfs_config_drive.c is committed so offline
builds stay reproducible.
"""

import argparse
import hashlib
import os
import urllib.request

BYTES_PER_LINE = 16

DEFAULT_PRG_URL = (
    "https://github.com/RetroLoft/SideTNFS-Config/releases/latest/download/SIDETNFS.PRG"
)
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_README_PATH = os.path.join(SCRIPT_DIR, "assets", "config_drive", "README.TXT")
DEFAULT_OUTPUT_PATH = os.path.join(SCRIPT_DIR, "sidetnfs_config_drive.c")


def download_binary(url):
    with urllib.request.urlopen(url) as response:
        return response.read()


def read_binary_from_file(file_path):
    with open(file_path, "rb") as f:
        return f.read()


def load_binary(input_source):
    if input_source.startswith(("http://", "https://")):
        return download_binary(input_source)
    return read_binary_from_file(input_source)


def bytes_to_c_array(data, array_name):
    """Byte-exact, no endian conversion: each source byte becomes exactly
    one array element, in file order."""
    lines = []
    for i in range(0, len(data), BYTES_PER_LINE):
        chunk = data[i : i + BYTES_PER_LINE]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    body = "\n".join(lines)
    if body:
        body = body.rstrip(",") + ","
    # __attribute__((used)): nothing reads these arrays yet this phase (no
    # configdrive backend), and this project's build enables
    # -ffunction-sections/-fdata-sections with the Pico SDK's usual
    # --gc-sections link step -- without `used`, the linker discards a
    # const array with no live reference, silently defeating the whole
    # point of embedding it. Verified by inspecting the ELF (see report).
    return (
        f"const uint8_t {array_name}[] __attribute__((used)) = {{\n{body}\n}};\n"
        f"const uint32_t {array_name}_length __attribute__((used)) = sizeof({array_name});\n"
    )


def array_comment_header(label, data):
    sha256 = hashlib.sha256(data).hexdigest()
    return (
        f"// {label}\n"
        f"// length: {len(data)} bytes\n"
        f"// sha256: {sha256}\n"
    )


def generate(prg_source, readme_path, output_path):
    prg_data = load_binary(prg_source)
    readme_data = load_binary(readme_path)

    content = '#include "include/sidetnfs_config_drive.h"\n\n'

    content += array_comment_header(f"SIDETNFS.PRG, from {prg_source}", prg_data)
    content += bytes_to_c_array(prg_data, "sidetnfs_config_prg")
    content += "\n"

    content += array_comment_header(f"README.TXT, from {readme_path}", readme_data)
    content += bytes_to_c_array(readme_data, "sidetnfs_config_readme")
    content += "\n"

    content += (
        "// Fase 10A compile-time guarantees (see sidetnfs_config_drive.h for\n"
        "// the two size ceilings) -- both files present, combined content\n"
        "// within the flash budget, and each individually small enough to fit\n"
        "// the GEMDOS file-size fields a future configdrive backend will use.\n"
        '_Static_assert(sizeof(sidetnfs_config_prg) > 0, "sidetnfs_config_prg must not be empty");\n'
        '_Static_assert(sizeof(sidetnfs_config_readme) > 0, "sidetnfs_config_readme must not be empty");\n'
        "_Static_assert(sizeof(sidetnfs_config_prg) + sizeof(sidetnfs_config_readme) <= SIDETNFS_CONFIG_DRIVE_MAX_TOTAL_BYTES,\n"
        '               "combined SIDETNFS.PRG + README.TXT content exceeds the flash budget");\n'
        "_Static_assert(sizeof(sidetnfs_config_prg) <= SIDETNFS_CONFIG_DRIVE_MAX_FILE_BYTES,\n"
        '               "sidetnfs_config_prg does not fit in a GEMDOS file-size field");\n'
        "_Static_assert(sizeof(sidetnfs_config_readme) <= SIDETNFS_CONFIG_DRIVE_MAX_FILE_BYTES,\n"
        '               "sidetnfs_config_readme does not fit in a GEMDOS file-size field");\n'
    )

    with open(output_path, "w", newline="\n") as f:
        f.write(content)

    print(f"{output_path} generated successfully!")
    print(f"  sidetnfs_config_prg:    {len(prg_data)} bytes, sha256={hashlib.sha256(prg_data).hexdigest()}")
    print(f"  sidetnfs_config_readme: {len(readme_data)} bytes, sha256={hashlib.sha256(readme_data).hexdigest()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Generate sidetnfs_config_drive.c from SIDETNFS.PRG and README.TXT"
    )
    parser.add_argument(
        "--prg",
        default=DEFAULT_PRG_URL,
        help="SIDETNFS.PRG source: URL or local file path "
        "(e.g. --prg /home/frank/retro/sidecart/AtariConfig/SIDETNFS.PRG for development)",
    )
    parser.add_argument(
        "--readme",
        default=DEFAULT_README_PATH,
        help="README.TXT source file path",
    )
    parser.add_argument(
        "--output",
        default=DEFAULT_OUTPUT_PATH,
        help="Output .c file path",
    )
    args = parser.parse_args()

    generate(args.prg, args.readme, args.output)
