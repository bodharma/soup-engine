#include "film_host.h"
#include "se.h"
#include "stock.h"
#include "jpeg_dec.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

int soup_develop_file(const char *, const char *, int, se_stock *);
int soup_process_pair(const char *, const char *, int, se_stock *);
int soup_finish_attempt(int, int *);

static int busy, unstable, copy_read_fails, copy_write_fails;
static FILE *copy_reader, *copy_writer;
static long read_limit = -1, write_limit = -1;
static size_t read_count, write_count;

int FIO_GetFileSize(const char *p, uint32_t *n)
{
    struct stat st;
    if (stat(p, &st) || !S_ISREG(st.st_mode)) { *n = UINT32_MAX; return -1; }
    *n = (uint32_t)st.st_size + (unstable > 1 ? 1 : 0);
    return 0;
}
int is_file(const char *p) { struct stat s; return !stat(p, &s) && S_ISREG(s.st_mode); }
int is_dir(const char *p) { struct stat s; return !stat(p, &s) && S_ISDIR(s.st_mode); }
FILE *FIO_OpenFile(const char *p, unsigned mode)
{
    FILE *f;
    assert(!mode); f = fopen(p, "rb");
    if (strstr(p, ".TMP")) copy_reader = f;
    return f;
}
FILE *FIO_CreateFile(const char *p)
{
    FILE *f;
    assert(!is_file(p)); f = fopen(p, "wb");
    if (strstr(p, "ML/SOUP/") && strstr(p, ".JPG")) copy_writer = f;
    return f;
}
int FIO_ReadFile(FILE *f, void *p, size_t n)
{
    if (read_limit >= 0 && read_count >= (size_t)read_limit) return -1;
    if (copy_read_fails && f == copy_reader) return -1;
    if (n > 513) n = 513;
    n = fread(p, 1, n, f);
    read_count += n;
    return (int)n;
}
int FIO_WriteFile(FILE *f, const void *p, size_t n)
{
    if (write_limit >= 0 && write_count + n > (size_t)write_limit) return -1;
    if (copy_write_fails && f == copy_writer) return -1;
    n = fwrite(p, 1, n, f);
    write_count += n;
    return (int)n;
}
void FIO_CloseFile(FILE *f)
{
    if (f == copy_reader) copy_reader = 0;
    if (f == copy_writer) copy_writer = 0;
    assert(!fclose(f));
}
int FIO_SeekSkipFile(FILE *f, int off, int whence) { return fseek(f, off, whence); }
int FIO_RemoveFile(const char *p) { return remove(p); }
void *fio_malloc(size_t n) { return malloc(n); }
void fio_free(void *p) { free(p); }
void msleep(int n) { (void)n; if (unstable) unstable++; }
int soup_host_idle(void) { return !busy; }
int FIO_CreateDirectory(const char *p)
{
    char path[256];
    size_t i;
    assert(strlen(p) < sizeof(path));
    strcpy(path, p);
    for (i = 1; path[i]; i++) if (path[i] == '/') {
        path[i] = 0;
        if (mkdir(path, 0700) && errno != EEXIST) return -1;
        path[i] = '/';
    }
    return mkdir(path, 0700) && errno != EEXIST ? -1 : 0;
}

static void copy_file(const char *from, const char *to)
{
    char buf[8192];
    size_t n;
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    assert(in && out);
    while ((n = fread(buf, 1, sizeof(buf), in))) assert(fwrite(buf, 1, n, out) == n);
    assert(!ferror(in));
    assert(!fclose(in)); assert(!fclose(out));
}

static uint64_t file_hash(const char *path)
{
    uint64_t h = UINT64_C(14695981039346656037);
    int c;
    FILE *f = fopen(path, "rb");
    assert(f);
    while ((c = fgetc(f)) != EOF) h = (h ^ (unsigned)c) * UINT64_C(1099511628211);
    assert(!ferror(f)); fclose(f);
    return h;
}

static void *file_bytes(const char *path, size_t *size)
{
    long n;
    FILE *f = fopen(path, "rb");
    void *p;
    assert(f && !fseek(f, 0, SEEK_END)); n = ftell(f); rewind(f);
    assert(n > 0); p = malloc((size_t)n); assert(p);
    assert(fread(p, 1, (size_t)n, f) == (size_t)n); fclose(f);
    *size = (size_t)n;
    return p;
}

static int count_rows(void *ctx, const uint8_t *rgb, int rows, int width)
{
    int *n = ctx;
    (void)rgb;
    assert(width > 0); *n += rows; return 0;
}

static void check_jpeg(const char *path, int width, int height)
{
    size_t n;
    uint8_t *p = file_bytes(path, &n);
    void *memory = malloc(2u << 20);
    se_arena arena;
    se_jdec_info info;
    int rows = 0;
    assert(p[0] == 0xff && p[1] == 0xd8 && p[n - 2] == 0xff && p[n - 1] == 0xd9);
    assert(se_jpeg_dec_info(p, n, &info) == SE_OK);
    assert(info.width == width && info.height == height);
    se_arena_init(&arena, memory, 2u << 20);
    assert(se_jpeg_decode(p, n, &arena, count_rows, &rows, &info) == SE_OK);
    assert(rows == height);
    free(memory); free(p);
}

int main(int argc, char **argv)
{
    const char *dir = "DCIM/100EOS5D";
    const char *out = "ML/SOUP/100EOS5D/";
    se_stock stock;
    se_jdec_info info;
    uint64_t raw_hash, jpg_hash, only_hash, incomplete_hash;
    size_t n;
    void *text, *jpeg;
    char path[256];
    int i, width, height, failures = 0, advanced = 0;
    if (argc != 5) { fprintf(stderr, "usage: test-5d2 sample.CR2 sample.JPG stock.fs empty-workdir\n"); return 2; }
    raw_hash = file_hash(argv[1]); jpg_hash = file_hash(argv[2]);
    text = file_bytes(argv[3], &n); se_stock_init(&stock);
    assert(se_stock_parse(text, n, &stock) == SE_OK); free(text);
    jpeg = file_bytes(argv[2], &n);
    assert(se_jpeg_dec_info(jpeg, n, &info) == SE_OK);
    se_jpeg_scaled_size(info.width, info.height, 2, &width, &height); free(jpeg);
    assert(!chdir(argv[4]));
    assert(!is_file("DCIM/100EOS5D/IMG_0001.CR2"));
    assert(!FIO_CreateDirectory(dir)); assert(!FIO_CreateDirectory("ML"));
    copy_file(argv[1], "DCIM/100EOS5D/IMG_0001.CR2");
    copy_file(argv[2], "DCIM/100EOS5D/IMG_0002.JPG");
    copy_file(argv[1], "DCIM/100EOS5D/IMG_0003.CR2");
    copy_file(argv[2], "DCIM/100EOS5D/IMG_0003.JPG");
    for (i = 4; i <= 9; i++) {
        snprintf(path, sizeof(path), "%s/IMG_%04d.JPG", dir, i);
        copy_file(argv[2], path);
    }
    copy_file(argv[2], "DCIM/100EOS5D/FLM_0010.JPG");
    copy_file(argv[2], "DCIM/100EOS5D/_MG_0011.JPG");
    copy_file(argv[2], "DCIM/100EOS5D/IMG_0012.JPG");
    copy_file(argv[2], "DCIM/100EOS5D/IMG_0013.JPG");

    busy = 1;
    assert(soup_process_pair(dir, "IMG_", 2, &stock) == 0);
    busy = 0; unstable = 1;
    assert(soup_process_pair(dir, "IMG_", 2, &stock) == 0);
    unstable = 0;
    for (i = 1; i <= 3; i++) {
        assert(soup_process_pair(dir, "IMG_", i, &stock) == 1);
        snprintf(path, sizeof(path), "%sIMG_%04d.JPG", out, i);
        check_jpeg(path, width, height);
    }
    only_hash = file_hash("ML/SOUP/100EOS5D/IMG_0002.JPG");
    assert(file_hash("ML/SOUP/100EOS5D/IMG_0003.JPG") == only_hash);
    assert(soup_process_pair(dir, "IMG_", 3, &stock) == 1);
    assert(file_hash("ML/SOUP/100EOS5D/IMG_0003.JPG") == only_hash);

    write_limit = 100;
    assert(soup_process_pair(dir, "IMG_", 4, &stock) == -1);
    assert(!is_file("ML/SOUP/100EOS5D/IMG_0004.JPG") && !is_file("ML/SOUP/100EOS5D/IMG_0004.TMP"));
    write_limit = -1; read_count = 0; read_limit = 600000;
    assert(soup_process_pair(dir, "IMG_", 5, &stock) == -1);
    assert(!is_file("ML/SOUP/100EOS5D/IMG_0005.JPG") && !is_file("ML/SOUP/100EOS5D/IMG_0005.TMP"));
    read_limit = -1; copy_read_fails = 1;
    assert(soup_process_pair(dir, "IMG_", 6, &stock) == -1);
    assert(!is_file("ML/SOUP/100EOS5D/IMG_0006.JPG") && !is_file("ML/SOUP/100EOS5D/IMG_0006.TMP"));
    copy_read_fails = 0;
    copy_file(argv[2], "ML/SOUP/100EOS5D/IMG_0007.TMP");
    assert(soup_process_pair(dir, "IMG_", 7, &stock) == -1);
    assert(file_hash("ML/SOUP/100EOS5D/IMG_0007.TMP") == jpg_hash);
    copy_file("ML/SOUP/100EOS5D/IMG_0002.JPG", "ML/SOUP/100EOS5D/IMG_0008.JPG");
    assert(soup_process_pair(dir, "IMG_", 8, &stock) == 1);
    assert(file_hash("ML/SOUP/100EOS5D/IMG_0008.JPG") == only_hash);
    assert(!truncate("DCIM/100EOS5D/IMG_0009.JPG", 512));
    assert(soup_process_pair(dir, "IMG_", 9, &stock) == -1);
    assert(!is_file("ML/SOUP/100EOS5D/IMG_0009.JPG"));
    assert(soup_process_pair(dir, "FLM_", 10, &stock) == 1);
    assert(soup_process_pair(dir, "_MG_", 11, &stock) == 1);
    assert(soup_process_pair(dir, "../_", 1, &stock) == -1);
    copy_write_fails = 1;
    assert(soup_process_pair(dir, "IMG_", 12, &stock) == -1);
    assert(!is_file("ML/SOUP/100EOS5D/IMG_0012.JPG") && !is_file("ML/SOUP/100EOS5D/IMG_0012.TMP"));
    copy_write_fails = 0;
    {
        FILE *f = fopen("ML/SOUP/100EOS5D/IMG_0013.JPG", "wb");
        assert(f && fwrite("partial", 1, 7, f) == 7); fclose(f);
    }
    incomplete_hash = file_hash("ML/SOUP/100EOS5D/IMG_0013.JPG");
    for (i = 0; i < 3; i++) {
        int rc = soup_process_pair(dir, "IMG_", 13, &stock);
        assert(rc == -1);
        advanced = soup_finish_attempt(rc, &failures);
        assert(advanced == (i == 2 ? -1 : 0));
    }
    assert(advanced == -1 && !failures);
    assert(file_hash("ML/SOUP/100EOS5D/IMG_0013.JPG") == incomplete_hash);
    assert(soup_process_pair(dir, "IMG_", 12, &stock) == 1);
    assert(soup_finish_attempt(1, &failures) == 1);

    assert(file_hash(argv[1]) == raw_hash && file_hash(argv[2]) == jpg_hash);
    assert(file_hash("DCIM/100EOS5D/IMG_0001.CR2") == raw_hash);
    assert(file_hash("DCIM/100EOS5D/IMG_0003.CR2") == raw_hash);
    assert(file_hash("DCIM/100EOS5D/FLM_0010.JPG") == jpg_hash);
    assert(file_hash("DCIM/100EOS5D/_MG_0011.JPG") == jpg_hash);
    for (i = 2; i <= 8; i++) {
        snprintf(path, sizeof(path), "%s/IMG_%04d.JPG", dir, i);
        assert(file_hash(path) == jpg_hash);
    }
    printf("5D2 host checks passed: RAW, JPEG, paired dedup, prefixes, full output decode,\n");
    assert(file_hash("DCIM/100EOS5D/IMG_0012.JPG") == jpg_hash);
    assert(file_hash("DCIM/100EOS5D/IMG_0013.JPG") == jpg_hash);
    printf("busy/unstable inputs, read/write/copy failures, existing files, truncated input, bounded retries, originals unchanged.\n");
    return 0;
}
