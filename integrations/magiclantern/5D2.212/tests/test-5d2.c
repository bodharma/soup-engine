#include "film_host.h"
#include "film_transaction.h"
#include "se.h"
#include "stock.h"
#include "jpeg_dec.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

int soup_process_pair(const char *, const char *, int, se_stock *);
int soup_finish_attempt(int, int *);
int soup_frame_geometry(int, int, int *, int *, int *, int *, int *);

typedef struct { FILE *file; char path[256]; } host_file;
static host_file handles[32];
static int busy, unstable, locked, locks, refuse_lock_once, short_read = 513;
static const char *read_fault, *write_fault, *change_on_lock;
static long read_budget = -1, write_budget = -1;
static int zero_read, short_write, receipt_remove_fails, ignore_exclusive;
static const char *hidden_path;
FILE *soup_create_new(const char *);

static const char *name_of(FILE *f)
{
    for (int i = 0; i < 32; i++) if (handles[i].file == f) return handles[i].path;
    assert(!"untracked file"); return "";
}
static FILE *track(FILE *f, const char *path)
{
    if (!f) return NULL;
    for (int i = 0; i < 32; i++) if (!handles[i].file) {
        handles[i].file = f; snprintf(handles[i].path, sizeof(handles[i].path), "%s", path); return f;
    }
    assert(!"too many open files"); return NULL;
}
int FIO_GetFileSize(const char *p, uint32_t *n)
{
    struct stat st;
    if ((hidden_path && !strcmp(p, hidden_path)) || stat(p, &st) || !S_ISREG(st.st_mode)) { *n = UINT32_MAX; return -1; }
    *n = (uint32_t)st.st_size + (unstable > 1 && !strncmp(p, "DCIM/", 5) ? 1 : 0);
    return 0;
}
int is_file(const char *p)
{ struct stat s; return !(hidden_path && !strcmp(p, hidden_path)) && !stat(p, &s) && S_ISREG(s.st_mode); }
int is_dir(const char *p) { struct stat s; return !stat(p, &s) && S_ISDIR(s.st_mode); }
FILE *FIO_OpenFile(const char *p, unsigned mode)
{
    FILE *f;
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
    return (int)fwrite(p, 1, n, f);
}
void FIO_CloseFile(FILE *f)
{
    for (int i = 0; i < 32; i++) if (handles[i].file == f) { handles[i].file = NULL; assert(!fclose(f)); return; }
    assert(!"close of untracked file");
}
int FIO_SeekSkipFile(FILE *f, int off, int whence) { return fseek(f, off, whence); }
int FIO_RemoveFile(const char *p)
{ return receipt_remove_fails && !strcmp(p, SOUP_RECOVERY) ? -1 : remove(p); }
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
typedef struct { int rows, width, spread; uint64_t visible; } decoded;
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
    }
    return 0;
}
static void check_jpeg_bytes(const uint8_t *p, size_t n, int width, int height)
{
    se_jdec_info info; se_arena arena; void *mem = malloc(4u << 20);
    decoded d = {0, width, 0, 0}; assert(mem);
    assert(se_jpeg_dec_info(p, n, &info) == SE_OK && info.width == width && info.height == height);
    se_arena_init(&arena, mem, 4u << 20);
    assert(se_jpeg_decode(p, n, &arena, check_rows, &d, &info) == SE_OK);
    assert(d.rows == height && d.spread <= 3 && d.visible > (uint64_t)width * height / 20);
    free(mem);
}
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
static void check_jpeg(const char *path)
{
    size_t n, tn; uint8_t *p = file_bytes(path, &n); const uint8_t *t; uint32_t off, len, ifd;
    check_jpeg_bytes(p, n, 2784, 1856);
    assert(n > 20 && p[2] == 0xff && p[3] == 0xe1 && !memcmp(p + 6, "Exif\0\0II", 8));
    tn = ((unsigned)p[4] << 8 | p[5]) - 8; t = p + 12; assert(tn <= n - 12);
    ifd = le32(t + 4); assert(ifd < tn && tn - ifd >= 6);
    assert(le16(t + ifd) <= (tn - ifd - 6) / 12);
    ifd = le32(t + ifd + 2 + 12 * le16(t + ifd));
    off = tag_long(t, tn, ifd, 0x201); len = tag_long(t, tn, ifd, 0x202);
    assert(off <= tn && len <= tn - off); check_jpeg_bytes(t + off, len, 160, 120); free(p);
}
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
}
static void clear_faults(void)
{
    read_fault = write_fault = change_on_lock = hidden_path = NULL;
    read_budget = write_budget = -1; zero_read = short_write = receipt_remove_fails = 0;
}

int main(int argc, char **argv)
{
    const char *dir = "DCIM/100EOS5D"; se_stock stock; size_t n; void *text;
    uint64_t raw_hash, jpeg_hash; soup_transaction tx; char path[256];
    int scale, width, height, x, y, failures = 0;
    if (argc != 5) return 2;
    raw_hash = file_hash(argv[1]); jpeg_hash = file_hash(argv[2]);
    text = file_bytes(argv[3], &n); se_stock_init(&stock); assert(se_stock_parse(text, n, &stock) == SE_OK); free(text);
    assert(!chdir(argv[4])); assert(!FIO_CreateDirectory(dir)); assert(!FIO_CreateDirectory("ML"));
    /* The capability probe must reject a backend that ignores O_EXCL.
     * A separate process keeps its one-time capability cache isolated. */
    {
        pid_t child = fork(); int status;
        assert(child >= 0);
        if (!child) {
            assert(!FIO_CreateDirectory("probe-ignored") && !chdir("probe-ignored"));
            assert(!FIO_CreateDirectory("ML/SOUP")); ignore_exclusive = 1;
            assert(!soup_create_new("ML/SOUP/should-not-exist.NEW"));
            assert(!is_file("ML/SOUP/should-not-exist.NEW")); _exit(0);
        }
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
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
        snprintf(path, sizeof(path), "DCIM/100EOS5D/IMG_%04d.%s", i, i == 1 ? "CR2" : "JPG");
        if (i == 1) check_raw(path, argv[1]); else check_jpeg(path);
    }
    check_raw("DCIM/100EOS5D/IMG_0003.CR2", argv[1]);
    assert(file_hash("ML/SOUP/ORIGINALS/100EOS5D/IMG_0001.CR2") == raw_hash);
    assert(file_hash("ML/SOUP/ORIGINALS/100EOS5D/IMG_0002.JPG") == jpeg_hash);
    assert(file_hash("ML/SOUP/ORIGINALS/100EOS5D/IMG_0003.CR2") == raw_hash);
    assert(file_hash("ML/SOUP/ORIGINALS/100EOS5D/IMG_0003.JPG") == jpeg_hash);
    assert(file_hash("DCIM/100EOS5D/IMG_0003.CR2") != raw_hash && file_hash("DCIM/100EOS5D/IMG_0003.JPG") != jpeg_hash);
    assert(soup_process_pair(dir, "IMG_", 3, &stock) == 1);
    printf("HAPPY_OUTPUTS_READY: %s\n", argv[4]); fflush(stdout);
    if (getenv("SOUP_HAPPY_ONLY")) return 0;

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
    /* A fully committed pair remains recoverable when receipt removal fails. */
    receipt_remove_fails = 1;
    assert(soup_transaction_commit(&tx) == -1 && is_file(SOUP_RECOVERY));
    assert(!soup_equal_files(tx.files[0].source, tx.files[0].stage) &&
           !soup_equal_files(tx.files[1].source, tx.files[1].stage));
    clear_faults(); assert(soup_recover_pending() == 1);
    assert(file_hash(tx.files[0].source) == jpeg_hash && file_hash(tx.files[1].source) == raw_hash);
    /* A recomputed stage CRC cannot authorize a changed sensor byte. */
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
        else { write_fault = "ORIGINALS/"; write_budget = 100; short_write = k == 3; }
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
    assert(soup_finish_attempt(1, &failures) == 1 && !failures);
    assert(file_hash(argv[1]) == raw_hash && file_hash(argv[2]) == jpeg_hash && !locked && locks > 3);
    for (int i = 0; i < 32; i++) assert(!handles[i].file);
    puts("5D2 native PLAY checks passed: Acros main/thumbnail decode, RAW byte isolation, backups, geometry, short I/O, interrupted pair commit and repeated rollback, source/conflict guards.");
    return 0;
}
