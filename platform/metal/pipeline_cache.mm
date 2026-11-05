#include "pipeline_cache.h"

#include <cstring>
#include <sstream>

#include "enum_translation.h"

namespace cane::gl_metal {

namespace {

template <typename T>
void AppendBytes(std::string* out, const T& value) {
  const char* bytes = reinterpret_cast<const char*>(&value);
  out->append(bytes, sizeof(T));
}

void AppendBlend(std::string* out, const BlendState& blend) {
  AppendBytes(out, blend.enabled);
  AppendBytes(out, blend.src_rgb);
  AppendBytes(out, blend.dst_rgb);
  AppendBytes(out, blend.src_alpha);
  AppendBytes(out, blend.dst_alpha);
  AppendBytes(out, blend.equation_rgb);
  AppendBytes(out, blend.equation_alpha);
  for (bool mask : blend.write_mask) AppendBytes(out, mask);
}

void AppendStencilFace(std::string* out, const StencilFace& face) {
  AppendBytes(out, face.func);
  AppendBytes(out, face.compare_mask);
  AppendBytes(out, face.write_mask);
  AppendBytes(out, face.fail);
  AppendBytes(out, face.depth_fail);
  AppendBytes(out, face.pass);
}

MTLColorWriteMask ToWriteMask(const std::array<bool, 4>& mask) {
  MTLColorWriteMask result = MTLColorWriteMaskNone;
  if (mask[0]) result |= MTLColorWriteMaskRed;
  if (mask[1]) result |= MTLColorWriteMaskGreen;
  if (mask[2]) result |= MTLColorWriteMaskBlue;
  if (mask[3]) result |= MTLColorWriteMaskAlpha;
  return result;
}

MTLStencilDescriptor* StencilDescriptor(const StencilFace& face) {
  MTLStencilDescriptor* descriptor = [MTLStencilDescriptor new];
  descriptor.stencilCompareFunction = ToCompareFunction(face.func);
  descriptor.stencilFailureOperation = ToStencilOperation(face.fail);
  descriptor.depthFailureOperation = ToStencilOperation(face.depth_fail);
  descriptor.depthStencilPassOperation = ToStencilOperation(face.pass);
  descriptor.readMask = face.compare_mask;
  descriptor.writeMask = face.write_mask;
  return descriptor;
}

}  // namespace

std::string PipelineKey::Hash() const {
  std::string out;
  out.reserve(256);
  AppendBytes(&out, program);
  for (const VertexAttributeKey& attribute : attributes) {
    AppendBytes(&out, attribute.enabled);
    if (!attribute.enabled) continue;
    AppendBytes(&out, attribute.format);
    AppendBytes(&out, attribute.offset);
    AppendBytes(&out, attribute.buffer_index);
  }
  for (const VertexBufferKey& buffer : buffers) {
    AppendBytes(&out, buffer.stride);
    AppendBytes(&out, buffer.step);
    AppendBytes(&out, buffer.step_rate);
  }
  AppendBytes(&out, target.color);
  AppendBytes(&out, target.depth);
  AppendBytes(&out, target.stencil);
  AppendBytes(&out, target.samples);
  AppendBlend(&out, blend);
  return out;
}

std::string DepthStencilKey::Hash() const {
  std::string out;
  AppendBytes(&out, has_depth);
  AppendBytes(&out, has_stencil);
  AppendBytes(&out, state.depth_test);
  AppendBytes(&out, state.depth_write);
  AppendBytes(&out, state.depth_func);
  AppendBytes(&out, state.stencil_test);
  AppendStencilFace(&out, state.front);
  AppendStencilFace(&out, state.back);
  return out;
}

std::string SamplerKey::Hash() const {
  std::string out;
  AppendBytes(&out, min_filter);
  AppendBytes(&out, mag_filter);
  AppendBytes(&out, wrap_s);
  AppendBytes(&out, wrap_t);
  AppendBytes(&out, wrap_r);
  AppendBytes(&out, max_anisotropy);
  AppendBytes(&out, mipmapped);
  return out;
}

PipelineCache::PipelineCache(id<MTLDevice> device) : device_(device) {}

id<MTLRenderPipelineState> PipelineCache::Pipeline(const PipelineKey& key, id<MTLFunction> vertex,
                                                   id<MTLFunction> fragment, std::string* error) {
  const std::string hash = key.Hash();
  const auto found = pipelines_.find(hash);
  if (found != pipelines_.end()) return found->second;

  MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.rasterSampleCount = key.target.samples;

  bool any_attribute = false;
  MTLVertexDescriptor* vertex_descriptor = [MTLVertexDescriptor new];
  for (int i = 0; i < kVertexAttributeCount; ++i) {
    const VertexAttributeKey& attribute = key.attributes[i];
    if (!attribute.enabled) continue;
    any_attribute = true;
    vertex_descriptor.attributes[i].format = attribute.format;
    vertex_descriptor.attributes[i].offset = attribute.offset;
    vertex_descriptor.attributes[i].bufferIndex = attribute.buffer_index;
  }
  for (int i = 0; i < kVertexBufferCount; ++i) {
    const VertexBufferKey& buffer = key.buffers[i];
    if (buffer.stride == 0) continue;
    vertex_descriptor.layouts[i].stride = buffer.stride;
    vertex_descriptor.layouts[i].stepFunction = buffer.step;
    vertex_descriptor.layouts[i].stepRate = buffer.step_rate;
  }
  if (any_attribute) descriptor.vertexDescriptor = vertex_descriptor;

  MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
  color.pixelFormat = key.target.color;
  color.writeMask = ToWriteMask(key.blend.write_mask);
  color.blendingEnabled = key.blend.enabled;
  if (key.blend.enabled) {
    color.sourceRGBBlendFactor = ToBlendFactor(key.blend.src_rgb);
    color.destinationRGBBlendFactor = ToBlendFactor(key.blend.dst_rgb);
    color.sourceAlphaBlendFactor = ToBlendFactor(key.blend.src_alpha);
    color.destinationAlphaBlendFactor = ToBlendFactor(key.blend.dst_alpha);
    color.rgbBlendOperation = ToBlendOperation(key.blend.equation_rgb);
    color.alphaBlendOperation = ToBlendOperation(key.blend.equation_alpha);
  }
  descriptor.depthAttachmentPixelFormat = key.target.depth;
  descriptor.stencilAttachmentPixelFormat = key.target.stencil;

  NSError* ns_error = nil;
  id<MTLRenderPipelineState> pipeline = [device_ newRenderPipelineStateWithDescriptor:descriptor
                                                                                error:&ns_error];
  if (!pipeline) {
    if (error) *error = ns_error ? ns_error.localizedDescription.UTF8String : "pipeline failed";
    return nil;
  }
  pipelines_[hash] = pipeline;
  return pipeline;
}

id<MTLDepthStencilState> PipelineCache::DepthStencil(const DepthStencilKey& key) {
  const std::string hash = key.Hash();
  const auto found = depth_stencils_.find(hash);
  if (found != depth_stencils_.end()) return found->second;

  MTLDepthStencilDescriptor* descriptor = [MTLDepthStencilDescriptor new];
  if (key.has_depth && key.state.depth_test) {
    descriptor.depthCompareFunction = ToCompareFunction(key.state.depth_func);
    descriptor.depthWriteEnabled = key.state.depth_write;
  } else {
    descriptor.depthCompareFunction = MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = NO;
  }
  if (key.has_stencil && key.state.stencil_test) {
    descriptor.frontFaceStencil = StencilDescriptor(key.state.front);
    descriptor.backFaceStencil = StencilDescriptor(key.state.back);
  }
  id<MTLDepthStencilState> state = [device_ newDepthStencilStateWithDescriptor:descriptor];
  depth_stencils_[hash] = state;
  return state;
}

id<MTLSamplerState> PipelineCache::Sampler(const SamplerKey& key) {
  const std::string hash = key.Hash();
  const auto found = samplers_.find(hash);
  if (found != samplers_.end()) return found->second;

  MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
  descriptor.minFilter = ToMinMagFilter(key.min_filter);
  descriptor.magFilter = ToMinMagFilter(key.mag_filter);
  descriptor.mipFilter =
      key.mipmapped ? ToMipFilter(key.min_filter) : MTLSamplerMipFilterNotMipmapped;
  descriptor.sAddressMode = ToAddressMode(key.wrap_s);
  descriptor.tAddressMode = ToAddressMode(key.wrap_t);
  descriptor.rAddressMode = ToAddressMode(key.wrap_r);
  descriptor.maxAnisotropy =
      static_cast<NSUInteger>(key.max_anisotropy < 1.0f ? 1.0f : key.max_anisotropy);
  descriptor.normalizedCoordinates = YES;
  id<MTLSamplerState> state = [device_ newSamplerStateWithDescriptor:descriptor];
  samplers_[hash] = state;
  return state;
}

void PipelineCache::ForgetProgram(uint32_t program) {
  for (auto it = pipelines_.begin(); it != pipelines_.end();) {
    uint32_t key_program = 0;
    std::memcpy(&key_program, it->first.data(), sizeof(key_program));
    if (key_program == program) {
      it = pipelines_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace cane::gl_metal
