/**
 * @file cgbi.h
 * @brief Apple CgBI-optimized PNG decompressor and byte swapper.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Checks if buffer contains an Apple CgBI PNG.
 * @param[in] data Pointer to raw file data.
 * @param[in] len Length of buffer in bytes.
 * @return 1 if CgBI PNG, 0 otherwise.
 */
int cgbi_is(const void *data, int len);

/**
 * @brief Decodes Apple CgBI PNG to premultiplied RGBA8 in memory allocated by alloc.
 * @param[in] data Pointer to raw PNG file data.
 * @param[in] len Length of buffer in bytes.
 * @param[out] w Decoded image width in pixels.
 * @param[out] h Decoded image height in pixels.
 * @param[out] bpp Original bit depth (24 or 32 bpp).
 * @param[in] alloc Allocation callback for output pixel buffer.
 * @return Pointer to decoded RGBA buffer, or NULL on error.
 */
uint8_t *cgbi_decode(const void *data, int len, int *w, int *h, int *bpp, void *(*alloc)(size_t));
