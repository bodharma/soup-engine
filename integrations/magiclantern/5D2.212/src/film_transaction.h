#ifndef SOUP_FILM_TRANSACTION_H
#define SOUP_FILM_TRANSACTION_H
#ifdef SOUP_HOST_TEST
#include "film_host.h"
#else
#include "dryos.h"
#include "fio-ml.h"
#endif
#include "film_tiff.h"

#define SOUP_PATH 96
#define SOUP_IO 8192u
#define SOUP_MAX_FILE (128u * 1024 * 1024)
#define SOUP_RECOVERY "ML/SOUP/RECOVER.BIN"
#define SOUP_CLEANUP "ML/SOUP/CLEANUP.BIN"
#define SOUP_JPEG_MEMBER 1u
#define SOUP_RAW_MEMBER 2u
#define SOUP_RECOVERY_BUSY -2
#define SOUP_COMMIT_BUSY -2
#define SOUP_COMMIT_CLEANUP_PENDING 1
#define SOUP_CLEANUP_DONE 2
#define SOUP_CLEANUP_PENDING 3
#define SOUP_MODE_EFFECTS 0
#define SOUP_MODE_FILM 1

typedef struct {
    int mode, grain, grain_size, halation, hal_radius, bloom, bloom_radius;
} soup_settings;

typedef struct {
    char directory[SOUP_PATH], prefix[5];
    int number;
    unsigned generation;
} soup_capture_baseline;

int soup_capture_arm(const char *, const char *, int);
int soup_capture_snapshot(soup_capture_baseline *);
void soup_capture_disable(void);
int soup_capture_claim(unsigned generation);
void soup_capture_release(void);
void soup_get_settings(soup_settings *);
int soup_develop_busy(void);

typedef struct {
    char source[SOUP_PATH], backup[SOUP_PATH], stage[SOUP_PATH];
    uint32_t size, backup_crc, stage_crc, span_count;
    soup_span spans[SOUP_SPANS];
} soup_member;

typedef struct {
    char magic[8], directory[SOUP_PATH], prefix[5];
    uint32_t version, number, members;
    soup_member files[2];
    uint32_t render_size, render_crc;
    uint32_t checksum;
} soup_transaction;

int soup_read_at(FILE *, uint32_t, void *, uint32_t);
int soup_write_at(FILE *, uint32_t, const void *, uint32_t);
int soup_file_crc(const char *, uint32_t *, uint32_t *);
int soup_equal_files(const char *, const char *);
int soup_equal_ranges(const char *, uint32_t, const char *, uint32_t, uint32_t);
int soup_copy_new(const char *, const char *);
int soup_storage_check(void);
FILE *soup_create_new(const char *);
int soup_patch_ranges(const char *, const char *, const soup_span *, uint32_t);
int soup_transaction_init(soup_transaction *, const char *, const char *, int, unsigned);
int soup_render_path(const soup_transaction *, char path[SOUP_PATH]);
int soup_transaction_prepare(soup_transaction *);
int soup_transaction_commit(soup_transaction *);
int soup_recover_pending(void);
int soup_receipt_pending(void);
int soup_cleanup_pending(void);
int soup_cleanup_completed(void);
int soup_component(const char *, int, int);
void soup_diag_begin(int number);
void soup_diag_paths(const char *source, const char *backup, const char *stage);
void soup_diag_phase(const char *phase, int rc);
void soup_diag_fail(const char *phase, int rc);
void soup_diag_note(const char *text);
const char *soup_status_text(void);
void soup_diag_flush(void);
#endif
