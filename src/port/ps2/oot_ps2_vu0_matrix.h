#ifndef OOT_PS2_VU0_MATRIX_H
#define OOT_PS2_VU0_MATRIX_H

#include <stdint.h>

#include "ultra64.h"
#include "z_math.h"

static inline int OotPs2Vu0_IsAligned16(const void* ptr) {
    return (((uintptr_t)ptr & 15U) == 0U);
}

static inline int OotPs2Vu0_MtxFMult(const MtxF* a, const MtxF* b, MtxF* dest) {
    if (!OotPs2Vu0_IsAligned16(a) || !OotPs2Vu0_IsAligned16(b) || !OotPs2Vu0_IsAligned16(dest)) {
        return 0;
    }

    __asm__ volatile(
        "lqc2 $vf1, 0(%[a])\n"
        "lqc2 $vf2, 16(%[a])\n"
        "lqc2 $vf3, 32(%[a])\n"
        "lqc2 $vf4, 48(%[a])\n"
        "lqc2 $vf5, 0(%[b])\n"
        "vmulax $ACC, $vf1, $vf5\n"
        "vmadday $ACC, $vf2, $vf5\n"
        "vmaddaz $ACC, $vf3, $vf5\n"
        "vmaddw $vf6, $vf4, $vf5\n"
        "lqc2 $vf7, 16(%[b])\n"
        "vmulax $ACC, $vf1, $vf7\n"
        "vmadday $ACC, $vf2, $vf7\n"
        "vmaddaz $ACC, $vf3, $vf7\n"
        "vmaddw $vf8, $vf4, $vf7\n"
        "lqc2 $vf9, 32(%[b])\n"
        "vmulax $ACC, $vf1, $vf9\n"
        "vmadday $ACC, $vf2, $vf9\n"
        "vmaddaz $ACC, $vf3, $vf9\n"
        "vmaddw $vf10, $vf4, $vf9\n"
        "lqc2 $vf11, 48(%[b])\n"
        "vmulax $ACC, $vf1, $vf11\n"
        "vmadday $ACC, $vf2, $vf11\n"
        "vmaddaz $ACC, $vf3, $vf11\n"
        "vmaddw $vf12, $vf4, $vf11\n"
        "sqc2 $vf6, 0(%[dest])\n"
        "sqc2 $vf8, 16(%[dest])\n"
        "sqc2 $vf10, 32(%[dest])\n"
        "sqc2 $vf12, 48(%[dest])\n"
        :
        : [a] "r"(a), [b] "r"(b), [dest] "r"(dest)
        : "memory");
    return 1;
}

static inline int OotPs2Vu0_MtxFMultVec3f(const MtxF* mf, const Vec3f* src, Vec3f* dest) {
    float in[4] __attribute__((aligned(16))) = { src->x, src->y, src->z, 1.0f };
    float out[4] __attribute__((aligned(16)));

    if (!OotPs2Vu0_IsAligned16(mf)) {
        return 0;
    }

    __asm__ volatile(
        "lqc2 $vf1, 0(%[mf])\n"
        "lqc2 $vf2, 16(%[mf])\n"
        "lqc2 $vf3, 32(%[mf])\n"
        "lqc2 $vf4, 48(%[mf])\n"
        "lqc2 $vf5, 0(%[in])\n"
        "vmulax $ACC, $vf1, $vf5\n"
        "vmadday $ACC, $vf2, $vf5\n"
        "vmaddaz $ACC, $vf3, $vf5\n"
        "vmaddw $vf6, $vf4, $vf5\n"
        "sqc2 $vf6, 0(%[out])\n"
        :
        : [mf] "r"(mf), [in] "r"(in), [out] "r"(out)
        : "memory");

    dest->x = out[0];
    dest->y = out[1];
    dest->z = out[2];
    return 1;
}

static inline int OotPs2Vu0_MtxFMultVec3fW(const MtxF* mf, const Vec3f* src, Vec3f* dest, f32* wDest) {
    float in[4] __attribute__((aligned(16))) = { src->x, src->y, src->z, 1.0f };
    float out[4] __attribute__((aligned(16)));

    if (!OotPs2Vu0_IsAligned16(mf)) {
        return 0;
    }

    __asm__ volatile(
        "lqc2 $vf1, 0(%[mf])\n"
        "lqc2 $vf2, 16(%[mf])\n"
        "lqc2 $vf3, 32(%[mf])\n"
        "lqc2 $vf4, 48(%[mf])\n"
        "lqc2 $vf5, 0(%[in])\n"
        "vmulax $ACC, $vf1, $vf5\n"
        "vmadday $ACC, $vf2, $vf5\n"
        "vmaddaz $ACC, $vf3, $vf5\n"
        "vmaddw $vf6, $vf4, $vf5\n"
        "sqc2 $vf6, 0(%[out])\n"
        :
        : [mf] "r"(mf), [in] "r"(in), [out] "r"(out)
        : "memory");

    dest->x = out[0];
    dest->y = out[1];
    dest->z = out[2];
    *wDest = out[3];
    return 1;
}

static inline int OotPs2Vu0_MtxFTranslate(MtxF* mf, f32 x, f32 y, f32 z) {
    float v[4] __attribute__((aligned(16))) = { x, y, z, 1.0f };

    if (!OotPs2Vu0_IsAligned16(mf)) {
        return 0;
    }

    __asm__ volatile(
        "lqc2 $vf1, 0(%[mf])\n"
        "lqc2 $vf2, 16(%[mf])\n"
        "lqc2 $vf3, 32(%[mf])\n"
        "lqc2 $vf4, 48(%[mf])\n"
        "lqc2 $vf5, 0(%[v])\n"
        "vmulax $ACC, $vf1, $vf5\n"
        "vmadday $ACC, $vf2, $vf5\n"
        "vmaddaz $ACC, $vf3, $vf5\n"
        "vmaddw $vf6, $vf4, $vf5\n"
        "sqc2 $vf6, 48(%[mf])\n"
        :
        : [mf] "r"(mf), [v] "r"(v)
        : "memory");
    return 1;
}

static inline int OotPs2Vu0_MtxFScale(MtxF* mf, f32 x, f32 y, f32 z) {
    float v[4] __attribute__((aligned(16))) = { x, y, z, 1.0f };

    if (!OotPs2Vu0_IsAligned16(mf)) {
        return 0;
    }

    __asm__ volatile(
        "lqc2 $vf1, 0(%[mf])\n"
        "lqc2 $vf2, 16(%[mf])\n"
        "lqc2 $vf3, 32(%[mf])\n"
        "lqc2 $vf5, 0(%[v])\n"
        "vmulx $vf1, $vf1, $vf5\n"
        "vmuly $vf2, $vf2, $vf5\n"
        "vmulz $vf3, $vf3, $vf5\n"
        "sqc2 $vf1, 0(%[mf])\n"
        "sqc2 $vf2, 16(%[mf])\n"
        "sqc2 $vf3, 32(%[mf])\n"
        :
        : [mf] "r"(mf), [v] "r"(v)
        : "memory");
    return 1;
}

static inline int OotPs2Vu0_MtxFRotateX(MtxF* mf, f32 sinValue, f32 cosValue) {
    float v[4] __attribute__((aligned(16))) = { sinValue, cosValue, 0.0f, 0.0f };

    if (!OotPs2Vu0_IsAligned16(mf)) {
        return 0;
    }

    __asm__ volatile(
        "lqc2 $vf1, 16(%[mf])\n"
        "lqc2 $vf2, 32(%[mf])\n"
        "lqc2 $vf5, 0(%[v])\n"
        "vmuly $vf3, $vf1, $vf5\n"
        "vmulx $vf4, $vf2, $vf5\n"
        "vadd $vf3, $vf3, $vf4\n"
        "vmuly $vf4, $vf2, $vf5\n"
        "vmulx $vf6, $vf1, $vf5\n"
        "vsub $vf4, $vf4, $vf6\n"
        "sqc2 $vf3, 16(%[mf])\n"
        "sqc2 $vf4, 32(%[mf])\n"
        :
        : [mf] "r"(mf), [v] "r"(v)
        : "memory");
    return 1;
}

static inline int OotPs2Vu0_MtxFRotateY(MtxF* mf, f32 sinValue, f32 cosValue) {
    float v[4] __attribute__((aligned(16))) = { sinValue, cosValue, 0.0f, 0.0f };

    if (!OotPs2Vu0_IsAligned16(mf)) {
        return 0;
    }

    __asm__ volatile(
        "lqc2 $vf1, 0(%[mf])\n"
        "lqc2 $vf2, 32(%[mf])\n"
        "lqc2 $vf5, 0(%[v])\n"
        "vmuly $vf3, $vf1, $vf5\n"
        "vmulx $vf4, $vf2, $vf5\n"
        "vsub $vf3, $vf3, $vf4\n"
        "vmulx $vf4, $vf1, $vf5\n"
        "vmuly $vf6, $vf2, $vf5\n"
        "vadd $vf4, $vf4, $vf6\n"
        "sqc2 $vf3, 0(%[mf])\n"
        "sqc2 $vf4, 32(%[mf])\n"
        :
        : [mf] "r"(mf), [v] "r"(v)
        : "memory");
    return 1;
}

static inline int OotPs2Vu0_MtxFRotateZ(MtxF* mf, f32 sinValue, f32 cosValue) {
    float v[4] __attribute__((aligned(16))) = { sinValue, cosValue, 0.0f, 0.0f };

    if (!OotPs2Vu0_IsAligned16(mf)) {
        return 0;
    }

    __asm__ volatile(
        "lqc2 $vf1, 0(%[mf])\n"
        "lqc2 $vf2, 16(%[mf])\n"
        "lqc2 $vf5, 0(%[v])\n"
        "vmuly $vf3, $vf1, $vf5\n"
        "vmulx $vf4, $vf2, $vf5\n"
        "vadd $vf3, $vf3, $vf4\n"
        "vmuly $vf4, $vf2, $vf5\n"
        "vmulx $vf6, $vf1, $vf5\n"
        "vsub $vf4, $vf4, $vf6\n"
        "sqc2 $vf3, 0(%[mf])\n"
        "sqc2 $vf4, 16(%[mf])\n"
        :
        : [mf] "r"(mf), [v] "r"(v)
        : "memory");
    return 1;
}

#endif
