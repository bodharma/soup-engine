#ifndef SOUP_FILM_TIFF_H
#define SOUP_FILM_TIFF_H
#include <stdint.h>

#define SOUP_SPANS 8
typedef struct { uint32_t offset, length; } soup_span;
typedef struct {
    uint32_t main_offset, main_length, thumb_offset, thumb_length;
    uint32_t raw_offset, raw_length;
    uint32_t width_field, height_field, main_count_field, thumb_count_field;
    int width_type, height_type;
    int width, height, orientation;
    uint32_t span_count;
    soup_span spans[SOUP_SPANS];
} soup_raw_layout;

int soup_raw_inspect(const char *path, soup_raw_layout *layout);
int soup_raw_stage(const char *backup, const char *stage, const char *jpeg,
                   const uint8_t *thumb, uint32_t thumb_bytes,
                   int width, int height, soup_raw_layout *layout);
int soup_jpeg_metadata(const char *path, int *orientation, char date[24]);
int soup_jpeg_thumbnail(const char *path, uint8_t *destination, uint32_t capacity, uint32_t *bytes);
#endif
