#ifndef CANE_PLATFORM_METAL_GL_ENUMS_H_
#define CANE_PLATFORM_METAL_GL_ENUMS_H_

#include <cstdint>

namespace cane::gl_metal {

using GLenum = uint32_t;
using GLbitfield = uint32_t;

namespace gl {

inline constexpr GLenum kNoError = 0;
inline constexpr GLenum kInvalidEnum = 0x0500;
inline constexpr GLenum kInvalidValue = 0x0501;
inline constexpr GLenum kInvalidOperation = 0x0502;
inline constexpr GLenum kOutOfMemory = 0x0505;
inline constexpr GLenum kInvalidFramebufferOperation = 0x0506;

inline constexpr GLenum kFalse = 0;
inline constexpr GLenum kTrue = 1;

inline constexpr GLenum kByte = 0x1400;
inline constexpr GLenum kUnsignedByte = 0x1401;
inline constexpr GLenum kShort = 0x1402;
inline constexpr GLenum kUnsignedShort = 0x1403;
inline constexpr GLenum kInt = 0x1404;
inline constexpr GLenum kUnsignedInt = 0x1405;
inline constexpr GLenum kFloat = 0x1406;
inline constexpr GLenum kHalfFloat = 0x140B;

inline constexpr GLenum kPoints = 0x0000;
inline constexpr GLenum kLines = 0x0001;
inline constexpr GLenum kLineStrip = 0x0003;
inline constexpr GLenum kTriangles = 0x0004;
inline constexpr GLenum kTriangleStrip = 0x0005;

inline constexpr GLenum kNever = 0x0200;
inline constexpr GLenum kLess = 0x0201;
inline constexpr GLenum kEqual = 0x0202;
inline constexpr GLenum kLequal = 0x0203;
inline constexpr GLenum kGreater = 0x0204;
inline constexpr GLenum kNotequal = 0x0205;
inline constexpr GLenum kGequal = 0x0206;
inline constexpr GLenum kAlways = 0x0207;

inline constexpr GLenum kZero = 0;
inline constexpr GLenum kOne = 1;
inline constexpr GLenum kSrcColor = 0x0300;
inline constexpr GLenum kOneMinusSrcColor = 0x0301;
inline constexpr GLenum kSrcAlpha = 0x0302;
inline constexpr GLenum kOneMinusSrcAlpha = 0x0303;
inline constexpr GLenum kDstAlpha = 0x0304;
inline constexpr GLenum kOneMinusDstAlpha = 0x0305;
inline constexpr GLenum kDstColor = 0x0306;
inline constexpr GLenum kOneMinusDstColor = 0x0307;
inline constexpr GLenum kSrcAlphaSaturate = 0x0308;
inline constexpr GLenum kConstantColor = 0x8001;
inline constexpr GLenum kOneMinusConstantColor = 0x8002;
inline constexpr GLenum kConstantAlpha = 0x8003;
inline constexpr GLenum kOneMinusConstantAlpha = 0x8004;

inline constexpr GLenum kFuncAdd = 0x8006;
inline constexpr GLenum kMin = 0x8007;
inline constexpr GLenum kMax = 0x8008;
inline constexpr GLenum kFuncSubtract = 0x800A;
inline constexpr GLenum kFuncReverseSubtract = 0x800B;

inline constexpr GLenum kKeep = 0x1E00;
inline constexpr GLenum kReplace = 0x1E01;
inline constexpr GLenum kIncr = 0x1E02;
inline constexpr GLenum kDecr = 0x1E03;
inline constexpr GLenum kInvert = 0x150A;
inline constexpr GLenum kIncrWrap = 0x8507;
inline constexpr GLenum kDecrWrap = 0x8508;

inline constexpr GLenum kFront = 0x0404;
inline constexpr GLenum kBack = 0x0405;
inline constexpr GLenum kFrontAndBack = 0x0408;
inline constexpr GLenum kCw = 0x0900;
inline constexpr GLenum kCcw = 0x0901;

inline constexpr GLenum kCullFace = 0x0B44;
inline constexpr GLenum kDepthTest = 0x0B71;
inline constexpr GLenum kStencilTest = 0x0B90;
inline constexpr GLenum kBlend = 0x0BE2;
inline constexpr GLenum kScissorTest = 0x0C11;
inline constexpr GLenum kDepthClamp = 0x864F;
inline constexpr GLenum kPrimitiveRestart = 0x8F9D;
inline constexpr GLenum kMultisample = 0x809D;
inline constexpr GLenum kFramebufferSrgb = 0x8DB9;
inline constexpr GLenum kPolygonOffsetFill = 0x8037;
inline constexpr GLenum kLineSmooth = 0x0B20;
inline constexpr GLenum kDither = 0x0BD0;

inline constexpr GLenum kTexture2D = 0x0DE1;
inline constexpr GLenum kTexture3D = 0x806F;
inline constexpr GLenum kTexture2DArray = 0x8C1A;
inline constexpr GLenum kTexture2DMultisample = 0x9100;
inline constexpr GLenum kTexture0 = 0x84C0;

inline constexpr GLenum kTextureMagFilter = 0x2800;
inline constexpr GLenum kTextureMinFilter = 0x2801;
inline constexpr GLenum kTextureWrapS = 0x2802;
inline constexpr GLenum kTextureWrapT = 0x2803;
inline constexpr GLenum kTextureWrapR = 0x8072;
inline constexpr GLenum kTextureBaseLevel = 0x813C;
inline constexpr GLenum kTextureMaxLevel = 0x813D;
inline constexpr GLenum kTextureMaxAnisotropy = 0x84FE;
inline constexpr GLenum kMaxTextureMaxAnisotropy = 0x84FF;
inline constexpr GLenum kTextureSwizzleR = 0x8E42;
inline constexpr GLenum kTextureSwizzleG = 0x8E43;
inline constexpr GLenum kTextureSwizzleB = 0x8E44;
inline constexpr GLenum kTextureSwizzleA = 0x8E45;
inline constexpr GLenum kTextureLodBias = 0x8501;
inline constexpr GLenum kTextureMinLod = 0x813A;
inline constexpr GLenum kTextureMaxLod = 0x813B;

inline constexpr GLenum kNearest = 0x2600;
inline constexpr GLenum kLinear = 0x2601;
inline constexpr GLenum kNearestMipmapNearest = 0x2700;
inline constexpr GLenum kLinearMipmapNearest = 0x2701;
inline constexpr GLenum kNearestMipmapLinear = 0x2702;
inline constexpr GLenum kLinearMipmapLinear = 0x2703;
inline constexpr GLenum kRepeat = 0x2901;
inline constexpr GLenum kClampToEdge = 0x812F;
inline constexpr GLenum kClampToBorder = 0x812D;
inline constexpr GLenum kMirroredRepeat = 0x8370;

inline constexpr GLenum kRed = 0x1903;
inline constexpr GLenum kGreen = 0x1904;
inline constexpr GLenum kBlue = 0x1905;
inline constexpr GLenum kAlpha = 0x1906;
inline constexpr GLenum kRgb = 0x1907;
inline constexpr GLenum kRgba = 0x1908;
inline constexpr GLenum kBgra = 0x80E1;
inline constexpr GLenum kRg = 0x8227;
inline constexpr GLenum kR8 = 0x8229;
inline constexpr GLenum kRg8 = 0x822B;
inline constexpr GLenum kRgb8 = 0x8051;
inline constexpr GLenum kRgba8 = 0x8058;
inline constexpr GLenum kSrgb8Alpha8 = 0x8C43;
inline constexpr GLenum kR32f = 0x822E;
inline constexpr GLenum kRgba16f = 0x881A;
inline constexpr GLenum kRgba32f = 0x8814;
inline constexpr GLenum kDepthComponent = 0x1902;
inline constexpr GLenum kDepthComponent24 = 0x81A6;
inline constexpr GLenum kDepthComponent32f = 0x8CAC;
inline constexpr GLenum kDepth24Stencil8 = 0x88F0;
inline constexpr GLenum kDepth32fStencil8 = 0x8CAD;
inline constexpr GLenum kDepthStencil = 0x84F9;
inline constexpr GLenum kStencilIndex8 = 0x8D48;

inline constexpr GLenum kArrayBuffer = 0x8892;
inline constexpr GLenum kElementArrayBuffer = 0x8893;
inline constexpr GLenum kCopyReadBuffer = 0x8F36;
inline constexpr GLenum kCopyWriteBuffer = 0x8F37;
inline constexpr GLenum kDrawIndirectBuffer = 0x8F3F;
inline constexpr GLenum kShaderStorageBuffer = 0x90D2;
inline constexpr GLenum kUniformBuffer = 0x8A11;
inline constexpr GLenum kPixelPackBuffer = 0x88EB;
inline constexpr GLenum kPixelUnpackBuffer = 0x88EC;

inline constexpr GLenum kStreamDraw = 0x88E0;
inline constexpr GLenum kStaticDraw = 0x88E4;
inline constexpr GLenum kDynamicDraw = 0x88E8;

inline constexpr GLbitfield kMapReadBit = 0x0001;
inline constexpr GLbitfield kMapWriteBit = 0x0002;
inline constexpr GLbitfield kMapInvalidateRangeBit = 0x0004;
inline constexpr GLbitfield kMapInvalidateBufferBit = 0x0008;
inline constexpr GLbitfield kMapFlushExplicitBit = 0x0010;
inline constexpr GLbitfield kMapUnsynchronizedBit = 0x0020;
inline constexpr GLbitfield kMapPersistentBit = 0x0040;
inline constexpr GLbitfield kMapCoherentBit = 0x0080;

inline constexpr GLenum kFramebuffer = 0x8D40;
inline constexpr GLenum kReadFramebuffer = 0x8CA8;
inline constexpr GLenum kDrawFramebuffer = 0x8CA9;
inline constexpr GLenum kRenderbuffer = 0x8D41;
inline constexpr GLenum kColorAttachment0 = 0x8CE0;
inline constexpr GLenum kDepthAttachment = 0x8D00;
inline constexpr GLenum kStencilAttachment = 0x8D20;
inline constexpr GLenum kDepthStencilAttachment = 0x821A;
inline constexpr GLenum kFramebufferComplete = 0x8CD5;
inline constexpr GLenum kFramebufferIncompleteAttachment = 0x8CD6;
inline constexpr GLenum kFramebufferUnsupported = 0x8CDD;

inline constexpr GLbitfield kDepthBufferBit = 0x00000100;
inline constexpr GLbitfield kStencilBufferBit = 0x00000400;
inline constexpr GLbitfield kColorBufferBit = 0x00004000;

inline constexpr GLenum kPackAlignment = 0x0D05;
inline constexpr GLenum kUnpackAlignment = 0x0CF5;
inline constexpr GLenum kUnpackRowLength = 0x0CF2;
inline constexpr GLenum kPackRowLength = 0x0D02;

inline constexpr GLenum kVertexShader = 0x8B31;
inline constexpr GLenum kFragmentShader = 0x8B30;
inline constexpr GLenum kCompileStatus = 0x8B81;
inline constexpr GLenum kLinkStatus = 0x8B82;
inline constexpr GLenum kInfoLogLength = 0x8B84;
inline constexpr GLenum kShaderSourceLength = 0x8B88;
inline constexpr GLenum kDeleteStatus = 0x8B80;
inline constexpr GLenum kValidateStatus = 0x8B83;
inline constexpr GLenum kActiveUniforms = 0x8B86;
inline constexpr GLenum kAttachedShaders = 0x8B85;

inline constexpr GLenum kVendor = 0x1F00;
inline constexpr GLenum kRenderer = 0x1F01;
inline constexpr GLenum kVersion = 0x1F02;
inline constexpr GLenum kExtensions = 0x1F03;
inline constexpr GLenum kShadingLanguageVersion = 0x8B8C;
inline constexpr GLenum kNumExtensions = 0x821D;
inline constexpr GLenum kMajorVersion = 0x821B;
inline constexpr GLenum kMinorVersion = 0x821C;
inline constexpr GLenum kMaxTextureSize = 0x0D33;
inline constexpr GLenum kMaxSamples = 0x8D57;
inline constexpr GLenum kMaxArrayTextureLayers = 0x88FF;
inline constexpr GLenum kMaxTextureImageUnits = 0x8872;
inline constexpr GLenum kMaxVertexAttribs = 0x8869;
inline constexpr GLenum kShaderStorageBufferOffsetAlignment = 0x90DF;
inline constexpr GLenum kUniformBufferOffsetAlignment = 0x8A34;
inline constexpr GLenum kViewport = 0x0BA2;
inline constexpr GLenum kScissorBox = 0x0C10;
inline constexpr GLenum kFramebufferBinding = 0x8CA6;
inline constexpr GLenum kReadFramebufferBinding = 0x8CAA;
inline constexpr GLenum kCurrentProgram = 0x8B8D;
inline constexpr GLenum kActiveTexture = 0x84E0;
inline constexpr GLenum kTextureBinding2D = 0x8069;
inline constexpr GLenum kArrayBufferBinding = 0x8894;
inline constexpr GLenum kVertexArrayBinding = 0x85B5;

inline constexpr GLenum kTimeElapsed = 0x88BF;
inline constexpr GLenum kQueryResult = 0x8866;
inline constexpr GLenum kQueryResultAvailable = 0x8867;

inline constexpr GLenum kSyncGpuCommandsComplete = 0x9117;
inline constexpr GLbitfield kSyncFlushCommandsBit = 0x00000001;
inline constexpr GLenum kAlreadySignaled = 0x911A;
inline constexpr GLenum kTimeoutExpired = 0x911B;
inline constexpr GLenum kConditionSatisfied = 0x911C;
inline constexpr GLenum kWaitFailed = 0x911D;

inline constexpr GLenum kLowerLeft = 0x8CA1;
inline constexpr GLenum kUpperLeft = 0x8CA2;
inline constexpr GLenum kNegativeOneToOne = 0x935E;
inline constexpr GLenum kZeroToOne = 0x935F;

inline constexpr GLbitfield kShaderStorageBarrierBit = 0x00002000;
inline constexpr GLbitfield kAllBarrierBits = 0xFFFFFFFF;

}  // namespace gl
}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_GL_ENUMS_H_
