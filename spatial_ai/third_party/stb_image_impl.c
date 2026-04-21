/*
 * stb_image_impl.c — translation unit that instantiates the
 * stb_image / stb_image_write single-header implementations.
 *
 *   Vendored from https://github.com/nothings/stb (public domain
 *   / MIT dual-licensed, Sean Barrett). Kept untouched; any local
 *   changes belong here in the wrapper, not in the third_party
 *   headers, so updates are mechanical.
 */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM   /* we keep our own PPM path */
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
