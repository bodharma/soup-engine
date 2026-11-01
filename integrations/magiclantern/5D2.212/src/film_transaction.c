/* One pending pair transaction; close/readback is logical verification, not a CF flush guarantee. */
#include "film_transaction.h"
#ifndef SOUP_HOST_TEST
#include "mem.h"
#include "lens.h"
#include "debug.h"
#include "gui-common.h"
#include "propvalues.h"
#include "property.h"
#endif
#include "crc32.h"
#include <stddef.h>

/* 5D2.212 ROM 0xFFB92AE0: create=0x100, exclusive=0x400; ML's POSIX values differ. */
#define SOUP_NEW_FILE_FLAGS (O_RDWR | 0x100u | 0x400u)

static int soup_lock(void)
{
#ifdef SOUP_HOST_TEST
    return soup_host_lock();
#else
    if (lens_info.job_state || get_halfshutter_pressed() || RECORDING || (icu_uilock & 0xffff)) return -1;
    gui_uilock(UILOCK_EVERYTHING);
    if ((icu_uilock & UILOCK_EVERYTHING) != UILOCK_EVERYTHING || lens_info.job_state || get_halfshutter_pressed() || RECORDING) {
        if (icu_uilock & UILOCK_EVERYTHING) gui_uilock((icu_uilock & 0xffff) & ~UILOCK_EVERYTHING);
        return -1;
    }
    return 0;
#endif
}

static void soup_unlock(void)
{
#ifdef SOUP_HOST_TEST
    soup_host_unlock();
#else
    if (icu_uilock & UILOCK_EVERYTHING) gui_uilock((icu_uilock & 0xffff) & ~UILOCK_EVERYTHING);
#endif
}

int soup_component(const char *text, int min, int max)
{
    int n;
    if (!text) return 0;
    for (n = 0; text[n]; n++) {
        char c = text[n];
        if (n >= max || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return n >= min;
}

int soup_read_at(FILE *f, uint32_t offset, void *dst, uint32_t bytes)
{
    uint32_t done = 0;
    if (offset > SOUP_MAX_FILE || bytes > SOUP_MAX_FILE - offset ||
        FIO_SeekSkipFile(f, offset, 0) < 0) return -1;
    while (done < bytes) {
        int got = FIO_ReadFile(f, (uint8_t *)dst + done, bytes - done);
        if (got <= 0 || (uint32_t)got > bytes - done) return -1;
        done += (uint32_t)got;
    }
    return 0;
}

int soup_write_at(FILE *f, uint32_t offset, const void *src, uint32_t bytes)
{
    uint32_t done = 0;
    if (offset > SOUP_MAX_FILE || bytes > SOUP_MAX_FILE - offset ||
        FIO_SeekSkipFile(f, offset, 0) < 0) return -1;
    while (done < bytes) {
        uint32_t n = bytes - done;
        int wrote;
        if (n > SOUP_IO) n = SOUP_IO;
        wrote = FIO_WriteFile(f, (const uint8_t *)src + done, n);
        if (wrote <= 0 || (uint32_t)wrote != n) return -1;
        done += n;
    }
    return 0;
}

static uint32_t soup_crc(const void *data, uint32_t bytes, uint32_t seed)
{
    static int initialized;
    if (!initialized) { crc32_init(); initialized = 1; }
    return crc32((void *)data, bytes, seed);
}

int soup_storage_check(void)
{
    static int ready;
    static const char probe[] = "ML/SOUP/EXCLTEST.TMP";
    static const char token[] = "SOUP-EXCLUSIVE-CREATE-PROBE-V1";
    FILE *f = 0, *second = 0;
    uint8_t *buffer = 0;
    uint32_t size = 0;
    int created = 0, ok = 0;
    if (ready) return ready > 0 ? 0 : -1;
    ready = -1;
    f = FIO_OpenFile(probe, O_RDONLY | O_SYNC);
    if (f || is_file(probe)) goto done;
    buffer = fio_malloc(sizeof(token));
    if (!buffer) goto done;
    f = FIO_OpenFile(probe, SOUP_NEW_FILE_FLAGS);
    if (!f) goto done;
    created = 1;
    if (FIO_GetFileSize(probe, &size) || size) goto done;
    memcpy(buffer, token, sizeof(token));
    if (soup_write_at(f, 0, buffer, sizeof(token))) goto done;
    FIO_CloseFile(f); f = 0;
    f = FIO_OpenFile(probe, O_RDONLY | O_SYNC);
    if (!f || FIO_GetFileSize(probe, &size) || size != sizeof(token) ||
        soup_read_at(f, 0, buffer, sizeof(token)) || memcmp(buffer, token, sizeof(token))) goto done;
    FIO_CloseFile(f); f = 0;
    second = FIO_OpenFile(probe, SOUP_NEW_FILE_FLAGS);
    if (second) goto done;
    f = FIO_OpenFile(probe, O_RDONLY | O_SYNC);
    if (!f || FIO_GetFileSize(probe, &size) || size != sizeof(token) ||
        soup_read_at(f, 0, buffer, sizeof(token)) || memcmp(buffer, token, sizeof(token))) goto done;
    ok = 1;
done:
    if (second) FIO_CloseFile(second);
    if (f) FIO_CloseFile(f);
    fio_free(buffer);
    if (created && FIO_RemoveFile(probe)) ok = 0;
    if (ok) ready = 1;
    return ok ? 0 : -1;
}

FILE *soup_create_new(const char *path)
{
    if (soup_storage_check()) return 0;
    return FIO_OpenFile(path, SOUP_NEW_FILE_FLAGS);
}

int soup_receipt_pending(void)
{
    FILE *f = FIO_OpenFile(SOUP_RECOVERY, O_RDONLY | O_SYNC);
    if (f) { FIO_CloseFile(f); return 1; }
    return is_file(SOUP_RECOVERY);
}

int soup_file_crc(const char *path, uint32_t *size, uint32_t *digest)
{
    FILE *f = 0;
    uint8_t *buffer = 0;
    uint32_t length = 0, after = 0, offset = 0, crc = 0xffffffffu;
    int result = -1;
    if (FIO_GetFileSize(path, &length) || !length || length > SOUP_MAX_FILE) return -1;
    f = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    buffer = fio_malloc(SOUP_IO);
    if (!f || !buffer) goto done;
    while (offset < length) {
        uint32_t n = length - offset;
        if (n > SOUP_IO) n = SOUP_IO;
        if (soup_read_at(f, offset, buffer, n)) goto done;
        crc = soup_crc(buffer, n, crc);
        offset += n;
    }
    if (FIO_GetFileSize(path, &after) || after != length) goto done;
    *size = length; *digest = crc ^ 0xffffffffu;
    result = 0;
done:
    if (f) FIO_CloseFile(f);
    fio_free(buffer);
    return result;
}

int soup_equal_files(const char *left, const char *right)
{
    FILE *a = 0, *b = 0;
    uint8_t *x = 0, *y = 0;
    uint32_t size = 0, other = 0, offset = 0;
    int result = -1;
    if (FIO_GetFileSize(left, &size) || FIO_GetFileSize(right, &other) ||
        !size || size > SOUP_MAX_FILE || size != other) return -1;
    a = FIO_OpenFile(left, O_RDONLY | O_SYNC); b = FIO_OpenFile(right, O_RDONLY | O_SYNC);
    x = fio_malloc(SOUP_IO); y = fio_malloc(SOUP_IO);
    if (!a || !b || !x || !y) goto done;
    while (offset < size) {
        uint32_t n = size - offset;
        if (n > SOUP_IO) n = SOUP_IO;
        if (soup_read_at(a, offset, x, n) || soup_read_at(b, offset, y, n) || memcmp(x, y, n)) goto done;
        offset += n;
    }
    if (FIO_GetFileSize(left, &other) || other != size ||
        FIO_GetFileSize(right, &other) || other != size) goto done;
    result = 0;
done:
    if (a) FIO_CloseFile(a);
    if (b) FIO_CloseFile(b);
    fio_free(x); fio_free(y);
    return result;
}

int soup_equal_ranges(const char *left, uint32_t left_offset,
                       const char *right, uint32_t right_offset, uint32_t bytes)
{
    FILE *a = 0, *b = 0;
    uint8_t *x = 0, *y = 0;
    uint32_t left_size = 0, right_size = 0, done = 0;
    int result = -1;
    if (FIO_GetFileSize(left, &left_size) || FIO_GetFileSize(right, &right_size) ||
        left_offset > left_size || bytes > left_size - left_offset ||
        right_offset > right_size || bytes > right_size - right_offset) return -1;
    a = FIO_OpenFile(left, O_RDONLY | O_SYNC); b = FIO_OpenFile(right, O_RDONLY | O_SYNC);
    x = fio_malloc(SOUP_IO); y = fio_malloc(SOUP_IO);
    if (!a || !b || !x || !y) goto done;
    while (done < bytes) {
        uint32_t n = bytes - done; if (n > SOUP_IO) n = SOUP_IO;
        if (soup_read_at(a, left_offset + done, x, n) || soup_read_at(b, right_offset + done, y, n) ||
            memcmp(x, y, n)) goto done;
        done += n;
    }
    result = 0;
done:
    if (a) FIO_CloseFile(a);
    if (b) FIO_CloseFile(b);
    fio_free(x); fio_free(y);
    return result;
}

int soup_copy_new(const char *source, const char *target)
{
    FILE *in = 0, *out = 0;
    uint8_t *buffer = 0;
    uint32_t size = 0, offset = 0;
    int created = 0, result = -1;
    if (is_file(target)) return soup_equal_files(source, target);
    if (FIO_GetFileSize(source, &size) || !size || size > SOUP_MAX_FILE) return -1;
    buffer = fio_malloc(SOUP_IO); in = FIO_OpenFile(source, O_RDONLY | O_SYNC);
    if (!buffer || !in) goto done;
    out = soup_create_new(target);
    if (!out) goto done;
    created = 1;
    while (offset < size) {
        uint32_t n = size - offset;
        if (n > SOUP_IO) n = SOUP_IO;
        if (soup_read_at(in, offset, buffer, n) || soup_write_at(out, offset, buffer, n)) goto done;
        offset += n;
    }
    FIO_CloseFile(out); out = 0;
    if (!soup_equal_files(source, target)) result = 0;
done:
    if (out) FIO_CloseFile(out);
    if (in) FIO_CloseFile(in);
    fio_free(buffer);
    if (result < 0 && created) FIO_RemoveFile(target);
    return result;
}

int soup_patch_ranges(const char *source, const char *target, const soup_span *spans, uint32_t count)
{
    FILE *in = 0, *out = 0;
    uint8_t *buffer = 0;
    uint32_t size = 0, other = 0, i;
    int result = -1;
    if (!count || count > SOUP_SPANS || FIO_GetFileSize(source, &size) ||
        FIO_GetFileSize(target, &other) || size != other || !size || size > SOUP_MAX_FILE) return -1;
    for (i = 0; i < count; i++)
        if (!spans[i].length || spans[i].offset > size || spans[i].length > size - spans[i].offset) return -1;
    in = FIO_OpenFile(source, O_RDONLY | O_SYNC);
    buffer = fio_malloc(SOUP_IO);
    if (!in || !buffer) goto done;
    out = FIO_OpenFile(target, O_RDWR | O_SYNC);
    if (!out) goto done;
    for (i = 0; i < count; i++) {
        uint32_t done = 0;
        while (done < spans[i].length) {
            uint32_t offset = spans[i].offset + done, n = spans[i].length - done;
            if (n > SOUP_IO) n = SOUP_IO;
            if (soup_read_at(in, offset, buffer, n) || soup_write_at(out, offset, buffer, n)) goto done;
            done += n;
        }
    }
    FIO_CloseFile(out); out = 0;
    result = 0;
done:
    if (out) FIO_CloseFile(out);
    if (in) FIO_CloseFile(in);
    fio_free(buffer);
    return result;
}

int soup_transaction_init(soup_transaction *tx, const char *directory,
                          const char *prefix, int number, unsigned members)
{
    const char *folder, *drive = "";
    char root[16];
    if (!directory || !prefix || strlen(directory) >= SOUP_PATH ||
        !soup_component(prefix, 4, 4) || number < 1 || number > 9999 || !members || members > 3) return -1;
    folder = directory;
    if (folder[0] == 'A' && folder[1] == ':' && folder[2] == '/') { drive = "A:/"; folder += 3; }
    if (strncmp(folder, "DCIM/", 5)) return -1;
    folder += 5;
    if (!soup_component(folder, 8, 8) || folder[0] < '1' || folder[0] > '9' ||
        folder[1] < '0' || folder[1] > '9' || folder[2] < '0' || folder[2] > '9') return -1;
    memset(tx, 0, sizeof(*tx));
    memcpy(tx->magic, "SOUPTX2", 8);
    tx->version = 2; tx->number = number; tx->members = members;
    snprintf(tx->directory, sizeof(tx->directory), "%s", directory);
    snprintf(tx->prefix, sizeof(tx->prefix), "%s", prefix);
    snprintf(root, sizeof(root), "%sML/SOUP", drive);
    for (unsigned i = 0; i < 2; i++) {
        const char *extension = i ? "CR2" : "JPG";
        soup_member *m = &tx->files[i];
        if (!(members & (1u << i))) continue;
        snprintf(m->source, sizeof(m->source), "%s/%s%04d.%s", directory, prefix, number, extension);
        snprintf(m->backup, sizeof(m->backup), "%s/ORIGINALS/%s/%s%04d.%s", root, folder, prefix, number, extension);
        snprintf(m->stage, sizeof(m->stage), "%s/STAGED/%s/%s%04d.%s", root, folder, prefix, number, extension);
    }
    return 0;
}

static int soup_valid_transaction(const soup_transaction *tx)
{
    soup_transaction expected;
    int directory_end = 0, prefix_end = 0;
    for (unsigned i = 0; i < sizeof(tx->directory); i++) if (!tx->directory[i]) directory_end = 1;
    for (unsigned i = 0; i < sizeof(tx->prefix); i++) if (!tx->prefix[i]) prefix_end = 1;
    if (memcmp(tx->magic, "SOUPTX2", 8) || tx->version != 2 ||
        !directory_end || !prefix_end ||
        soup_transaction_init(&expected, tx->directory, tx->prefix, (int)tx->number, tx->members)) return -1;
    for (unsigned i = 0; i < 2; i++) {
        const soup_member *m = &tx->files[i];
        if (!(tx->members & (1u << i))) continue;
        if (memcmp(m->source, expected.files[i].source, SOUP_PATH) ||
            memcmp(m->backup, expected.files[i].backup, SOUP_PATH) ||
            memcmp(m->stage, expected.files[i].stage, SOUP_PATH) ||
            !m->size || m->size > SOUP_MAX_FILE || !m->span_count || m->span_count > SOUP_SPANS) return -1;
        if (!i) {
            if (m->span_count != 1 || m->spans[0].offset || m->spans[0].length != m->size) return -1;
        } else {
            soup_raw_layout layout;
            if (soup_raw_inspect(m->backup, &layout) || m->span_count != layout.span_count ||
                memcmp(m->spans, layout.spans, sizeof(soup_span) * layout.span_count)) return -1;
        }
    }
    return 0;
}

static int soup_digest_matches(const char *path, uint32_t size, uint32_t digest)
{
    uint32_t actual = 0, crc = 0;
    return !soup_file_crc(path, &actual, &crc) && actual == size && crc == digest;
}

/* Accept original-prefix/stage-middle/original-suffix, including interrupted rollback. */
static int soup_target_owned(const soup_member *m, int raw)
{
    FILE *current = 0, *backup = 0, *stage = 0;
    uint8_t *c = 0, *b = 0, *s = 0;
    uint32_t size = 0, offset = 0;
    int result = -1;
    unsigned states = 1;
    if (FIO_GetFileSize(m->source, &size) || size != m->size ||
        !soup_digest_matches(m->stage, m->size, m->stage_crc)) return -1;
    current = FIO_OpenFile(m->source, O_RDONLY | O_SYNC);
    backup = FIO_OpenFile(m->backup, O_RDONLY | O_SYNC);
    stage = FIO_OpenFile(m->stage, O_RDONLY | O_SYNC);
    c = fio_malloc(SOUP_IO); b = fio_malloc(SOUP_IO); s = fio_malloc(SOUP_IO);
    if (!current || !backup || !stage || !c || !b || !s) goto done;
    while (offset < size) {
        uint32_t n = size - offset;
        if (n > SOUP_IO) n = SOUP_IO;
        if (soup_read_at(current, offset, c, n) || soup_read_at(backup, offset, b, n) ||
            soup_read_at(stage, offset, s, n)) goto done;
        for (uint32_t k = 0; k < n; k++) {
            if (raw) {
                int allowed = 0;
                for (uint32_t j = 0; j < m->span_count; j++)
                    if (offset + k >= m->spans[j].offset && offset + k - m->spans[j].offset < m->spans[j].length) allowed = 1;
                if (!allowed && c[k] != b[k]) goto done;
            } else {
                unsigned next = 0;
                if ((states & 1) && c[k] == b[k]) next |= 1;
                if ((states & 3) && c[k] == s[k]) next |= 2;
                if ((states & 6) && c[k] == b[k]) next |= 4;
                if (!next) goto done;
                states = next;
            }
        }
        offset += n;
    }
    result = 0;
done:
    if (current) FIO_CloseFile(current);
    if (backup) FIO_CloseFile(backup);
    if (stage) FIO_CloseFile(stage);
    fio_free(c); fio_free(b); fio_free(s);
    return result;
}

int soup_transaction_prepare(soup_transaction *tx)
{
    if (soup_receipt_pending()) return -1;
    for (unsigned i = 0; i < 2; i++) if (tx->members & (1u << i)) {
        soup_member *m = &tx->files[i];
        if (soup_copy_new(m->source, m->backup) || soup_equal_files(m->source, m->backup) ||
            soup_file_crc(m->backup, &m->size, &m->backup_crc)) return -1;
        if (!i) { m->span_count = 1; m->spans[0].length = m->size; }
        else {
            soup_raw_layout layout;
            if (soup_raw_inspect(m->backup, &layout)) return -1;
            m->span_count = layout.span_count;
            memcpy(m->spans, layout.spans, sizeof(soup_span) * layout.span_count);
        }
    }
    return soup_valid_transaction(tx);
}

static int soup_read_receipt(soup_transaction *tx)
{
    FILE *f = 0;
    void *buffer = 0;
    uint32_t size = 0;
    int result = -1;
    if (FIO_GetFileSize(SOUP_RECOVERY, &size) || size != sizeof(*tx)) return -1;
    buffer = fio_malloc(sizeof(*tx)); f = FIO_OpenFile(SOUP_RECOVERY, O_RDONLY | O_SYNC);
    if (!buffer || !f || soup_read_at(f, 0, buffer, sizeof(*tx))) goto done;
    memcpy(tx, buffer, sizeof(*tx));
    if (soup_crc(tx, offsetof(soup_transaction, checksum), 0xffffffffu) != tx->checksum ||
        soup_valid_transaction(tx)) goto done;
    result = 0;
done:
    if (f) FIO_CloseFile(f);
    fio_free(buffer);
    return result;
}

static int soup_recover_locked(void)
{
    soup_transaction tx;
    if (!soup_receipt_pending()) return 0;
    if (soup_read_receipt(&tx)) return -1;
    /* Validate every member before restoring either member. */
    for (unsigned i = 0; i < 2; i++) if (tx.members & (1u << i)) {
        soup_member *m = &tx.files[i];
        if (!soup_digest_matches(m->backup, m->size, m->backup_crc)) return -1;
        if (soup_equal_files(m->source, m->backup) && soup_target_owned(m, i)) return -1;
    }
    for (unsigned i = 0; i < 2; i++) if (tx.members & (1u << i)) {
        soup_member *m = &tx.files[i];
        if (soup_equal_files(m->source, m->backup) &&
            (soup_patch_ranges(m->backup, m->source, m->spans, m->span_count) ||
             soup_equal_files(m->source, m->backup))) return -1;
    }
    return FIO_RemoveFile(SOUP_RECOVERY) ? -1 : 1;
}

int soup_recover_pending(void)
{
    int result;
    if (!soup_receipt_pending()) return 0;
    if (soup_lock()) return SOUP_RECOVERY_BUSY;
    result = soup_recover_locked();
    soup_unlock();
    return result;
}

int soup_transaction_commit(soup_transaction *tx)
{
    soup_transaction check;
    FILE *f = 0;
    void *buffer = 0;
    int result = -1, created = 0, armed = 0;
    if (soup_receipt_pending() || soup_valid_transaction(tx)) return -1;
    if (soup_lock()) return SOUP_COMMIT_BUSY;
    for (unsigned i = 0; i < 2; i++) if (tx->members & (1u << i)) {
        soup_member *m = &tx->files[i];
        if (soup_equal_files(m->source, m->backup) ||
            !soup_digest_matches(m->backup, m->size, m->backup_crc) ||
            !soup_digest_matches(m->stage, m->size, m->stage_crc)) goto done;
        if (i) {
            soup_member staged = *m;
            snprintf(staged.source, sizeof(staged.source), "%s", m->stage);
            if (soup_target_owned(&staged, 1)) goto done;
        }
    }
    tx->checksum = soup_crc(tx, offsetof(soup_transaction, checksum), 0xffffffffu);
    buffer = fio_malloc(sizeof(*tx));
    if (!buffer) goto done;
    memcpy(buffer, tx, sizeof(*tx));
    f = soup_create_new(SOUP_RECOVERY);
    created = f != 0;
    if (!f || soup_write_at(f, 0, buffer, sizeof(*tx))) goto done;
    FIO_CloseFile(f); f = 0;
    if (soup_read_receipt(&check) || memcmp(&check, tx, sizeof(check))) goto done;
    armed = 1;
    for (unsigned i = 0; i < 2; i++) if (tx->members & (1u << i)) {
        soup_member *m = &tx->files[i];
        if (soup_patch_ranges(m->stage, m->source, m->spans, m->span_count) ||
            soup_equal_files(m->stage, m->source)) goto rollback;
    }
    result = FIO_RemoveFile(SOUP_RECOVERY) ? -1 : 0;
    goto done;
rollback:
    /* Keep the receipt if rollback cannot be fully verified. */
    soup_recover_locked();
done:
    if (f) FIO_CloseFile(f);
    fio_free(buffer);
    if (created && !armed) FIO_RemoveFile(SOUP_RECOVERY);
    soup_unlock();
    return result;
}
