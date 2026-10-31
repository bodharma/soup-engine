#ifndef SOUP_FILM_HOST_H
#define SOUP_FILM_HOST_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

int FIO_GetFileSize(const char *, uint32_t *);
FILE *FIO_OpenFile(const char *, unsigned);
FILE *FIO_CreateFile(const char *);
int FIO_ReadFile(FILE *, void *, size_t);
int FIO_WriteFile(FILE *, const void *, size_t);
void FIO_CloseFile(FILE *);
int FIO_SeekSkipFile(FILE *, int, int);
int FIO_RemoveFile(const char *);
int FIO_RenameFile(const char *, const char *);
int FIO_CreateDirectory(const char *);
int is_file(const char *);
int is_dir(const char *);
void *fio_malloc(size_t);
void fio_free(void *);
void msleep(int);
int soup_host_idle(void);
#endif
