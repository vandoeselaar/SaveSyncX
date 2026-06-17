#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdio.h>

/* Initialiseer de logger met een expliciet pad (aanroepen vóór log_print) */
void log_init_path(const char *path);

/* Initialiseer de logger — pad wordt bepaald via get_app_dir() in util.c  */
void log_init(void);
void log_print(const char *fmt, ...);

void util_forward_slash(char *path);
void util_backslash(char *path);
void util_dirname(const char *path, char *out, int out_sz);
void util_format_bytes(size_t bytes, char *out, int out_sz);

/* POSIX strncasecmp / strcasecmp zijn niet beschikbaar in nxdk's libc */
int util_strcasecmp(const char *a, const char *b);
int util_strncasecmp(const char *a, const char *b, size_t n);

#endif /* UTIL_H */
