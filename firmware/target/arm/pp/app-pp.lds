#define __ASSEMBLER__
#include "cpu.h"

/* Will have been included from app.lds */
ENTRY(start)

OUTPUT_FORMAT(elf32-littlearm)
OUTPUT_ARCH(arm)
STARTUP(target/arm/pp/crt0-pp.o)

#define PLUGINSIZE PLUGIN_BUFFER_SIZE
#define CODECSIZE CODEC_SIZE

#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGINSIZE - CODECSIZE

#define DRAMORIG 0x00000000
#define IRAMORIG 0x40000000
#define IRAMSIZE 0xc000

/* End of the audio buffer, where the codec buffer starts */
#define ENDAUDIOADDR  (DRAMORIG + DRAMSIZE)

/* Where the codec buffer ends, and the plugin buffer starts */
#define ENDADDR (ENDAUDIOADDR + CODECSIZE)

MEMORY
{
    DRAM : ORIGIN = DRAMORIG,     LENGTH = DRAMSIZE
    IRAM : ORIGIN = IRAMORIG,     LENGTH = IRAMSIZE
}

SECTIONS
{
    .text :
    {
        loadaddress = .;
        _loadaddress = .;
        . = ALIGN(0x200);
        *(.init.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(0x4);
    } > DRAM

    .rodata :
    {
        *(.rodata)  /* problems without this, dunno why */
        *(.rodata*)
        *(.rodata.str1.1)
        *(.rodata.str1.4)
        . = ALIGN(0x4);
    } > DRAM

    .data :
    {
        *(.data*)
        . = ALIGN(0x4);
    } > DRAM

#if NOCACHE_BASE != 0
    /* .ncdata section is placed at uncached physical alias address and is
     * loaded at the proper cached virtual address - no copying is
     * performed in the init code */
    .ncdata . + NOCACHE_BASE :
    {
        . = ALIGN(CACHEALIGN_SIZE);
        *(.ncdata*)
        . = ALIGN(CACHEALIGN_SIZE);
    } AT> DRAM
#endif
    
#if defined(IPOD_VIDEO) && CONFIG_CPU == PP5022
    /* bcm overlay v125: the second copy of the CABAC -> CAVLC transcoder (apps/bcm_cab2cav_iram.c), linked to run at
     * the start of the IRAM Rockbox's codecs and plugins use (0x4000c000, 80 KB).  Nothing copies it at boot: the load
     * image stays in SDRAM (before .vectors, so .bss never overlays it) and debug_menu.c copies it in while a video
     * plays with Rockbox's playback stopped. */
    .c2ciram 0x4000c000 :
    {
        _c2ciram_start = .;
        *(.c2ciram.text*)
        *(.c2ciram.rodata*)
        *(.c2ciram.bss*)
        . = ALIGN(0x4);
        _c2ciram_end = .;
    } AT> DRAM
    _c2ciram_copy = LOADADDR(.c2ciram);
    ASSERT(_c2ciram_end <= 0x40020000, "bcm: .c2ciram is larger than the codec IRAM")
#endif

    /DISCARD/ :
    {
        *(.eh_frame)
    }

    .vectors 0x0 :
    {
        _vectorsstart = .;
        KEEP(*(.vectors));
        _vectorsend = .;
    } AT> DRAM

    _vectorscopy = LOADADDR(.vectors);
    _noloaddram  = LOADADDR(.vectors);

    .ibss IRAMORIG (NOLOAD) :
    {
        _iedata = .;
        *(.qharray)
        *(.ibss*)
        . = ALIGN(0x4);
        _iend = .;
    } > IRAM

    .iram _iend :
    {
        _iramstart = .;
        *(.icode*)
        *(.irodata*)
        *(.idata*)
        . = ALIGN(0x4);
        _iramend = .;
    } > IRAM AT> DRAM

    _iramcopy = LOADADDR(.iram);


    .init ENDAUDIOADDR : 
    {
        . = ALIGN(4);
        _initstart = .;
        *(.init*)
        _initend = .;
    } AT> DRAM

    _initcopy = LOADADDR(.init);

    .idle_stacks (NOLOAD) :
    {
       *(.idle_stacks)
       . = ALIGN(8);
#if NUM_CORES > 1
       cpu_idlestackbegin = .;
       . += IDLE_STACK_SIZE;
       cpu_idlestackend = .;
#endif
       cop_idlestackbegin = .;
       . += IDLE_STACK_SIZE;
       cop_idlestackend = .;
    } > IRAM

    .stack (NOLOAD) :
    {
       *(.stack)
       stackbegin = .;
       . += 0x2000;
       stackend = .;
    } > IRAM
    
    /* .bss and .ncbss are treated as a single section to use one init loop to
     * zero it - note "_edata" and "_end" */
    .bss _noloaddram (NOLOAD) :
    {
       _edata = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(0x4);
    } > DRAM

#if NOCACHE_BASE != 0
    .ncbss . + NOCACHE_BASE (NOLOAD):
    {
    	. = ALIGN(CACHEALIGN_SIZE);
        *(.ncbss*)
    	. = ALIGN(CACHEALIGN_SIZE);
    } AT> DRAM
#endif

    /* This will be aligned by preceding alignments */
    .endaddr . - NOCACHE_BASE (NOLOAD) :
    {
        _end = .;
    } > DRAM

    .audiobuf _end (NOLOAD) :
    {
        _audiobuffer = .;
        . = ALIGN(0x4);
        audiobuffer = .;
    } > DRAM
    
    .audiobufend ENDAUDIOADDR (NOLOAD) :
    {
#ifdef IPOD_VIDEO
        audiobufend_lds = .;
#else
        audiobufend = .;
#endif
        _audiobufend = .;
    } > DRAM

    .codec ENDAUDIOADDR (NOLOAD) :
    {
        codecbuf = .;
        _codecbuf = .;
    }

    .plugin ENDADDR (NOLOAD) :
    {
        _pluginbuf = .;
        pluginbuf = .;
    }
}
