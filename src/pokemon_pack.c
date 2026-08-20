/* The sprite archive, laid into .rodata straight from the file the build
 * produced. `incbin` keeps a 1.6 MiB blob out of the C source. */
__asm__(".section .rodata\n"
        ".globl pixy_pokemon_pack\n"
        ".balign 8\n"
        "pixy_pokemon_pack:\n"
        ".incbin \"build/pokemon.pack\"\n"
        "pixy_pokemon_pack_end:\n"
        ".globl pixy_pokemon_pack_len\n"
        ".balign 4\n"
        "pixy_pokemon_pack_len:\n"
        ".int pixy_pokemon_pack_end - pixy_pokemon_pack\n"
        ".text\n");
