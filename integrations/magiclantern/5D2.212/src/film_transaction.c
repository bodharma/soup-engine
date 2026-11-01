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
#include <stdarg.h>

/* 5D2.212 ROM 0xFFB92AE0: create=0x100, exclusive=0x400; ML's POSIX values differ. */
#define SOUP_NEW_FILE_FLAGS (O_RDWR | 0x100u | 0x400u)

static char soup_status[64] = "Ready";
static char soup_events[512];
static char soup_source[SOUP_PATH], soup_backup[SOUP_PATH], soup_stage[SOUP_PATH];
static unsigned soup_event_bytes;
static int soup_diag_number, soup_diag_failed, soup_diag_dirty;
static FILE *soup_probe_handle;

static void soup_diag_event(const char *format, ...)
{
    char line[112];
    va_list ap;
    unsigned n, drop = 0;
    va_start(ap, format);
    vsnprintf(line, sizeof(line) - 1, format, ap);
    va_end(ap);
    line[sizeof(line) - 1] = 0;
    n = (unsigned)strlen(line);
    if (soup_event_bytes + n >= sizeof(soup_events)) {
        drop = soup_event_bytes + n - (sizeof(soup_events) - 1);
        while (drop < soup_event_bytes && soup_events[drop - 1] != '\n') drop++;
        for (unsigned i = 0; i < soup_event_bytes - drop; i++) soup_events[i] = soup_events[i + drop];
        soup_event_bytes -= drop;
    }
    memcpy(soup_events + soup_event_bytes, line, n);
    soup_event_bytes += n;
    soup_events[soup_event_bytes] = 0;
    soup_diag_dirty = 1;
}

const char *soup_status_text(void) { return soup_status; }

void soup_diag_begin(int number)
{
    soup_diag_number = number;
    soup_event_bytes = 0; soup_events[0] = 0;
    soup_source[0] = soup_backup[0] = soup_stage[0] = 0;
    soup_diag_failed = 0;
    snprintf(soup_status, sizeof(soup_status), "Starting rc=0");
    soup_diag_dirty = 1;
}

void soup_diag_paths(const char *source, const char *backup, const char *stage)
{
    snprintf(soup_source, sizeof(soup_source), "%s", source ? source : "");
    snprintf(soup_backup, sizeof(soup_backup), "%s", backup ? backup : "");
    snprintf(soup_stage, sizeof(soup_stage), "%s", stage ? stage : "");
    soup_diag_dirty = 1;
}

void soup_diag_phase(const char *phase, int rc)
{
    if (soup_diag_failed) return;
    snprintf(soup_status, sizeof(soup_status), "%s rc=%d", phase, rc);
    if (!soup_probe_handle) soup_diag_event("%s=%d\n", phase, rc);
    if (rc < 0) soup_diag_failed = 1;
    soup_diag_dirty = 1;
}

void soup_diag_fail(const char *phase, int rc)
{
    if (!soup_diag_failed) {
        snprintf(soup_status, sizeof(soup_status), "%s rc=%d", phase, rc);
        soup_diag_failed = 1;
        soup_diag_dirty = 1;
    }
}

/* Only this pre-existing owned report may be updated; logger errors are ignored. */
void soup_diag_flush(void)
{
    static const char cookie[] = "SOUP_DIAGNOSTIC_V1\n";
    static const char path[] = "ML/SOUP/DIAG.TXT";
    uint32_t size = 0;
    uint8_t *buffer;
    FILE *f;
    unsigned at, room, start = 0;
    int read_count, written;
    if (!soup_diag_dirty || FIO_GetFileSize(path, &size) || size != 512) return;
    buffer = fio_malloc(512);
    if (!buffer) return;
    f = FIO_OpenFile(path, O_RDONLY);
    if (!f) { fio_free(buffer); return; }
    read_count = FIO_ReadFile(f, buffer, 512);
    FIO_CloseFile(f);
    if (read_count != 512 || memcmp(buffer, cookie, sizeof(cookie) - 1)) { fio_free(buffer); return; }
    memset(buffer, 0, 512);
    at = (unsigned)snprintf((char *)buffer, 512,
        "%sStatus: %s\nShot: %d\nSource: %s\nBackup: %s\nStage: %s\n",
        cookie, soup_status, soup_diag_number, soup_source, soup_backup, soup_stage);
    if (at >= 512) { fio_free(buffer); return; }
    room = 511 - at;
    if (soup_event_bytes > room) {
        start = soup_event_bytes - room;
        while (start < soup_event_bytes && soup_events[start - 1] != '\n') start++;
    }
    memcpy(buffer + at, soup_events + start, soup_event_bytes - start);
    /* Reopen at byte zero so diagnostics do not depend on the seek under investigation. */
    if (FIO_GetFileSize(path, &size) || size != 512) { fio_free(buffer); return; }
    f = FIO_OpenFile(path, O_RDWR);
    if (!f) { fio_free(buffer); return; }
    written = FIO_WriteFile(f, buffer, 512);
    FIO_CloseFile(f);
    if (written == 512) soup_diag_dirty = 0;
    fio_free(buffer);
}

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
    int64_t seek;
    if (offset > SOUP_MAX_FILE || bytes > SOUP_MAX_FILE - offset) return -1;
    soup_diag_phase("read seek", 0);
    seek = FIO_SeekSkipFile(f, offset, 0);
    if (f == soup_probe_handle) soup_diag_event("seekR=%08x:%08x\n", (unsigned)((uint64_t)seek >> 32), (unsigned)seek);
    if (seek < 0) {
        if (f != soup_probe_handle) soup_diag_event("read seek64=%08x:%08x\n", (unsigned)((uint64_t)seek >> 32), (unsigned)seek);
        soup_diag_fail("read seek", (int32_t)seek); return -1;
    }
    while (done < bytes) {
        soup_diag_phase("read", 0);
        int got = FIO_ReadFile(f, (uint8_t *)dst + done, bytes - done);
        if (f == soup_probe_handle) soup_diag_event("read=%d/%u\n", got, (unsigned)(bytes - done));
        if (got <= 0 || (uint32_t)got > bytes - done) { soup_diag_fail("read", got); return -1; }
        done += (uint32_t)got;
    }
    return 0;
}

int soup_write_at(FILE *f, uint32_t offset, const void *src, uint32_t bytes)
{
    uint32_t done = 0;
    int64_t seek;
    if (offset > SOUP_MAX_FILE || bytes > SOUP_MAX_FILE - offset) return -1;
    soup_diag_phase("write seek", 0);
    seek = FIO_SeekSkipFile(f, offset, 0);
    if (f == soup_probe_handle) soup_diag_event("seekW=%08x:%08x\n", (unsigned)((uint64_t)seek >> 32), (unsigned)seek);
    if (seek < 0) {
        if (f != soup_probe_handle) soup_diag_event("write seek64=%08x:%08x\n", (unsigned)((uint64_t)seek >> 32), (unsigned)seek);
        soup_diag_fail("write seek", (int32_t)seek); return -1;
    }
    while (done < bytes) {
        uint32_t n = bytes - done;
        int wrote;
        if (n > SOUP_IO) n = SOUP_IO;
        soup_diag_phase("write", 0);
        wrote = FIO_WriteFile(f, (const uint8_t *)src + done, n);
        if (f == soup_probe_handle) soup_diag_event("write=%d/%u\n", wrote, (unsigned)n);
        if (wrote <= 0 || (uint32_t)wrote != n) { soup_diag_fail("write", wrote); return -1; }
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
    int created = 0, ok = 0, rc, match;
    if (ready) { soup_diag_phase("probe cached", ready); return ready > 0 ? 0 : -1; }
    ready = -1;
    soup_diag_phase("probe existing", 0);
    f = FIO_OpenFile(probe, O_RDONLY | O_SYNC);
    soup_diag_event("existing=%08x\n", (unsigned)(uintptr_t)f);
    if (f || is_file(probe)) { soup_diag_fail("probe existing", f != 0); goto done; }
    soup_diag_phase("probe allocate", 0);
    buffer = fio_malloc(sizeof(token));
    if (!buffer) { soup_diag_fail("probe allocate", -1); goto done; }
    soup_diag_phase("probe create", 0);
    f = FIO_OpenFile(probe, SOUP_NEW_FILE_FLAGS);
    soup_diag_event("create=%08x flags=%x\n", (unsigned)(uintptr_t)f, (unsigned)SOUP_NEW_FILE_FLAGS);
    if (!f) { soup_diag_fail("probe create", 0); goto done; }
    soup_probe_handle = f;
    created = 1;
    soup_diag_phase("probe size(open)", 0);
    rc = FIO_GetFileSize(probe, &size);
    soup_diag_event("stat0=%d size=%08x\n", rc, (unsigned)size);
    if (rc || size) { soup_diag_fail("probe size(open)", rc); goto done; }
    memcpy(buffer, token, sizeof(token));
    if (soup_write_at(f, 0, buffer, sizeof(token))) goto done;
    FIO_CloseFile(f); f = 0; soup_probe_handle = 0;
    soup_diag_event("close1=done\n");
    soup_diag_phase("probe reopen1", 0);
    f = FIO_OpenFile(probe, O_RDONLY | O_SYNC);
    soup_diag_event("reopen1=%08x\n", (unsigned)(uintptr_t)f);
    if (!f) { soup_diag_fail("probe reopen1", 0); goto done; }
    soup_probe_handle = f;
    soup_diag_phase("probe size1", 0);
    rc = FIO_GetFileSize(probe, &size);
    soup_diag_event("stat1=%d size=%08x\n", rc, (unsigned)size);
    if (rc || size != sizeof(token)) { soup_diag_fail("probe size1", rc); goto done; }
    if (soup_read_at(f, 0, buffer, sizeof(token))) goto done;
    match = memcmp(buffer, token, sizeof(token));
    soup_diag_event("match1=%d\n", match);
    if (match) { soup_diag_fail("probe match1", match); goto done; }
    FIO_CloseFile(f); f = 0; soup_probe_handle = 0;
    soup_diag_event("close2=done\n");
    soup_diag_phase("probe exclusive", 0);
    second = FIO_OpenFile(probe, SOUP_NEW_FILE_FLAGS);
    soup_diag_event("probe exclusive=%08x\n", (unsigned)(uintptr_t)second);
    if (second) { soup_diag_fail("probe exclusive", 1); goto done; }
    soup_diag_phase("probe reopen2", 0);
    f = FIO_OpenFile(probe, O_RDONLY | O_SYNC);
    soup_diag_event("reopen2=%08x\n", (unsigned)(uintptr_t)f);
    if (!f) { soup_diag_fail("probe reopen2", 0); goto done; }
    soup_probe_handle = f;
    soup_diag_phase("probe size2", 0);
    rc = FIO_GetFileSize(probe, &size);
    soup_diag_event("stat2=%d size=%08x\n", rc, (unsigned)size);
    if (rc || size != sizeof(token)) { soup_diag_fail("probe size2", rc); goto done; }
    if (soup_read_at(f, 0, buffer, sizeof(token))) goto done;
    match = memcmp(buffer, token, sizeof(token));
    soup_diag_event("match2=%d\n", match);
    if (match) { soup_diag_fail("probe match2", match); goto done; }
    ok = 1;
done:
    if (second) { FIO_CloseFile(second); soup_diag_event("closeX=done\n"); }
    if (f) { FIO_CloseFile(f); soup_diag_event("closeF=done\n"); }
    soup_probe_handle = 0;
    fio_free(buffer);
    if (created) {
        soup_diag_phase("probe cleanup", 0);
        rc = FIO_RemoveFile(probe);
        soup_diag_event("cleanup=%d\n", rc);
        if (rc) { soup_diag_fail("probe cleanup", rc); ok = 0; }
    }
    if (ok) ready = 1;
    if (ok) soup_diag_phase("probe passed", 0);
    return ok ? 0 : -1;
}

FILE *soup_create_new(const char *path)
{
    FILE *f;
    if (soup_storage_check()) return 0;
    soup_diag_phase("create open", 0);
    f = FIO_OpenFile(path, SOUP_NEW_FILE_FLAGS);
    if (!f) soup_diag_fail("create open", 0);
    return f;
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
    soup_diag_phase("digest size", 0);
    if (FIO_GetFileSize(path, &length) || !length || length > SOUP_MAX_FILE) { soup_diag_fail("digest size", -1); return -1; }
    soup_diag_phase("digest open", 0);
    f = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    buffer = fio_malloc(SOUP_IO);
    if (!f || !buffer) { soup_diag_fail(f ? "digest allocate" : "digest open", 0); goto done; }
    while (offset < length) {
        uint32_t n = length - offset;
        if (n > SOUP_IO) n = SOUP_IO;
        if (soup_read_at(f, offset, buffer, n)) goto done;
        crc = soup_crc(buffer, n, crc);
        offset += n;
    }
    soup_diag_phase("digest verify", 0);
    if (FIO_GetFileSize(path, &after) || after != length) { soup_diag_fail("digest verify", -1); goto done; }
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
    soup_diag_phase("verify sizes", 0);
    if (FIO_GetFileSize(left, &size) || FIO_GetFileSize(right, &other) ||
        !size || size > SOUP_MAX_FILE || size != other) return -1;
    soup_diag_phase("verify open", 0);
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
    int created = 0, result = -1, rc;
    soup_diag_phase("copy target", 0);
    if (is_file(target)) return soup_equal_files(source, target);
    soup_diag_phase("copy size", 0);
    rc = FIO_GetFileSize(source, &size);
    if (rc || !size || size > SOUP_MAX_FILE) { soup_diag_fail("copy size", rc); return -1; }
    soup_diag_phase("copy source open", 0);
    buffer = fio_malloc(SOUP_IO); in = FIO_OpenFile(source, O_RDONLY | O_SYNC);
    if (!buffer || !in) { soup_diag_fail(in ? "copy allocate" : "copy source open", 0); goto done; }
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
    soup_diag_phase("copy verify", 0);
    if (!soup_equal_files(source, target)) result = 0;
    else soup_diag_fail("copy verify", -1);
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
    soup_diag_phase("backup pending", 0);
    if (soup_receipt_pending()) { soup_diag_fail("backup pending", 1); return -1; }
    for (unsigned i = 0; i < 2; i++) if (tx->members & (1u << i)) {
        soup_member *m = &tx->files[i];
        soup_diag_paths(m->source, m->backup, m->stage);
        soup_diag_phase("backup copy", 0);
        if (soup_copy_new(m->source, m->backup)) { soup_diag_fail("backup copy", -1); return -1; }
        soup_diag_phase("backup verify", 0);
        if (soup_equal_files(m->source, m->backup)) { soup_diag_fail("backup verify", -1); return -1; }
        soup_diag_phase("backup digest", 0);
        if (soup_file_crc(m->backup, &m->size, &m->backup_crc)) { soup_diag_fail("backup digest", -1); return -1; }
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
