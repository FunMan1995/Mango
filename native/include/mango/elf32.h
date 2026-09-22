#ifndef MANGO_ELF32_H_
#define MANGO_ELF32_H_

#include <stdint.h>

#define MANGO_ELF32_MAX_SEGMENTS 16

/* A single PT_LOAD segment: copy filesz bytes from file_offset into guest
 * memory at vaddr, zero the remaining (memsz - filesz) bytes after it (the
 * standard ELF convention for a segment's .bss tail). */
typedef struct MangoElf32Segment {
  uint32_t vaddr;
  uint32_t memsz;
  uint32_t file_offset;
  uint32_t filesz;
  int executable;
} MangoElf32Segment;

#define MANGO_R_ARM_ABS32 2u
#define MANGO_R_ARM_GLOB_DAT 21u
#define MANGO_R_ARM_JUMP_SLOT 22u
#define MANGO_R_ARM_RELATIVE 23u

typedef struct MangoElf32Image {
  const uint8_t* data; /* caller-owned, must outlive this; not copied */
  uint32_t size;
  uint32_t entry;
  MangoElf32Segment segments[MANGO_ELF32_MAX_SEGMENTS];
  uint32_t segment_count;
  uint32_t symtab_offset; /* .dynsym, an Elf32_Sym[symtab_count] array */
  uint32_t symtab_count;
  uint32_t strtab_offset; /* .dynstr, the names symtab entries index into */
  uint32_t rel_vaddr;     /* DT_REL, guest vaddr of Elf32_Rel[] */
  uint32_t rel_size;
  uint32_t jmprel_vaddr; /* DT_JMPREL */
  uint32_t jmprel_size;
} MangoElf32Image;

/* Resolve an imported (UND) symbol to a guest address. Defined symbols
 * skip this and use st_value + load_bias. Return 0 to leave the slot as 0. */
typedef uint32_t (*MangoElf32ResolveFn)(void* ctx, const char* name, uint32_t st_value,
                                        uint32_t st_shndx);

/* Parses `data`/`size` as an ELF32 shared object built for
 * `expected_machine` (an ELF e_machine value, e.g. 40 for EM_ARM). Reads
 * PT_LOAD segments from the program header table and .dynsym/.dynstr from
 * the section header table (so, unlike a real OS loader, this needs
 * section headers to be present; stripping them entirely is rare in
 * practice but does exist, see native/README.md). 0 on success, -1 if
 * it's not a supported ELF32 image for that machine. */
int mango_elf32_parse(const uint8_t* data, uint32_t size, uint16_t expected_machine,
                      MangoElf32Image* out);

/* A symbol's value: for a function, its entry address relative to this
 * image's own vaddr space, same convention as MangoElf32Segment.vaddr.
 * 0 if `name` isn't in .dynsym; this is a linear scan, not a hash lookup,
 * fine for the small libraries this project currently deals with. */
uint32_t mango_elf32_find_symbol(const MangoElf32Image* image, const char* name);

/* Apply R_ARM_RELATIVE / GLOB_DAT / JUMP_SLOT / ABS32 against `mem`, which
 * is the already-copied PT_LOAD image (vaddr 0 at mem[0]). load_bias is
 * added to RELATIVE and defined-symbol addresses; 0 is correct when guest
 * pointers are vaddrs. 0 on success, -1 on a reloc outside mem. */
int mango_elf32_apply_relocs(const MangoElf32Image* image, uint8_t* mem, uint32_t mem_size,
                             uint32_t load_bias, MangoElf32ResolveFn resolve, void* ctx);

#endif /* MANGO_ELF32_H_ */
