/*
 * WynlandOS - WynLang Interpreter Header
 *
 * WynLang: Python-inspired interpreted language for WynlandOS.
 * Supports: variables, arithmetic, strings, print, if/elif/else,
 *           while, for/range, functions, arrays, built-in OS calls.
 */
#pragma once

#include <wynland/types.h>

#define WLANG_OK    0
#define WLANG_ERROR 1

/* Run WynLang code from a null-terminated string */
int wlang_run(const char *code);

/* Run WynLang code from a .wyn file on the filesystem */
int wlang_run_file(const char *path);
