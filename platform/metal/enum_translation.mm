#include "enum_translation.h"

namespace cane::gl_metal {

MTLCompareFunction ToCompareFunction(GLenum func) {
  switch (func) {
    case gl::kNever:
      return MTLCompareFunctionNever;
    case gl::kLess:
      return MTLCompareFunctionLess;
    case gl::kEqual:
      return MTLCompareFunctionEqual;
    case gl::kLequal:
      return MTLCompareFunctionLessEqual;
    case gl::kGreater:
      return MTLCompareFunctionGreater;
    case gl::kNotequal:
      return MTLCompareFunctionNotEqual;
    case gl::kGequal:
      return MTLCompareFunctionGreaterEqual;
    default:
      return MTLCompareFunctionAlways;
  }
}

MTLStencilOperation ToStencilOperation(GLenum op) {
  switch (op) {
    case gl::kZero:
      return MTLStencilOperationZero;
    case gl::kReplace:
      return MTLStencilOperationReplace;
    case gl::kIncr:
      return MTLStencilOperationIncrementClamp;
    case gl::kDecr:
      return MTLStencilOperationDecrementClamp;
    case gl::kInvert:
      return MTLStencilOperationInvert;
    case gl::kIncrWrap:
      return MTLStencilOperationIncrementWrap;
    case gl::kDecrWrap:
      return MTLStencilOperationDecrementWrap;
    default:
      return MTLStencilOperationKeep;
  }
}

MTLBlendFactor ToBlendFactor(GLenum factor) {
  switch (factor) {
    case gl::kZero:
      return MTLBlendFactorZero;
    case gl::kOne:
      return MTLBlendFactorOne;
    case gl::kSrcColor:
      return MTLBlendFactorSourceColor;
    case gl::kOneMinusSrcColor:
      return MTLBlendFactorOneMinusSourceColor;
    case gl::kSrcAlpha:
      return MTLBlendFactorSourceAlpha;
    case gl::kOneMinusSrcAlpha:
      return MTLBlendFactorOneMinusSourceAlpha;
    case gl::kDstAlpha:
      return MTLBlendFactorDestinationAlpha;
    case gl::kOneMinusDstAlpha:
      return MTLBlendFactorOneMinusDestinationAlpha;
    case gl::kDstColor:
      return MTLBlendFactorDestinationColor;
    case gl::kOneMinusDstColor:
      return MTLBlendFactorOneMinusDestinationColor;
    case gl::kSrcAlphaSaturate:
      return MTLBlendFactorSourceAlphaSaturated;
    case gl::kConstantColor:
      return MTLBlendFactorBlendColor;
    case gl::kOneMinusConstantColor:
      return MTLBlendFactorOneMinusBlendColor;
    case gl::kConstantAlpha:
      return MTLBlendFactorBlendAlpha;
    case gl::kOneMinusConstantAlpha:
      return MTLBlendFactorOneMinusBlendAlpha;
    default:
      return MTLBlendFactorOne;
  }
}

MTLBlendOperation ToBlendOperation(GLenum equation) {
  switch (equation) {
    case gl::kFuncSubtract:
      return MTLBlendOperationSubtract;
    case gl::kFuncReverseSubtract:
      return MTLBlendOperationReverseSubtract;
    case gl::kMin:
      return MTLBlendOperationMin;
    case gl::kMax:
      return MTLBlendOperationMax;
    default:
      return MTLBlendOperationAdd;
  }
}

MTLPrimitiveType ToPrimitiveType(GLenum mode) {
  switch (mode) {
    case gl::kPoints:
      return MTLPrimitiveTypePoint;
    case gl::kLines:
      return MTLPrimitiveTypeLine;
    case gl::kLineStrip:
      return MTLPrimitiveTypeLineStrip;
    case gl::kTriangleStrip:
      return MTLPrimitiveTypeTriangleStrip;
    default:
      return MTLPrimitiveTypeTriangle;
  }
}

MTLSamplerMinMagFilter ToMinMagFilter(GLenum filter) {
  switch (filter) {
    case gl::kNearest:
    case gl::kNearestMipmapNearest:
    case gl::kNearestMipmapLinear:
      return MTLSamplerMinMagFilterNearest;
    default:
      return MTLSamplerMinMagFilterLinear;
  }
}

MTLSamplerMipFilter ToMipFilter(GLenum min_filter) {
  switch (min_filter) {
    case gl::kNearestMipmapNearest:
    case gl::kLinearMipmapNearest:
      return MTLSamplerMipFilterNearest;
    case gl::kNearestMipmapLinear:
    case gl::kLinearMipmapLinear:
      return MTLSamplerMipFilterLinear;
    default:
      return MTLSamplerMipFilterNotMipmapped;
  }
}

bool HasMipmapFilter(GLenum min_filter) {
  return ToMipFilter(min_filter) != MTLSamplerMipFilterNotMipmapped;
}

MTLSamplerAddressMode ToAddressMode(GLenum wrap) {
  switch (wrap) {
    case gl::kClampToEdge:
      return MTLSamplerAddressModeClampToEdge;
    case gl::kClampToBorder:
      return MTLSamplerAddressModeClampToZero;
    case gl::kMirroredRepeat:
      return MTLSamplerAddressModeMirrorRepeat;
    default:
      return MTLSamplerAddressModeRepeat;
  }
}

MTLCullMode ToCullMode(GLenum face) {
  switch (face) {
    case gl::kFront:
      return MTLCullModeFront;
    case gl::kBack:
      return MTLCullModeBack;
    default:
      return MTLCullModeNone;
  }
}

MTLWinding ToWinding(GLenum front_face) {
  return front_face == gl::kCw ? MTLWindingClockwise : MTLWindingCounterClockwise;
}

MTLIndexType ToIndexType(GLenum type) {
  return type == gl::kUnsignedShort ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
}

MTLTextureSwizzle ToSwizzle(GLenum component) {
  switch (component) {
    case gl::kZero:
      return MTLTextureSwizzleZero;
    case gl::kOne:
      return MTLTextureSwizzleOne;
    case gl::kRed:
      return MTLTextureSwizzleRed;
    case gl::kGreen:
      return MTLTextureSwizzleGreen;
    case gl::kBlue:
      return MTLTextureSwizzleBlue;
    default:
      return MTLTextureSwizzleAlpha;
  }
}

PixelFormatInfo ToPixelFormat(GLenum internal_format) {
  switch (internal_format) {
    case gl::kR8:
    case gl::kRed:
      return {MTLPixelFormatR8Unorm, 1, false, false};
    case gl::kRg8:
    case gl::kRg:
      return {MTLPixelFormatRG8Unorm, 2, false, false};
    case gl::kRgba8:
    case gl::kRgba:
    case gl::kRgb8:
    case gl::kRgb:
      return {MTLPixelFormatRGBA8Unorm, 4, false, false};
    case gl::kSrgb8Alpha8:
      return {MTLPixelFormatRGBA8Unorm_sRGB, 4, false, false};
    case gl::kR32f:
      return {MTLPixelFormatR32Float, 4, false, false};
    case gl::kRgba16f:
      return {MTLPixelFormatRGBA16Float, 8, false, false};
    case gl::kRgba32f:
      return {MTLPixelFormatRGBA32Float, 16, false, false};
    case gl::kDepthComponent:
    case gl::kDepthComponent24:
    case gl::kDepthComponent32f:
      return {MTLPixelFormatDepth32Float, 4, true, false};
    case gl::kDepth24Stencil8:
    case gl::kDepth32fStencil8:
    case gl::kDepthStencil:
      return {MTLPixelFormatDepth32Float_Stencil8, 8, true, true};
    case gl::kStencilIndex8:
      return {MTLPixelFormatStencil8, 1, false, true};
    default:
      return {};
  }
}

size_t ClientPixelBytes(GLenum format, GLenum type) {
  size_t components;
  switch (format) {
    case gl::kRed:
    case gl::kDepthComponent:
    case gl::kAlpha:
      components = 1;
      break;
    case gl::kRg:
      components = 2;
      break;
    case gl::kRgb:
      components = 3;
      break;
    default:
      components = 4;
      break;
  }
  size_t component_bytes;
  switch (type) {
    case gl::kByte:
    case gl::kUnsignedByte:
      component_bytes = 1;
      break;
    case gl::kShort:
    case gl::kUnsignedShort:
    case gl::kHalfFloat:
      component_bytes = 2;
      break;
    default:
      component_bytes = 4;
      break;
  }
  return components * component_bytes;
}

}  // namespace cane::gl_metal
