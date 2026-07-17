/*
 * WynlandOS - ELF64 Format Definitions
 * Copyright (c) 2026 WynlandOS Project
 *
 * ELF64 header definitions for loading the kernel binary.
 */

#pragma once

/* ============================================================
 * ELF64 Type Definitions
 * ============================================================ */

typedef unsigned short      Elf64_Half;
typedef unsigned int        Elf64_Word;
typedef unsigned long long  Elf64_Xword;
typedef unsigned long long  Elf64_Addr;
typedef unsigned long long  Elf64_Off;

/* ============================================================
 * ELF Magic Numbers
 * ============================================================ */

#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3

#define ELFMAG0     0x7F
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

/* ============================================================
 * ELF Identification
 * ============================================================ */

#define EI_CLASS    4
#define ELFCLASS64  2

#define EI_DATA     5
#define ELFDATA2LSB 1

/* ============================================================
 * ELF Object File Types
 * ============================================================ */

#define ET_EXEC     2

/* ============================================================
 * ELF Machine Types
 * ============================================================ */

#define EM_X86_64   62

/* ============================================================
 * Program Header Types
 * ============================================================ */

#define PT_LOAD     1

/* ============================================================
 * Elf64_Ehdr - ELF64 File Header
 * ============================================================ */

typedef struct {
    unsigned char   e_ident[16];
    Elf64_Half      e_type;
    Elf64_Half      e_machine;
    Elf64_Word      e_version;
    Elf64_Addr      e_entry;
    Elf64_Off       e_phoff;
    Elf64_Off       e_shoff;
    Elf64_Word      e_flags;
    Elf64_Half      e_ehsize;
    Elf64_Half      e_phentsize;
    Elf64_Half      e_phnum;
    Elf64_Half      e_shentsize;
    Elf64_Half      e_shnum;
    Elf64_Half      e_shstrndx;
} Elf64_Ehdr;

/* ============================================================
 * Elf64_Phdr - ELF64 Program Header
 * ============================================================ */

typedef struct {
    Elf64_Word      p_type;
    Elf64_Word      p_flags;
    Elf64_Off       p_offset;
    Elf64_Addr      p_vaddr;
    Elf64_Addr      p_paddr;
    Elf64_Xword     p_filesz;
    Elf64_Xword     p_memsz;
    Elf64_Xword     p_align;
} Elf64_Phdr;
