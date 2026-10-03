#ifndef LIBVEXA_STRING_H
#define LIBVEXA_STRING_H

#include <stddef.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strerror(int error);
int strerror_r(int error, char *out, size_t size);
char *strtok(char *s, const char *delimiters);
char *strtok_r(char *s, const char *delimiters, char **state);
char *strsep(char **s, const char *delimiters);
int strcoll(const char *a, const char *b);
size_t strxfrm(char *out, const char *s, size_t n);
char *stpcpy(char *dest, const char *src);
char *stpncpy(char *dest, const char *src, size_t n);
size_t strlcpy(char *dest, const char *src, size_t size);
size_t strlcat(char *dest, const char *src, size_t size);
void *memccpy(void *dest, const void *src, int c, size_t n);
void *memrchr(const void *s, int c, size_t n);
void *mempcpy(void *dest, const void *src, size_t n);
char *strcasestr(const char *haystack, const char *needle);
char *strsignal(int signal);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#endif
