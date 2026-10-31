/* Half-size film JPEGs from completed Canon JPEGs or CR2 embedded previews. */
#ifdef SOUP_HOST_TEST
#include "film_host.h"
#else
#include "dryos.h"
#include "mem.h"
#include "fio-ml.h"
#include "fileprefix.h"
#include "lens.h"
#include "notify_box.h"
#endif
#include "se.h"
#include "cr2.h"
#include "stock.h"
#include "look.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"

#define SOUP_HDR (256u * 1024)
#define SOUP_SCRATCH (768u * 1024)
#define SOUP_IO 8192u
#define SOUP_MAX_FILE (128u * 1024 * 1024)
#define SOUP_MAX_OUTPUT (32u * 1024 * 1024)
#define SOUP_PATH 96
#define SOUP_SCALE 2

typedef struct {
    FILE *file;
    uint8_t *dma;
    uint32_t base, length, written;
} soup_io;

typedef struct {
    se_jpeg *encoder;
    se_look_run *look;
    int width;
} soup_rows;

static int soup_idle(void)
{
#ifdef SOUP_HOST_TEST
    return soup_host_idle();
#else
    return lens_info.job_state == 0;
#endif
}

static int soup_read_at(FILE *f, uint32_t off, void *dst, uint32_t bytes)
{
    uint32_t done = 0;
    if (off > SOUP_MAX_FILE || bytes > SOUP_MAX_FILE - off ||
        FIO_SeekSkipFile(f, (int)off, 0) < 0) return -1;
    while (done < bytes) {
        int got = FIO_ReadFile(f, (uint8_t *)dst + done, bytes - done);
        if (got <= 0 || (uint32_t)got > bytes - done) return -1;
        done += (uint32_t)got;
    }
    return 0;
}

static int soup_read(void *ctx, uint32_t off, void *dst, uint32_t bytes)
{
    soup_io *io = ctx;
    if (off > io->length || bytes > io->length - off || bytes > SOUP_IO) return -1;
    if (soup_read_at(io->file, io->base + off, io->dma, bytes)) return -1;
    memcpy(dst, io->dma, bytes);
    return 0;
}

static int soup_write(void *ctx, const uint8_t *data, size_t bytes)
{
    soup_io *io = ctx;
    if (bytes > SOUP_MAX_OUTPUT - io->written) return -1;
    while (bytes) {
        uint32_t n = bytes > SOUP_IO ? SOUP_IO : (uint32_t)bytes;
        memcpy(io->dma, data, n);
        if (FIO_WriteFile(io->file, io->dma, n) != (int)n) return -1;
        io->written += n;
        data += n;
        bytes -= n;
    }
    return 0;
}

static int soup_emit(void *ctx, const uint8_t *row, int width)
{
    soup_rows *rows = ctx;
    return width == rows->width &&
           se_jpeg_write_rows(rows->encoder, row, 1) == SE_OK ? 0 : -1;
}

static int soup_grade(void *ctx, const uint8_t *rgb, int count, int width)
{
    soup_rows *rows = ctx;
    return width == rows->width &&
           se_look_rows(rows->look, rgb, count, soup_emit, rows) == SE_OK ? 0 : -1;
}

static int soup_has_eoi(FILE *file, uint32_t offset, uint32_t bytes, void *dma)
{
    uint8_t *p = dma;
    return bytes >= 4 && !soup_read_at(file, offset + bytes - 2, dma, 2) &&
           p[0] == 0xff && p[1] == 0xd9;
}

/* Returns the number of bytes written, or -1. Output must not already exist. */
int soup_develop_file(const char *input, const char *output, int is_cr2, se_stock *stock)
{
    uint32_t size = 0, header_size, offset = 0, length = 0, final_size = 0;
    uint8_t *header = 0, *block = 0, *read_dma = 0, *write_dma = 0;
    se_arena arena, look_arena;
    se_jdec_info image;
    se_cr2_info metadata;
    se_jpeg_exif exif;
    se_look look;
    soup_io in = {0}, out = {0};
    soup_rows rows = {0};
    size_t look_bytes;
    int width, height, result = -1;
    se_err rc;

    if (!input || !output || !stock || is_file(output) ||
        FIO_GetFileSize(input, &size) || size < 4 || size > SOUP_MAX_FILE) return -1;
    in.file = FIO_OpenFile(input, 0);
    header = fio_malloc(SOUP_HDR);
    if (!in.file || !header) goto done;
    header_size = size < SOUP_HDR ? size : SOUP_HDR;
    if (soup_read_at(in.file, 0, header, header_size)) goto done;
    memset(&metadata, 0, sizeof(metadata));
    length = size;
    if (is_cr2) {
        if (se_cr2_find_preview(header, header_size, &offset, &length, 0, 0) != SE_OK ||
            offset >= header_size || offset > size || length > size - offset) goto done;
        if (se_cr2_parse(header, header_size, &metadata) != SE_OK)
            memset(&metadata, 0, sizeof(metadata));
    }
    if (se_jpeg_dec_info(header + offset, header_size - offset, &image) != SE_OK ||
        image.width < 2 || image.height < 2 || image.width > 6000 || image.height > 6000 ||
        !soup_has_eoi(in.file, offset, length, header)) goto done;
    /* The tail check reused the first two header bytes. Restore them before decode. */
    if (soup_read_at(in.file, 0, header, header_size)) goto done;
    se_jpeg_scaled_size(image.width, image.height, SOUP_SCALE, &width, &height);
    se_look_from_stock(stock, SOUP_SCALE, &look);
    look_bytes = se_look_fit(&look, width, SE_LOOK_BUDGET);
    block = malloc(SOUP_SCRATCH + look_bytes);
    read_dma = fio_malloc(SOUP_IO);
    write_dma = fio_malloc(SOUP_IO);
    if (!block || !read_dma || !write_dma) goto done;
    se_arena_init(&arena, block, SOUP_SCRATCH);
    se_arena_init(&look_arena, block + SOUP_SCRATCH, look_bytes);
    rows.encoder = SE_NEW(&arena, se_jpeg, 1);
    if (!rows.encoder || se_look_begin(&rows.look, &look, width, &look_arena) != SE_OK) goto done;
    rows.width = width;
    memset(&exif, 0, sizeof(exif));
    exif.make = metadata.make[0] ? metadata.make : "Canon";
    exif.model = metadata.model[0] ? metadata.model : "Canon EOS 5D Mark II";
    exif.software = stock->name;
    exif.datetime = metadata.datetime;
    exif.exposure_time = metadata.exposure_time;
    exif.aperture = metadata.aperture;
    exif.iso = metadata.iso;
    {
        uint8_t *strip = SE_NEW(&arena, uint8_t, (size_t)8 * width * 3);
        if (!strip) goto done;
        out.file = FIO_CreateFile(output);
        if (!out.file) goto done;
        out.dma = write_dma;
        rc = se_jpeg_begin(rows.encoder, width, height, 88, strip, &exif, soup_write, &out);
        if (rc != SE_OK) goto done;
    }
    in.dma = read_dma;
    in.base = offset;
    in.length = length;
    rc = se_jpeg_decode_stream_scaled(header + offset, header_size - offset, length,
                                      soup_read, &in, SOUP_SCALE, &arena,
                                      soup_grade, &rows, &image);
    if (rc == SE_OK) rc = se_look_end(rows.look, soup_emit, &rows);
    if (rc == SE_OK) rc = se_jpeg_end(rows.encoder);
    FIO_CloseFile(out.file);
    out.file = 0;
    if (rc == SE_OK && out.written > 4 &&
        !FIO_GetFileSize(input, &final_size) && final_size == size &&
        !FIO_GetFileSize(output, &final_size) && final_size == out.written)
        result = (int)out.written;
done:
    if (out.file) FIO_CloseFile(out.file);
    if (in.file) FIO_CloseFile(in.file);
    /* Only an output opened by this call is ours to remove. */
    if (result < 0 && out.dma && is_file(output)) FIO_RemoveFile(output);
    free(block);
    fio_free(header);
    fio_free(read_dma);
    fio_free(write_dma);
    return result;
}

static int soup_component(const char *text, int min, int max)
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

static int soup_complete_jpeg(const char *path)
{
    uint32_t size = 0;
    uint8_t *header;
    FILE *file;
    se_jdec_info info;
    int ok = 0;
    if (FIO_GetFileSize(path, &size) || size < 4 || size > SOUP_MAX_OUTPUT) return 0;
    header = fio_malloc(SOUP_HDR);
    file = FIO_OpenFile(path, 0);
    if (header && file && !soup_read_at(file, 0, header, size < SOUP_HDR ? size : SOUP_HDR) &&
        se_jpeg_dec_info(header, size < SOUP_HDR ? size : SOUP_HDR, &info) == SE_OK &&
        info.width > 0 && info.height > 0 && info.width <= 3000 && info.height <= 3000 &&
        soup_has_eoi(file, 0, size, header)) ok = 1;
    if (file) FIO_CloseFile(file);
    fio_free(header);
    return ok;
}

/* ML's 5D2 rename fallback does not safely handle read errors. */
static int soup_publish(const char *temp, const char *output, uint32_t expected)
{
    FILE *in = 0, *out = 0;
    uint8_t *dma = 0;
    uint32_t size = 0, done = 0;
    int created = 0, result = -1;
    if (is_file(output) || FIO_GetFileSize(temp, &size) || size != expected ||
        size < 4 || size > SOUP_MAX_OUTPUT) return -1;
    dma = fio_malloc(SOUP_IO);
    in = FIO_OpenFile(temp, 0);
    if (!dma || !in) goto done;
    out = FIO_CreateFile(output);
    if (!out) goto done;
    created = 1;
    while (done < expected) {
        uint32_t bytes = expected - done;
        if (bytes > SOUP_IO) bytes = SOUP_IO;
        if (soup_read_at(in, done, dma, bytes) ||
            FIO_WriteFile(out, dma, bytes) != (int)bytes) goto done;
        done += bytes;
    }
    FIO_CloseFile(out); out = 0;
    if (!FIO_GetFileSize(output, &size) && size == expected && soup_complete_jpeg(output)) result = 0;
done:
    if (out) FIO_CloseFile(out);
    if (in) FIO_CloseFile(in);
    fio_free(dma);
    if (result < 0 && created) FIO_RemoveFile(output);
    if (result == 0) FIO_RemoveFile(temp);
    return result;
}

/* 1 = completed, -1 = skipped after three failures, 0 = wait/retry. */
int soup_finish_attempt(int result, int *failures)
{
    if (result > 0) { *failures = 0; return 1; }
    if (result < 0 && ++*failures >= 3) { *failures = 0; return -1; }
    return 0;
}

/* 1 = made/already present, 0 = not yet stable, -1 = error; never writes sources. */
int soup_process_pair(const char *dir, const char *prefix, int number, se_stock *stock)
{
    char jpg[SOUP_PATH], cr2[SOUP_PATH], root[SOUP_PATH], folder[SOUP_PATH];
    char output[SOUP_PATH], temp[SOUP_PATH];
    const char *base, *input;
    uint32_t jpg_size = 0, cr2_size = 0, now_jpg = 0, now_cr2 = 0;
    int written;
    if (!dir || !soup_component(prefix, 4, 4) || number < 1 || number > 9999) return -1;
    base = dir;
    for (const char *p = dir; *p; p++) if (*p == '/') base = p + 1;
    if (!soup_component(base, 1, 16)) return -1;
    if (snprintf(jpg, sizeof(jpg), "%s/%s%04d.JPG", dir, prefix, number) >= (int)sizeof(jpg) ||
        snprintf(cr2, sizeof(cr2), "%s/%s%04d.CR2", dir, prefix, number) >= (int)sizeof(cr2)) return -1;
    if (dir[0] && dir[1] == ':') snprintf(root, sizeof(root), "%c:/ML/SOUP", dir[0]);
    else snprintf(root, sizeof(root), "ML/SOUP");
    if (snprintf(folder, sizeof(folder), "%s/%s", root, base) >= (int)sizeof(folder) ||
        snprintf(output, sizeof(output), "%s/%s%04d.JPG", folder, prefix, number) >= (int)sizeof(output) ||
        snprintf(temp, sizeof(temp), "%s/%s%04d.TMP", folder, prefix, number) >= (int)sizeof(temp)) return -1;
    if (is_file(output)) return soup_complete_jpeg(output) ? 1 : -1;
    if (is_file(temp)) return -1;
    if (!soup_idle()) return 0;
    if (FIO_GetFileSize(jpg, &jpg_size)) jpg_size = 0;
    if (FIO_GetFileSize(cr2, &cr2_size)) cr2_size = 0;
    if (!jpg_size && !cr2_size) return 0;
    msleep(1000);
    if (FIO_GetFileSize(jpg, &now_jpg)) now_jpg = 0;
    if (FIO_GetFileSize(cr2, &now_cr2)) now_cr2 = 0;
    if (!soup_idle() || jpg_size != now_jpg || cr2_size != now_cr2) return 0;
    input = jpg_size ? jpg : cr2;
    if ((!is_dir(root) && FIO_CreateDirectory(root)) ||
        (!is_dir(folder) && FIO_CreateDirectory(folder))) return -1;
    written = soup_develop_file(input, temp, !jpg_size, stock);
    if (written < 0) return -1;
    if (soup_publish(temp, output, (uint32_t)written)) {
        if (is_file(temp)) FIO_RemoveFile(temp);
        return -1;
    }
    return 1;
}

#ifndef SOUP_HOST_TEST
extern int soup_is_on(void);
extern int soup_stock_path(char *path, int size);

static int soup_load_stock(se_stock *stock)
{
    char path[64];
    char *text;
    uint32_t size = 0;
    int got, result = -1;
    if (soup_stock_path(path, sizeof(path)) || FIO_GetFileSize(path, &size) ||
        !size || size >= 16384) return -1;
    text = fio_malloc(size + 1);
    if (!text) return -1;
    got = (int)read_file(path, text, size);
    if (got == (int)size) {
        text[size] = 0;
        se_stock_init(stock);
        if (se_stock_parse(text, size, stock) == SE_OK) result = 0;
    }
    fio_free(text);
    return result;
}

static int soup_last_number(const char *dir, const char *prefix)
{
    struct fio_file file;
    struct fio_dirent *it = FIO_FindFirstEx(dir, &file);
    int last = 0;
    if (!it || it == (void *)-1) return -1;
    do {
        int i, number = 0;
        if (strlen(file.name) != 12 || strncmp(file.name, prefix, 4) ||
            (strcmp(file.name + 8, ".JPG") && strcmp(file.name + 8, ".CR2"))) continue;
        for (i = 4; i < 8; i++) {
            if (file.name[i] < '0' || file.name[i] > '9') { number = 0; break; }
            number = number * 10 + file.name[i] - '0';
        }
        if (number > last) last = number;
    } while (FIO_FindNextEx(it, &file) == 0);
    FIO_FindClose(it);
    return last;
}

void soup_worker(void *unused)
{
    char dir[SOUP_PATH] = "", prefix[5] = "";
    int cursor = -1, failures = 0;
    (void)unused;
    for (;;) {
        struct card_info *card;
        const char *current_dir, *current_prefix;
        int current, target, changing, rc, settled;
        se_stock stock;
        if (!soup_is_on()) { cursor = -1; failures = 0; msleep(250); continue; }
        card = get_shooting_card();
        current_dir = get_dcim_dir();
        current_prefix = get_file_prefix();
        current = card ? card->file_number : 0;
        if (!current_dir || !current_prefix || current < 0 || current > 9999 || !soup_idle()) {
            msleep(500); continue;
        }
        if (cursor < 0) {
            snprintf(dir, sizeof(dir), "%s", current_dir);
            snprintf(prefix, sizeof(prefix), "%s", current_prefix);
            cursor = current;
        }
        changing = strcmp(dir, current_dir) || strcmp(prefix, current_prefix) || current < cursor;
        target = changing ? soup_last_number(dir, prefix) : current;
        if (target < 0) { NotifyBox(3000, "Soup: cannot finish previous folder"); msleep(3000); continue; }
        if (cursor >= target) {
            if (changing) {
                int same_dir = !strcmp(dir, current_dir);
                snprintf(dir, sizeof(dir), "%s", current_dir);
                snprintf(prefix, sizeof(prefix), "%s", current_prefix);
                cursor = same_dir && current >= cursor ? cursor : 0;
            }
            msleep(250); continue;
        }
        if (soup_load_stock(&stock)) { NotifyBox(3000, "Soup: cannot load stock"); msleep(3000); continue; }
        NotifyBox(600000, "Soup: developing %s%04d", prefix, cursor + 1);
        rc = soup_process_pair(dir, prefix, cursor + 1, &stock);
        NotifyBoxHide();
        settled = soup_finish_attempt(rc, &failures);
        if (settled) {
            if (settled < 0) {
                NotifyBox(5000, "Soup: skipped %s%04d; originals kept", prefix, cursor + 1);
                msleep(3000);
            }
            cursor++;
        }
        else if (!rc && (changing || cursor + 1 < target)) {
            char input[SOUP_PATH];
            snprintf(input, sizeof(input), "%s/%s%04d.JPG", dir, prefix, cursor + 1);
            if (!is_file(input)) {
                snprintf(input, sizeof(input), "%s/%s%04d.CR2", dir, prefix, cursor + 1);
                if (!is_file(input)) { cursor++; failures = 0; }
            }
        } else if (rc < 0) {
            NotifyBox(3000, "Soup: output failed; originals kept");
            msleep(3000);
        }
        msleep(250);
    }
}
#endif
