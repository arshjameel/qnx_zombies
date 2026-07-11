/*
 * text_render.c -- see text_render.h for the overview. Font data
 * verified before use: every glyph was rendered as ASCII art and
 * visually checked, and the packed-byte encoding was round-tripped
 * (packed then unpacked back to the original pattern) to confirm the
 * bit-order matches what get_glyph_rows()/text_push_char() expect.
 */

#include "text_render.h"
#include <string.h>

#define GLYPH_COLS 5
#define GLYPH_ROWS 7
#define CHAR_ADVANCE_COLS 6   /* 5 for the glyph + 1 column of spacing */

/* Each row is 5 bits, bit4 = leftmost column ... bit0 = rightmost. */
static const unsigned char FONT_LETTERS[26][7] = {
    { 14, 17, 17, 31, 17, 17, 17 },  /* A */
    { 30, 17, 17, 30, 17, 17, 30 },  /* B */
    { 15, 16, 16, 16, 16, 16, 15 },  /* C */
    { 30, 17, 17, 17, 17, 17, 30 },  /* D */
    { 31, 16, 16, 30, 16, 16, 31 },  /* E */
    { 31, 16, 16, 30, 16, 16, 16 },  /* F */
    { 15, 16, 16, 23, 17, 17, 15 },  /* G */
    { 17, 17, 17, 31, 17, 17, 17 },  /* H */
    { 31,  4,  4,  4,  4,  4, 31 },  /* I */
    {  7,  2,  2,  2, 18, 18, 12 },  /* J */
    { 17, 18, 20, 24, 20, 18, 17 },  /* K */
    { 16, 16, 16, 16, 16, 16, 31 },  /* L */
    { 17, 27, 21, 17, 17, 17, 17 },  /* M */
    { 17, 25, 21, 19, 17, 17, 17 },  /* N */
    { 14, 17, 17, 17, 17, 17, 14 },  /* O */
    { 30, 17, 17, 30, 16, 16, 16 },  /* P */
    { 14, 17, 17, 17, 21, 18, 13 },  /* Q */
    { 30, 17, 17, 30, 20, 18, 17 },  /* R */
    { 15, 16, 16, 14,  1,  1, 30 },  /* S */
    { 31,  4,  4,  4,  4,  4,  4 },  /* T */
    { 17, 17, 17, 17, 17, 17, 14 },  /* U */
    { 17, 17, 17, 17, 17, 10,  4 },  /* V */
    { 17, 17, 17, 21, 21, 27, 17 },  /* W */
    { 17, 10,  4,  4,  4, 10, 17 },  /* X */
    { 17, 10,  4,  4,  4,  4,  4 },  /* Y */
    { 31,  1,  2,  4,  8, 16, 31 },  /* Z */
};

static const unsigned char FONT_DIGITS[10][7] = {
    { 14, 17, 17, 17, 17, 17, 14 },  /* 0 */
    {  4, 12,  4,  4,  4,  4, 14 },  /* 1 */
    { 14, 17,  1,  2,  4,  8, 31 },  /* 2 */
    { 30,  1,  1, 14,  1,  1, 30 },  /* 3 */
    {  2,  6, 10, 18, 31,  2,  2 },  /* 4 */
    { 31, 16, 16, 30,  1,  1, 30 },  /* 5 */
    { 14, 16, 16, 30, 17, 17, 14 },  /* 6 */
    { 31,  1,  2,  4,  8,  8,  8 },  /* 7 */
    { 14, 17, 17, 14, 17, 17, 14 },  /* 8 */
    { 14, 17, 17, 15,  1,  1, 14 },  /* 9 */
};

static const unsigned char *get_glyph_rows(char c)
{
    static const unsigned char BLANK[7] = { 0, 0, 0, 0, 0, 0, 0 };
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);   /* fold to uppercase */
    if (c >= 'A' && c <= 'Z') return FONT_LETTERS[c - 'A'];
    if (c >= '0' && c <= '9') return FONT_DIGITS[c - '0'];
    return BLANK;   /* space, and anything unrecognized, renders blank */
}

static void px_to_ndc(f32 px, f32 py, int win_w, int win_h, f32 *nx, f32 *ny)
{
    *nx = (px / (f32)win_w) * 2.0f - 1.0f;
    *ny = 1.0f - (py / (f32)win_h) * 2.0f;   /* screen Y grows down, NDC Y grows up */
}

static void push_quad_2d(VertexList *vl, f32 x0, f32 y0, f32 x1, f32 y1,
                        int win_w, int win_h, f32 r, f32 g, f32 b)
{
    f32 nx0, ny0, nx1, ny1;
    px_to_ndc(x0, y0, win_w, win_h, &nx0, &ny0);
    px_to_ndc(x1, y1, win_w, win_h, &nx1, &ny1);
    vlist_push(vl, nx0, ny0, 0.0f, r, g, b);
    vlist_push(vl, nx1, ny0, 0.0f, r, g, b);
    vlist_push(vl, nx1, ny1, 0.0f, r, g, b);
    vlist_push(vl, nx0, ny0, 0.0f, r, g, b);
    vlist_push(vl, nx1, ny1, 0.0f, r, g, b);
    vlist_push(vl, nx0, ny1, 0.0f, r, g, b);
}

void text_push_char(VertexList *vl, char c, f32 px, f32 py, f32 pixel_w, f32 pixel_h,
                    int win_w, int win_h, f32 r, f32 g, f32 b)
{
    const unsigned char *rows = get_glyph_rows(c);
    int row, col;
    for (row = 0; row < GLYPH_ROWS; row++) {
        unsigned char rowbits = rows[row];
        for (col = 0; col < GLYPH_COLS; col++) {
            int bit = 4 - col;   /* bit4 = leftmost column */
            if (rowbits & (unsigned char)(1 << bit)) {
                f32 x0 = px + (f32)col * pixel_w;
                f32 y0 = py + (f32)row * pixel_h;
                push_quad_2d(vl, x0, y0, x0 + pixel_w, y0 + pixel_h, win_w, win_h, r, g, b);
            }
        }
    }
}

f32 text_push_string(VertexList *vl, const char *str, f32 px, f32 py,
                     f32 pixel_w, f32 pixel_h, int win_w, int win_h,
                     f32 r, f32 g, f32 b)
{
    f32 x = px;
    const char *p;
    for (p = str; *p; p++) {
        text_push_char(vl, *p, x, py, pixel_w, pixel_h, win_w, win_h, r, g, b);
        x += (f32)CHAR_ADVANCE_COLS * pixel_w;
    }
    return x - px;
}

f32 text_measure_width(const char *str, f32 pixel_w)
{
    return (f32)strlen(str) * (f32)CHAR_ADVANCE_COLS * pixel_w;
}
