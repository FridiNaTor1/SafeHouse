#include <algorithm>
#include <cstring>

#include "context.h"
#include "enum_translation.h"

namespace cane::gl_metal {

namespace {

constexpr size_t kMaxRetiredBuffers = 64;

int MipLevelCount(int width, int height, int depth = 1) {
  int levels = 1;
  int size = std::max({width, height, depth});
  while (size > 1) {
    size >>= 1;
    ++levels;
  }
  return levels;
}

}  // namespace

// ---------------------------------------------------------------------------
// Buffers

uint32_t* Context::BindingSlot(GLenum target) {
  switch (target) {
    case gl::kArrayBuffer:
      return &state_.array_buffer;
    case gl::kCopyWriteBuffer:
      return &state_.copy_write_buffer;
    case gl::kCopyReadBuffer:
      return &state_.copy_read_buffer;
    case gl::kDrawIndirectBuffer:
      return &state_.draw_indirect_buffer;
    case gl::kShaderStorageBuffer:
      return &state_.shader_storage_buffer;
    case gl::kUniformBuffer:
      return &state_.uniform_buffer;
    case gl::kElementArrayBuffer: {
      VertexArrayRecord* vao = vertex_arrays_.Find(state_.vertex_array);
      return vao ? &vao->element_buffer : nullptr;
    }
    default:
      return nullptr;
  }
}

BufferRecord* Context::BoundBuffer(GLenum target) {
  uint32_t* slot = BindingSlot(target);
  if (!slot) {
    SetError(gl::kInvalidEnum, __func__, target);
    return nullptr;
  }
  BufferRecord* record = buffers_.Find(*slot);
  if (!record) SetError(gl::kInvalidOperation);
  return record;
}

void Context::GenBuffers(int count, uint32_t* names) {
  for (int i = 0; i < count; ++i) names[i] = buffers_.Create();
}

void Context::DeleteBuffers(int count, const uint32_t* names) {
  for (int i = 0; i < count; ++i) {
    BufferRecord* record = buffers_.Find(names[i]);
    if (!record) continue;
    if (record->buffer) RetireBuffer(record->buffer, record->last_use_submission);
    buffers_.Destroy(names[i]);
    for (GLenum target : {gl::kArrayBuffer, gl::kCopyWriteBuffer, gl::kCopyReadBuffer,
                          gl::kDrawIndirectBuffer, gl::kShaderStorageBuffer, gl::kUniformBuffer}) {
      uint32_t* slot = BindingSlot(target);
      if (slot && *slot == names[i]) *slot = 0;
    }
  }
}

void Context::BindBuffer(GLenum target, uint32_t name) {
  uint32_t* slot = BindingSlot(target);
  if (!slot) {
    SetError(gl::kInvalidEnum, __func__, target);
    return;
  }
  if (name) buffers_.FindOrCreate(name);
  *slot = name;
}

void Context::BindBufferRange(GLenum target, uint32_t index, uint32_t name, size_t offset,
                              size_t size) {
  std::array<BufferRange, kStorageBufferCount>* ranges = nullptr;
  if (target == gl::kShaderStorageBuffer) {
    ranges = &state_.storage_ranges;
  } else if (target == gl::kUniformBuffer) {
    ranges = &state_.uniform_ranges;
  }
  if (!ranges || index >= static_cast<uint32_t>(kStorageBufferCount)) {
    SetError(gl::kInvalidValue);
    return;
  }
  if (name) buffers_.FindOrCreate(name);
  (*ranges)[index] = {name, offset, size};
  *BindingSlot(target) = name;
}

id<MTLBuffer> Context::NewBuffer(size_t size) {
  const uint64_t completed = tracker_.last_completed();
  size_t best = SIZE_MAX;
  for (size_t i = 0; i < retired_buffers_.size(); ++i) {
    const RetiredBuffer& retired = retired_buffers_[i];
    if (retired.serial > completed) continue;
    const size_t length = retired.buffer.length;
    if (length >= size && length <= size * 2 + 4096 &&
        (best == SIZE_MAX || length < retired_buffers_[best].buffer.length)) {
      best = i;
    }
  }
  if (best != SIZE_MAX) {
    id<MTLBuffer> buffer = retired_buffers_[best].buffer;
    retired_buffers_.erase(retired_buffers_.begin() + static_cast<ptrdiff_t>(best));
    return buffer;
  }
  return [device_ newBufferWithLength:std::max<size_t>(size, 16)
                              options:MTLResourceStorageModeShared];
}

void Context::RetireBuffer(id<MTLBuffer> buffer, uint64_t serial) {
  retired_buffers_.push_back({buffer, serial});
  if (retired_buffers_.size() <= kMaxRetiredBuffers) return;
  const uint64_t completed = tracker_.last_completed();
  auto it = std::find_if(retired_buffers_.begin(), retired_buffers_.end(),
                         [&](const RetiredBuffer& r) { return r.serial <= completed; });
  if (it != retired_buffers_.end()) retired_buffers_.erase(it);
}

bool Context::BufferInUse(const BufferRecord& record) const {
  return record.last_use_submission != 0 && record.last_use_submission > tracker_.last_completed();
}

void Context::TouchBuffer(BufferRecord* record) { record->last_use_submission = CurrentSerial(); }

void Context::OrphanBuffer(BufferRecord* record, size_t size) {
  if (record->buffer && !BufferInUse(*record) && record->buffer.length >= size &&
      record->buffer.length <= size * 2 + 4096) {
    record->size = size;
    return;
  }
  if (record->buffer) RetireBuffer(record->buffer, record->last_use_submission);
  record->buffer = NewBuffer(size);
  record->size = size;
  record->last_use_submission = 0;
}

void Context::WriteBuffer(BufferRecord* record, size_t offset, size_t size, const void* data) {
  if (!size) return;
  if (!BufferInUse(*record)) {
    std::memcpy(static_cast<uint8_t*>(record->buffer.contents) + offset, data, size);
    return;
  }
  const TransientAllocation staging = transient_->Allocate(size, 16);
  std::memcpy(staging.cpu, data, size);
  [BlitEncoder() copyFromBuffer:staging.buffer
                   sourceOffset:staging.offset
                       toBuffer:record->buffer
              destinationOffset:offset
                           size:size];
  TouchBuffer(record);
}

void Context::BufferData(GLenum target, size_t size, const void* data, GLenum usage) {
  (void)usage;
  BufferRecord* record = BoundBuffer(target);
  if (!record) return;
  OrphanBuffer(record, size);
  if (data) WriteBuffer(record, 0, size, data);
}

void Context::BufferSubData(GLenum target, size_t offset, size_t size, const void* data) {
  BufferRecord* record = BoundBuffer(target);
  if (!record) return;
  if (offset + size > record->size) {
    SetError(gl::kInvalidValue);
    return;
  }
  WriteBuffer(record, offset, size, data);
}

void* Context::MapBufferRange(GLenum target, size_t offset, size_t length, GLbitfield access) {
  BufferRecord* record = BoundBuffer(target);
  if (!record || record->mapped || offset + length > record->size) {
    SetError(gl::kInvalidOperation);
    return nullptr;
  }
  const bool direct = (access & gl::kMapPersistentBit) || !BufferInUse(*record);
  record->mapped_offset = offset;
  record->mapped_length = length;
  record->mapped_persistent = direct;
  if (direct) {
    record->mapped = static_cast<uint8_t*>(record->buffer.contents) + offset;
  } else {
    record->staging.assign(length, 0);
    if (access & gl::kMapReadBit) {
      std::memcpy(record->staging.data(), static_cast<uint8_t*>(record->buffer.contents) + offset,
                  length);
    }
    record->mapped = record->staging.data();
  }
  return record->mapped;
}

bool Context::UnmapBuffer(GLenum target) {
  BufferRecord* record = BoundBuffer(target);
  if (!record || !record->mapped) return false;
  if (!record->mapped_persistent) {
    WriteBuffer(record, record->mapped_offset, record->mapped_length, record->staging.data());
    record->staging.clear();
  }
  record->mapped = nullptr;
  return true;
}

// ---------------------------------------------------------------------------
// Textures

uint32_t Context::BoundTextureName(GLenum target) const {
  const TextureUnit& unit = state_.units[state_.active_unit];
  return target == gl::kTexture2DArray ? unit.texture_2d_array : unit.texture_2d;
}

TextureRecord* Context::BoundTexture(GLenum target) {
  TextureRecord* record = textures_.Find(BoundTextureName(target));
  if (!record) SetError(gl::kInvalidOperation);
  return record;
}

void Context::GenTextures(int count, uint32_t* names) {
  for (int i = 0; i < count; ++i) names[i] = textures_.Create();
}

void Context::DeleteTextures(int count, const uint32_t* names) {
  for (int i = 0; i < count; ++i) {
    if (!textures_.Find(names[i])) continue;
    textures_.Destroy(names[i]);
    for (TextureUnit& unit : state_.units) {
      if (unit.texture_2d == names[i]) unit.texture_2d = 0;
      if (unit.texture_2d_array == names[i]) unit.texture_2d_array = 0;
    }
  }
}

void Context::ActiveTexture(GLenum unit) {
  const int index = static_cast<int>(unit - gl::kTexture0);
  if (index < 0 || index >= kTextureSlotCount) {
    SetError(gl::kInvalidEnum, __func__, unit);
    return;
  }
  state_.active_unit = index;
}

void Context::BindTexture(GLenum target, uint32_t name) {
  TextureUnit& unit = state_.units[state_.active_unit];
  if (name) {
    TextureRecord* record = textures_.FindOrCreate(name);
    if (record->target == 0) record->target = target;
  }
  if (target == gl::kTexture2DArray) {
    unit.texture_2d_array = name;
  } else if (target == gl::kTexture2D) {
    unit.texture_2d = name;
  } else {
    SetError(gl::kInvalidEnum, __func__, target);
  }
}

id<MTLTexture> Context::AllocateTexture(GLenum target, MTLPixelFormat format, int width, int height,
                                        int layers, int levels, int samples) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor new];
  descriptor.pixelFormat = format;
  descriptor.width = static_cast<NSUInteger>(width);
  descriptor.height = static_cast<NSUInteger>(height);
  descriptor.mipmapLevelCount = static_cast<NSUInteger>(levels);
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
  if (samples > 1) {
    descriptor.textureType = MTLTextureType2DMultisample;
    descriptor.sampleCount = static_cast<NSUInteger>(samples);
    descriptor.mipmapLevelCount = 1;
  } else if (target == gl::kTexture2DArray) {
    descriptor.textureType = MTLTextureType2DArray;
    descriptor.arrayLength = static_cast<NSUInteger>(layers);
  } else {
    descriptor.textureType = MTLTextureType2D;
  }
  return [device_ newTextureWithDescriptor:descriptor];
}

bool Context::DefineTexture(TextureRecord* record, GLenum target, GLenum internal_format, int width,
                            int height, int layers, int levels) {
  const PixelFormatInfo info = ToPixelFormat(internal_format);
  if (info.format == MTLPixelFormatInvalid || width <= 0 || height <= 0) {
    SetError(gl::kInvalidEnum, __func__, internal_format);
    return false;
  }
  record->texture = AllocateTexture(target, info.format, width, height, layers, levels, 1);
  if (!record->texture) {
    SetError(gl::kOutOfMemory);
    return false;
  }
  record->target = target;
  record->format = info.format;
  record->internal_format = internal_format;
  record->width = width;
  record->height = height;
  record->layers = layers;
  record->levels = levels;
  record->samples = 1;
  return true;
}

void Context::UploadTexture(TextureRecord* record, int level, int x, int y, int layer, int width,
                            int height, GLenum format, GLenum type, const void* data) {
  if (!data || !record->texture) return;
  const PixelFormatInfo info = ToPixelFormat(record->internal_format);
  const size_t client_bpp = ClientPixelBytes(format, type);
  const size_t client_row =
      (static_cast<size_t>(width) * client_bpp + static_cast<size_t>(state_.unpack_alignment) - 1) /
      static_cast<size_t>(state_.unpack_alignment) * static_cast<size_t>(state_.unpack_alignment);
  const size_t gpu_row = static_cast<size_t>(width) * info.bytes_per_pixel;
  const size_t gpu_bytes = gpu_row * static_cast<size_t>(height);
  const TransientAllocation staging = transient_->Allocate(gpu_bytes, 256);
  const uint8_t* src = static_cast<const uint8_t*>(data);
  uint8_t* dst = static_cast<uint8_t*>(staging.cpu);
  if (client_bpp == info.bytes_per_pixel) {
    for (int row = 0; row < height; ++row) {
      std::memcpy(dst + static_cast<size_t>(row) * gpu_row,
                  src + static_cast<size_t>(row) * client_row, gpu_row);
    }
  } else if (format == gl::kRgb && type == gl::kUnsignedByte && info.bytes_per_pixel == 4) {
    for (int row = 0; row < height; ++row) {
      const uint8_t* s = src + static_cast<size_t>(row) * client_row;
      uint8_t* d = dst + static_cast<size_t>(row) * gpu_row;
      for (int column = 0; column < width; ++column) {
        d[column * 4 + 0] = s[column * 3 + 0];
        d[column * 4 + 1] = s[column * 3 + 1];
        d[column * 4 + 2] = s[column * 3 + 2];
        d[column * 4 + 3] = 255;
      }
    }
  } else {
    SetError(gl::kInvalidOperation);
    return;
  }
  [BlitEncoder()
           copyFromBuffer:staging.buffer
             sourceOffset:staging.offset
        sourceBytesPerRow:gpu_row
      sourceBytesPerImage:gpu_bytes
               sourceSize:MTLSizeMake(static_cast<NSUInteger>(width),
                                      static_cast<NSUInteger>(height), 1)
                toTexture:record->texture
         destinationSlice:static_cast<NSUInteger>(layer)
         destinationLevel:static_cast<NSUInteger>(level)
        destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(x), static_cast<NSUInteger>(y), 0)];
}

void Context::TexImage2D(GLenum target, int level, GLenum internal_format, int width, int height,
                         GLenum format, GLenum type, const void* data) {
  TextureRecord* record = BoundTexture(target);
  if (!record) return;
  if (level != 0) {
    if (!record->texture || level >= record->levels) {
      SetError(gl::kInvalidOperation);
      return;
    }
    UploadTexture(record, level, 0, 0, 0, width, height, format, type, data);
    return;
  }
  if (record->immutable) {
    SetError(gl::kInvalidOperation);
    return;
  }
  const bool reuse = record->texture && record->width == width && record->height == height &&
                     record->internal_format == internal_format && record->levels == 1 &&
                     record->samples == 1 && record->target == target;
  if (!reuse && !DefineTexture(record, target, internal_format, width, height, 1, 1)) return;
  UploadTexture(record, 0, 0, 0, 0, width, height, format, type, data);
}

void Context::TexStorage2D(GLenum target, int levels, GLenum internal_format, int width,
                           int height) {
  TextureRecord* record = BoundTexture(target);
  if (!record) return;
  if (DefineTexture(record, target, internal_format, width, height, 1, levels)) {
    record->immutable = true;
  }
}

void Context::TexStorage3D(GLenum target, int levels, GLenum internal_format, int width, int height,
                           int depth) {
  TextureRecord* record = BoundTexture(target);
  if (!record) return;
  if (DefineTexture(record, target, internal_format, width, height, depth, levels)) {
    record->immutable = true;
  }
}

void Context::TexSubImage2D(GLenum target, int level, int x, int y, int width, int height,
                            GLenum format, GLenum type, const void* data) {
  TextureRecord* record = BoundTexture(target);
  if (!record || !record->texture) return;
  UploadTexture(record, level, x, y, 0, width, height, format, type, data);
}

void Context::TexSubImage3D(GLenum target, int level, int x, int y, int z, int width, int height,
                            int depth, GLenum format, GLenum type, const void* data) {
  TextureRecord* record = BoundTexture(target);
  if (!record || !record->texture) return;
  const size_t layer_bytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * ClientPixelBytes(format, type);
  for (int layer = 0; layer < depth; ++layer) {
    UploadTexture(record, level, x, y, z + layer, width, height, format, type,
                  static_cast<const uint8_t*>(data) + static_cast<size_t>(layer) * layer_bytes);
  }
}

void Context::SetTextureParameter(TextureRecord* record, GLenum pname, int value) {
  switch (pname) {
    case gl::kTextureMinFilter:
      record->min_filter = static_cast<GLenum>(value);
      break;
    case gl::kTextureMagFilter:
      record->mag_filter = static_cast<GLenum>(value);
      break;
    case gl::kTextureWrapS:
      record->wrap_s = static_cast<GLenum>(value);
      break;
    case gl::kTextureWrapT:
      record->wrap_t = static_cast<GLenum>(value);
      break;
    case gl::kTextureWrapR:
      record->wrap_r = static_cast<GLenum>(value);
      break;
    case gl::kTextureMaxLevel:
      record->max_level = value;
      break;
    case gl::kTextureBaseLevel:
      break;
    case gl::kTextureSwizzleR:
    case gl::kTextureSwizzleG:
    case gl::kTextureSwizzleB:
    case gl::kTextureSwizzleA:
      record->swizzle[pname - gl::kTextureSwizzleR] = static_cast<GLenum>(value);
      break;
    default:
      SetError(gl::kInvalidEnum, __func__, pname);
      break;
  }
}

void Context::TexParameteri(GLenum target, GLenum pname, int value) {
  TextureRecord* record = BoundTexture(target);
  if (record) SetTextureParameter(record, pname, value);
}

void Context::GenerateMipmap(GLenum target) {
  TextureRecord* record = BoundTexture(target);
  if (!record || !record->texture) return;
  if (record->levels == 1) {
    const int levels = MipLevelCount(record->width, record->height);
    if (levels == 1) return;
    id<MTLTexture> mipped = AllocateTexture(target, record->format, record->width, record->height,
                                            record->layers, levels, 1);
    if (!mipped) {
      SetError(gl::kOutOfMemory);
      return;
    }
    id<MTLBlitCommandEncoder> blit = BlitEncoder();
    for (int layer = 0; layer < record->layers; ++layer) {
      [blit copyFromTexture:record->texture
                sourceSlice:static_cast<NSUInteger>(layer)
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(static_cast<NSUInteger>(record->width),
                                        static_cast<NSUInteger>(record->height), 1)
                  toTexture:mipped
           destinationSlice:static_cast<NSUInteger>(layer)
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    record->texture = mipped;
    record->levels = levels;
  }
  [BlitEncoder() generateMipmapsForTexture:record->texture];
}

void Context::PixelStorei(GLenum pname, int value) {
  if (pname == gl::kPackAlignment) {
    state_.pack_alignment = value;
  } else if (pname == gl::kUnpackAlignment) {
    state_.unpack_alignment = value;
  } else if (pname != gl::kUnpackRowLength && pname != gl::kPackRowLength) {
    SetError(gl::kInvalidEnum, __func__, pname);
  }
}

SamplerKey Context::SamplerKeyFor(const TextureRecord& texture,
                                  const SamplerRecord* sampler) const {
  SamplerKey key;
  if (sampler) {
    key.min_filter = sampler->min_filter;
    key.mag_filter = sampler->mag_filter;
    key.wrap_s = sampler->wrap_s;
    key.wrap_t = sampler->wrap_t;
    key.wrap_r = sampler->wrap_r;
    key.max_anisotropy = sampler->max_anisotropy;
  } else {
    key.min_filter = texture.min_filter;
    key.mag_filter = texture.mag_filter;
    key.wrap_s = texture.wrap_s;
    key.wrap_t = texture.wrap_t;
    key.wrap_r = texture.wrap_r;
  }
  key.mipmapped = texture.levels > 1 && texture.max_level > 0 && HasMipmapFilter(key.min_filter);
  return key;
}

id<MTLTexture> Context::TextureForSampling(TextureRecord* record) {
  if (!record || !record->texture) return nil;
  return record->texture;
}

// ---------------------------------------------------------------------------
// Samplers and renderbuffers

void Context::GenSamplers(int count, uint32_t* names) {
  for (int i = 0; i < count; ++i) names[i] = samplers_.Create();
}

void Context::DeleteSamplers(int count, const uint32_t* names) {
  for (int i = 0; i < count; ++i) {
    samplers_.Destroy(names[i]);
    for (TextureUnit& unit : state_.units) {
      if (unit.sampler == names[i]) unit.sampler = 0;
    }
  }
}

void Context::BindSampler(uint32_t unit, uint32_t name) {
  if (unit >= static_cast<uint32_t>(kTextureSlotCount)) {
    SetError(gl::kInvalidValue);
    return;
  }
  if (name && !samplers_.Find(name)) {
    SetError(gl::kInvalidOperation);
    return;
  }
  state_.units[unit].sampler = name;
}

void Context::SamplerParameteri(uint32_t name, GLenum pname, int value) {
  SamplerRecord* sampler = samplers_.Find(name);
  if (!sampler) {
    SetError(gl::kInvalidOperation);
    return;
  }
  switch (pname) {
    case gl::kTextureMinFilter:
      sampler->min_filter = static_cast<GLenum>(value);
      break;
    case gl::kTextureMagFilter:
      sampler->mag_filter = static_cast<GLenum>(value);
      break;
    case gl::kTextureWrapS:
      sampler->wrap_s = static_cast<GLenum>(value);
      break;
    case gl::kTextureWrapT:
      sampler->wrap_t = static_cast<GLenum>(value);
      break;
    case gl::kTextureWrapR:
      sampler->wrap_r = static_cast<GLenum>(value);
      break;
    default:
      SetError(gl::kInvalidEnum, __func__, pname);
      break;
  }
}

void Context::SamplerParameterf(uint32_t name, GLenum pname, float value) {
  SamplerRecord* sampler = samplers_.Find(name);
  if (!sampler) {
    SetError(gl::kInvalidOperation);
    return;
  }
  switch (pname) {
    case gl::kTextureMaxAnisotropy:
      sampler->max_anisotropy = value;
      break;
    case gl::kTextureMinLod:
      sampler->min_lod = value;
      break;
    case gl::kTextureMaxLod:
      sampler->max_lod = value;
      break;
    default:
      SamplerParameteri(name, pname, static_cast<int>(value));
      break;
  }
}

void Context::GenRenderbuffers(int count, uint32_t* names) {
  for (int i = 0; i < count; ++i) names[i] = renderbuffers_.Create();
}

void Context::DeleteRenderbuffers(int count, const uint32_t* names) {
  for (int i = 0; i < count; ++i) {
    renderbuffers_.Destroy(names[i]);
    if (state_.renderbuffer == names[i]) state_.renderbuffer = 0;
  }
}

void Context::BindRenderbuffer(GLenum target, uint32_t name) {
  if (target != gl::kRenderbuffer) {
    SetError(gl::kInvalidEnum, __func__, target);
    return;
  }
  if (name) renderbuffers_.FindOrCreate(name);
  state_.renderbuffer = name;
}

void Context::RenderbufferStorage(GLenum target, int samples, GLenum internal_format, int width,
                                  int height) {
  (void)target;
  RenderbufferRecord* record = renderbuffers_.Find(state_.renderbuffer);
  if (!record) {
    SetError(gl::kInvalidOperation);
    return;
  }
  const PixelFormatInfo info = ToPixelFormat(internal_format);
  if (info.format == MTLPixelFormatInvalid) {
    SetError(gl::kInvalidEnum, __func__, internal_format);
    return;
  }
  int chosen = std::max(samples, 1);
  while (chosen > 1 && ![device_ supportsTextureSampleCount:chosen]) --chosen;
  record->texture = AllocateTexture(gl::kTexture2D, info.format, width, height, 1, 1, chosen);
  if (!record->texture) {
    SetError(gl::kOutOfMemory);
    return;
  }
  record->format = info.format;
  record->internal_format = internal_format;
  record->width = width;
  record->height = height;
  record->samples = chosen;
}

// ---------------------------------------------------------------------------
// Vertex arrays

void Context::GenVertexArrays(int count, uint32_t* names) {
  for (int i = 0; i < count; ++i) names[i] = vertex_arrays_.Create();
}

void Context::DeleteVertexArrays(int count, const uint32_t* names) {
  for (int i = 0; i < count; ++i) {
    vertex_arrays_.Destroy(names[i]);
    if (state_.vertex_array == names[i]) state_.vertex_array = 0;
  }
}

void Context::BindVertexArray(uint32_t name) {
  if (name && !vertex_arrays_.Find(name)) {
    SetError(gl::kInvalidOperation);
    return;
  }
  state_.vertex_array = name;
}

void Context::EnableVertexAttribArray(uint32_t index, bool enabled) {
  VertexArrayRecord* vao = vertex_arrays_.Find(state_.vertex_array);
  if (!vao || index >= static_cast<uint32_t>(kVertexAttributeCount)) {
    SetError(gl::kInvalidOperation);
    return;
  }
  vao->attributes[index].enabled = enabled;
}

void Context::VertexAttribPointer(uint32_t index, int components, GLenum type, bool normalized,
                                  bool integer, int stride, size_t offset) {
  VertexArrayRecord* vao = vertex_arrays_.Find(state_.vertex_array);
  if (!vao || index >= static_cast<uint32_t>(kVertexAttributeCount)) {
    SetError(gl::kInvalidOperation);
    return;
  }
  VertexAttribute& attribute = vao->attributes[index];
  attribute.buffer = state_.array_buffer;
  attribute.components = components;
  attribute.type = type;
  attribute.normalized = normalized;
  attribute.integer = integer;
  attribute.offset = offset;
  if (stride == 0) {
    const size_t element = ClientPixelBytes(gl::kRed, type);
    stride = static_cast<int>(element * static_cast<size_t>(components));
  }
  attribute.stride = stride;
}

void Context::VertexAttribDivisor(uint32_t index, int divisor) {
  VertexArrayRecord* vao = vertex_arrays_.Find(state_.vertex_array);
  if (!vao || index >= static_cast<uint32_t>(kVertexAttributeCount)) {
    SetError(gl::kInvalidOperation);
    return;
  }
  vao->attributes[index].divisor = divisor;
}

// ---------------------------------------------------------------------------
// Framebuffers

FramebufferRecord* Context::BoundFramebuffer(GLenum target) {
  const uint32_t name =
      target == gl::kReadFramebuffer ? state_.read_framebuffer : state_.draw_framebuffer;
  FramebufferRecord* record = framebuffers_.Find(name);
  if (!record) SetError(gl::kInvalidOperation);
  return record;
}

Attachment* Context::AttachmentFor(FramebufferRecord* framebuffer, GLenum attachment) {
  switch (attachment) {
    case gl::kColorAttachment0:
      return &framebuffer->color;
    case gl::kDepthAttachment:
      return &framebuffer->depth;
    case gl::kStencilAttachment:
      return &framebuffer->stencil;
    default:
      return nullptr;
  }
}

void Context::GenFramebuffers(int count, uint32_t* names) {
  for (int i = 0; i < count; ++i) names[i] = framebuffers_.Create();
}

void Context::DeleteFramebuffers(int count, const uint32_t* names) {
  for (int i = 0; i < count; ++i) {
    if (!framebuffers_.Find(names[i])) continue;
    if (encoder_framebuffer_ == names[i]) EndRenderEncoder();
    if (pending_clear_.active && pending_clear_.framebuffer == names[i]) FlushPendingClear();
    framebuffers_.Destroy(names[i]);
    if (state_.draw_framebuffer == names[i]) state_.draw_framebuffer = 0;
    if (state_.read_framebuffer == names[i]) state_.read_framebuffer = 0;
  }
}

void Context::BindFramebuffer(GLenum target, uint32_t name) {
  if (name) framebuffers_.FindOrCreate(name);
  if (target == gl::kFramebuffer || target == gl::kDrawFramebuffer) {
    state_.draw_framebuffer = name;
  }
  if (target == gl::kFramebuffer || target == gl::kReadFramebuffer) {
    state_.read_framebuffer = name;
  }
}

void Context::FramebufferTexture2D(GLenum target, GLenum attachment, uint32_t texture, int level) {
  FramebufferRecord* framebuffer = BoundFramebuffer(target);
  if (!framebuffer) return;
  const uint32_t name =
      target == gl::kReadFramebuffer ? state_.read_framebuffer : state_.draw_framebuffer;
  if (encoder_framebuffer_ == name) EndRenderEncoder();
  Attachment value;
  value.kind = texture ? Attachment::Kind::kTexture : Attachment::Kind::kNone;
  value.name = texture;
  value.level = level;
  if (attachment == gl::kDepthStencilAttachment) {
    framebuffer->depth = value;
    framebuffer->stencil = value;
    return;
  }
  Attachment* slot = AttachmentFor(framebuffer, attachment);
  if (!slot) {
    SetError(gl::kInvalidEnum, __func__, attachment);
    return;
  }
  *slot = value;
}

void Context::FramebufferRenderbuffer(GLenum target, GLenum attachment, uint32_t renderbuffer) {
  FramebufferRecord* framebuffer = BoundFramebuffer(target);
  if (!framebuffer) return;
  const uint32_t name =
      target == gl::kReadFramebuffer ? state_.read_framebuffer : state_.draw_framebuffer;
  if (encoder_framebuffer_ == name) EndRenderEncoder();
  Attachment value;
  value.kind = renderbuffer ? Attachment::Kind::kRenderbuffer : Attachment::Kind::kNone;
  value.name = renderbuffer;
  if (attachment == gl::kDepthStencilAttachment) {
    framebuffer->depth = value;
    framebuffer->stencil = value;
    return;
  }
  Attachment* slot = AttachmentFor(framebuffer, attachment);
  if (!slot) {
    SetError(gl::kInvalidEnum, __func__, attachment);
    return;
  }
  *slot = value;
}

id<MTLTexture> Context::AttachmentTexture(const Attachment& attachment, int* samples) {
  if (attachment.kind == Attachment::Kind::kTexture) {
    TextureRecord* texture = textures_.Find(attachment.name);
    if (!texture) return nil;
    if (samples) *samples = texture->samples;
    return texture->texture;
  }
  if (attachment.kind == Attachment::Kind::kRenderbuffer) {
    RenderbufferRecord* renderbuffer = renderbuffers_.Find(attachment.name);
    if (!renderbuffer) return nil;
    if (samples) *samples = renderbuffer->samples;
    return renderbuffer->texture;
  }
  return nil;
}

bool Context::ResolveTarget(uint32_t framebuffer, RenderTarget* out) {
  *out = RenderTarget{};
  if (framebuffer == 0) {
    id<CAMetalDrawable> drawable = AcquireDrawable();
    if (!drawable) return false;
    out->color = drawable.texture;
    out->width = static_cast<int>(drawable.texture.width);
    out->height = static_cast<int>(drawable.texture.height);
    out->is_default = true;
    return true;
  }
  FramebufferRecord* record = framebuffers_.Find(framebuffer);
  if (!record) return false;
  int samples = 1;
  out->color = AttachmentTexture(record->color, &samples);
  out->depth = AttachmentTexture(record->depth, &samples);
  out->stencil = AttachmentTexture(record->stencil, &samples);
  if (out->depth && out->depth.pixelFormat == MTLPixelFormatDepth32Float) out->stencil = nil;
  if (out->stencil && out->stencil.pixelFormat == MTLPixelFormatStencil8) out->depth = nil;
  id<MTLTexture> any = out->color ? out->color : (out->depth ? out->depth : out->stencil);
  if (!any) return false;
  out->width = static_cast<int>(any.width);
  out->height = static_cast<int>(any.height);
  out->samples = samples;
  return true;
}

GLenum Context::CheckFramebufferStatus(GLenum target) {
  const uint32_t name =
      target == gl::kReadFramebuffer ? state_.read_framebuffer : state_.draw_framebuffer;
  if (name == 0) return gl::kFramebufferComplete;
  RenderTarget resolved;
  return ResolveTarget(name, &resolved) ? gl::kFramebufferComplete
                                        : gl::kFramebufferIncompleteAttachment;
}

id<MTLTexture> Context::ReadColorTexture(uint32_t framebuffer, int* width, int* height) {
  RenderTarget target;
  if (!ResolveTarget(framebuffer, &target) || !target.color) return nil;
  *width = target.width;
  *height = target.height;
  return target.color;
}

}  // namespace cane::gl_metal
