#include "film_host.h"
#include "film_transaction.h"
#include "se.h"
#include "stock.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

int soup_process_pair(const char *, const char *, int, se_stock *);
int soup_process_pair_with_settings(const char *, const char *, int, se_stock *, const soup_settings *);
int soup_finish_attempt(int, int *);
int soup_frame_geometry(int, int, int *, int *, int *, int *, int *);

typedef struct { FILE *file; char path[256]; int backup_checked; } host_file;
static host_file handles[32];
static int busy, unstable, locked, locks, refuse_lock_once, short_read = 513;
static const char *read_fault, *write_fault, *change_on_lock;
static long read_budget = -1, write_budget = -1;
static int zero_read, short_write, receipt_remove_fails, ignore_exclusive;
static const char *hidden_path;
static const char *remove_fault;
static uint64_t original_raw_hash, original_jpeg_hash;
static uint8_t completion_bytes[4096];
static size_t completion_size;
static int indexed_writes, removals;
static int all_writes;
static int record_checkpoints, checkpoint_count, checkpoint_base_writes;
static struct { char report[513]; int indexed; } checkpoint_reports[8];
static struct { char name[13]; uint64_t hash; } custom_originals[4];
static uint64_t file_hash(const char *);
FILE *soup_create_new(const char *);

static int short_directory(const char *path)
{
    const char *p = path;
    if (p[0] == 'A' && p[1] == ':' && p[2] == '/') p += 3;
    if (strncmp(p, "ML/SOUP", 7)) return 1; /* Test workdirs are ordinary host paths. */
    while (*p) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!n || n > 8) return 0;
        if (!end) break;
        p = end + 1;
    }
    return 1;
}

static const char *name_of(FILE *f)
{
    for (int i = 0; i < 32; i++) if (handles[i].file == f) return handles[i].path;
    assert(!"untracked file"); return "";
}
static FILE *track(FILE *f, const char *path)
{
    if (!f) return NULL;
    for (int i = 0; i < 32; i++) if (!handles[i].file) {
        handles[i].file = f; handles[i].backup_checked = 0;
        snprintf(handles[i].path, sizeof(handles[i].path), "%s", path); return f;
    }
    assert(!"too many open files"); return NULL;
}
int FIO_GetFileSize(const char *p, uint32_t *n)
{
    struct stat st;
    if (!strcmp(p, "ML/SOUP/DIAG.TXT"))
        for (int i = 0; i < 32; i++)
            assert(!handles[i].file || !strcmp(handles[i].path, "ML/SOUP/DIAG.TXT"));
    if ((hidden_path && !strcmp(p, hidden_path)) || stat(p, &st) || !S_ISREG(st.st_mode)) { *n = UINT32_MAX; return -1; }
    *n = (uint32_t)st.st_size + (unstable > 1 && !strncmp(p, "DCIM/", 5) ? 1 : 0);
    return 0;
}
int is_file(const char *p)
{ struct stat s; return !(hidden_path && !strcmp(p, hidden_path)) && !stat(p, &s) && S_ISREG(s.st_mode); }
int is_dir(const char *p)
{ struct stat s; return short_directory(p) && !stat(p, &s) && S_ISDIR(s.st_mode); }
FILE *FIO_OpenFile(const char *p, unsigned mode)
{
    FILE *f;
    if (!strcmp(p, "ML/SOUP/DIAG.TXT")) {
        assert(mode == 0 || mode == 2u); /* Existing file only, without O_SYNC. */
        for (int i = 0; i < 32; i++)
            assert(!handles[i].file || !strcmp(handles[i].path, "ML/SOUP/DIAG.TXT"));
    }
    /* 5D2.212 ROM _FIO_OpenFile bits, deliberately independent of the
     * ML header: create=0x100, truncate=0x200, exclusive=0x400. The old
     * ML 0x40/0x80 bits do not create a missing native file. */
    assert(!(mode & ~(2u | 0x40u | 0x80u | 0x100u | 0x200u | 0x400u | 0x1000u)));
    if ((mode & 2u) && !strncmp(p, "DCIM/", 5)) assert(locked);
    if (mode & 0x100u) {
        assert(strncmp(p, "DCIM/", 5));
        if ((mode & 0x400u) && !ignore_exclusive) f = fopen(p, "w+bx");
        else { struct stat st; f = fopen(p, !stat(p, &st) ? "r+b" : "w+b"); }
    } else f = fopen(p, mode & 2u ? "r+b" : "rb");
    if (f && (mode & 0x200u) && ftruncate(fileno(f), 0)) { fclose(f); return NULL; }
    return track(f, p);
}
FILE *FIO_CreateFile(const char *p)
{
    assert(strcmp(p, "ML/SOUP/DIAG.TXT"));
    assert(strncmp(p, "DCIM/", 5)); /* Indexed files must never be truncated. */
    assert(!is_file(p));
    return track(fopen(p, "wb"), p);
}
int FIO_ReadFile(FILE *f, void *p, size_t n)
{
    if (short_read && n > (size_t)short_read) n = (size_t)short_read;
    if (read_fault && strstr(name_of(f), read_fault) && read_budget >= 0) {
        if (!read_budget) return zero_read ? 0 : -1;
        if (n > (size_t)read_budget) n = (size_t)read_budget;
        read_budget -= (long)n;
    }
    return (int)fread(p, 1, n, f);
}
int FIO_WriteFile(FILE *f, const void *p, size_t n)
{
    const char *path = name_of(f);
    all_writes++;
    if (!strncmp(path, "DCIM/", 5)) {
        indexed_writes++;
        for (int i = 0; i < 32; i++) if (handles[i].file == f && !handles[i].backup_checked) {
            const char *base = strrchr(path, '/') + 1;
            char source[128], backup[128];
            for (int raw = 0; raw < 2; raw++) {
                snprintf(source, sizeof(source), "DCIM/100EOS5D/%.8s.%s", base, raw ? "CR2" : "JPG");
                snprintf(backup, sizeof(backup), "ML/SOUP/ORIG/100EOS5D/%.8s.%s", base, raw ? "CR2" : "JPG");
                if (is_file(source)) {
                    uint64_t expected = raw ? original_raw_hash : original_jpeg_hash;
                    const char *name = strrchr(source, '/') + 1;
                    for (int k = 0; k < 4; k++) if (!strcmp(name, custom_originals[k].name)) expected = custom_originals[k].hash;
                    assert(is_file(backup) && file_hash(backup) == expected);
                }
            }
            handles[i].backup_checked = 1;
        }
    }
    if (!strcmp(path, SOUP_CLEANUP) && ftell(f) == 0) {
        assert(n <= sizeof(completion_bytes)); memcpy(completion_bytes, p, n); completion_size = n;
    }
    if (!strcmp(name_of(f), "ML/SOUP/DIAG.TXT"))
        assert(ftell(f) >= 0 && (uint64_t)ftell(f) + n <= 512);
    if (write_fault && strstr(name_of(f), write_fault) && write_budget >= 0) {
        if (!write_budget) return -1;
        if (n > (size_t)write_budget) {
            size_t partial = short_write ? (size_t)write_budget : 0;
            write_budget = 0;
            if (partial) assert(fwrite(p, 1, partial, f) == partial);
            return short_write ? (int)partial : -1;
        }
        write_budget -= (long)n;
    }
    {
        int written = (int)fwrite(p, 1, n, f);
        if (record_checkpoints && !strcmp(path, "ML/SOUP/DIAG.TXT") && written == 512) {
            assert(checkpoint_count < 8);
            memcpy(checkpoint_reports[checkpoint_count].report, p, 512);
            checkpoint_reports[checkpoint_count].report[512] = 0;
            checkpoint_reports[checkpoint_count].indexed = indexed_writes - checkpoint_base_writes;
            checkpoint_count++;
        }
        return written;
    }
}
void FIO_CloseFile(FILE *f)
{
    for (int i = 0; i < 32; i++) if (handles[i].file == f) { handles[i].file = NULL; assert(!fclose(f)); return; }
    assert(!"close of untracked file");
}
int64_t FIO_SeekSkipFile(FILE *f, int64_t off, int whence)
{ return fseek(f, (long)off, whence) ? -1 : (int64_t)ftell(f); }
int FIO_RemoveFile(const char *p)
{
    removals++;
    if ((remove_fault && !strcmp(p, remove_fault)) ||
        (receipt_remove_fails && !strcmp(p, SOUP_RECOVERY))) return -1;
    return remove(p);
}
int FIO_RenameFile(const char *a, const char *b) { return rename(a, b); }
void *fio_malloc(size_t n) { return malloc(n); }
void fio_free(void *p) { free(p); }
void msleep(int n) { (void)n; if (unstable) unstable++; }
int soup_host_idle(void) { return !busy; }
static void flip_byte(const char *path, long at)
{
    FILE *f = fopen(path, "r+b"); int c;
    assert(f && !fseek(f, at, SEEK_SET)); c = fgetc(f); assert(c != EOF);
    assert(!fseek(f, at, SEEK_SET) && fputc(c ^ 0x5a, f) != EOF && !fclose(f));
}
int soup_host_lock(void)
{
    assert(!locked); if (busy) return -1;
    if (refuse_lock_once) { refuse_lock_once = 0; return -1; }
    locked = 1; locks++;
    if (change_on_lock) { flip_byte(change_on_lock, 9); change_on_lock = NULL; }
    return 0;
}
void soup_host_unlock(void) { assert(locked); locked = 0; }
int FIO_CreateDirectory(const char *p)
{
    char path[256]; size_t i;
    if (!short_directory(p)) return -1;
    assert(strlen(p) < sizeof(path)); strcpy(path, p);
    for (i = 1; path[i]; i++) if (path[i] == '/') {
        path[i] = 0; if (mkdir(path, 0700) && errno != EEXIST) return -1; path[i] = '/';
    }
    return mkdir(path, 0700) && errno != EEXIST ? -1 : 0;
}
static void copy_file(const char *from, const char *to)
{
    uint8_t buf[8192]; size_t n; FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    assert(in && out);
    while ((n = fread(buf, 1, sizeof(buf), in))) assert(fwrite(buf, 1, n, out) == n);
    assert(!ferror(in) && !fclose(in) && !fclose(out));
}
static uint64_t file_hash(const char *path)
{
    uint64_t h = UINT64_C(14695981039346656037); int c; FILE *f = fopen(path, "rb"); assert(f);
    while ((c = fgetc(f)) != EOF) h = (h ^ (unsigned)c) * UINT64_C(1099511628211);
    assert(!ferror(f) && !fclose(f)); return h;
}
static uint8_t *file_bytes(const char *path, size_t *size)
{
    long n; FILE *f = fopen(path, "rb"); uint8_t *p;
    assert(f && !fseek(f, 0, SEEK_END)); n = ftell(f); rewind(f); assert(n > 0);
    p = malloc((size_t)n); assert(p && fread(p, 1, (size_t)n, f) == (size_t)n && !fclose(f));
    *size = (size_t)n; return p;
}
static void seed_report(size_t bytes, int valid_cookie)
{
    uint8_t data[1024] = {0};
    FILE *f = fopen("ML/SOUP/DIAG.TXT", "wb");
    const char *cookie = valid_cookie ? "SOUP_DIAGNOSTIC_V1\n" : "UNOWNED\n";
    assert(bytes <= sizeof(data)); memcpy(data, cookie, strlen(cookie));
    assert(f && fwrite(data, 1, bytes, f) == bytes && !fclose(f));
}
static int report_has(const char *text)
{
    size_t n, len = strlen(text); uint8_t *data = file_bytes("ML/SOUP/DIAG.TXT", &n);
    int found = 0;
    for (size_t i = 0; len <= n && i <= n - len; i++)
        if (!memcmp(data + i, text, len)) { found = 1; break; }
    free(data); return found;
}
typedef struct { int rows, width, spread; uint64_t visible, hash; } decoded;
static int check_rows(void *ctx, const uint8_t *rgb, int rows, int width)
{
    decoded *d = ctx; d->rows += rows; assert(width == d->width);
    for (int i = 0; i < rows * width; i++) {
        int r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        int max = r > g ? r : g, min = r < g ? r : g;
        if (b > max) max = b;
        if (b < min) min = b;
        if (max - min > d->spread) d->spread = max - min;
        if (max > 8) d->visible++;
        for (int c = 0; c < 3; c++) d->hash = (d->hash ^ rgb[i * 3 + c]) * UINT64_C(1099511628211);
    }
    return 0;
}
static uint64_t jpeg_pixels(const uint8_t *p, size_t n, int width, int height, int exact_neutral)
{
    se_jdec_info info; se_arena arena; void *mem = malloc(4u << 20);
    decoded d = {0, width, 0, 0, UINT64_C(14695981039346656037)}; assert(mem);
    assert(se_jpeg_dec_info(p, n, &info) == SE_OK && info.width == width && info.height == height);
    se_arena_init(&arena, mem, 4u << 20);
    assert(se_jpeg_decode(p, n, &arena, check_rows, &d, &info) == SE_OK);
    assert(d.rows == height && d.spread <= 3 && d.visible > (uint64_t)width * height / 20);
    if (exact_neutral) assert(d.spread == 0);
    free(mem); return d.hash;
}
static void check_jpeg_bytes(const uint8_t *p, size_t n, int width, int height)
{ (void)jpeg_pixels(p, n, width, height, 0); }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned le16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t tag_long(const uint8_t *t, size_t n, uint32_t off, unsigned tag)
{
    unsigned count; assert(off <= n && n - off >= 6); count = le16(t + off);
    assert(count <= (n - off - 6) / 12);
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *e = t + off + 2 + 12 * i;
        if (le16(e) == tag) { assert(le16(e + 2) == 4 && le32(e + 4) == 1); return le32(e + 8); }
    }
    assert(!"missing LONG tag"); return 0;
}
static void check_jpeg_mode(const char *path, int exact_neutral)
{
    size_t n, tn; uint8_t *p = file_bytes(path, &n); const uint8_t *t; uint32_t off, len, ifd;
    (void)jpeg_pixels(p, n, 2784, 1856, exact_neutral);
    assert(n > 20 && p[2] == 0xff && p[3] == 0xe1 && !memcmp(p + 6, "Exif\0\0II", 8));
    tn = ((unsigned)p[4] << 8 | p[5]) - 8; t = p + 12; assert(tn <= n - 12);
    ifd = le32(t + 4); assert(ifd < tn && tn - ifd >= 6);
    assert(le16(t + ifd) <= (tn - ifd - 6) / 12);
    ifd = le32(t + ifd + 2 + 12 * le16(t + ifd));
    off = tag_long(t, tn, ifd, 0x201); len = tag_long(t, tn, ifd, 0x202);
    assert(off <= tn && len <= tn - off); (void)jpeg_pixels(t + off, len, 160, 120, exact_neutral); free(p);
}
static void check_jpeg(const char *path) { check_jpeg_mode(path, 0); }
static void check_raw(const char *path, const char *original)
{
    soup_raw_layout before, after; size_t a, b; uint8_t *old, *now;
    assert(!soup_raw_inspect(original, &before) && !soup_raw_inspect(path, &after));
    assert(after.width == 2784 && after.height == 1856 && after.main_offset == before.main_offset &&
           after.thumb_offset == before.thumb_offset && after.raw_offset == before.raw_offset && after.raw_length == before.raw_length);
    old = file_bytes(original, &a); now = file_bytes(path, &b); assert(a == b);
    check_jpeg_bytes(now + after.main_offset, after.main_length, 2784, 1856);
    check_jpeg_bytes(now + after.thumb_offset, after.thumb_length, 160, 120);
    for (size_t i = 0; i < a; i++) {
        int allowed = 0;
        for (uint32_t k = 0; k < before.span_count; k++)
            if (i >= before.spans[k].offset && i - before.spans[k].offset < before.spans[k].length) allowed = 1;
        if (!allowed) assert(old[i] == now[i]);
    }
    assert(!memcmp(old + before.raw_offset, now + before.raw_offset, before.raw_length)); free(old); free(now);
}
static void make_render(const char *path)
{
    size_t n, at = 2, end = 0; uint8_t *p = file_bytes("DCIM/100EOS5D/IMG_0003.JPG", &n);
    while (at + 4 <= n) {
        unsigned marker, length;
        assert(p[at++] == 0xff); marker = p[at++];
        length = (unsigned)p[at] << 8 | p[at + 1]; assert(length >= 2 && length <= n - at);
        at += length;
        if (marker == 0xda) {
            for (; at + 1 < n; at++) if (p[at] == 0xff && p[at + 1] == 0xd9) { end = at + 2; break; }
            break;
        }
    }
    assert(end);
    { FILE *f = fopen(path, "wb"); assert(f && fwrite(p, 1, end, f) == end && !fclose(f)); }
    free(p);
}
static void assert_clean(const soup_transaction *tx)
{
    char render[SOUP_PATH]; assert(!soup_render_path(tx, render));
    assert(!is_file(render) && !is_file(SOUP_RECOVERY) && !is_file(SOUP_CLEANUP));
    for (unsigned i = 0; i < 2; i++) if (tx->members & (1u << i))
        assert(!is_file(tx->files[i].backup) && !is_file(tx->files[i].stage));
}
static int fixture_sink(void *ctx, const uint8_t *bytes, size_t n)
{ return fwrite(bytes, 1, n, (FILE *)ctx) == n ? 0 : -1; }
static void make_neutral_fixture(const char *path)
{
    enum { W = 2784, H = 1856 };
    static se_jpeg encoder;
    uint8_t *strip = malloc(8 * W * 3), *row = malloc(W * 3);
    se_jpeg_exif exif;
    FILE *f = fopen(path, "wb"); unsigned state = 7;
    assert(strip && row && f); memset(&exif, 0, sizeof(exif));
    exif.make = "Synthetic"; exif.model = "Neutral fixture";
    exif.software = "Synthetic B&W source"; exif.datetime = "2026:01:02 03:04:05"; exif.orientation = 6;
    assert(se_jpeg_begin(&encoder, W, H, 98, strip, &exif, fixture_sink, f) == SE_OK);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int value;
            state = state * 1664525u + 1013904223u;
            value = 32 + (int)((state >> 24) * 191 / 255);
            if (x >= W / 3 && x < W * 2 / 3 && y >= H / 3 && y < H * 2 / 3) value = 255;
            row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = (uint8_t)value;
        }
        assert(se_jpeg_write_rows(&encoder, row, 1) == SE_OK);
    }
    assert(se_jpeg_end(&encoder) == SE_OK && !fclose(f)); free(strip); free(row);
}
static uint64_t neutral_pixels(const char *path)
{
    size_t n; uint8_t *p = file_bytes(path, &n);
    uint64_t hash = jpeg_pixels(p, n, 2784, 1856, 1); free(p); return hash;
}
static void test_baseline(void)
{
    soup_capture_baseline first, later;
    int camera_number = 100;
    assert(soup_capture_snapshot(NULL) == -1);
    assert(!soup_capture_arm("DCIM/100EOS5D", "IMG_", camera_number));
    camera_number = 101; busy = 1;
    assert(!soup_capture_snapshot(&first) && first.number == 100 && camera_number == 101);
    assert(!strcmp(first.directory, "DCIM/100EOS5D") && !strcmp(first.prefix, "IMG_")); busy = 0;
    assert(!soup_capture_snapshot(&later) && later.number == first.number && later.generation == first.generation &&
           !strcmp(later.directory, first.directory) && !strcmp(later.prefix, first.prefix));
    assert(soup_capture_arm("DCIM/../bad", "IMG_", 100) == -1);
    assert(soup_capture_arm("DCIM/100EOS5D", "IMG_", -1) == -1);
    assert(soup_capture_arm("DCIM/100EOS5D", "IMG_", 10000) == -1);
    assert(!soup_capture_snapshot(&later) && later.generation == first.generation && later.number == 100);
    assert(!soup_capture_claim(first.generation));
    assert(soup_capture_claim(first.generation) == -1);
    assert(soup_capture_arm("DCIM/100EOS5D", "IMG_", 101) == -1);
    soup_capture_disable();
    assert(soup_capture_claim(first.generation) == -1);
    assert(soup_capture_arm("DCIM/100EOS5D", "IMG_", 101) == -1);
    soup_capture_release();
    assert(soup_capture_claim(first.generation) == -1);
    assert(!soup_capture_arm("DCIM/100EOS5D", "_MG_", 0));
    assert(!soup_capture_snapshot(&later) && later.number == 0 && later.generation != first.generation);
    assert(soup_capture_claim(first.generation) == -1 && !soup_capture_claim(later.generation));
    soup_capture_release(); soup_capture_disable();
    assert(soup_capture_claim(later.generation) == -1);
    puts("Enable baseline helper retains the pre-shot counter through a busy interval.");
}
static void test_hybrid(const char *raw, const char *jpeg)
{
    const char *dir = "DCIM/100EOS5D";
    soup_settings off = {SOUP_MODE_EFFECTS, 0, 1, 0, 1, 0, 1}, settings, saved;
    soup_transaction tx;
    uint64_t source_hash, source_pixels, same_capture = 0, first_pixels = 0;
    char path[SOUP_PATH], date[24]; int orientation;
    /* All zero means byte preservation, even for varied colour input. */
    copy_file(raw, "DCIM/100EOS5D/IMG_0080.CR2"); copy_file(jpeg, "DCIM/100EOS5D/IMG_0080.JPG");
    {
        int writes = all_writes, deleted = removals;
        saved = off;
        assert(soup_process_pair_with_settings(dir, "IMG_", 80, NULL, &off) == 1);
        assert(!memcmp(&saved, &off, sizeof(off)) && all_writes == writes && removals == deleted);
        assert(file_hash("DCIM/100EOS5D/IMG_0080.CR2") == file_hash(raw));
        assert(file_hash("DCIM/100EOS5D/IMG_0080.JPG") == file_hash(jpeg));
        assert(!soup_transaction_init(&tx, dir, "IMG_", 80, 3)); assert_clean(&tx);
    }
    for (int k = 0; k < 2; k++) {
        const char *pending = k ? SOUP_CLEANUP : SOUP_RECOVERY;
        FILE *f = fopen(pending, "wb"); uint64_t hash; int writes = all_writes;
        assert(f && fwrite("invalid!", 1, 8, f) == 8 && !fclose(f)); hash = file_hash(pending);
        assert(soup_process_pair_with_settings(dir, "IMG_", 80, NULL, &off) == -2);
        assert(all_writes == writes && file_hash(pending) == hash); assert(!remove(pending));
    }
    for (int k = 0; k < 8; k++) {
        settings = off;
        switch (k) {
        case 0: settings.mode = -1; break;
        case 1: settings.mode = 2; break;
        case 2: settings.grain = 101; break;
        case 3: settings.grain_size = 0; break;
        case 4: settings.halation = -1; break;
        case 5: settings.hal_radius = 9; break;
        case 6: settings.bloom = 31; break;
        default: settings.bloom_radius = 17; break;
        }
        { int writes = all_writes;
            assert(soup_process_pair_with_settings(dir, "IMG_", 80, NULL, &settings) == -1 && all_writes == writes);
        }
    }
    assert(file_hash("DCIM/100EOS5D/IMG_0080.JPG") == file_hash(jpeg)); assert_clean(&tx);
    make_neutral_fixture("BW_SRC.JPG"); source_hash = file_hash("BW_SRC.JPG"); source_pixels = neutral_pixels("BW_SRC.JPG");
    settings = (soup_settings){SOUP_MODE_EFFECTS, 30, 2, 60, 3, 15, 8}; saved = settings;
    for (int k = 0; k < 3; k++) {
        int number = k == 2 ? 82 : 81; uint64_t pixels;
        snprintf(path, sizeof(path), "%s/IMG_%04d.JPG", dir, number); copy_file("BW_SRC.JPG", path);
        snprintf(custom_originals[k].name, sizeof(custom_originals[k].name), "IMG_%04d.JPG", number);
        custom_originals[k].hash = source_hash;
        assert(soup_process_pair_with_settings(dir, "IMG_", number, NULL, &settings) == 1);
        assert(!memcmp(&saved, &settings, sizeof(settings)));
        assert(!soup_transaction_init(&tx, dir, "IMG_", number, 1)); assert_clean(&tx); check_jpeg_mode(path, 1);
        pixels = neutral_pixels(path); assert(pixels != source_pixels);
        assert(!soup_jpeg_metadata(path, &orientation, date) && orientation == 6 && !strcmp(date, "2026:01:02 03:04:05"));
        if (!k) { same_capture = file_hash(path); first_pixels = pixels; }
        else if (k == 1) assert(file_hash(path) == same_capture && pixels == first_pixels);
        else assert(pixels != first_pixels);
    }
    printf("HYBRID_OUTPUTS_READY: synthetic BW_SRC.JPG -> DCIM/100EOS5D/IMG_0081.JPG; all-off colour DCIM/100EOS5D/IMG_0080.JPG\n");
    fflush(stdout);
}
static void prepare_tx(soup_transaction *tx, int number, const char *jpeg, const char *raw)
{
    assert(!soup_transaction_init(tx, "DCIM/100EOS5D", "IMG_", number, raw ? 3 : 1));
    copy_file(jpeg, tx->files[0].source);
    if (raw) copy_file(raw, tx->files[1].source);
    assert(!soup_transaction_prepare(tx));
    for (unsigned i = 0; i < 2; i++) if (tx->members & (1u << i)) {
        uint32_t size;
        copy_file(i ? "DCIM/100EOS5D/IMG_0003.CR2" : "DCIM/100EOS5D/IMG_0003.JPG", tx->files[i].stage);
        assert(!soup_file_crc(tx->files[i].stage, &size, &tx->files[i].stage_crc) && size == tx->files[i].size);
    }
    { char render[SOUP_PATH]; assert(!soup_render_path(tx, render)); make_render(render);
        assert(!soup_file_crc(render, &tx->render_size, &tx->render_crc)); }
}
static void clear_faults(void)
{
    read_fault = write_fault = change_on_lock = hidden_path = remove_fault = NULL;
    read_budget = write_budget = -1; zero_read = short_write = receipt_remove_fails = 0;
}
static void assert_applied(const soup_transaction *tx)
{
    assert(file_hash(tx->files[0].source) == file_hash("DCIM/100EOS5D/IMG_0003.JPG"));
    if (tx->members & SOUP_RAW_MEMBER)
        assert(file_hash(tx->files[1].source) == file_hash("DCIM/100EOS5D/IMG_0003.CR2"));
}
static void resume_cleanup(void)
{
    pid_t child = fork(); int status;
    assert(child >= 0);
    if (!child) {
        int writes = indexed_writes;
        assert(soup_recover_pending() == SOUP_CLEANUP_DONE && indexed_writes == writes);
        assert(soup_recover_pending() == 0); _exit(0);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void test_cleanup(const char *jpeg, const char *raw, se_stock *stock)
{
    soup_transaction tx;
    char render[SOUP_PATH], failed[SOUP_PATH];
    /* Each deletion can fail after earlier artifacts have already vanished.
     * A fresh process must finish cleanup without touching indexed photos. */
    for (int k = 0; k < 7; k++) {
        const char *paths[7];
        prepare_tx(&tx, 60, jpeg, raw); assert(!soup_render_path(&tx, render));
        paths[0] = tx.files[0].backup; paths[1] = tx.files[0].stage;
        paths[2] = tx.files[1].backup; paths[3] = tx.files[1].stage;
        paths[4] = render; paths[5] = SOUP_RECOVERY; paths[6] = SOUP_CLEANUP;
        snprintf(failed, sizeof(failed), "%s", paths[k]); remove_fault = failed;
        assert(soup_transaction_commit(&tx) == SOUP_COMMIT_CLEANUP_PENDING);
        assert(is_file(SOUP_CLEANUP) && is_file(failed)); assert_applied(&tx);
        clear_faults(); busy = 1;
        assert(soup_recover_pending() == SOUP_RECOVERY_BUSY && is_file(SOUP_CLEANUP)); busy = 0;
        resume_cleanup(); assert_applied(&tx); assert_clean(&tx);
    }
    /* Partial completion records block rollback and preserve all artifacts.
     * Replaying the captured full record only repairs this test fixture. */
    for (int k = 0; k < 3; k++) {
        int deleted, writes;
        prepare_tx(&tx, 61, jpeg, raw); deleted = removals;
        if (k < 2) { write_fault = "CLEANUP.BIN"; write_budget = 10; short_write = k; }
        else { read_fault = "CLEANUP.BIN"; read_budget = 0; }
        assert(soup_transaction_commit(&tx) == SOUP_COMMIT_CLEANUP_PENDING && is_file(SOUP_CLEANUP));
        assert(removals == deleted); assert_applied(&tx); writes = indexed_writes;
        for (unsigned i = 0; i < 2; i++) assert(is_file(tx.files[i].backup) && is_file(tx.files[i].stage));
        clear_faults();
        if (k < 2) {
            FILE *f;
            assert(soup_recover_pending() == -1 && indexed_writes == writes && removals == deleted);
            assert_applied(&tx); assert(is_file(SOUP_RECOVERY));
            f = fopen(SOUP_CLEANUP, "wb");
            assert(completion_size && f && fwrite(completion_bytes, 1, completion_size, f) == completion_size && !fclose(f));
        }
        resume_cleanup(); assert_applied(&tx); assert_clean(&tx);
    }
    /* Changed source, artifact or RECOVER record must be preserved, even
     * though CLEANUP exists and records a previously verified publication. */
    prepare_tx(&tx, 62, jpeg, raw); assert(!soup_render_path(&tx, render));
    read_fault = "CLEANUP.BIN"; read_budget = 0;
    assert(soup_transaction_commit(&tx) == SOUP_COMMIT_CLEANUP_PENDING); clear_faults();
    {
        const char *paths[] = {tx.files[0].backup, tx.files[1].stage, render, SOUP_RECOVERY, tx.files[0].source};
        for (unsigned k = 0; k < sizeof(paths) / sizeof(paths[0]); k++) {
            uint64_t changed; int writes = indexed_writes, deleted = removals;
            flip_byte(paths[k], 9); changed = file_hash(paths[k]);
            assert(soup_recover_pending() == -1 && is_file(SOUP_CLEANUP));
            assert(file_hash(paths[k]) == changed && indexed_writes == writes && removals == deleted);
            flip_byte(paths[k], 9);
        }
    }
    resume_cleanup(); assert_applied(&tx); assert_clean(&tx);
    /* Process-level applied/pending is successful publication, not a retry. */
    assert(!soup_transaction_init(&tx, "DCIM/100EOS5D", "IMG_", 70, 1)); copy_file(jpeg, tx.files[0].source);
    remove_fault = tx.files[0].backup;
    assert(soup_process_pair("DCIM/100EOS5D", "IMG_", 70, stock) == 2);
    assert(is_file(SOUP_CLEANUP)); check_jpeg(tx.files[0].source);
    { int failures = 0, writes = indexed_writes;
        assert(soup_finish_attempt(2, &failures) == 1 && !failures);
        clear_faults(); resume_cleanup(); assert(indexed_writes == writes);
    }
    assert_clean(&tx);
    puts("Temporary backups: publication guards, seven deletion cuts, reboot cleanup, partial records and ownership conflicts passed.");
}
static void test_checkpoints(const char *jpeg, se_stock *stock)
{
    static const char *labels[] = {"Status: backup ready", "Status: render ready", "Status: before commit"};
    const char *dir = "DCIM/100EOS5D";
    soup_transaction tx;
    int found = 0;
    copy_file(jpeg, "DCIM/100EOS5D/IMG_0090.JPG"); seed_report(512, 1);
    checkpoint_count = 0; checkpoint_base_writes = indexed_writes; record_checkpoints = 1;
    assert(soup_process_pair(dir, "IMG_", 90, stock) == 1); record_checkpoints = 0;
    for (int i = 0; i < checkpoint_count && found < 3; i++)
        if (strstr(checkpoint_reports[i].report, labels[found])) {
            assert(checkpoint_reports[i].indexed == 0);
            assert(strstr(checkpoint_reports[i].report, "Shot: 90\n")); found++;
        }
    assert(found == 3);
    assert(report_has("Status: before commit") && report_has("Shot: 90\n"));
    assert(!soup_transaction_init(&tx, dir, "IMG_", 90, 1)); assert_clean(&tx); check_jpeg(tx.files[0].source);
    /* Checkpoint-report failure must not alter publication or cleanup. */
    assert(!remove("ML/SOUP/DIAG.TXT")); seed_report(512, 1);
    {
        uint64_t report = file_hash("ML/SOUP/DIAG.TXT");
        copy_file(jpeg, "DCIM/100EOS5D/IMG_0091.JPG");
        write_fault = "ML/SOUP/DIAG.TXT"; write_budget = 0;
        assert(soup_process_pair(dir, "IMG_", 91, stock) == 1); clear_faults();
        assert(file_hash("ML/SOUP/DIAG.TXT") == report);
        assert(!soup_transaction_init(&tx, dir, "IMG_", 91, 1)); assert_clean(&tx); check_jpeg(tx.files[0].source);
    }
    assert(!remove("ML/SOUP/DIAG.TXT"));
    puts("Three ordered DIAG checkpoints precede indexed writes; report failure leaves publication unchanged.");
}

int main(int argc, char **argv)
{
    const char *dir = "DCIM/100EOS5D"; se_stock stock; size_t n; void *text;
    uint64_t raw_hash, jpeg_hash; soup_transaction tx; char path[256];
    int scale, width, height, x, y, failures = 0;
    if (argc != 5) return 2;
    raw_hash = original_raw_hash = file_hash(argv[1]); jpeg_hash = original_jpeg_hash = file_hash(argv[2]);
    text = file_bytes(argv[3], &n); se_stock_init(&stock); assert(se_stock_parse(text, n, &stock) == SE_OK); free(text);
    assert(!chdir(argv[4])); assert(!FIO_CreateDirectory(dir)); assert(!FIO_CreateDirectory("ML"));
    /* The capability probe must reject a backend that ignores O_EXCL.
     * A separate process keeps its one-time capability cache isolated. */
    {
        static const struct { const char *dir; int bytes, cookie, write_fails; } cases[] = {
            {"probe-ignored", 512, 1, 0}, {"diag-absent", 0, 1, 0},
            {"diag-cookie", 512, 0, 0}, {"diag-short", 511, 1, 0},
            {"diag-long", 513, 1, 0}, {"diag-write", 512, 1, 1},
            {"diag-rollover", 512, 1, 0}
        };
        for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
            pid_t child = fork(); int status;
            assert(child >= 0);
            if (!child) {
                uint64_t before = 0;
                char status_before[128];
                assert(!FIO_CreateDirectory(cases[k].dir) && !chdir(cases[k].dir));
                assert(!FIO_CreateDirectory("ML/SOUP")); ignore_exclusive = 1;
                if (cases[k].bytes) {
                    seed_report((size_t)cases[k].bytes, cases[k].cookie);
                    before = file_hash("ML/SOUP/DIAG.TXT");
                }
                if (cases[k].write_fails) { write_fault = "ML/SOUP/DIAG.TXT"; write_budget = 0; }
                assert(!soup_create_new("ML/SOUP/should-not-exist.NEW"));
                if (k == 6) {
                    char phase[100];
                    soup_diag_begin(7);
                    soup_diag_paths("A:/DCIM/100EOS5D/IMG_0001.CR2",
                        "A:/ML/SOUP/ORIG/100EOS5D/IMG_0001.CR2",
                        "A:/ML/SOUP/STAGED/100EOS5D/IMG_0001.CR2");
                    for (int i = 0; i < 100; i++) {
                        snprintf(phase, sizeof(phase), "event %03d %080d", i, 0);
                        soup_diag_phase(phase, 0);
                    }
                    soup_diag_fail("probe exclusive", -7);
                }
                soup_diag_phase("cleanup", 0);
                soup_diag_phase("before commit", 0);
                assert(strstr(soup_status_text(), "probe exclusive"));
                snprintf(status_before, sizeof(status_before), "%s", soup_status_text());
                soup_diag_note("late aggregate test note");
                soup_diag_flush();
                assert(!strcmp(status_before, soup_status_text()));
                if (!k || k == 6) {
                    uint32_t bytes;
                    size_t report_bytes;
                    uint8_t *report;
                    assert(report_has("probe exclusive"));
                    assert(report_has("late aggregate test note"));
                    assert(!FIO_GetFileSize("ML/SOUP/DIAG.TXT", &bytes) && bytes == 512);
                    report = file_bytes("ML/SOUP/DIAG.TXT", &report_bytes);
                    assert(report_bytes == 512 && !memcmp(report, "SOUP_DIAGNOSTIC_V1\n", 19)); free(report);
                    if (k == 6) assert(report_has("event 099") && !report_has("event 000"));
                } else if (!cases[k].bytes) assert(!is_file("ML/SOUP/DIAG.TXT"));
                else assert(file_hash("ML/SOUP/DIAG.TXT") == before);
                assert(!is_file("ML/SOUP/should-not-exist.NEW"));
                for (int i = 0; i < 32; i++) assert(!handles[i].file);
                _exit(0);
            }
            assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
    }
    assert(!soup_frame_geometry(5616, 3744, &scale, &width, &height, &x, &y));
    assert(scale == 2 && width == 2784 && height == 1856 && x == 12 && y == 8);
    assert(!soup_frame_geometry(4080, 2720, &scale, &width, &height, &x, &y) && scale == 1 && width == 4080 && height == 2720 && !x && !y);
    assert(!soup_frame_geometry(2784, 1856, &scale, &width, &height, &x, &y) && scale == 1 && width == 2784 && height == 1856);
    assert(soup_frame_geometry(123, 456, &scale, &width, &height, &x, &y) == -1);
    copy_file(argv[1], "DCIM/100EOS5D/IMG_0001.CR2"); copy_file(argv[2], "DCIM/100EOS5D/IMG_0002.JPG");
    copy_file(argv[1], "DCIM/100EOS5D/IMG_0003.CR2"); copy_file(argv[2], "DCIM/100EOS5D/IMG_0003.JPG");
    busy = 1; assert(soup_process_pair(dir, "IMG_", 2, &stock) == 0); busy = 0;
    unstable = 1; assert(soup_process_pair(dir, "IMG_", 2, &stock) == 0); unstable = 0;
    for (int i = 1; i <= 3; i++) {
        fprintf(stderr, "Rendering Acros frame %d/3\n", i);
        assert(soup_process_pair(dir, "IMG_", i, &stock) == 1 && !is_file(SOUP_RECOVERY));
        assert(!soup_transaction_init(&tx, dir, "IMG_", i, i == 1 ? 2 : i == 2 ? 1 : 3)); assert_clean(&tx);
        snprintf(path, sizeof(path), "DCIM/100EOS5D/IMG_%04d.%s", i, i == 1 ? "CR2" : "JPG");
        if (i == 1) check_raw(path, argv[1]); else check_jpeg(path);
    }
    check_raw("DCIM/100EOS5D/IMG_0003.CR2", argv[1]);
    assert(file_hash("DCIM/100EOS5D/IMG_0003.CR2") != raw_hash && file_hash("DCIM/100EOS5D/IMG_0003.JPG") != jpeg_hash);
    test_baseline();
    test_hybrid(argv[1], argv[2]);
    test_checkpoints(argv[2], &stock);
    printf("HAPPY_OUTPUTS_READY: %s\n", argv[4]); fflush(stdout);
    if (getenv("SOUP_HAPPY_ONLY")) return 0;
    assert(!is_file("ML/SOUP/DIAG.TXT"));
    seed_report(512, 1); /* Exercise checkpoint I/O under the path-filtered fault budgets. */

    /* Interrupted pair commit, then repeated interrupted rollback. */
    prepare_tx(&tx, 20, argv[2], argv[1]);
    write_fault = "DCIM/"; write_budget = 16384; short_write = 1;
    assert(soup_transaction_commit(&tx) == -1 && is_file(SOUP_RECOVERY) && !locked);
    for (int k = 1; k <= 2; k++) {
        write_budget = 4096 * k;
        assert(soup_recover_pending() == -1 && is_file(SOUP_RECOVERY) && !locked);
    }
    clear_faults(); assert(soup_recover_pending() == 1 && !is_file(SOUP_RECOVERY));
    assert(file_hash(tx.files[0].source) == jpeg_hash && file_hash(tx.files[1].source) == raw_hash);
    assert(soup_recover_pending() == 0);
    /* Pair-member boundary, RAW main, RAW thumbnail and final TIFF field.
     * Reuse the same immutable originals/stages to bound fixture storage. */
    prepare_tx(&tx, 24, argv[2], argv[1]);
    {
        long boundary = tx.files[0].size;
        long main_end = boundary + tx.files[1].spans[0].length;
        long total = boundary;
        for (uint32_t k = 0; k < tx.files[1].span_count; k++) total += tx.files[1].spans[k].length;
        long checkpoints[4] = { boundary, boundary + 8199,
            main_end + tx.files[1].spans[1].length / 2, total - 1 };
        for (int k = 0; k < 4; k++) {
            write_fault = "DCIM/"; write_budget = checkpoints[k]; short_write = 1;
            assert(soup_transaction_commit(&tx) == -1 && is_file(SOUP_RECOVERY));
            if (k == 1) {
                for (int j = 1; j <= 2; j++) {
                    int rc, jpeg_restored = !soup_equal_files(tx.files[0].source, tx.files[0].backup);
                    /* The first recovery restores JPEG; later recoveries
                     * skip it, so only the RAW write budget remains. */
                    write_budget = (j == 1 ? boundary : 0) + 1024 * j;
                    rc = soup_recover_pending();
                    if (rc != -1 || !is_file(SOUP_RECOVERY))
                        fprintf(stderr, "RAW rollback %d: JPEG already restored=%d, result=%d, receipt=%d, unused write budget=%ld\n",
                                j, jpeg_restored, rc, is_file(SOUP_RECOVERY), write_budget);
                    assert(rc == -1 && is_file(SOUP_RECOVERY));
                    assert(!soup_equal_files(tx.files[0].source, tx.files[0].backup));
                }
            }
            clear_faults(); assert(soup_recover_pending() == 1 && !is_file(SOUP_RECOVERY));
            assert(file_hash(tx.files[0].source) == jpeg_hash && file_hash(tx.files[1].source) == raw_hash);
            assert(file_hash(tx.files[0].backup) == jpeg_hash && file_hash(tx.files[1].backup) == raw_hash);
        }
    }
    /* Cleanup failures keep applied images and resume cleanup, never rollback. */
    receipt_remove_fails = 1;
    assert(soup_transaction_commit(&tx) == SOUP_COMMIT_CLEANUP_PENDING && is_file(SOUP_CLEANUP));
    assert(file_hash(tx.files[0].source) == file_hash("DCIM/100EOS5D/IMG_0003.JPG") &&
           file_hash(tx.files[1].source) == file_hash("DCIM/100EOS5D/IMG_0003.CR2"));
    clear_faults(); assert(soup_recover_pending() == SOUP_CLEANUP_DONE); assert_clean(&tx);
    /* A recomputed stage CRC cannot authorize a changed sensor byte. */
    prepare_tx(&tx, 25, argv[2], argv[1]);
    {
        soup_raw_layout layout; uint32_t size;
        assert(!soup_raw_inspect(tx.files[1].backup, &layout));
        flip_byte(tx.files[1].stage, layout.raw_offset + 5);
        assert(!soup_file_crc(tx.files[1].stage, &size, &tx.files[1].stage_crc));
        assert(soup_transaction_commit(&tx) == -1 && !is_file(SOUP_RECOVERY));
        assert(file_hash(tx.files[0].source) == jpeg_hash && file_hash(tx.files[1].source) == raw_hash);
    }
    /* Receipt write failure must leave both indexed originals intact. */
    prepare_tx(&tx, 21, argv[2], NULL); write_fault = "RECOVER.BIN"; write_budget = 10;
    assert(soup_transaction_commit(&tx) == -1 && !is_file(SOUP_RECOVERY)); clear_faults();
    assert(file_hash(tx.files[0].source) == jpeg_hash);
    read_fault = "RECOVER.BIN"; read_budget = 0;
    assert(soup_transaction_commit(&tx) == -1 && !is_file(SOUP_RECOVERY)); clear_faults();
    assert(file_hash(tx.files[0].source) == jpeg_hash);
    /* Mutated source and unavailable UI lock are rejected before arming. */
    prepare_tx(&tx, 22, argv[2], NULL); busy = 1;
    assert(soup_transaction_commit(&tx) == SOUP_COMMIT_BUSY && !is_file(SOUP_RECOVERY)); busy = 0;
    assert(file_hash(tx.files[0].source) == jpeg_hash && !soup_finish_attempt(0, &failures) && !failures);
    change_on_lock = tx.files[0].source;
    assert(soup_transaction_commit(&tx) == -1 && !is_file(SOUP_RECOVERY));
    assert(file_hash(tx.files[0].backup) == jpeg_hash && file_hash(tx.files[0].source) != jpeg_hash);
    /* Recovery conflict preserves the receipt and the externally changed file. */
    prepare_tx(&tx, 23, argv[2], NULL); write_fault = "DCIM/"; write_budget = 16384; short_write = 1;
    assert(soup_transaction_commit(&tx) == -1 && is_file(SOUP_RECOVERY)); clear_faults();
    hidden_path = SOUP_RECOVERY;
    assert(soup_process_pair(dir, "IMG_", 2, &stock) == -2);
    hidden_path = NULL; assert(is_file(SOUP_RECOVERY));
    copy_file(SOUP_RECOVERY, "receipt.saved");
    { uint64_t h = file_hash(tx.files[0].source);
        flip_byte(SOUP_RECOVERY, 8);
        assert(soup_recover_pending() == -1 && is_file(SOUP_RECOVERY) && file_hash(tx.files[0].source) == h);
    }
    copy_file("receipt.saved", SOUP_RECOVERY); assert(!remove("receipt.saved"));
    flip_byte(tx.files[0].source, 9); { uint64_t changed = file_hash(tx.files[0].source);
        assert(soup_recover_pending() == -1 && is_file(SOUP_RECOVERY));
        assert(file_hash(tx.files[0].source) == changed);
    }
    copy_file(tx.files[0].backup, tx.files[0].source);
    assert(soup_recover_pending() == 1 && !is_file(SOUP_RECOVERY));
    /* Negative/zero reads and partial/negative writes while staging. */
    for (int k = 0; k < 4; k++) {
        snprintf(path, sizeof(path), "%s/IMG_%04d.JPG", dir, 30 + k); copy_file(argv[2], path);
        if (k < 2) { read_fault = "DCIM/"; read_budget = 600000; zero_read = k; }
        else { write_fault = "ORIG/"; write_budget = 100; short_write = k == 3; }
        assert(soup_process_pair(dir, "IMG_", 30 + k, &stock) == -1); clear_faults();
        assert(file_hash(path) == jpeg_hash && !is_file(SOUP_RECOVERY));
    }
    /* Existing staged/backup conflicts are immutable and bounded retries advance. */
    assert(!soup_transaction_init(&tx, dir, "IMG_", 40, 1)); copy_file(argv[2], tx.files[0].source);
    copy_file(argv[2], tx.files[0].stage); flip_byte(tx.files[0].stage, 10);
    { uint64_t h = file_hash(tx.files[0].stage);
        for (int k = 0; k < 3; k++) {
            int rc = soup_process_pair(dir, "IMG_", 40, &stock); assert(rc == -1);
            assert(soup_finish_attempt(rc, &failures) == (k == 2 ? -1 : 0));
        }
        assert(file_hash(tx.files[0].stage) == h && file_hash(tx.files[0].source) == jpeg_hash);
    }
    assert(soup_process_pair(dir, "../_", 1, &stock) == -1);
    assert(soup_transaction_init(&tx, "DCIM/../bad", "IMG_", 1, 1) == -1);
    assert(!soup_transaction_init(&tx, dir, "_MG_", 1, 1) && strstr(tx.files[0].source, "/_MG_0001.JPG"));
    assert(!soup_transaction_init(&tx, dir, "FLM_", 1, 1) && strstr(tx.files[0].source, "/FLM_0001.JPG"));
    assert(!soup_transaction_init(&tx, dir, "IMG_", 41, 1));
    copy_file(argv[2], tx.files[0].source); copy_file(argv[2], tx.files[0].backup); flip_byte(tx.files[0].backup, 11);
    { uint64_t h = file_hash(tx.files[0].backup);
        assert(soup_process_pair(dir, "IMG_", 41, &stock) == -1);
        assert(file_hash(tx.files[0].backup) == h && file_hash(tx.files[0].source) == jpeg_hash);
    }
    hidden_path = tx.files[0].backup;
    { uint64_t h = file_hash(tx.files[0].backup);
        assert(soup_copy_new(tx.files[0].source, tx.files[0].backup) == -1);
        assert(file_hash(tx.files[0].backup) == h);
    }
    clear_faults();
    copy_file(argv[2], "DCIM/100EOS5D/IMG_0042.JPG"); assert(!truncate("DCIM/100EOS5D/IMG_0042.JPG", 512));
    { uint64_t h = file_hash("DCIM/100EOS5D/IMG_0042.JPG");
        assert(soup_process_pair(dir, "IMG_", 42, &stock) == -1);
        assert(file_hash("DCIM/100EOS5D/IMG_0042.JPG") == h && !is_file(SOUP_RECOVERY));
    }
    /* A lock race after rendering is transient and leaves a retryable frame. */
    assert(!soup_transaction_init(&tx, dir, "IMG_", 43, 1)); copy_file(argv[2], tx.files[0].source);
    refuse_lock_once = 1;
    assert(soup_process_pair(dir, "IMG_", 43, &stock) == 0 && !is_file(SOUP_RECOVERY));
    assert(file_hash(tx.files[0].source) == jpeg_hash && !is_file(tx.files[0].stage));
    assert(!is_file("ML/SOUP/STAGED/100EOS5D/IMG_0043.NEW"));
    assert(!soup_finish_attempt(0, &failures) && !failures);
    assert(soup_process_pair(dir, "IMG_", 43, &stock) == 1); check_jpeg(tx.files[0].source);
    assert_clean(&tx);
    test_cleanup(argv[2], argv[1], &stock);
    assert(soup_finish_attempt(1, &failures) == 1 && !failures);
    assert(file_hash(argv[1]) == raw_hash && file_hash(argv[2]) == jpeg_hash && !locked && locks > 3);
    for (int i = 0; i < 32; i++) assert(!handles[i].file);
    puts("5D2 native PLAY checks passed: Acros main/thumbnail decode, RAW byte isolation, backups, geometry, short I/O, interrupted pair commit and repeated rollback, source/conflict guards.");
    return 0;
}
