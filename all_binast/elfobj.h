/* elfobj.h -- minimal ELF64 writer: build a real, runnable executable from
 * sections we place ourselves. No assembler, no linker, no system headers
 * beyond the C library.
 *
 * Used by blink to turn a patch (patch a word / append code) into a new ELF.
 * Deliberately explicit: every header field is written out longhand so the
 * output can be diffed against what a system linker produces.
 */
#ifndef ELFOBJ_H
#define ELFOBJ_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ALLOCATION. Every malloc/calloc/realloc/free in this file resolves to arbit (the CCP
 * cosmic sea) through arbishim.h. There is no libc allocator in the build, and `make test`
 * greps the tree and fails if one comes back. The family is mapped by macro so the call
 * sites stay as written -- the same idiom cygemm/ccp_compat.h uses. */
#include "arbishim.h"

#define EI_NIDENT 16

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr_t;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr_t;

typedef struct {
    Elf64_Ehdr_t eh;
    Elf64_Phdr_t ph[4];
    int          nph;
    Elf64_Shdr_t sh[8];
    int          nsh;
    uint8_t     *image[8];      /* section contents, in the order added */
    char         names[8][24];
    uint64_t     cursor;        /* file offset where the next section lands */
    uint64_t     vcursor;       /* matching virtual address */
    uint64_t     entry;
} ElfImage;

/* File and memory layout: everything lives in one RWX segment starting at 0.
 * For a fresh, self-contained executable that is the simplest honest layout --
 * no dynamic loader, no relocations, no interpreter. */
static void elf_init_at(ElfImage *e, uint64_t base_addr) {
    memset(e, 0, sizeof *e);
    /* THE NULL SECTION HEADER AT INDEX 0 IS MANDATORY (ELF spec: sh[0] must be all
     * zeros). The emitted version started nsh at 1 so it never wrote one, which made
     * every produced ELF corrupt -- readelf: "section header table goes past the end of
     * the file", because e_shnum counted the real sections while the table was written
     * from index 0 with no null slot reserved.
     *
     * FIXED IN THE EMITTED HEADER, not the .ccp, because the .ccp for elfobj is not on
     * this device. When the .ccp is available this must be applied there and this file
     * re-emitted -- the .ccp is the source of truth and a fix here is a copy. Named so
     * the divergence is findable. */
    e->nsh = 1;                    /* slot 0 stays zeroed: the null section header */
    e->image[0] = NULL;
    e->names[0][0] = 0;
    static const uint8_t magic[EI_NIDENT] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    memcpy(e->eh.e_ident, magic, EI_NIDENT);
    e->eh.e_type = 2;              /* ET_EXEC */
    e->eh.e_machine = 183;         /* EM_AARCH64 */
    e->eh.e_version = 1;
    e->eh.e_ehsize = 64;
    e->eh.e_phentsize = 56;
    e->eh.e_shentsize = 64;
    e->cursor = 0x1000;            /* one page of headers */
    e->vcursor = base_addr ? base_addr : 0x400000;         /* conventional load base */
}

/* Add a section at the current cursor. Alignment is honoured in both file and
 * address space. */
__attribute__((unused)) static void elf_init(ElfImage *e) { elf_init_at(e, 0); }

static int elf_add_section(ElfImage *e, const char *name, const void *data, size_t len,
                           uint64_t flags, uint64_t align) {
    if (e->nsh >= 8) return -1;
    if (align < 4) align = 4;
    uint64_t pad = (align - (e->cursor % align)) % align;
    e->cursor += pad;
    e->vcursor += pad;
    Elf64_Shdr_t *s = &e->sh[e->nsh];
    memset(s, 0, sizeof *s);
    uint32_t ft = ((flags & 4) && (flags & 1)) ? 0 : (((flags & 2) && (flags & 4)) ? 0 : 1);
    (void)ft;
    s->sh_type = strcmp(name, ".bss") ? 1 : 8;   /* PROGBITS, or NOBITS for .bss */
    s->sh_flags = flags;
    s->sh_addr = e->vcursor;
    s->sh_offset = e->cursor;
    s->sh_size = len;
    s->sh_addralign = align;
    snprintf(e->names[e->nsh], sizeof e->names[0], "%s", name);
    if (s->sh_type != 8) {
        e->image[e->nsh] = arb_bytes(len ? len : 1);
        if (!e->image[e->nsh]) return -1;
        if (data) memcpy(e->image[e->nsh], data, len);
        else memset(e->image[e->nsh], 0, len);
        e->cursor += len;
        e->vcursor += len;
    }
    e->nsh++;
    return (int)(e->nsh - 1);
}

/* One PT_LOAD covering every allocated section, so the kernel maps it all. */
static void elf_finish(ElfImage *e, const char *outpath, uint64_t entry) {
    e->entry = entry;
    e->eh.e_entry = entry;
    e->eh.e_phoff = 64;
    e->eh.e_phnum = 1;
    e->eh.e_shoff = e->cursor;
    e->eh.e_shnum = (uint16_t)e->nsh;
    e->eh.e_shstrndx = 0;          /* no string table: names are for our own debug only */

    e->ph[0].p_type = 1;           /* PT_LOAD */
    e->ph[0].p_flags = 7;          /* R|W|X */
    e->ph[0].p_offset = 0;
    e->ph[0].p_vaddr = e->vcursor; /* patched below to the lowest section address */
    e->ph[0].p_align = 0x1000;

    /* The loadable span, computed over REAL sections only. The null header at index 0 has
     * sh_addr = 0 and sh_size = 0, and including it made lo = 0, so the PT_LOAD claimed to
     * map 0x400028 bytes of file at virtual address 0 -- a segment that covers the whole
     * address space and maps nothing useful. The produced program loaded and then faulted
     * reading its own data, because the data was outside the real span. */
    uint64_t lo = 0, hi = 0;
    for (int i = 1; i < e->nsh; i++) {
        if (!e->sh[i].sh_size) continue;
        uint64_t a = e->sh[i].sh_addr, b = a + e->sh[i].sh_size;
        if (!lo || a < lo) lo = a;
        if (b > hi) hi = b;
    }
    e->ph[0].p_vaddr = e->ph[0].p_paddr = lo;
    e->ph[0].p_filesz = e->ph[0].p_memsz = hi - lo;
    /* p_offset must be where the vaddr maps FROM. The sections live at file offsets that
     * differ from their vaddrs by a constant page-aligned base, so the segment starts at
     * that base in the file and covers hi-lo bytes. */
    {
        /* find the file offset corresponding to `lo`: the section that starts at lo */
        uint64_t fbase = 0;
        for (int i = 1; i < e->nsh; i++) {
            if (e->sh[i].sh_size && e->sh[i].sh_addr == lo) { fbase = e->sh[i].sh_offset; break; }
        }
        e->ph[0].p_offset = fbase;
        e->ph[0].p_filesz = e->ph[0].p_memsz = (e->cursor - fbase);
    }

    FILE *f = fopen(outpath, "wb");
    if (!f) return;
    uint8_t hdr[64];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, e->eh.e_ident, EI_NIDENT);
    memcpy(hdr + 16, &e->eh.e_type, 2);
    memcpy(hdr + 18, &e->eh.e_machine, 2);
    memcpy(hdr + 20, &e->eh.e_version, 4);
    memcpy(hdr + 24, &e->eh.e_entry, 8);
    memcpy(hdr + 32, &e->eh.e_phoff, 8);
    memcpy(hdr + 40, &e->eh.e_shoff, 8);
    memcpy(hdr + 48, &e->eh.e_flags, 4);
    memcpy(hdr + 52, &e->eh.e_ehsize, 2);
    memcpy(hdr + 54, &e->eh.e_phentsize, 2);
    memcpy(hdr + 56, &e->eh.e_phnum, 2);
    memcpy(hdr + 58, &e->eh.e_shentsize, 2);
    memcpy(hdr + 60, &e->eh.e_shnum, 2);
    memcpy(hdr + 62, &e->eh.e_shstrndx, 2);
    fwrite(hdr, 1, sizeof hdr, f);

    uint8_t ph[56];
    memset(ph, 0, sizeof ph);
    memcpy(ph + 0, &e->ph[0].p_type, 4);
    memcpy(ph + 4, &e->ph[0].p_flags, 4);
    memcpy(ph + 8, &e->ph[0].p_offset, 8);
    memcpy(ph + 16, &e->ph[0].p_vaddr, 8);
    memcpy(ph + 24, &e->ph[0].p_paddr, 8);
    memcpy(ph + 32, &e->ph[0].p_filesz, 8);
    memcpy(ph + 40, &e->ph[0].p_memsz, 8);
    memcpy(ph + 48, &e->ph[0].p_align, 8);
    /* THE PROGRAM HEADER WAS BUILT AND NEVER WRITTEN. `pos` counted it (sizeof hdr +
     * sizeof ph) but no fwrite followed the memcpys, so every produced ELF was missing its
     * PT_LOAD header and every offset after it was 56 bytes off -- which is why readelf
     * reported "section table goes past the end of file" and why no produced binary could
     * be executed (e_phoff pointed at nothing). FIXED IN THE EMITTED HEADER because the
     * elfobj .ccp is not on this device; apply it there and re-emit when it is. */
    fwrite(ph, 1, sizeof ph, f);

    /* pad from end-of-headers (0x1000) to the first section, then write sections.
     * `pos` is where the file ACTUALLY is, so it must be advanced by the phdr that was
     * just written. Counting it without writing it gave a corrupt file; writing it without
     * advancing pos made the pad loop overwrite it with zeros, which left the file the
     * same length and the phdr missing -- both failures of the same one-line bookkeeping. */
    long pos = (long)sizeof hdr + (long)sizeof ph;   /* == where the file is now, since
                                                      * the phdr above was just written */
    while (pos < (long)e->sh[1].sh_offset && pos < 0x1000) { fputc(0, f); pos++; }
    if (pos < (long)sizeof hdr + (long)sizeof ph) pos = (long)sizeof hdr + (long)sizeof ph;
    for (int i = 0; i < e->nsh; i++) {
        if (e->sh[i].sh_type == 8) continue;
        while ((uint64_t)pos < e->sh[i].sh_offset) { fputc(0, f); pos++; }
        if (e->image[i] && e->sh[i].sh_size)
            fwrite(e->image[i], 1, e->sh[i].sh_size, f);
        pos += (long)e->sh[i].sh_size;
    }
    /* section header table */
    for (int i = 0; i < e->nsh; i++) {
        uint8_t sh[64];
        memset(sh, 0, sizeof sh);
        memcpy(sh + 0, &e->sh[i].sh_name, 4);
        memcpy(sh + 4, &e->sh[i].sh_type, 4);
        memcpy(sh + 8, &e->sh[i].sh_flags, 8);
        memcpy(sh + 16, &e->sh[i].sh_addr, 8);
        memcpy(sh + 24, &e->sh[i].sh_offset, 8);
        memcpy(sh + 32, &e->sh[i].sh_size, 8);
        memcpy(sh + 40, &e->sh[i].sh_link, 4);
        memcpy(sh + 44, &e->sh[i].sh_info, 4);
        memcpy(sh + 48, &e->sh[i].sh_addralign, 8);
        memcpy(sh + 56, &e->sh[i].sh_entsize, 8);
        fwrite(sh, 1, sizeof sh, f);
    }
    fclose(f);
}

#endif /* ELFOBJ_H */
