/* RP2350 (pico2_w) counterpart of memmap_romemul.ld.
   Based on GCC ARM embedded samples / pico-sdk's own
   src/rp2_common/pico_crt0/rp2350/memmap_default.ld.

   This is memmap_romemul.ld (the RP2040/pico_w script -- see that file for
   the SideTNFS-specific MEMORY regions and .rodata/.rom_in_ram additions,
   all preserved here unchanged) with exactly the deltas pico-sdk's own
   rp2040 vs rp2350 default templates carry between each other, needed
   because RP2350 boots differently from RP2040:
     - RP2350 has no boot-time "vector straight out of boot2" step; the
       bootrom instead enters via an IMAGE_DEF/"embedded block" (picobin
       metadata) that must live in the first 4096 bytes of flash, and the
       machine-readable .binary_info block gets a bigger 1024-byte (not
       256-byte) budget. .boot2 itself becomes optional/advisory (kept only
       for its XIP-setup code, reused by the debugger-launch path) and is
       moved after .text instead of before it, matching where the RP2350
       template places it.
     - RP2350's two single-cycle SCRATCH_X/SCRATCH_Y SRAM banks sit at
       0x20080000/0x20081000 (top of its larger 512K SRAM), not
       0x20040000/0x20041000 like RP2040's -- these addresses are fixed by
       the chip's memory map, not a project choice.
     - RAM/ROM_IN_RAM stay at the same addresses/sizes as the RP2040 script
       (128K + 128K): RP2350 has far more SRAM (512K) available, but this
       project doesn't need it, and reusing the exact same split avoids
       introducing new address-math this port hasn't exercised. The extra
       ~256K of RP2350 SRAM is simply left outside every MEMORY region here
       (unused, not unsafe).
     - CONFIG_FLASH/ROM_FLASH/SIDETNFS_CONFIG_FLASH (flash-side reservations)
       are untouched -- flash address space and size are unaffected by any
       of the above.
   See individual comments below for exactly which lines differ from
   memmap_romemul.ld.
*/

MEMORY
{
    FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 1912k
    RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 128k
    /* RP2350 delta: SCRATCH_X/SCRATCH_Y move to 0x20080000/0x20081000
       (fixed hardware addresses of RP2350's scratch SRAM banks -- RP2040's
       0x20040000/0x20041000 would just be ordinary RAM on RP2350, not the
       dedicated single-cycle scratch banks). */
    SCRATCH_X(rwx) : ORIGIN = 0x20080000, LENGTH = 4k
    SCRATCH_Y(rwx) : ORIGIN = 0x20081000, LENGTH = 4k
    CONFIG_FLASH(rwx): ORIGIN = 0x100DE000, LENGTH = 8k
    ROM_FLASH(rwx) : ORIGIN = 0x100E0000, LENGTH = 128k
    ROM_IN_RAM (rwx) : ORIGIN = 0x20020000, LENGTH = 128K
    /* Fase 9B2: SideTNFS server-list config sector. Immediately follows
       ROM_FLASH (which ends at exactly this address), matching the same
       documentation-only reservation style as CONFIG_FLASH/ROM_FLASH above
       -- none of these three are referenced by any SECTIONS output, they
       exist here so the address is visible and reserved against accidental
       reuse. See romemul/include/sidetnfs_config.h. */
    SIDETNFS_CONFIG_FLASH(rwx) : ORIGIN = 0x10100000, LENGTH = 4k
}

ENTRY(_entry_point)

SECTIONS
{
    .flash_begin : {
        __flash_binary_start = .;
    } > FLASH

    /* RP2350 delta: the bootrom enters via the IMAGE_DEF/"embedded block"
       inside .text below (found by scanning, not by a fixed jump through
       boot2 like on RP2040), so .boot2 moves to after .text and becomes
       optional -- see the ASSERT below. */

    .text : {
        __logical_binary_start = .;
        KEEP (*(.vectors))
        KEEP (*(.binary_info_header))
        __binary_info_header_end = .;
        /* RP2350 delta: the picobin IMAGE_DEF metadata block the bootrom
           scans for on startup (not present/needed on RP2040). */
        KEEP (*(.embedded_block))
        __embedded_block_end = .;
        KEEP (*(.reset))
        /* TODO revisit this now memset/memcpy/float in ROM */
        /* bit of a hack right now to exclude all floating point and time critical (e.g. memset, memcpy) code from
         * FLASH ... we will include any thing excluded here in .data below by default */
        *(.init)
        /* RP2350 delta: Arm Secure/Non-secure call veneer helper, needed
           for the M33's TrustZone-derived cmse support even though this
           project doesn't use non-secure calls itself -- pico-sdk's own
           rp2350 template always includes it. */
        *libgcc.a:cmse_nonsecure_call.o
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)
        *(.fini)
        /* Pull all c'tors into .text */
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        /* Followed by destructors */
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        *(.eh_frame*)
        . = ALIGN(4);
    } > FLASH

    /* RP2350 delta (moved here from before .text, and relaxed): boot2 is no
       longer the bootrom's entry vector on RP2350, just optional reusable
       XIP-setup code for the debugger-launch path, so it's no longer
       required to be exactly 256 bytes (nor even present) -- only bounded
       by the 256-byte boot-RAM staging area it would run from if used. */
    .boot2 : {
        __boot2_start__ = .;
        *(.boot2)
        __boot2_end__ = .;
    } > FLASH

    ASSERT(__boot2_end__ - __boot2_start__ <= 256,
        "ERROR: Pico second stage bootloader must be no more than 256 bytes in size")

    .rodata : {
        /* Fase 10A: sidetnfs_config_drive.c's embedded SIDETNFS.PRG/
           README.TXT arrays have no reader yet this phase (no configdrive
           backend) -- __attribute__((used)) alone was not enough to
           survive this toolchain's --gc-sections (verified: still absent
           from the linked ELF with only that attribute), so KEEP() them
           explicitly by their -fdata-sections section names, same
           technique already used for .mutex_array/.preinit_array/
           .init_array below. */
        KEEP(*(.rodata.sidetnfs_config_prg))
        KEEP(*(.rodata.sidetnfs_config_prg_length))
        KEEP(*(.rodata.sidetnfs_config_readme))
        KEEP(*(.rodata.sidetnfs_config_readme_length))
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .rodata*)
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.flashdata*)))
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH
    __exidx_end = .;

    /* Machine inspectable binary information */
    . = ALIGN(4);
    __binary_info_start = .;
    .binary_info :
    {
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
    } > FLASH
    __binary_info_end = .;
    . = ALIGN(4);

   .ram_vector_table (NOLOAD): {
        *(.ram_vector_table)
    } > RAM

    .data : {
        __data_start__ = .;
        *(vtable)

        *(.time_critical*)

        /* remaining .text and .rodata; i.e. stuff we exclude above because we want it in RAM */
        *(.text*)
        . = ALIGN(4);
        *(.rodata*)
        . = ALIGN(4);

        *(.data*)

        . = ALIGN(4);
        *(.after_data.*)
        . = ALIGN(4);
        /* preinit data */
        PROVIDE_HIDDEN (__mutex_array_start = .);
        KEEP(*(SORT(.mutex_array.*)))
        KEEP(*(.mutex_array))
        PROVIDE_HIDDEN (__mutex_array_end = .);

        . = ALIGN(4);
        /* preinit data */
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        /* init data */
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        /* finit data */
        PROVIDE_HIDDEN (__fini_array_start = .);
        *(SORT(.fini_array.*))
        *(.fini_array)
        PROVIDE_HIDDEN (__fini_array_end = .);

        *(.jcr)
        . = ALIGN(4);
        /* All data end */
        __data_end__ = .;
    } > RAM AT> FLASH
    /* __etext is (for backwards compatibility) the name of the .data init source pointer (...) */
    __etext = LOADADDR(.data);

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM

    /* Start and end symbols must be word-aligned */
    .scratch_x : {
        __scratch_x_start__ = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end__ = .;
    } > SCRATCH_X AT > FLASH
    __scratch_x_source__ = LOADADDR(.scratch_x);

    .scratch_y : {
        __scratch_y_start__ = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end__ = .;
    } > SCRATCH_Y AT > FLASH
    __scratch_y_source__ = LOADADDR(.scratch_y);

    /* RAM used for ROM_IN_RAM */
    .rom_in_ram : {
        __rom_in_ram_start__ = .;
        *(.rom_in_ram.*)
        . = ALIGN(4);
        __rom_in_ram_end__ = .;
    } > ROM_IN_RAM AT > FLASH
    __rom_in_ram_source__ = LOADADDR(.rom_in_ram);

    .bss  : {
        . = ALIGN(4);
        __bss_start__ = .;
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    .heap (NOLOAD):
    {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
        __HeapLimit = .;
    } > RAM

    /* .stack*_dummy section doesn't contains any symbols. It is only
     * used for linker to calculate size of stack sections, and assign
     * values to stack symbols later
     *
     * stack1 section may be empty/missing if platform_launch_core1 is not used */

    /* by default we put core 0 stack at the end of scratch Y, so that if core 1
     * stack is not used then all of SCRATCH_X is free.
     */
    .stack1_dummy (NOLOAD):
    {
        *(.stack1*)
    } > SCRATCH_X
    .stack_dummy (NOLOAD):
    {
        KEEP(*(.stack*))
    } > SCRATCH_Y

    /* RP2350 delta: the trailing embedded_end_block marker (part of the
       picobin metadata scan) and a fill byte (0xaa, matching pico-sdk's own
       rp2350 template) for any padding this section introduces. */
    .flash_end : {
        KEEP(*(.embedded_end_block*))
        PROVIDE(__flash_binary_end = .);
    } > FLASH =0xaa

    /* stack limit is poorly named, but historically is maximum heap ptr */
    __StackLimit = ORIGIN(RAM) + LENGTH(RAM) + LENGTH(ROM_IN_RAM);
    __StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);
    __StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);
    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);
    __StackBottom = __StackTop - SIZEOF(.stack_dummy);
    PROVIDE(__stack = __StackTop);

    /* Check if data + heap + stack exceeds RAM limit */
    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")

    /* RP2350 delta: 1024-byte binary_info budget (not 256) and a new
       4096-byte budget for the embedded_block metadata, matching
       pico-sdk's own rp2350 template ASSERTs. */
    ASSERT( __binary_info_header_end - __logical_binary_start <= 1024, "Binary info must be in first 1024 bytes of the binary")
    ASSERT( __embedded_block_end - __logical_binary_start <= 4096, "Embedded block must be in first 4096 bytes of the binary")
    /* todo assert on extra code */

    /* Fase 10A: __flash_binary_end (.text/.rodata/.data, including the
       embedded sidetnfs_config_drive.c arrays) must stay below
       CONFIG_FLASH's fixed origin (0x100DE000, see CONFIG_FLASH above and
       CONFIG_FLASH_OFFSET in romemul/constants.c) -- CONFIG_FLASH is
       accessed directly via XIP_BASE + CONFIG_FLASH_OFFSET at runtime
       (romemul/config.c), not through a SECTIONS placement the linker
       would otherwise catch, so nothing else stops ordinary code/rodata
       growth from silently overrunning it. This turns that into a build
       failure instead of runtime flash corruption. */
    ASSERT( __flash_binary_end <= ORIGIN(CONFIG_FLASH), "flash_end has grown into CONFIG_FLASH (0x100DE000) -- reduce embedded content or move CONFIG_FLASH")
}
