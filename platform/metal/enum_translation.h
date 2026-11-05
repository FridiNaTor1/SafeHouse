#ifndef CANE_PLATFORM_METAL_ENUM_TRANSLATION_H_
#define CANE_PLATFORM_METAL_ENUM_TRANSLATION_H_

#import <Metal/Metal.h>

#include <cstddef>

#include "gl_enums.h"

namespace cane::gl_metal {

struct PixelFormatInfo {
  MTLPixelFormat format = MTLPixelFormatInvalid;
  size_t bytes_per_pixel = 0;
  bool depth = false;
  bool stencil = false;
};

MTLCompareFunction ToCompareFunction(GLenum func);
MTLStencilOperation ToStencilOperation(GLenum op);
MTLBlendFactor ToBlendFactor(GLenum factor);
MTLBlendOperation ToBlendOperation(GLenum equation);
MTLPrimitiveType ToPrimitiveType(GLenum mode);
MTLSamplerMinMagFilter ToMinMagFilter(GLenum filter);
MTLSamplerMipFilter ToMipFilter(GLenum min_filter);
MTLSamplerAddressMode ToAddressMode(GLenum wrap);
MTLCullMode ToCullMode(GLenum face);
MTLWinding ToWinding(GLenum front_face);
MTLIndexType ToIndexType(GLenum type);
MTLTextureSwizzle ToSwizzle(GLenum component);

// The internal format a texture is stored in, with its upload footprint.
PixelFormatInfo ToPixelFormat(GLenum internal_format);
// Bytes per pixel of client data described by (format, type).
size_t ClientPixelBytes(GLenum format, GLenum type);
bool HasMipmapFilter(GLenum min_filter);

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_ENUM_TRANSLATION_H_
