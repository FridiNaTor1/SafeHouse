#pragma once
// Model files for the map editor (desktop only; notes: Sly2-Release/notes/EDITOR_2026-11-12.md, phase 4): a small
// scene of meshes, materials, images, a skeleton and animations, read from glTF 2.0 (.glb / .gltf, cgltf) or FBX
// (.fbx, ufbx) and written as .glb. Game-agnostic: the game's own format is made from this by the game's editor
// (Sly2-Release/editor_model.cpp). Coordinates are the file's (glTF: Y up, metres); the game side converts.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cane::editor::model {

namespace fs = std::filesystem;

struct Image {
    std::string name;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;   // RGBA8, rows top-down (as a PNG)
};

struct Material {
    std::string name;
    glm::vec4 base{1.0f};        // base colour factor (linear as stored in the file)
    int image = -1;              // its base colour image (Scene::images), -1: none
    bool blend = false;          // alpha blended (glTF alphaMode BLEND); mask counts as blended here
    bool doubleSided = false;
};

// One triangle list with one material. Positions / normals: the mesh's space (static meshes: already in the scene's
// space, the node transforms applied; skinned: the bind pose, as the file stores it).
struct Primitive {
    int material = -1;
    std::vector<glm::vec3> pos, nrm;
    std::vector<glm::vec2> uv;          // glTF convention (v = 0: the image's top row)
    std::vector<glm::vec4> color;       // optional vertex colours (0..1)
    std::vector<glm::u16vec4> joints;   // optional: Scene::joints indices
    std::vector<glm::vec4> weights;
    std::vector<uint32_t> idx;
};

struct Mesh {
    std::string name;
    std::vector<Primitive> prims;
    bool skinned = false;
};

struct Joint {
    std::string name;
    int parent = -1;
    glm::mat4 local{1.0f};          // the rest pose relative to the parent
    glm::mat4 inverseBind{1.0f};    // world -> joint at the bind pose
};

struct Channel {
    int joint = -1;
    int path = 0;                    // 0 translation, 1 rotation (quaternion x y z w), 2 scale
    std::vector<float> t;            // seconds
    std::vector<glm::vec4> v;        // per key (translation / scale: xyz)
};

struct Animation {
    std::string name;
    std::vector<Channel> ch;
};

struct Scene {
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Image> images;
    std::vector<Joint> joints;       // one skeleton (parents before children)
    std::vector<Animation> anims;
    // counts for messages
    std::size_t triangles() const;
    std::size_t vertices() const;
};

// .glb / .gltf / .fbx by the extension. Images referenced by the file are decoded (PNG / JPEG, embedded or beside it).
bool load(const fs::path& path, Scene& scene, std::string& error);
// A binary glTF 2.0 file: every mesh as one node (skinned meshes with the skeleton's joints), materials with their
// images as PNG, the animations as joint channels.
bool saveGlb(const fs::path& path, const Scene& scene, std::string& error);

// PNG / JPEG file -> RGBA8 top-down
bool loadImage(const fs::path& path, Image& img, std::string& error);
bool savePng(const fs::path& path, const Image& img, std::string& error);
// PNG bytes of an image (empty: failed)
std::vector<uint8_t> pngBytes(const Image& img);

}  // namespace cane::editor::model
