// SPDX-License-Identifier: GPL-2.0-only
/*
 * genelf.c
 * Copyright (C) 2014, Google, Inc
 *
 * Contributed by:
 * 	Stephane Eranian <eranian@gmail.com>
 */

#include <sys/types.h>
#include <stddef.h>
#include <libelf.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <err.h>
#ifdef HAVE_LIBDW_SUPPORT
#include <dwarf.h>
#endif

#include "genelf.h"
#include "sha1.h"
#include "../util/jitdump.h"
#include <linux/compiler.h>

#ifndef NT_GNU_BUILD_ID
#define NT_GNU_BUILD_ID 3
#endif

typedef struct {
  unsigned int namesz;  /* Size of entry's owner string */
  unsigned int descsz;  /* Size of the note descriptor */
  unsigned int type;    /* Interpretation of the descriptor */
  char         name[0]; /* Start of the name+desc data */
} Elf_Note;

static char shd_string_table[] = {
	0,
	'.', 't', 'e', 'x', 't', 0,			/*  1 */
	'.', 's', 'h', 's', 't', 'r', 't', 'a', 'b', 0, /*  7 */
	'.', 's', 'y', 'm', 't', 'a', 'b', 0,		/* 17 */
	'.', 's', 't', 'r', 't', 'a', 'b', 0,		/* 25 */
	'.', 'n', 'o', 't', 'e', '.', 'g', 'n', 'u', '.', 'b', 'u', 'i', 'l', 'd', '-', 'i', 'd', 0, /* 33 */
	'.', 'd', 'e', 'b', 'u', 'g', '_', 'l', 'i', 'n', 'e', 0, /* 52 */
	'.', 'd', 'e', 'b', 'u', 'g', '_', 'i', 'n', 'f', 'o', 0, /* 64 */
	'.', 'd', 'e', 'b', 'u', 'g', '_', 'a', 'b', 'b', 'r', 'e', 'v', 0, /* 76 */
	'.', 'e', 'h', '_', 'f', 'r', 'a', 'm', 'e', '_', 'h', 'd', 'r', 0, /* 90 */
	'.', 'e', 'h', '_', 'f', 'r', 'a', 'm', 'e', 0, /* 104 */
};

static struct buildid_note {
	Elf_Note desc;		/* descsz: size of build-id, must be multiple of 4 */
	char	 name[4];	/* GNU\0 */
	u8	 build_id[SHA1_DIGEST_SIZE];
} bnote;

static Elf_Sym symtab[]={
	/* symbol 0 MUST be the undefined symbol */
	{ .st_name  = 0, /* index in sym_string table */
	  .st_info  = ELF_ST_TYPE(STT_NOTYPE),
	  .st_shndx = 0, /* for now */
	  .st_value = 0x0,
	  .st_other = ELF_ST_VIS(STV_DEFAULT),
	  .st_size  = 0,
	},
	{ .st_name  = 1, /* index in sym_string table */
	  .st_info  = ELF_ST_BIND(STB_LOCAL) | ELF_ST_TYPE(STT_FUNC),
	  .st_shndx = 1,
	  .st_value = 0, /* for now */
	  .st_other = ELF_ST_VIS(STV_DEFAULT),
	  .st_size  = 0, /* for now */
	}
};

static int
jit_add_eh_frame_info(Elf *e, void* unwinding, uint64_t unwinding_header_size,
		      uint64_t unwinding_size, uint64_t base_offset)
{
	Elf_Data *d;
	Elf_Scn *scn;
	Elf_Shdr *shdr;
	uint64_t unwinding_table_size = unwinding_size - unwinding_header_size;

	/*
	 * setup eh_frame section
	 */
	scn = elf_newscn(e);
	if (!scn) {
		warnx("cannot create section");
		return -1;
	}

	d = elf_newdata(scn);
	if (!d) {
		warnx("cannot get new data");
		return -1;
	}

	d->d_align = 8;
	d->d_off = 0LL;
	d->d_buf = unwinding;
	d->d_type = ELF_T_BYTE;
	d->d_size = unwinding_table_size;
	d->d_version = EV_CURRENT;

	shdr = elf_getshdr(scn);
	if (!shdr) {
		warnx("cannot get section header");
		return -1;
	}

	shdr->sh_name = 104;
	shdr->sh_type = SHT_PROGBITS;
	shdr->sh_addr = base_offset;
	shdr->sh_flags = SHF_ALLOC;
	shdr->sh_entsize = 0;

	/*
	 * setup eh_frame_hdr section
	 */
	scn = elf_newscn(e);
	if (!scn) {
		warnx("cannot create section");
		return -1;
	}

	d = elf_newdata(scn);
	if (!d) {
		warnx("cannot get new data");
		return -1;
	}

	d->d_align = 4;
	d->d_off = 0LL;
	d->d_buf = unwinding + unwinding_table_size;
	d->d_type = ELF_T_BYTE;
	d->d_size = unwinding_header_size;
	d->d_version = EV_CURRENT;

	shdr = elf_getshdr(scn);
	if (!shdr) {
		warnx("cannot get section header");
		return -1;
	}

	shdr->sh_name = 90;
	shdr->sh_type = SHT_PROGBITS;
	shdr->sh_addr = base_offset + unwinding_table_size;
	shdr->sh_flags = SHF_ALLOC;
	shdr->sh_entsize = 0;

	return 0;
}

/*
 * fd: file descriptor open for writing for the output file
 * load_addr: code load address (could be zero, just used for buildid)
 * sym: function name (for native code - used as the symbol)
 * code: the native code
 * csize: the code size in bytes
 */
int
jit_write_elf(int fd, uint64_t load_addr __maybe_unused, const char *sym,
	      const void *code, int csize,
	      void *debug __maybe_unused, int nr_debug_entries __maybe_unused,
	      void *unwinding, uint64_t unwinding_header_size, uint64_t unwinding_size)
{
	Elf *e;
	Elf_Data *d;
	Elf_Scn *scn;
	Elf_Ehdr *ehdr;
	Elf_Phdr *phdr;
	Elf_Shdr *shdr;
	uint64_t eh_frame_base_offset;
	char *strsym = NULL;
	int symlen;
	int retval = -1;

	if (elf_version(EV_CURRENT) == EV_NONE) {
		warnx("ELF initialization failed");
		return -1;
	}

	e = elf_begin(fd, ELF_C_WRITE, NULL);
	if (!e) {
		warnx("elf_begin failed");
		goto error;
	}

	/*
	 * setup ELF header
	 */
	ehdr = elf_newehdr(e);
	if (!ehdr) {
		warnx("cannot get ehdr");
		goto error;
	}

	ehdr->e_ident[EI_DATA] = GEN_ELF_ENDIAN;
	ehdr->e_ident[EI_CLASS] = GEN_ELF_CLASS;
	ehdr->e_machine = GEN_ELF_ARCH;
	ehdr->e_type = ET_DYN;
	ehdr->e_entry = GEN_ELF_TEXT_OFFSET;
	ehdr->e_version = EV_CURRENT;
	ehdr->e_shstrndx= unwinding ? 4 : 2; /* shdr index for section name */

	/*
	 * setup program header
	 */
	phdr = elf_newphdr(e, 1);
	phdr[0].p_type = PT_LOAD;
	phdr[0].p_offset = GEN_ELF_TEXT_OFFSET;
	phdr[0].p_vaddr = GEN_ELF_TEXT_OFFSET;
	phdr[0].p_paddr = GEN_ELF_TEXT_OFFSET;
	phdr[0].p_filesz = csize;
	phdr[0].p_memsz = csize;
	phdr[0].p_flags = PF_X | PF_R;
	phdr[0].p_align = 8;

	/*
	 * setup text section
	 */
	scn = elf_newscn(e);
	if (!scn) {
		warnx("cannot create section");
		goto error;
	}

	d = elf_newdata(scn);
	if (!d) {
		warnx("cannot get new data");
		goto error;
	}

	d->d_align = 16;
	d->d_off = 0LL;
	d->d_buf = (void *)code;
	d->d_type = ELF_T_BYTE;
	d->d_size = csize;
	d->d_version = EV_CURRENT;

	shdr = elf_getshdr(scn);
	if (!shdr) {
		warnx("cannot get section header");
		goto error;
	}

	shdr->sh_name = 1;
	shdr->sh_type = SHT_PROGBITS;
	shdr->sh_addr = GEN_ELF_TEXT_OFFSET;
	shdr->sh_flags = SHF_EXECINSTR | SHF_ALLOC;
	shdr->sh_entsize = 0;

	/*
	 * Setup .eh_frame_hdr and .eh_frame
	 */
	if (unwinding) {
		eh_frame_base_offset = ALIGN_8(GEN_ELF_TEXT_OFFSET + csize);
		retval = jit_add_eh_frame_info(e, unwinding,
					       unwinding_header_size, unwinding_size,
					       eh_frame_base_offset);
		if (retval)
			goto error;
		retval = -1;
	}

	/*
	 * setup section headers string table
	 */
	scn = elf_newscn(e);
	if (!scn) {
		warnx("cannot create section");
		goto error;
	}

	d = elf_newdata(scn);
	if (!d) {
		warnx("cannot get new data");
		goto error;
	}

	d->d_align = 1;
	d->d_off = 0LL;
	d->d_buf = shd_string_table;
	d->d_type = ELF_T_BYTE;
	d->d_size = sizeof(shd_string_table);
	d->d_version = EV_CURRENT;

	shdr = elf_getshdr(scn);
	if (!shdr) {
		warnx("cannot get section header");
		goto error;
	}

	shdr->sh_name = 7; /* offset of '.shstrtab' in shd_string_table */
	shdr->sh_type = SHT_STRTAB;
	shdr->sh_flags = 0;
	shdr->sh_entsize = 0;

	/*
	 * setup symtab section
	 */
	symtab[1].st_size  = csize;
	symtab[1].st_value = GEN_ELF_TEXT_OFFSET;

	scn = elf_newscn(e);
	if (!scn) {
		warnx("cannot create section");
		goto error;
	}

	d = elf_newdata(scn);
	if (!d) {
		warnx("cannot get new data");
		goto error;
	}

	d->d_align = 8;
	d->d_off = 0LL;
	d->d_buf = symtab;
	d->d_type = ELF_T_SYM;
	d->d_size = sizeof(symtab);
	d->d_version = EV_CURRENT;

	shdr = elf_getshdr(scn);
	if (!shdr) {
		warnx("cannot get section header");
		goto error;
	}

	shdr->sh_name = 17; /* offset of '.symtab' in shd_string_table */
	shdr->sh_type = SHT_SYMTAB;
	shdr->sh_flags = 0;
	shdr->sh_entsize = sizeof(Elf_Sym);
	shdr->sh_link = unwinding ? 6 : 4; /* index of .strtab section */

	/*
	 * setup symbols string table
	 * 2 = 1 for 0 in 1st entry, 1 for the 0 at end of symbol for 2nd entry
	 */
	symlen = 2 + strlen(sym);
	strsym = calloc(1, symlen);
	if (!strsym) {
		warnx("cannot allocate strsym");
		goto error;
	}
	strcpy(strsym + 1, sym);

	scn = elf_newscn(e);
	if (!scn) {
		warnx("cannot create section");
		goto error;
	}

	d = elf_newdata(scn);
	if (!d) {
		warnx("cannot get new data");
		goto error;
	}

	d->d_align = 1;
	d->d_off = 0LL;
	d->d_buf = strsym;
	d->d_type = ELF_T_BYTE;
	d->d_size = symlen;
	d->d_version = EV_CURRENT;

	shdr = elf_getshdr(scn);
	if (!shdr) {
		warnx("cannot get section header");
		goto error;
	}

	shdr->sh_name = 25; /* offset in shd_string_table */
	shdr->sh_type = SHT_STRTAB;
	shdr->sh_flags = 0;
	shdr->sh_entsize = 0;

	/*
	 * setup build-id section
	 */
	scn = elf_newscn(e);
	if (!scn) {
		warnx("cannot create section");
		goto error;
	}

	d = elf_newdata(scn);
	if (!d) {
		warnx("cannot get new data");
		goto error;
	}

	/*
	 * build-id generation
	 */
	sha1(code, csize, bnote.build_id);
	bnote.desc.namesz = sizeof(bnote.name); /* must include 0 termination */
	bnote.desc.descsz = sizeof(bnote.build_id);
	bnote.desc.type   = NT_GNU_BUILD_ID;
	strcpy(bnote.name, "GNU");

	d->d_align = 4;
	d->d_off = 0LL;
	d->d_buf = &bnote;
	d->d_type = ELF_T_BYTE;
	d->d_size = sizeof(bnote);
	d->d_version = EV_CURRENT;

	shdr = elf_getshdr(scn);
	if (!shdr) {
		warnx("cannot get section header");
		goto error;
	}

	shdr->sh_name = 33; /* offset in shd_string_table */
	shdr->sh_type = SHT_NOTE;
	shdr->sh_addr = 0x0;
	shdr->sh_flags = SHF_ALLOC;
	shdr->sh_size = sizeof(bnote);
	shdr->sh_entsize = 0;

#ifdef HAVE_LIBDW_SUPPORT
	if (debug && nr_debug_entries) {
		retval = jit_add_debug_info(e, load_addr, debug, nr_debug_entries);
		if (retval)
			goto error;
	} else
#endif
	{
		if (elf_update(e, ELF_C_WRITE) < 0) {
			warnx("elf_update 4 failed");
			goto error;
		}
	}

	retval = 0;
error:
	(void)elf_end(e);

	free(strsym);


	return retval;
}
