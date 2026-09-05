#ifndef MAT4_H
#define MAT4_H

#include <math.h>
#include "common.h"   

typedef struct { f32 m[16]; } Mat4;

static Mat4 mat4_identity(void)
{
    Mat4 r;
    int i;
    for (i = 0; i < 16; i++) r.m[i] = 0.0f;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

static Mat4 mat4_multiply(Mat4 a, Mat4 b)
{
    Mat4 r;
    int col, row, k;
    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            f32 sum = 0.0f;
            for (k = 0; k < 4; k++)
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

static Mat4 mat4_translate(f32 x, f32 y, f32 z)
{
    Mat4 r = mat4_identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

static Mat4 mat4_rotate_x(f32 rad)
{
    Mat4 r = mat4_identity();
    f32 c = cosf(rad), s = sinf(rad);
    r.m[5] = c;  r.m[6]  = s;
    r.m[9] = -s; r.m[10] = c;
    return r;
}

static Mat4 mat4_rotate_y(f32 rad)
{
    Mat4 r = mat4_identity();
    f32 c = cosf(rad), s = sinf(rad);
    r.m[0] = c;  r.m[2]  = -s;
    r.m[8] = s;  r.m[10] = c;
    return r;
}

static Mat4 mat4_rotate_z(f32 rad)
{
    Mat4 r = mat4_identity();
    f32 c = cosf(rad), s = sinf(rad);
    r.m[0] = c; r.m[1] = s;
    r.m[4] = -s; r.m[5] = c;
    return r;
}

static Mat4 mat4_perspective(f32 fovy_rad, f32 aspect, f32 znear, f32 zfar)
{
    Mat4 r;
    int i;
    f32 f = 1.0f / tanf(fovy_rad * 0.5f);
    for (i = 0; i < 16; i++) r.m[i] = 0.0f;
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}

static Mat4 mat4_view_from_yaw_pitch(f32 eye_x, f32 eye_y, f32 eye_z, f32 yaw, f32 pitch)
{
    Mat4 t  = mat4_translate(-eye_x, -eye_y, -eye_z);
    Mat4 ry = mat4_rotate_y(-yaw);
    Mat4 rx = mat4_rotate_x(-pitch);
    return mat4_multiply(rx, mat4_multiply(ry, t));
}

static void mat4_transform_point(Mat4 m, f32 x, f32 y, f32 z, f32 *ox, f32 *oy, f32 *oz)
{
    *ox = m.m[0] * x + m.m[4] * y + m.m[8]  * z + m.m[12];
    *oy = m.m[1] * x + m.m[5] * y + m.m[9]  * z + m.m[13];
    *oz = m.m[2] * x + m.m[6] * y + m.m[10] * z + m.m[14];
}

#endif 
