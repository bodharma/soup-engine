/* Bounded TIFF inspection. RAW edits are confined to the two existing JPEG spans. */
#include "film_transaction.h"
#ifndef SOUP_HOST_TEST
#include "mem.h"
#endif
#include "cr2.h"
#include "jpeg_dec.h"

#define SOUP_HEADER (256u * 1024)
typedef struct { uint8_t *data; uint32_t bytes; int be; } soup_tiff;

static int range(uint32_t offset, uint32_t bytes, uint32_t total)
{ return offset <= total && bytes <= total - offset; }
static uint16_t u16(const soup_tiff *t, uint32_t at)
{
    const uint8_t *p = t->data + at;
    return t->be ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t u32(const soup_tiff *t, uint32_t at)
{
    const uint8_t *p = t->data + at;
    return t->be ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]
                 : p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int table(const soup_tiff *t, uint32_t at, soup_span *span, uint32_t *next)
{
    uint32_t n;
    if (!range(at, 2, t->bytes)) return -1;
    n = u16(t, at);
    if (!n || n > 128 || !range(at, 2 + n * 12 + 4, t->bytes)) return -1;
    span->offset = at; span->length = 2 + n * 12 + 4;
    *next = u32(t, at + 2 + n * 12);
    return 0;
}

static int entry(const soup_tiff *t, uint32_t ifd, unsigned tag, uint32_t *value,
                  uint32_t *field, int *type)
{
    soup_span span;
    uint32_t next, found = 0;
    if (table(t, ifd, &span, &next)) return -1;
    for (uint32_t k = ifd + 2; k < ifd + span.length - 4; k += 12) {
        int ty = u16(t, k + 2);
        if (u16(t, k) != tag) continue;
        if (found++ || u32(t, k + 4) != 1 || (ty != 3 && ty != 4)) return -1;
        *value = ty == 3 ? u16(t, k + 8) : u32(t, k + 8);
        if (field) *field = k + 8;
        if (type) *type = ty;
    }
    return found ? 0 : -1;
}

static int overlaps(soup_span a, soup_span b)
{ return a.offset < b.offset + b.length && b.offset < a.offset + a.length; }

int soup_raw_inspect(const char *path, soup_raw_layout *layout)
{
    uint8_t *header = 0;
    FILE *f = 0;
    uint32_t size = 0, bytes, ifds[4], next, value;
    soup_span protected[12];
    int protected_count = 1, result = -1, type;
    soup_tiff t;
    se_cr2_info info;
    se_jdec_info jpeg;
    if (!layout || FIO_GetFileSize(path, &size) || size < 16 || size > SOUP_MAX_FILE) return -1;
    memset(layout, 0, sizeof(*layout));
    header = fio_malloc(SOUP_HEADER); f = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    bytes = size < SOUP_HEADER ? size : SOUP_HEADER;
    if (!header || !f || soup_read_at(f, 0, header, bytes)) goto done;
    if (memcmp(header, "II\052\000", 4) || memcmp(header + 8, "CR\002\000", 4)) goto done;
    t = (soup_tiff){header, bytes, 0};
    protected[0] = (soup_span){0, 16};
    ifds[0] = u32(&t, 4);
    for (int i = 0; i < 4; i++) {
        if (table(&t, ifds[i], &protected[protected_count++], &next)) goto done;
        if (i < 3) { if (!next || next == ifds[i]) goto done; ifds[i + 1] = next; }
        else if (next || ifds[i] != u32(&t, 12)) goto done;
    }
    if (entry(&t, ifds[0], 0x100, &value, &layout->width_field, &layout->width_type)) goto done;
    layout->width = (int)value;
    if (entry(&t, ifds[0], 0x101, &value, &layout->height_field, &layout->height_type)) goto done;
    layout->height = (int)value;
    if (entry(&t, ifds[0], 0x103, &value, 0, 0) || value != 6 ||
        entry(&t, ifds[0], 0x111, &layout->main_offset, 0, &type) || type != 4 ||
        entry(&t, ifds[0], 0x117, &layout->main_length, &layout->main_count_field, &type) || type != 4 ||
        entry(&t, ifds[1], 0x201, &layout->thumb_offset, 0, &type) || type != 4 ||
        entry(&t, ifds[1], 0x202, &layout->thumb_length, &layout->thumb_count_field, &type) || type != 4) goto done;
    layout->orientation = 1;
    if (!entry(&t, ifds[0], 0x112, &value, 0, 0) && value >= 1 && value <= 8) layout->orientation = (int)value;
    if (se_cr2_parse(header, bytes, &info) != SE_OK || strcmp(info.model, "Canon EOS 5D Mark II") ||
        !info.strip_bytes || !range(info.strip_offset, info.strip_bytes, size)) goto done;
    layout->raw_offset = info.strip_offset; layout->raw_length = info.strip_bytes;
    protected[protected_count++] = (soup_span){info.strip_offset, info.strip_bytes};
    /* IFD2's separate RGB preview and maker-note data must never be touched. */
    {
        uint32_t off, len;
        if (entry(&t, ifds[2], 0x111, &off, 0, 0) || entry(&t, ifds[2], 0x117, &len, 0, 0) ||
            !len || !range(off, len, size)) goto done;
        protected[protected_count++] = (soup_span){off, len};
        if (!entry(&t, ifds[0], 0x8769, &off, 0, 0)) {
            soup_span exif;
            int maker_seen = 0;
            if (table(&t, off, &exif, &next)) goto done;
            if (protected_count >= (int)(sizeof(protected) / sizeof(protected[0]))) goto done;
            protected[protected_count++] = exif;
            for (uint32_t k = off + 2; k < off + exif.length - 4; k += 12)
                if (u16(&t, k) == 0x927c) {
                    if (maker_seen++ || u16(&t, k + 2) != 7 ||
                        protected_count >= (int)(sizeof(protected) / sizeof(protected[0]))) goto done;
                    len = u32(&t, k + 4); value = u32(&t, k + 8);
                    if (!len || !range(value, len, size)) goto done;
                    protected[protected_count++] = (soup_span){value, len};
                }
        }
    }
    {
        soup_span main = {layout->main_offset, layout->main_length};
        soup_span thumb = {layout->thumb_offset, layout->thumb_length};
        if (main.length < 4 || thumb.length < 4 || !range(main.offset, main.length, size) ||
            !range(thumb.offset, thumb.length, size) || overlaps(main, thumb)) goto done;
        for (int i = 0; i < protected_count; i++)
            if (overlaps(main, protected[i]) || overlaps(thumb, protected[i])) goto done;
        if (main.offset >= bytes || thumb.offset >= bytes ||
            se_jpeg_dec_info(header + main.offset, bytes - main.offset, &jpeg) != SE_OK ||
            jpeg.width != layout->width || jpeg.height != layout->height ||
            se_jpeg_dec_info(header + thumb.offset, bytes - thumb.offset, &jpeg) != SE_OK ||
            jpeg.width != 160 || jpeg.height != 120) goto done;
        layout->span_count = 6;
        layout->spans[0] = main;
        layout->spans[1] = thumb;
        layout->spans[2] = (soup_span){layout->width_field, layout->width_type == 3 ? 2u : 4u};
        layout->spans[3] = (soup_span){layout->height_field, layout->height_type == 3 ? 2u : 4u};
        layout->spans[4] = (soup_span){layout->main_count_field, 4};
        layout->spans[5] = (soup_span){layout->thumb_count_field, 4};
        for (unsigned i = 2; i < layout->span_count; i++) {
            if (!range(layout->spans[i].offset, layout->spans[i].length, bytes) ||
                overlaps(layout->spans[i], main) || overlaps(layout->spans[i], thumb) ||
                overlaps(layout->spans[i], (soup_span){info.strip_offset, info.strip_bytes})) goto done;
            for (unsigned j = 2; j < i; j++) if (overlaps(layout->spans[i], layout->spans[j])) goto done;
        }
    }
    result = 0;
done:
    if (f) FIO_CloseFile(f);
    fio_free(header);
    return result;
}

int soup_raw_stage(const char *backup, const char *stage, const char *jpeg,
                   const uint8_t *thumb, uint32_t thumb_bytes,
                   int width, int height, soup_raw_layout *layout)
{
    uint32_t jpg_bytes = 0, copied = 0;
    uint8_t *buffer = 0;
    FILE *input = 0, *output = 0;
    int created = 0, result = -1;
    if (!thumb || !thumb_bytes || width <= 0 || height <= 0 || is_file(stage) ||
        soup_raw_inspect(backup, layout) || FIO_GetFileSize(jpeg, &jpg_bytes) ||
        jpg_bytes > layout->main_length || thumb_bytes > layout->thumb_length) return -1;
    if (soup_copy_new(backup, stage)) return -1;
    created = 1;
    buffer = fio_malloc(SOUP_IO);
    input = FIO_OpenFile(jpeg, O_RDONLY | O_SYNC);
    output = FIO_OpenFile(stage, O_RDWR | O_SYNC);
    if (!buffer || !input || !output) goto done;
    while (copied < jpg_bytes) {
        uint32_t n = jpg_bytes - copied;
        if (n > SOUP_IO) n = SOUP_IO;
        if (soup_read_at(input, copied, buffer, n) ||
            soup_write_at(output, layout->main_offset + copied, buffer, n)) goto done;
        copied += n;
    }
    copied = 0;
    while (copied < thumb_bytes) {
        uint32_t n = thumb_bytes - copied;
        if (n > SOUP_IO) n = SOUP_IO;
        memcpy(buffer, thumb + copied, n);
        if (soup_write_at(output, layout->thumb_offset + copied, buffer, n)) goto done;
        copied += n;
    }
    put32(buffer, jpg_bytes);
    if (soup_write_at(output, layout->main_count_field, buffer, 4)) goto done;
    put32(buffer, thumb_bytes);
    if (soup_write_at(output, layout->thumb_count_field, buffer, 4)) goto done;
    if (layout->width_type == 3) put16(buffer, (uint16_t)width); else put32(buffer, (uint32_t)width);
    if (soup_write_at(output, layout->width_field, buffer, layout->width_type == 3 ? 2 : 4)) goto done;
    if (layout->height_type == 3) put16(buffer, (uint16_t)height); else put32(buffer, (uint32_t)height);
    if (soup_write_at(output, layout->height_field, buffer, layout->height_type == 3 ? 2 : 4)) goto done;
    FIO_CloseFile(output); output = 0;
    {
        soup_raw_layout after;
        FILE *check = 0;
        uint32_t checked = 0;
        if (soup_raw_inspect(stage, &after) || after.main_offset != layout->main_offset ||
            after.main_length != jpg_bytes || after.thumb_offset != layout->thumb_offset ||
            after.thumb_length != thumb_bytes || after.width != width || after.height != height ||
            after.raw_offset != layout->raw_offset || after.raw_length != layout->raw_length ||
            soup_equal_ranges(jpeg, 0, stage, layout->main_offset, jpg_bytes)) goto done;
        check = FIO_OpenFile(stage, O_RDONLY | O_SYNC);
        if (!check) goto done;
        while (checked < thumb_bytes) {
            uint32_t n = thumb_bytes - checked; if (n > SOUP_IO) n = SOUP_IO;
            if (soup_read_at(check, layout->thumb_offset + checked, buffer, n) ||
                memcmp(buffer, thumb + checked, n)) { FIO_CloseFile(check); goto done; }
            checked += n;
        }
        FIO_CloseFile(check);
    }
    result = 0;
done:
    if (input) FIO_CloseFile(input);
    if (output) FIO_CloseFile(output);
    fio_free(buffer);
    if (result < 0 && created) FIO_RemoveFile(stage);
    return result;
}

int soup_jpeg_metadata(const char *path, int *orientation, char date[24])
{
    uint8_t *buffer = 0;
    FILE *file = 0;
    uint32_t size = 0, n, at = 2;
    int result = -1;
    *orientation = 1; memset(date, 0, 24);
    if (FIO_GetFileSize(path, &size) || size < 4 || size > SOUP_MAX_FILE) return -1;
    n = size < SOUP_HEADER ? size : SOUP_HEADER;
    buffer = fio_malloc(n); file = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    if (!buffer || !file || soup_read_at(file, 0, buffer, n) || buffer[0] != 0xff || buffer[1] != 0xd8) goto done;
    while (at + 4 <= n) {
        uint32_t segment;
        unsigned marker;
        if (buffer[at++] != 0xff) goto done;
        while (at < n && buffer[at] == 0xff) at++;
        if (at >= n) goto done;
        marker = buffer[at++];
        if (marker == 0xda || marker == 0xd9) { result = 0; goto done; }
        if (at + 2 > n) goto done;
        segment = ((uint32_t)buffer[at] << 8) | buffer[at + 1];
        if (segment < 2 || !range(at, segment, n)) goto done;
        if (marker == 0xe1 && segment >= 16 && !memcmp(buffer + at + 2, "Exif\0\0", 6)) {
            soup_tiff t = {buffer + at + 8, segment - 8, 0};
            uint32_t ifd, value, next;
            soup_span span;
            if (t.bytes < 8) goto done;
            t.be = t.data[0] == 'M' && t.data[1] == 'M';
            if ((!t.be && (t.data[0] != 'I' || t.data[1] != 'I')) || u16(&t, 2) != 42) goto done;
            ifd = u32(&t, 4);
            if (table(&t, ifd, &span, &next)) goto done;
            if (!entry(&t, ifd, 0x112, &value, 0, 0) && value >= 1 && value <= 8) *orientation = (int)value;
            for (uint32_t k = ifd + 2; k < ifd + span.length - 4; k += 12)
                if (u16(&t, k) == 0x132 && u16(&t, k + 2) == 2 && u32(&t, k + 4) == 20) {
                    value = u32(&t, k + 8);
                    if (!range(value, 20, t.bytes) || t.data[value + 19]) goto done;
                    memcpy(date, t.data + value, 20);
                }
            result = 0; goto done;
        }
        at += segment;
    }
done:
    if (file) FIO_CloseFile(file);
    fio_free(buffer);
    return result;
}

int soup_jpeg_thumbnail(const char *path, uint8_t *destination, uint32_t capacity, uint32_t *bytes)
{
    uint8_t *buffer = 0;
    FILE *file = 0;
    uint32_t size = 0, n, at = 2;
    int result = -1;
    if (!destination || !bytes || FIO_GetFileSize(path, &size) || size < 4 || size > SOUP_MAX_FILE) return -1;
    n = size < SOUP_HEADER ? size : SOUP_HEADER;
    buffer = fio_malloc(n); file = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    if (!buffer || !file || soup_read_at(file, 0, buffer, n) || buffer[0] != 0xff || buffer[1] != 0xd8) goto done;
    while (at + 4 <= n) {
        uint32_t segment;
        unsigned marker;
        if (buffer[at++] != 0xff) goto done;
        while (at < n && buffer[at] == 0xff) at++;
        if (at >= n) goto done;
        marker = buffer[at++];
        if (marker == 0xda || marker == 0xd9 || at + 2 > n) goto done;
        segment = ((uint32_t)buffer[at] << 8) | buffer[at + 1];
        if (segment < 2 || !range(at, segment, n)) goto done;
        if (marker == 0xe1 && segment >= 16 && !memcmp(buffer + at + 2, "Exif\0\0", 6)) {
            soup_tiff t = {buffer + at + 8, segment - 8, 0};
            soup_span span;
            uint32_t ifd, next, off, len, compression;
            if (t.bytes < 8) goto done;
            t.be = t.data[0] == 'M' && t.data[1] == 'M';
            if ((!t.be && (t.data[0] != 'I' || t.data[1] != 'I')) || u16(&t, 2) != 42) goto done;
            ifd = u32(&t, 4);
            if (table(&t, ifd, &span, &next) || !next ||
                table(&t, next, &span, &ifd) ||
                entry(&t, next, 0x103, &compression, 0, 0) || compression != 6 ||
                entry(&t, next, 0x201, &off, 0, 0) ||
                entry(&t, next, 0x202, &len, 0, 0) || !len || len > capacity ||
                !range(off, len, t.bytes)) goto done;
            memcpy(destination, t.data + off, len); *bytes = len;
            result = 0; goto done;
        }
        at += segment;
    }
done:
    if (file) FIO_CloseFile(file);
    fio_free(buffer);
    return result;
}
