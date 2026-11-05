#ifndef CANE_PLATFORM_METAL_BINDING_SLOTS_H_
#define CANE_PLATFORM_METAL_BINDING_SLOTS_H_

// Vertex-stage buffer indices: attributes occupy [0, kVertexBufferCount),
// then the uniform block, then the storage buffers (GL binding N -> slot
// kStorageBufferBase + N). The fragment stage uses only the uniform block.
#define CANE_MTL_VERTEX_BUFFER_COUNT 8
#define CANE_MTL_UNIFORM_BUFFER_SLOT 8
#define CANE_MTL_STORAGE_BUFFER_BASE 9
#define CANE_MTL_STORAGE_BUFFER_COUNT 16
#define CANE_MTL_TEXTURE_SLOT_COUNT 8
#define CANE_MTL_UNIFORM_BLOCK_MAX_BYTES 1024

#ifndef __METAL_VERSION__
namespace cane::gl_metal {

inline constexpr int kVertexBufferCount = CANE_MTL_VERTEX_BUFFER_COUNT;
inline constexpr int kUniformBufferSlot = CANE_MTL_UNIFORM_BUFFER_SLOT;
inline constexpr int kStorageBufferBase = CANE_MTL_STORAGE_BUFFER_BASE;
inline constexpr int kStorageBufferCount = CANE_MTL_STORAGE_BUFFER_COUNT;
inline constexpr int kTextureSlotCount = CANE_MTL_TEXTURE_SLOT_COUNT;
inline constexpr int kUniformBlockMaxBytes = CANE_MTL_UNIFORM_BLOCK_MAX_BYTES;
inline constexpr int kVertexAttributeCount = 16;

}  // namespace cane::gl_metal
#endif  // __METAL_VERSION__

#endif  // CANE_PLATFORM_METAL_BINDING_SLOTS_H_
