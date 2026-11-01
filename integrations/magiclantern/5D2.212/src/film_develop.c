/* Styled native previews, with immutable originals and recoverable indexed updates. */
#include "film_transaction.h"
#ifndef SOUP_HOST_TEST
#include "mem.h"
#include "fileprefix.h"
#include "lens.h"
#include "notify_box.h"
#include "gui-common.h"
#include "debug.h"
#include "propvalues.h"
#endif
#include "se.h"
#include "cr2.h"
#include "stock.h"
#include "look.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"
#include "crc32.h"

#define SOUP_HDR (256u * 1024)
#define SOUP_SCRATCH (768u * 1024)
#define SOUP_THUMB 6144u

typedef struct { int scale, width, height, crop_x, crop_y; } soup_frame;
typedef struct {
    uint8_t thumb[SOUP_THUMB];
    uint32_t thumb_bytes;
    int width, height;
} soup_rendered;
typedef struct {
    FILE *file;
    uint8_t *dma;
    uint32_t base, length, written, limit, crc;
    int capped;
} soup_io;
typedef struct {
    se_jpeg *encoder;
    se_look_run *look;
    uint8_t *thumbnail;
    int decoded_width, width, height, crop_x, crop_y, row, thumb_row;
} soup_rows;

int soup_frame_geometry(int width, int height, int *scale, int *out_width,
                         int *out_height, int *crop_x, int *crop_y)
{
    *crop_x = *crop_y = 0;
    if (width == 5616 && height == 3744) {
        *scale = 2; *out_width = 2784; *out_height = 1856;
        *crop_x = 12; *crop_y = 8;
    } else if ((width == 4080 && height == 2720) || (width == 2784 && height == 1856)) {
        *scale = 1; *out_width = width; *out_height = height;
    } else return -1;
    return 0;
}

static int soup_idle(void)
{
#ifdef SOUP_HOST_TEST
    return soup_host_idle();
#else
    return lens_info.job_state == 0 && !RECORDING && !get_halfshutter_pressed();
#endif
}

static int soup_read(void *ctx, uint32_t off, void *dst, uint32_t bytes)
{
    soup_io *io = ctx;
    if (off > io->length || bytes > io->length - off || bytes > SOUP_IO ||
        soup_read_at(io->file, io->base + off, io->dma, bytes)) return -1;
    memcpy(dst, io->dma, bytes);
    return 0;
}

static int soup_write(void *ctx, const uint8_t *data, size_t bytes)
{
    soup_io *io = ctx;
    if (bytes > io->limit - io->written) { io->capped = 1; return -1; }
    while (bytes) {
        uint32_t n = bytes > SOUP_IO ? SOUP_IO : (uint32_t)bytes;
        memcpy(io->dma, data, n);
        if (FIO_WriteFile(io->file, io->dma, n) != (int)n) return -1;
        io->crc = crc32((void *)data, n, io->crc);
        io->written += n; data += n; bytes -= n;
    }
    return 0;
}

static int soup_emit(void *ctx, const uint8_t *row, int width)
{
    soup_rows *rows = ctx;
    int y = rows->row++ - rows->crop_y;
    const uint8_t *pixels;
    if (width != rows->decoded_width) return -1;
    if (y < 0 || y >= rows->height) return 0;
    pixels = row + rows->crop_x * 3;
    if (rows->thumbnail && rows->thumb_row < 107 &&
        y == rows->thumb_row * rows->height / 107) {
        uint8_t *dst = rows->thumbnail + (6 + rows->thumb_row) * 160 * 3;
        for (int x = 0; x < 160; x++)
            memcpy(dst + x * 3, pixels + (x * rows->width / 160) * 3, 3);
        rows->thumb_row++;
    }
    return rows->encoder && se_jpeg_write_rows(rows->encoder, pixels, 1) != SE_OK ? -1 : 0;
}

static int soup_grade(void *ctx, const uint8_t *rgb, int count, int width)
{
    soup_rows *rows = ctx;
    return width == rows->decoded_width &&
           se_look_rows(rows->look, rgb, count, soup_emit, rows) == SE_OK ? 0 : -1;
}

typedef struct { uint8_t *data; uint32_t size, limit; } soup_thumb_sink;
static int soup_thumb_write(void *ctx, const uint8_t *data, size_t bytes)
{
    soup_thumb_sink *sink = ctx;
    if (bytes > sink->limit - sink->size) return -1;
    memcpy(sink->data + sink->size, data, bytes); sink->size += (uint32_t)bytes;
    return 0;
}

static int soup_encode_thumb(se_arena *arena, const uint8_t *rgb, soup_rendered *rendered, uint32_t limit)
{
    if (limit > SOUP_THUMB) limit = SOUP_THUMB;
    for (int quality = 60; quality >= 20; quality -= 20) {
        se_jpeg *encoder;
        uint8_t *strip;
        soup_thumb_sink sink = {rendered->thumb, 0, limit};
        se_arena_reset(arena);
        encoder = SE_NEW(arena, se_jpeg, 1); strip = SE_NEW(arena, uint8_t, 8 * 160 * 3);
        if (!encoder || !strip) return -1;
        if (se_jpeg_begin(encoder, 160, 120, quality, strip, 0, soup_thumb_write, &sink) == SE_OK &&
            se_jpeg_write_rows(encoder, rgb, 120) == SE_OK && se_jpeg_end(encoder) == SE_OK) {
            rendered->thumb_bytes = sink.size;
            return 0;
        }
    }
    return -1;
}

static int soup_count_rows(void *ctx, const uint8_t *rgb, int count, int width)
{
    int *rows = ctx;
    (void)rgb;
    if (count <= 0 || width <= 0) return -1;
    *rows += count;
    return 0;
}

static int soup_validate_render(const char *path, const soup_rendered *expected, se_arena *arena)
{
    uint8_t *header = 0, *dma = 0, *thumb = 0;
    uint32_t size = 0, got_thumb = 0, bytes;
    soup_io input = {0};
    se_jdec_info info;
    int rows = 0, width, height, result = -1;
    if (FIO_GetFileSize(path, &size) || size < 4 || size > SOUP_MAX_FILE) return -1;
    bytes = size < SOUP_HDR ? size : SOUP_HDR;
    header = fio_malloc(bytes); dma = fio_malloc(SOUP_IO); thumb = malloc(SOUP_THUMB);
    input.file = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    if (!header || !dma || !thumb || !input.file || soup_read_at(input.file, 0, header, bytes) ||
        se_jpeg_dec_info(header, bytes, &info) != SE_OK ||
        info.width != expected->width || info.height != expected->height) goto done;
    input.dma = dma; input.length = size;
    se_jpeg_scaled_size(info.width, info.height, 2, &width, &height);
    se_arena_reset(arena);
    if (se_jpeg_decode_stream_scaled(header, bytes, size, soup_read, &input, 2,
                                     arena, soup_count_rows, &rows, &info) != SE_OK ||
        rows != height || info.width != width || info.height != height ||
        soup_jpeg_thumbnail(path, thumb, SOUP_THUMB, &got_thumb) ||
        got_thumb != expected->thumb_bytes || memcmp(thumb, expected->thumb, got_thumb) ||
        se_jpeg_dec_info(thumb, got_thumb, &info) != SE_OK || info.width != 160 || info.height != 120) goto done;
    rows = 0; se_arena_reset(arena);
    if (se_jpeg_decode(thumb, got_thumb, arena, soup_count_rows, &rows, &info) != SE_OK || rows != 120) goto done;
    result = 0;
done:
    if (input.file) FIO_CloseFile(input.file);
    fio_free(header); fio_free(dma); free(thumb);
    return result;
}

static int soup_render(const char *input, const char *output, int is_cr2, se_stock *stock,
                         uint32_t limit, uint32_t thumb_limit, soup_rendered *rendered)
{
    uint32_t size = 0, header_size, offset = 0, length = 0, after = 0;
    uint8_t *header = 0, *block = 0, *read_dma = 0, *write_dma = 0, *thumb_rgb = 0;
    se_arena arena, look_arena;
    se_jdec_info image;
    se_cr2_info metadata;
    se_jpeg_exif exif;
    se_look look;
    soup_frame frame;
    soup_io in = {0}, out = {0};
    soup_rows rows = {0};
    size_t look_bytes;
    int decoded_width, decoded_height, orientation = 1, result = -1, created = 0;
    char date[24] = "";
    se_err rc;
    if (!input || !output || !stock || !rendered || is_file(output) ||
        FIO_GetFileSize(input, &size) || size < 4 || size > SOUP_MAX_FILE) return -1;
    memset(rendered, 0, sizeof(*rendered));
    crc32_init();
    in.file = FIO_OpenFile(input, O_RDONLY | O_SYNC); header = fio_malloc(SOUP_HDR);
    if (!in.file || !header) goto done;
    header_size = size < SOUP_HDR ? size : SOUP_HDR;
    if (soup_read_at(in.file, 0, header, header_size)) goto done;
    memset(&metadata, 0, sizeof(metadata)); length = size;
    if (is_cr2) {
        soup_raw_layout layout;
        if (soup_raw_inspect(input, &layout) ||
            se_cr2_find_preview(header, header_size, &offset, &length, 0, 0) != SE_OK ||
            offset >= header_size || offset > size || length > size - offset) goto done;
        if (se_cr2_parse(header, header_size, &metadata) != SE_OK) goto done;
        orientation = layout.orientation;
        if (!limit || limit > layout.main_length) limit = layout.main_length;
        if (thumb_limit > layout.thumb_length) thumb_limit = layout.thumb_length;
    } else if (soup_jpeg_metadata(input, &orientation, date)) goto done;
    if (!limit || limit > length) limit = length;
    if (se_jpeg_dec_info(header + offset, header_size - offset, &image) != SE_OK ||
        soup_frame_geometry(image.width, image.height, &frame.scale, &frame.width, &frame.height,
                             &frame.crop_x, &frame.crop_y)) goto done;
    {
        uint8_t *tail = fio_malloc(2);
        int eoi = tail && length >= 4 && !soup_read_at(in.file, offset + length - 2, tail, 2) &&
                  tail[0] == 0xff && tail[1] == 0xd9;
        fio_free(tail);
        if (!eoi) goto done;
    }
    se_jpeg_scaled_size(image.width, image.height, frame.scale, &decoded_width, &decoded_height);
    (void)decoded_height;
    se_look_from_stock(stock, frame.scale, &look);
    look_bytes = se_look_fit(&look, decoded_width, SE_LOOK_BUDGET);
    block = malloc(SOUP_SCRATCH + look_bytes); read_dma = fio_malloc(SOUP_IO);
    write_dma = fio_malloc(SOUP_IO); thumb_rgb = malloc(160 * 120 * 3);
    if (!block || !read_dma || !write_dma || !thumb_rgb) goto done;
    memset(thumb_rgb, 0, 160 * 120 * 3);
    se_arena_init(&arena, block, SOUP_SCRATCH);
    se_arena_init(&look_arena, block + SOUP_SCRATCH, look_bytes);
    rows.decoded_width = decoded_width; rows.width = frame.width; rows.height = frame.height;
    rows.crop_x = frame.crop_x; rows.crop_y = frame.crop_y; rows.thumbnail = thumb_rgb;
    in.dma = read_dma; in.base = offset; in.length = length;
    if (se_look_begin(&rows.look, &look, decoded_width, &look_arena) != SE_OK) goto done;
    rc = se_jpeg_decode_stream_scaled(header + offset, header_size - offset, length,
                                      soup_read, &in, frame.scale, &arena, soup_grade, &rows, &image);
    if (rc != SE_OK || se_look_end(rows.look, soup_emit, &rows) != SE_OK || rows.thumb_row != 107 ||
        soup_encode_thumb(&arena, thumb_rgb, rendered, thumb_limit)) goto done;
    rendered->width = frame.width; rendered->height = frame.height;
    memset(&exif, 0, sizeof(exif));
    exif.make = metadata.make[0] ? metadata.make : "Canon";
    exif.model = metadata.model[0] ? metadata.model : "Canon EOS 5D Mark II";
    exif.software = stock->name; exif.orientation = orientation;
    exif.datetime = is_cr2 ? metadata.datetime : date;
    exif.exposure_time = metadata.exposure_time; exif.aperture = metadata.aperture; exif.iso = metadata.iso;
    exif.thumb = rendered->thumb; exif.thumb_len = (int)rendered->thumb_bytes;
    for (int quality = 88; quality >= 28; quality -= 15) {
        uint8_t *strip;
        se_arena_reset(&arena); se_arena_reset(&look_arena);
        rows.encoder = SE_NEW(&arena, se_jpeg, 1);
        strip = SE_NEW(&arena, uint8_t, (size_t)8 * frame.width * 3);
        if (!rows.encoder || !strip || se_look_begin(&rows.look, &look, decoded_width, &look_arena) != SE_OK) goto done;
        rows.thumbnail = 0; rows.row = rows.thumb_row = 0;
        memset(&out, 0, sizeof(out)); out.dma = write_dma; out.limit = limit; out.crc = 0xffffffffu;
        out.file = soup_create_new(output); if (!out.file) goto done;
        created = 1;
        rc = se_jpeg_begin(rows.encoder, frame.width, frame.height, quality, strip, &exif, soup_write, &out);
        if (rc == SE_OK)
            rc = se_jpeg_decode_stream_scaled(header + offset, header_size - offset, length,
                                              soup_read, &in, frame.scale, &arena, soup_grade, &rows, &image);
        if (rc == SE_OK) rc = se_look_end(rows.look, soup_emit, &rows);
        if (rc == SE_OK) rc = se_jpeg_end(rows.encoder);
        FIO_CloseFile(out.file); out.file = 0;
        if (rc == SE_OK && out.written > 4 && !FIO_GetFileSize(input, &after) && after == size &&
            !FIO_GetFileSize(output, &after) && after == out.written) {
            uint32_t read_size = 0, read_crc = 0;
            if (!soup_file_crc(output, &read_size, &read_crc) && read_size == out.written &&
                read_crc == (out.crc ^ 0xffffffffu) && !soup_validate_render(output, rendered, &arena)) {
                result = (int)out.written; break;
            }
        }
        if (FIO_RemoveFile(output)) goto done;
        created = 0;
        if (!out.capped) goto done;
    }
done:
    if (out.file) FIO_CloseFile(out.file);
    if (in.file) FIO_CloseFile(in.file);
    if (result < 0 && created) FIO_RemoveFile(output);
    free(block); free(thumb_rgb); fio_free(header); fio_free(read_dma); fio_free(write_dma);
    return result;
}

int soup_develop_file(const char *input, const char *output, int is_cr2, se_stock *stock)
{
    soup_rendered rendered;
    return soup_render(input, output, is_cr2, stock, 0, SOUP_THUMB, &rendered);
}

static int soup_overlay_jpeg(const char *jpeg, const char *stage)
{
    FILE *in = 0, *out = 0;
    uint8_t *buffer = 0;
    uint32_t size = 0, capacity = 0, offset = 0;
    int result = -1;
    if (FIO_GetFileSize(jpeg, &size) || FIO_GetFileSize(stage, &capacity) || size > capacity || !size) return -1;
    in = FIO_OpenFile(jpeg, O_RDONLY | O_SYNC); buffer = fio_malloc(SOUP_IO);
    if (!in || !buffer) goto done;
    out = FIO_OpenFile(stage, O_RDWR | O_SYNC); if (!out) goto done;
    /* shortcut: indexed JPEGs retain ignored bytes after the new EOI, replace this approach if hardware playback rejects trailing data. */
    while (offset < size) {
        uint32_t n = size - offset; if (n > SOUP_IO) n = SOUP_IO;
        if (soup_read_at(in, offset, buffer, n) || soup_write_at(out, offset, buffer, n)) goto done;
        offset += n;
    }
    FIO_CloseFile(out); out = 0;
    if (!soup_equal_ranges(jpeg, 0, stage, 0, size)) result = 0;
done:
    if (in) FIO_CloseFile(in);
    if (out) FIO_CloseFile(out);
    fio_free(buffer);
    return result;
}

static int soup_make_dirs(const soup_transaction *tx)
{
    char root[24], path[SOUP_PATH];
    const char *base = tx->directory;
    if (base[0] == 'A' && base[1] == ':') { snprintf(root, sizeof(root), "A:/ML/SOUP"); base += 8; }
    else { snprintf(root, sizeof(root), "ML/SOUP"); base += 5; }
    if (!is_dir(root) && FIO_CreateDirectory(root)) return -1;
    for (int i = 0; i < 2; i++) {
        const char *kind = i ? "STAGED" : "ORIGINALS";
        snprintf(path, sizeof(path), "%s/%s", root, kind);
        if (!is_dir(path) && FIO_CreateDirectory(path)) return -1;
        snprintf(path, sizeof(path), "%s/%s/%s", root, kind, base);
        if (!is_dir(path) && FIO_CreateDirectory(path)) return -1;
    }
    return 0;
}

/* 1 complete, 0 busy, -1 safely failed, -2 recovery needed, -3 backup creation unavailable. */
int soup_process_pair(const char *dir, const char *prefix, int number, se_stock *stock)
{
    soup_transaction tx;
    soup_raw_layout raw;
    soup_rendered rendered;
    char jpg[SOUP_PATH], cr2[SOUP_PATH], render[SOUP_PATH];
    uint32_t a = 0, b = 0, aa = 0, bb = 0, limit = 0, thumb_limit = SOUP_THUMB;
    unsigned members;
    int complete = 1, created[2] = {0, 0}, result = -1;
    if (soup_receipt_pending()) {
        int recovery = soup_recover_pending();
        if (recovery == SOUP_RECOVERY_BUSY) return 0;
        if (recovery < 0) return -2;
    }
    if (!dir || !prefix || !stock || soup_transaction_init(&tx, dir, prefix, number, 1) ||
        snprintf(jpg, sizeof(jpg), "%s/%s%04d.JPG", dir, prefix, number) >= (int)sizeof(jpg) ||
        snprintf(cr2, sizeof(cr2), "%s/%s%04d.CR2", dir, prefix, number) >= (int)sizeof(cr2)) return -1;
    if (!soup_idle()) return 0;
    if (FIO_GetFileSize(jpg, &a)) a = 0;
    if (FIO_GetFileSize(cr2, &b)) b = 0;
    members = (a ? SOUP_JPEG_MEMBER : 0) | (b ? SOUP_RAW_MEMBER : 0);
    if (!members) return 0;
    msleep(1000);
    if (FIO_GetFileSize(jpg, &aa)) aa = 0;
    if (FIO_GetFileSize(cr2, &bb)) bb = 0;
    if (!soup_idle() || a != aa || b != bb) return 0;
    if (soup_transaction_init(&tx, dir, prefix, number, members)) return -1;
    for (unsigned i = 0; i < 2; i++) if (members & (1u << i))
        if (!is_file(tx.files[i].stage) || !is_file(tx.files[i].backup) ||
            soup_equal_files(tx.files[i].source, tx.files[i].stage)) complete = 0;
    if (complete) return 1;
    for (unsigned i = 0; i < 2; i++) if ((members & (1u << i)) && is_file(tx.files[i].stage)) return -1;
    if (soup_make_dirs(&tx)) return -1;
    if (soup_storage_check()) return -3;
    if (soup_transaction_prepare(&tx)) return -1;
    {
        const char *stage = members & SOUP_JPEG_MEMBER ? tx.files[0].stage : tx.files[1].stage;
        snprintf(render, sizeof(render), "%s", stage);
        memcpy(render + strlen(render) - 3, "NEW", 3);
    }
    if (is_file(render)) return -1;
    if (members & SOUP_JPEG_MEMBER) limit = tx.files[0].size;
    if (members & SOUP_RAW_MEMBER) {
        if (soup_raw_inspect(tx.files[1].backup, &raw)) return -1;
        if (!limit || raw.main_length < limit) limit = raw.main_length;
        thumb_limit = raw.thumb_length < SOUP_THUMB ? raw.thumb_length : SOUP_THUMB;
    }
    if (soup_render(members & SOUP_JPEG_MEMBER ? tx.files[0].backup : tx.files[1].backup,
                    render, !(members & SOUP_JPEG_MEMBER), stock, limit, thumb_limit, &rendered) < 0) return -1;
    if (members & SOUP_JPEG_MEMBER) {
        if (soup_copy_new(tx.files[0].backup, tx.files[0].stage)) goto done;
        created[0] = 1;
        if (soup_overlay_jpeg(render, tx.files[0].stage)) goto done;
        {
            uint32_t prefix_bytes = 0;
            if (FIO_GetFileSize(render, &prefix_bytes) || prefix_bytes > tx.files[0].size ||
                soup_equal_ranges(tx.files[0].backup, prefix_bytes, tx.files[0].stage,
                                    prefix_bytes, tx.files[0].size - prefix_bytes)) goto done;
        }
    }
    if (members & SOUP_RAW_MEMBER) {
        if (soup_raw_stage(tx.files[1].backup, tx.files[1].stage, render, rendered.thumb,
                           rendered.thumb_bytes, rendered.width, rendered.height, &raw)) goto done;
        created[1] = 1;
    }
    for (unsigned i = 0; i < 2; i++) if (members & (1u << i)) {
        uint32_t stage_size = 0;
        if (soup_file_crc(tx.files[i].stage, &stage_size, &tx.files[i].stage_crc) || stage_size != tx.files[i].size) goto done;
    }
    {
        int committed = soup_transaction_commit(&tx);
        if (committed) {
            result = committed == SOUP_COMMIT_BUSY ? 0 : (soup_receipt_pending() ? -2 : -1);
            goto done;
        }
    }
    result = 1;
#ifndef SOUP_HOST_TEST
    if (soup_idle() && is_pure_play_photo_mode()) { exit_play_qr_mode(); if (soup_idle()) enter_play_mode(); }
#endif
done:
    if (result != 1 && !soup_receipt_pending()) {
        for (int i = 0; i < 2; i++) if (created[i]) FIO_RemoveFile(tx.files[i].stage);
        FIO_RemoveFile(render);
    }
    return result;
}

int soup_finish_attempt(int result, int *failures)
{
    if (result > 0) { *failures = 0; return 1; }
    if (result < 0 && ++*failures >= 3) { *failures = 0; return -1; }
    return 0;
}

#ifndef SOUP_HOST_TEST
extern int soup_is_on(void);
extern int soup_stock_path(char *path, int size);
extern void soup_disable_for_recovery(void);

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
        if (rc == -3) {
            soup_disable_for_recovery();
            NotifyBox(10000, "Soup cannot safely create backups; photo unchanged");
            continue;
        }
        if (rc == -2) {
            soup_disable_for_recovery();
            NotifyBox(10000, "Soup storage/recovery needs attention; disabled");
            continue;
        }
        settled = soup_finish_attempt(rc, &failures);
        if (settled) {
            if (settled < 0) {
                NotifyBox(5000, "Soup: skipped %s%04d; backups kept", prefix, cursor + 1);
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
            NotifyBox(3000, "Soup: update failed; backups kept");
            msleep(3000);
        }
        msleep(250);
    }
}
#endif
