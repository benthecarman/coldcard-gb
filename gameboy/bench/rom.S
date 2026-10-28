// the ROM under test, built in (path from the Makefile)
    .section .rodata
    .balign 4
    .global rom_image, rom_image_end
rom_image:
    .incbin ROM_PATH
rom_image_end:
