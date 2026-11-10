#pragma once
// The Sly Collection's (PS3) own game-select menu as data: reading it from the player's disc and converting it into
// the profile (<profile>/Collection/Menu). Nothing of it ships with SafeHouse.
//
// From the disc (folder, .iso or .zip of the .iso: platform/ps3disc.h), relative to PS3_GAME/USRDIR:
//   IMAGE.ZIP                   the menu's data. Used:            Levels/Frontend/Frontend_Art.san (the menu scene),
//                               Scripts/Levels.xmb and Scripts/Tuning.xmb (bloom, camera glide, input delay),
//                               Gui/Font_MCGames_*.tga + .bmp (the comic font and its glyph map), Gui/Banner_*.tga,
//                               Gui/Border_*.tga (the banner and the two frames), Gui/Screen_LoadingWide.tga
//   AUDIO/SLYAUDIO_MUSIC.FSB    the menu music: "FrontEnd_Loop" (FMOD FSB4, IMA ADPCM)
//   AUDIO/KRAZEAUDIO_GAME.FSB   the interface sounds: "Beep_18", "Positive_Beep_16", "Negative_Beep_4"
//
// The scene file (.san): "FORM", then chunks { 4-letter tag, little-endian size counted from the size field's own
// start (the next chunk is at offset + 4 + size), data }; containers nest. Header fields are packed little-endian;
// vertices, indices, textures (GTF) and attribute blocks (ATAG) are big-endian. See readScene().
//
// The converted menu: menu.json (the scene as nodes with meshes, the cameras, the emitters, the font's glyphs, what
// file is what), tex/*.png, gui/*.png, audio/*.wav.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <miniz.h>
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include "audio.h"
#include "install.h"
#include "platform.h"
#include "ps3disc.h"
#include "../l10n.h"

namespace cane::collection {
inline constexpr int kMenuVersion = 2;

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    bool empty() const { return rgba.empty(); }
};
inline Image decodeImage(const uint8_t* data, size_t size) {
    Image img;
    int n = 0;
    if (unsigned char* p = stbi_load_from_memory(data, int(size), &img.width, &img.height, &n, 4)) {
        img.rgba.assign(p, p + size_t(img.width) * size_t(img.height) * 4);
        stbi_image_free(p);
    }
    return img;
}
inline std::vector<uint8_t> readBytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

namespace ps3menu {
// ---------------------------------------------------------------- bytes
struct Bytes {
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool has(size_t at, size_t count) const { return at <= n && count <= n - at; }
    uint8_t u8(size_t at) const { return has(at, 1) ? p[at] : 0; }
    uint16_t le16(size_t at) const { return has(at, 2) ? uint16_t(p[at] | p[at + 1] << 8) : 0; }
    uint16_t be16(size_t at) const { return has(at, 2) ? uint16_t(p[at] << 8 | p[at + 1]) : 0; }
    uint32_t le32(size_t at) const {
        return has(at, 4) ? uint32_t(p[at]) | uint32_t(p[at + 1]) << 8 | uint32_t(p[at + 2]) << 16 | uint32_t(p[at + 3]) << 24 : 0;
    }
    uint32_t be32(size_t at) const {
        return has(at, 4) ? uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | uint32_t(p[at + 3]) : 0;
    }
    static float bits(uint32_t v) {
        float f;
        std::memcpy(&f, &v, 4);
        return std::isfinite(f) ? f : 0.0f;
    }
    float lef(size_t at) const { return bits(le32(at)); }
    float bef(size_t at) const { return bits(be32(at)); }
    // A big-endian IEEE half float.
    float half(size_t at) const {
        const uint16_t h = be16(at);
        const int e = (h >> 10) & 31, m = h & 1023;
        float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 0.0f : std::ldexp(float(m | 1024), e - 25);
        return (h & 0x8000) ? -v : v;
    }
    std::string text(size_t at, size_t max) const {
        std::string s;
        for (size_t i = 0; i < max && has(at + i, 1) && p[at + i]; ++i) s.push_back(char(p[at + i]));
        return s;
    }
    Bytes sub(size_t at, size_t count) const { return has(at, count) ? Bytes{p + at, count} : Bytes{}; }
};
// The names in the data are hashes: h = h * 31 + upper-case letter.
inline uint32_t nameHash(std::string_view s) {
    uint32_t h = 0;
    for (char c : s) h = h * 31 + uint32_t(uint8_t(c >= 'a' && c <= 'z' ? c - 32 : c));
    return h;
}
// The chunks of a container.
template <class F>
void chunks(Bytes b, F each) {
    for (size_t at = 0; b.has(at, 8);) {
        const uint32_t size = b.le32(at + 4);
        if (size < 4 || !b.has(at + 4, size)) break;
        char tag[5] = {char(b.p[at]), char(b.p[at + 1]), char(b.p[at + 2]), char(b.p[at + 3]), 0};
        each(std::string_view(tag, 4), b.sub(at + 8, size - 4));
        at += 4 + size_t(size);
    }
}

// ---------------------------------------------------------------- binary XML ("XMLB"): the scripts
// Header: "XMLB", version, "PS3 ", size, the root node's offset at 0x14. A node: name hash, text offset, parent,
// child count, attribute count, child table, attribute table (hash + string offset each). All big-endian.
struct Xml {
    Bytes b;
    explicit Xml(Bytes bytes) : b(bytes) {}
    bool ok() const { return b.n > 0x18 && std::memcmp(b.p, "XMLB", 4) == 0; }
    size_t root() const { return b.be32(0x14); }
    uint32_t name(size_t node) const { return b.be32(node); }
    std::string attribute(size_t node, std::string_view key) const {
        const uint32_t want = nameHash(key);
        const size_t count = b.be16(node + 14), table = b.be32(node + 20);
        for (size_t i = 0; i < count; ++i)
            if (b.be32(table + 8 * i) == want) return b.text(b.be32(table + 8 * i + 4), 4096);
        return {};
    }
    template <class F>
    void children(size_t node, F each) const {
        const size_t count = b.be16(node + 12), table = b.be32(node + 16);
        for (size_t i = 0; i < count && i < 4096; ++i) each(size_t(b.be32(table + 4 * i)));
    }
    // The first child with this element name and (when given) this value of its "name" attribute; 0 when none.
    size_t child(size_t node, std::string_view element, std::string_view named = {}) const {
        size_t found = 0;
        const uint32_t want = nameHash(element);
        children(node, [&](size_t c) {
            if (!found && name(c) == want && (named.empty() || attribute(c, "name") == named)) found = c;
        });
        return found;
    }
};

// ---------------------------------------------------------------- textures: GTF, DXT1 / DXT3 / DXT5
// One 4x4 block row by row into out (stride in pixels). kind: 1, 3 or 5.
inline void decodeDxtBlock(const uint8_t* s, int kind, uint8_t* out, int stride, int bw, int bh) {
    const uint8_t* c = kind == 1 ? s : s + 8;
    const uint32_t c0 = uint32_t(c[0] | c[1] << 8), c1 = uint32_t(c[2] | c[3] << 8);
    uint8_t pal[4][4];
    auto rgb = [](uint32_t v, uint8_t* o) {
        const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        o[0] = uint8_t(r << 3 | r >> 2), o[1] = uint8_t(g << 2 | g >> 4), o[2] = uint8_t(b << 3 | b >> 2), o[3] = 255;
    };
    rgb(c0, pal[0]), rgb(c1, pal[1]);
    const bool four = kind != 1 || c0 > c1;
    for (int k = 0; k < 3; ++k) {
        pal[2][k] = uint8_t(four ? (2 * pal[0][k] + pal[1][k] + 1) / 3 : (pal[0][k] + pal[1][k]) / 2);
        pal[3][k] = uint8_t(four ? (pal[0][k] + 2 * pal[1][k] + 1) / 3 : 0);
    }
    pal[2][3] = 255, pal[3][3] = four ? 255 : 0;
    uint8_t alpha[16];
    if (kind == 5) {
        uint8_t a[8] = {s[0], s[1]};
        for (int i = 2; i < 8; ++i)
            a[i] = a[0] > a[1] ? uint8_t(((8 - i) * a[0] + (i - 1) * a[1]) / 7) : i < 6 ? uint8_t(((6 - i) * a[0] + (i - 1) * a[1]) / 5) : i == 6 ? 0 : 255;
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(s[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) alpha[i] = a[(bits >> (3 * i)) & 7];
    } else if (kind == 3) {
        for (int i = 0; i < 16; ++i) alpha[i] = uint8_t(((s[i / 2] >> (4 * (i & 1))) & 15) * 17);
    }
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const int i = (c[4 + y] >> (2 * x)) & 3;
            uint8_t* o = out + (size_t(y) * size_t(stride) + size_t(x)) * 4;
            o[0] = pal[i][0], o[1] = pal[i][1], o[2] = pal[i][2];
            o[3] = kind == 1 ? pal[i][3] : alpha[y * 4 + x];
        }
}
inline Image decodeDxt(Bytes data, int width, int height, int kind) {
    Image img;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return img;
    const size_t block = kind == 1 ? 8 : 16, bx = size_t(width + 3) / 4, by = size_t(height + 3) / 4;
    if (!data.has(0, bx * by * block)) return img;
    img.width = width, img.height = height;
    img.rgba.resize(size_t(width) * size_t(height) * 4);
    for (size_t y = 0; y < by; ++y)
        for (size_t x = 0; x < bx; ++x)
            decodeDxtBlock(data.p + (y * bx + x) * block, kind, &img.rgba[(y * 4 * size_t(width) + x * 4) * 4], width,
                           int(std::min<size_t>(4, size_t(width) - x * 4)), int(std::min<size_t>(4, size_t(height) - y * 4)));
    return img;
}
// A GTF texture (the PS3's container): version, size, texture count, then per texture id, data offset, data size,
// format, mip count, dimension, cube map, remap, width, height, depth. The first mip level of the first texture.
inline Image decodeGtf(Bytes b) {
    const uint32_t offset = b.be32(16);
    const int format = b.u8(24) & 0x9F, width = b.be16(32), height = b.be16(34);
    const Bytes data = b.sub(offset, b.n > offset ? b.n - offset : 0);
    if (format == 0x86) return decodeDxt(data, width, height, 1);
    if (format == 0x87) return decodeDxt(data, width, height, 3);
    if (format == 0x88) return decodeDxt(data, width, height, 5);
    Image img;
    if (format == 0x85 && data.has(0, size_t(width) * size_t(height) * 4)) {  // A8R8G8B8
        img.width = width, img.height = height;
        img.rgba.resize(size_t(width) * size_t(height) * 4);
        for (size_t i = 0; i < img.rgba.size(); i += 4)
            img.rgba[i] = data.p[i + 1], img.rgba[i + 1] = data.p[i + 2], img.rgba[i + 2] = data.p[i + 3], img.rgba[i + 3] = data.p[i];
    }
    return img;
}

// ---------------------------------------------------------------- the scene
struct UvAnimation {  // MTLC: one texture-coordinate channel of a material
    int channel = 0;  // 0: U, 1: V
    int mode = 0;     // 0: runs and wraps, 1: sways (a sine)
    float rate = 0, frames = 0, range = 1;
};
struct Material {
    std::string name;
    uint32_t texture = 0;
    bool particle = false;  // the "ParticleMaterial" shader (else "Standard")
    std::vector<UvAnimation> uv;
};
struct Mesh {
    uint32_t material = 0;
    std::vector<float> vertices;  // x y z u v r g b a per vertex
    std::vector<uint16_t> indices;
};
struct Sway {  // TranslationController / RotationController, per axis
    float amplitude[3] = {}, period[3] = {};
    bool pingPong[3] = {}, randomStart[3] = {};
    bool any() const { return amplitude[0] != 0 || amplitude[1] != 0 || amplitude[2] != 0; }
};
struct Node {
    std::string name;
    int parent = -1;
    float position[3] = {}, scale[3] = {1, 1, 1}, xAxis[3] = {1, 0, 0}, zAxis[3] = {0, 0, 1};
    uint32_t geometry = 0;
    bool emitter = false;
    Sway move, turn;
};
struct Camera {
    std::string name;
    float position[3] = {}, fov = 45;
};
struct Emitter {
    std::string name;
    uint32_t mesh = 0;
    float life = 0.5f, speed = 0.25f, rate = 25, sizeFrom = 1, sizeTo = 1, spin = 0;
    float gravity[3] = {}, spread[3] = {};
    uint8_t colour[4] = {255, 255, 255, 255};
};
struct Scene {
    std::map<uint32_t, std::string> textureNames;
    std::map<uint32_t, Image> textures;
    std::map<uint32_t, Material> materials;
    std::map<uint32_t, std::vector<Mesh>> meshes;  // by mesh hash: its sub-meshes
    std::map<uint32_t, uint32_t> geometry;         // geometry hash -> mesh hash
    std::vector<Node> nodes;
    std::vector<Camera> cameras;
    std::map<uint32_t, Emitter> emitters;  // by the emitter definition's hash
};
namespace detail {
// A named value block: group count, { group hash, offset }, then per group a count and { name hash, value offset, 0 }.
inline void readSway(Bytes b, Node& node) {
    const uint32_t groups = b.be32(0);
    for (uint32_t g = 0; g < groups && g < 16; ++g) {
        const uint32_t group = b.be32(4 + 8 * g), at = b.be32(8 + 8 * g);
        Sway* sway = group == nameHash("TranslationController") ? &node.move : group == nameHash("RotationController") ? &node.turn : nullptr;
        if (!sway) continue;
        const bool turn = sway == &node.turn;
        const uint32_t count = b.be32(at);
        for (uint32_t i = 0; i < count && i < 64; ++i) {
            const uint32_t key = b.be32(at + 4 + 12 * i), value = b.be32(at + 8 + 12 * i);
            static const char* axes[3] = {"x", "y", "z"};
            for (int a = 0; a < 3; ++a) {
                const std::string base = std::string(axes[a]) + (turn ? "Angle" : "Pos");
                if (key == nameHash(base + "Amplitude")) sway->amplitude[a] = b.bef(value);
                else if (key == nameHash(base + "Period")) sway->period[a] = b.bef(value);
                else if (key == nameHash(base + "PingPong")) sway->pingPong[a] = b.u8(value) != 0;
                else if (key == nameHash(turn ? std::string(axes[a]) + "RandomStart" : base + "RandomStart")) sway->randomStart[a] = b.u8(value) != 0;
            }
        }
        // (the files hold 360 as "no movement" on the axes the artist left alone)
        for (int a = 0; a < 3; ++a)
            if (sway->period[a] <= 0) sway->amplitude[a] = 0;
    }
}
inline void readInstance(Bytes b, int parent, Scene& scene) {
    const int index = int(scene.nodes.size());
    scene.nodes.emplace_back();
    scene.nodes[size_t(index)].parent = parent;
    chunks(b, [&](std::string_view tag, Bytes c) {
        if (tag == "GINH" || tag == "EINH") {
            Node& n = scene.nodes[size_t(index)];
            for (int i = 0; i < 3; ++i) {
                n.xAxis[i] = c.lef(2 + 4 * size_t(i)), n.zAxis[i] = c.lef(14 + 4 * size_t(i));
                n.position[i] = c.lef(26 + 4 * size_t(i)), n.scale[i] = c.lef(38 + 4 * size_t(i));
            }
            n.geometry = c.le32(54);
            n.name = c.text(62, 32);
            n.emitter = tag == "EINH";
        } else if (tag == "ATAG") {
            readSway(c, scene.nodes[size_t(index)]);
        } else if (tag == "GINS" || tag == "EINS") {
            readInstance(c, index, scene);
        }
    });
}
}  // namespace detail
// The scene of a .san file. Throws with a sentence for the player when it is not one.
inline Scene readScene(Bytes file) {
    if (file.n < 12 || std::memcmp(file.p, "FORM", 4) != 0) throw std::runtime_error(trf("The Sly Collection's menu could not be read (%s).", "Frontend_Art.san"));
    Scene scene;
    chunks(file.sub(8, std::min<size_t>(file.n - 8, size_t(file.le32(4)) > 4 ? size_t(file.le32(4)) - 4 : 0)), [&](std::string_view tag, Bytes b) {
        if (tag == "TEXR") {
            uint32_t hash = 0;
            chunks(b, [&](std::string_view t, Bytes c) {
                if (t == "TXRH") {
                    hash = c.le32(1);
                    std::string name = c.text(12, 32);
                    if (auto dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);
                    scene.textureNames[hash] = name;
                } else if (t == "TGTF") {
                    scene.textures[hash] = decodeGtf(c);
                }
            });
        } else if (tag == "MATL") {
            Material m;
            uint32_t hash = 0;
            chunks(b, [&](std::string_view t, Bytes c) {
                if (t == "MTLH") {
                    m.texture = c.le32(1), hash = c.le32(0x15), m.name = c.text(0x19, 32);
                    m.particle = c.le32(0x4D) == nameHash("ParticleMaterial");
                } else if (t == "MTLC") {
                    UvAnimation a;
                    a.rate = c.lef(1), a.frames = c.lef(5), a.range = c.lef(9), a.mode = c.u8(33), a.channel = c.u8(34);
                    if (a.rate != 0 && a.channel < 2) m.uv.push_back(a);
                }
            });
            scene.materials[hash] = std::move(m);
        } else if (tag == "MESH") {
            uint32_t hash = 0;
            std::vector<Mesh> parts;
            chunks(b, [&](std::string_view t, Bytes c) {
                if (t == "MSHH") hash = c.le32(17);
                else if (t == "SMSH") {
                    Mesh mesh;
                    size_t count = 0;
                    chunks(c, [&](std::string_view s, Bytes d) {
                        if (s == "MHDR") count = d.le16(1), mesh.material = d.le32(6);
                        else if (s == "MVTX" && count && d.n >= count * 14) {
                            const size_t stride = d.n / count;  // (32: position, colour, texture coordinates, normal, tangent)
                            for (size_t i = 0; i < count; ++i) {
                                const size_t at = i * stride;
                                for (size_t k = 0; k < 3; ++k) mesh.vertices.push_back(d.half(at + 2 * k));
                                mesh.vertices.push_back(d.half(at + 10)), mesh.vertices.push_back(d.half(at + 12));
                                for (size_t k = 0; k < 4; ++k) mesh.vertices.push_back(float(d.u8(at + 6 + k)) / 255.0f);
                            }
                        } else if (s == "MIDX") {
                            for (size_t i = 0; i + 1 < d.n; i += 2) mesh.indices.push_back(d.be16(i));
                        }
                    });
                    while (mesh.indices.size() % 3) mesh.indices.pop_back();
                    for (uint16_t i : mesh.indices)
                        if (size_t(i) * 9 >= mesh.vertices.size()) mesh.indices.clear();
                    if (!mesh.indices.empty()) parts.push_back(std::move(mesh));
                }
            });
            scene.meshes[hash] = std::move(parts);
        } else if (tag == "GEOB") {
            uint32_t hash = 0;
            bool first = true;
            chunks(b, [&](std::string_view t, Bytes c) {
                if (t == "GEOH") hash = c.le32(25);
                else if (t == "GLOD" && first) scene.geometry[hash] = c.le32(1), first = false;
            });
        } else if (tag == "GINS" || tag == "EINS") {
            detail::readInstance(b, -1, scene);
        } else if (tag == "CAMC") {
            chunks(b, [&](std::string_view t, Bytes c) {
                if (t != "CAMH") return;
                Camera cam;
                for (size_t i = 0; i < 3; ++i) cam.position[i] = c.lef(25 + 4 * i);
                cam.fov = c.lef(37);
                cam.name = c.text(49, 32);
                scene.cameras.push_back(std::move(cam));
            });
        } else if (tag == "EMIT") {
            Emitter e;
            uint32_t hash = 0;
            chunks(b, [&](std::string_view t, Bytes c) {
                if (t == "EMIH") hash = c.le32(25), e.name = c.text(37, 32);
                else if (t == "EMPC")
                    chunks(c, [&](std::string_view s, Bytes d) {
                        // (what is understood of the emitter: see the notes; the rest is drawn by eye)
                        if (s == "EMPH") e.life = d.lef(1), e.speed = d.lef(5), e.rate = d.lef(9), e.mesh = d.le32(13);
                        else if (s == "EPIB")
                            for (size_t i = 0; i < 3; ++i) e.spread[i] = d.lef(27 + 4 * i);
                        else if (s == "EPMN")
                            for (size_t i = 0; i < 3; ++i) e.gravity[i] = d.lef(4 * i);
                        else if (s == "EPTC")
                            for (size_t i = 0; i < 4; ++i) e.colour[i] = d.u8(8 + i);
                        else if (s == "EPTS") e.sizeFrom = d.lef(5), e.sizeTo = d.lef(d.n >= 20 ? 13 : 9);
                        else if (s == "EPTR") e.spin = d.lef(17);
                    });
            });
            scene.emitters[hash] = e;
        }
    });
    if (scene.nodes.empty() || scene.cameras.empty() || scene.textures.empty())
        throw std::runtime_error(trf("The Sly Collection's menu could not be read (%s).", "Frontend_Art.san"));
    return scene;
}

// ---------------------------------------------------------------- the font's glyph map
// The .bmp beside a font page: a green box around every glyph in reading order, a blue line under every row (the
// baseline); a glyph without ink (the space) is a green stroke on the baseline. Returns x, y, w, h, baseline per glyph.
inline std::vector<std::array<int, 5>> readGlyphMap(const Image& map) {
    std::vector<std::array<int, 5>> glyphs;
    if (map.empty()) return glyphs;
    const int W = map.width, H = map.height;
    auto px = [&](int x, int y) { return &map.rgba[(size_t(y) * size_t(W) + size_t(x)) * 4]; };
    auto green = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && px(x, y)[1] > 128 && px(x, y)[0] < 128 && px(x, y)[2] < 128; };
    auto blue = [&](int x, int y) { return px(x, y)[2] > 128 && px(x, y)[1] < 128 && px(x, y)[0] < 128; };
    std::vector<int> baselines;
    for (int y = 0; y < H; ++y) {
        int count = 0;
        for (int x = 0; x < W; ++x) count += blue(x, y);
        if (count > W / 8 && (baselines.empty() || baselines.back() != y - 1)) baselines.push_back(y);
    }
    struct Box {
        int x, y, w, h;
    };
    std::vector<Box> boxes;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            // a box's top-left corner: green here, to the right and below, not to the left or above
            if (!green(x, y) || green(x - 1, y) || green(x, y - 1) || !green(x + 1, y)) continue;
            int x1 = x;
            while (green(x1 + 1, y)) ++x1;
            int y1 = y;
            while (green(x, y1 + 1) && green(x1, y1 + 1)) ++y1;
            boxes.push_back({x, y, x1 - x + 1, y1 - y + 1});
        }
    // (a box's bottom edge and the strokes inside taller boxes are found again as corners: keep the outer ones)
    std::vector<Box> outer;
    for (const Box& b : boxes) {
        bool inside = false;
        for (const Box& o : boxes)
            if (&o != &b && o.h > 1 && b.x >= o.x && b.x + b.w <= o.x + o.w && b.y > o.y && b.y + b.h <= o.y + o.h) inside = true;
        if (!inside) outer.push_back(b);
    }
    for (size_t row = 0; row < baselines.size(); ++row) {
        const int base = baselines[row], top = row ? baselines[row - 1] : -1;
        std::vector<Box> line;
        for (const Box& b : outer) {
            const int middle = b.y + b.h / 2;
            // a glyph belongs to the first baseline at or below its middle (descenders reach under it)
            if (middle > top && middle <= base) line.push_back(b);
            else if (b.h <= 2 && b.y > top && b.y <= base + 1) line.push_back(b);
        }
        std::sort(line.begin(), line.end(), [](const Box& a, const Box& b) { return a.x < b.x; });
        for (const Box& b : line) glyphs.push_back({b.x, b.y, b.w, b.h, base});
    }
    return glyphs;
}

// ---------------------------------------------------------------- the converted menu in the profile
inline fs::path menuDir(const fs::path& profile) { return profile / "Collection" / "Menu"; }
// (the first Collection Style Launcher kept raw copies of the disc's files here: removed by the next import)
inline fs::path legacyDir(const fs::path& profile) { return profile / "Collection" / "PS3 menu"; }
// The imported menu's description when it is present and whole (every file it names exists); null otherwise.
inline json readMenu(const fs::path& profile) {
    std::error_code ec;
    const fs::path dir = menuDir(profile);
    if (!fs::is_regular_file(dir / "menu.json", ec)) return nullptr;
    try {
        json m = readJson(dir / "menu.json");
        if (!m.is_object() || m.value("version", 0) != kMenuVersion || !m.contains("nodes") || !m.contains("cameras") || !m.contains("files")) return nullptr;
        for (const auto& f : m.at("files"))
            if (!fs::is_regular_file(dir / path(f.get<std::string>()), ec)) return nullptr;
        return m;
    } catch (...) {
        return nullptr;
    }
}
inline bool imported(const fs::path& profile) { return !readMenu(profile).is_null(); }
inline void removeMenu(const fs::path& profile) {
    std::error_code ec;
    fs::remove_all(menuDir(profile), ec);
    fs::remove_all(legacyDir(profile), ec);
    fs::remove(profile / "Collection", ec);  // (when empty)
}

namespace detail {
inline void savePng(const fs::path& file, const Image& img) {
    if (img.empty()) throw std::runtime_error(trf("The Sly Collection's menu could not be read (%s).", "art"));
    stbi_write_png_compression_level = 4;
    std::ofstream out(file, std::ios::binary);
    const int ok = stbi_write_png_to_func([](void* context, void* data, int size) { static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data), size); },
                                          &out, img.width, img.height, 4, img.rgba.data(), img.width * 4);
    if (!ok || !out) throw std::runtime_error(tr("The Sly Collection's menu could not be saved."));
}
inline void saveWav(const fs::path& file, const Pcm& pcm) {
    if (pcm.samples.empty()) throw std::runtime_error(trf("The Sly Collection's menu could not be read (%s).", "sound"));
    std::ofstream out(file, std::ios::binary);
    const uint32_t bytes = uint32_t(pcm.samples.size() * 2);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4), u32(36 + bytes), out.write("WAVEfmt ", 8), u32(16), u16(1), u16(2), u32(uint32_t(pcm.rate));
    u32(uint32_t(pcm.rate) * 4), u16(4), u16(16), out.write("data", 4), u32(bytes);
    out.write(reinterpret_cast<const char*>(pcm.samples.data()), bytes);  // (every target is little-endian)
    if (!out) throw std::runtime_error(tr("The Sly Collection's menu could not be saved."));
}
inline Image crop(const Image& img, int x0, int y0, int x1, int y1) {
    Image out;
    out.width = x1 - x0, out.height = y1 - y0;
    out.rgba.resize(size_t(out.width) * size_t(out.height) * 4);
    for (int y = 0; y < out.height; ++y)
        std::memcpy(&out.rgba[size_t(y) * size_t(out.width) * 4], &img.rgba[(size_t(y + y0) * size_t(img.width) + size_t(x0)) * 4], size_t(out.width) * 4);
    return out;
}
inline std::string fileName(std::string name) {
    for (char& c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) c = '_';
    return name;
}
}  // namespace detail

// What is read from the disc, relative to USRDIR.
inline const std::vector<std::string>& discFiles() {
    static const std::vector<std::string> files{"IMAGE.ZIP", "AUDIO/SLYAUDIO_MUSIC.FSB", "AUDIO/KRAZEAUDIO_GAME.FSB"};
    return files;
}
// Reads the menu from a Sly Collection source and converts it into the profile. Throws with a sentence for the
// player; the profile's menu is replaced only by a whole new one.
inline void importMenu(const fs::path& source, const fs::path& profile, const std::atomic<bool>& cancel, const std::function<void(float)>& progress) {
    auto step = [&](float p) {
        if (cancel) throw std::runtime_error(tr("Cancelled."));
        if (progress) progress(p);
    };
    ps3::Disc disc;
    std::string error;
    if (!disc.open(source, error)) throw std::runtime_error(error);
    for (const auto& f : discFiles())
        if (!disc.find(f)) throw std::runtime_error(tr("This is not The Sly Collection (PS3): its menu files (IMAGE.ZIP) are missing."));
    std::vector<std::vector<uint8_t>> raw(discFiles().size());
    const bool ok = disc.read(
        discFiles(),
        [&](size_t i, uint64_t, const uint8_t* p, size_t n) {
            raw[i].insert(raw[i].end(), p, p + n);
            return !cancel.load();
        },
        [&](uint64_t done, uint64_t total) {
            if (progress && total) progress(0.3f * float(double(done) / double(total)));
            return !cancel.load();
        },
        error);
    if (!ok || cancel) throw std::runtime_error(cancel ? tr("Cancelled.") : error);
    step(0.3f);

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, raw[0].data(), raw[0].size(), 0)) throw std::runtime_error(trf("The Sly Collection's menu could not be read (%s).", "IMAGE.ZIP"));
    struct CloseZip {
        mz_zip_archive* z;
        ~CloseZip() { mz_zip_reader_end(z); }
    } closeZip{&zip};
    auto entry = [&](const char* name, bool required = true) {
        std::vector<uint8_t> bytes;
        const int i = mz_zip_reader_locate_file(&zip, name, nullptr, 0);
        size_t n = 0;
        if (void* p = i < 0 ? nullptr : mz_zip_reader_extract_to_heap(&zip, mz_uint(i), &n, 0)) {
            bytes.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + n);
            mz_free(p);
        }
        if (bytes.empty() && required) throw std::runtime_error(trf("The Sly Collection's menu could not be read (%s).", name));
        return bytes;
    };
    auto picture = [&](const char* name, bool required = true) {
        const auto bytes = entry(name, required);
        Image img = bytes.empty() ? Image{} : decodeImage(bytes.data(), bytes.size());
        if (img.empty() && required) throw std::runtime_error(trf("The Sly Collection's menu could not be read (%s).", name));
        return img;
    };

    const auto sceneBytes = entry("Levels/Frontend/Frontend_Art.san");
    Scene scene = readScene({sceneBytes.data(), sceneBytes.size()});
    step(0.4f);

    const fs::path dir = menuDir(profile), staging = dir.parent_path() / "Menu.staging";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "tex");
    fs::create_directories(staging / "gui");
    fs::create_directories(staging / "audio");
    struct Cleanup {
        fs::path dir;
        bool keep = false;
        ~Cleanup() {
            std::error_code e;
            if (!keep) fs::remove_all(dir, e);
        }
    } cleanup{staging};
    json files = json::array();
    auto addPng = [&](const std::string& rel, const Image& img) {
        detail::savePng(staging / path(rel), img);
        files.push_back(rel);
    };

    // ---- the scene: nodes with their meshes. A big sheet used a piece at a time (the logos: four regions on one
    // 2048x2048 sheet) is cut into its pieces, so the menu loads only the ones it shows.
    json textures = json::object(), nodes = json::array();
    std::map<std::string, bool> saved;
    auto useTexture = [&](uint32_t hash, const Image& img, const std::string& name) {
        if (!saved[name]) {
            addPng("tex/" + detail::fileName(name) + ".png", img);
            textures[name] = {{"file", "tex/" + detail::fileName(name) + ".png"}, {"w", img.width}, {"h", img.height}};
            saved[name] = true;
        }
        (void)hash;
    };
    size_t done = 0;
    for (const Node& n : scene.nodes) {
        json node{{"name", n.name}, {"parent", n.parent}, {"pos", {n.position[0], n.position[1], n.position[2]}},
                  {"x", {n.xAxis[0], n.xAxis[1], n.xAxis[2]}}, {"z", {n.zAxis[0], n.zAxis[1], n.zAxis[2]}},
                  {"scale", {n.scale[0], n.scale[1], n.scale[2]}}, {"meshes", json::array()}};
        auto sway = [](const Sway& s) {
            json j = json::array();
            for (int a = 0; a < 3; ++a) j.push_back({s.amplitude[a], s.period[a], s.pingPong[a] ? 1 : 0, s.randomStart[a] ? 1 : 0});
            return j;
        };
        if (n.move.any()) node["move"] = sway(n.move);
        if (n.turn.any()) node["turn"] = sway(n.turn);
        if (n.emitter) {
            auto e = scene.emitters.find(n.geometry);
            if (e != scene.emitters.end()) {
                const Emitter& em = e->second;
                json j{{"life", em.life}, {"speed", em.speed}, {"rate", em.rate}, {"size", {em.sizeFrom, em.sizeTo}}, {"spin", em.spin},
                       {"gravity", {em.gravity[0], em.gravity[1], em.gravity[2]}}, {"spread", {em.spread[0], em.spread[1], em.spread[2]}},
                       {"colour", {em.colour[0], em.colour[1], em.colour[2], em.colour[3]}}};
                // the puff: the first sub-mesh of the emitter's mesh
                auto m = scene.meshes.find(em.mesh);
                if (m != scene.meshes.end() && !m->second.empty()) {
                    const Mesh& mesh = m->second.front();
                    auto mat = scene.materials.find(mesh.material);
                    if (mat != scene.materials.end() && scene.textures.count(mat->second.texture)) {
                        const std::string name = scene.textureNames[mat->second.texture];
                        useTexture(mat->second.texture, scene.textures[mat->second.texture], name);
                        float half = 0;
                        for (size_t v = 0; v + 8 < mesh.vertices.size(); v += 9) half = std::max(half, std::fabs(mesh.vertices[v]));
                        j["tex"] = name, j["half"] = half;
                        node["emitter"] = j;
                    }
                }
            }
        } else if (auto g = scene.geometry.find(n.geometry); g != scene.geometry.end()) {
            auto m = scene.meshes.find(g->second);
            if (m != scene.meshes.end())
                for (const Mesh& mesh : m->second) {
                    auto mat = scene.materials.find(mesh.material);
                    if (mat == scene.materials.end() || !scene.textures.count(mat->second.texture)) continue;
                    const Image& sheet = scene.textures[mat->second.texture];
                    if (sheet.empty()) continue;
                    std::string name = scene.textureNames[mat->second.texture];
                    std::vector<float> v = mesh.vertices;
                    float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
                    for (size_t i = 0; i + 8 < v.size(); i += 9) u0 = std::min(u0, v[i + 3]), u1 = std::max(u1, v[i + 3]), v0 = std::min(v0, v[i + 4]), v1 = std::max(v1, v[i + 4]);
                    const bool piece = mat->second.uv.empty() && sheet.width >= 1024 && sheet.height >= 1024 && u0 >= 0 && v0 >= 0 && u1 <= 1 && v1 <= 1 &&
                                       (u1 - u0) * (v1 - v0) <= 0.6f && u1 > u0 && v1 > v0;
                    if (piece) {
                        const int x0 = int(std::floor(u0 * float(sheet.width))), y0 = int(std::floor(v0 * float(sheet.height)));
                        const int x1 = int(std::ceil(u1 * float(sheet.width))), y1 = int(std::ceil(v1 * float(sheet.height)));
                        name += "_" + std::to_string(x0) + "_" + std::to_string(y0) + "_" + std::to_string(x1 - x0) + "x" + std::to_string(y1 - y0);
                        if (!saved[name]) useTexture(0, detail::crop(sheet, x0, y0, x1, y1), name);
                        for (size_t i = 0; i + 8 < v.size(); i += 9) v[i + 3] = (v[i + 3] - u0) / (u1 - u0), v[i + 4] = (v[i + 4] - v0) / (v1 - v0);
                    } else {
                        useTexture(mat->second.texture, sheet, name);
                    }
                    json uv = json::array();
                    for (const auto& a : mat->second.uv) uv.push_back({a.channel, a.mode, a.rate, a.frames, a.range});
                    node["meshes"].push_back({{"tex", name}, {"material", mat->second.name}, {"particle", mat->second.particle}, {"v", v}, {"i", mesh.indices}, {"uv", uv}});
                }
        }
        nodes.push_back(std::move(node));
        step(0.4f + 0.35f * float(++done) / float(scene.nodes.size()));
    }
    json cameras = json::object();
    for (const Camera& c : scene.cameras) cameras[c.name] = {{"pos", {c.position[0], c.position[1], c.position[2]}}, {"fov", c.fov}};
    for (const char* needed : {"Camera_Title", "Camera_Sly1", "Camera_Sly2", "Camera_Sly3"})
        if (!cameras.contains(needed)) throw std::runtime_error(trf("The Sly Collection's menu could not be read (%s).", "Frontend_Art.san"));

    // ---- the level's settings (bloom) and the menu's tuning
    json bloom{{"enable", false}, {"intensity", 4.0}, {"threshold", 0.25}}, tuning{{"cameraGlide", 0.1}, {"inputDelay", 0.5}};
    if (const auto bytes = entry("Scripts/Levels.xmb", false); !bytes.empty()) {
        const Xml xml({bytes.data(), bytes.size()});
        if (const size_t level = xml.ok() ? xml.child(xml.root(), "Level", "Frontend") : 0)
            if (const size_t b = xml.child(level, "Bloom")) {
                bloom["enable"] = xml.attribute(b, "enable") == "1";
                bloom["intensity"] = std::atof(xml.attribute(b, "Intensity").c_str());
                int t = 64;
                std::sscanf(xml.attribute(b, "threshold").c_str(), "{%d", &t);
                bloom["threshold"] = double(t) / 255.0;
            }
    }
    if (const auto bytes = entry("Scripts/Tuning.xmb", false); !bytes.empty()) {
        const Xml xml({bytes.data(), bytes.size()});
        if (xml.ok()) {
            if (const size_t v = xml.child(xml.root(), "var", "Game.UI.PageCamInterpFactor")) tuning["cameraGlide"] = std::atof(xml.attribute(v, "value").c_str());
            if (const size_t v = xml.child(xml.root(), "var", "Game.UI.FrontEndInputDelayTime")) tuning["inputDelay"] = std::atof(xml.attribute(v, "value").c_str());
        }
    }
    step(0.78f);

    // ---- the comic font (pages by their first character) and the GUI pieces
    json font = json::array();
    static const struct {
        const char* base;
        int first;
    } pages[] = {{"Gui/Font_MCGames_US_01", 0x20}, {"Gui/Font_MCGames_EU_01", 0xA0}, {"Gui/Font_MCGames_EU_02", 0x11D}};
    for (size_t i = 0; i < std::size(pages); ++i) {
        const Image art = picture((std::string(pages[i].base) + ".tga").c_str(), i == 0), map = picture((std::string(pages[i].base) + ".bmp").c_str(), i == 0);
        if (art.empty() || map.empty()) continue;
        const auto glyphs = readGlyphMap(map);
        if (glyphs.empty()) {
            if (i == 0) throw std::runtime_error(trf("The Sly Collection's menu could not be read (%s).", "Font_MCGames_US_01"));
            continue;
        }
        const std::string rel = "gui/font" + std::to_string(i) + ".png";
        addPng(rel, art);
        // (the map may be smaller than the art: the boxes scale with it)
        font.push_back({{"file", rel}, {"first", pages[i].first}, {"w", map.width}, {"h", map.height}, {"glyphs", glyphs}});
    }
    json gui = json::object();
    auto guiPiece = [&](const char* key, const char* name, bool required) {
        const Image img = picture((std::string("Gui/") + name + ".tga").c_str(), required);
        if (img.empty()) return;
        const std::string rel = std::string("gui/") + name + ".png";
        addPng(rel, img);
        gui[key].push_back(rel);
    };
    for (const char* n : {"Banner_Left", "Banner_Center", "Banner_Right"}) guiPiece("banner", n, true);
    for (const char* n : {"Border_NILeftTop", "Border_NICenterTop", "Border_NIRightTop", "Border_NILeft", "Border_NICenter", "Border_NIRight", "Border_NILeftBottom",
                          "Border_NICenterBottom", "Border_NIRightBottom"})
        guiPiece("frame", n, true);
    for (const char* n : {"Border_LeftTop", "Border_CenterTop", "Border_RightTop", "Border_Left", "Border_Center", "Border_Right", "Border_LeftBottom",
                          "Border_CenterBottom", "Border_RightBottom"})
        guiPiece("border", n, true);
    guiPiece("loading", "Screen_LoadingWide", false);
    step(0.86f);

    // ---- the music and the interface sounds
    json audio{{"music", json::array()}};
    auto sample = [&](const std::vector<uint8_t>& bank, const char* name, const std::string& rel, bool loops, bool required) {
        for (const auto& s : readFsb(bank))
            if (s.name == name) {
                Pcm pcm = decodeFsbSample(bank, s);
                if (pcm.samples.empty()) break;
                detail::saveWav(staging / path(rel), pcm);
                files.push_back(rel);
                (void)loops;
                return true;
            }
        if (required) throw std::runtime_error(trf("The Sly Collection's menu could not be read (%s).", name));
        return false;
    };
    if (sample(raw[1], "FrontEnd_Loop", "audio/music0.wav", true, true)) audio["music"].push_back("audio/music0.wav");
    step(0.93f);
    if (sample(raw[2], "Beep_18", "audio/move.wav", false, true)) audio["move"] = "audio/move.wav";
    if (sample(raw[2], "Positive_Beep_16", "audio/select.wav", false, true)) audio["select"] = "audio/select.wav";
    if (sample(raw[2], "Negative_Beep_4", "audio/cancel.wav", false, true)) audio["cancel"] = "audio/cancel.wav";
    step(0.98f);

    writeJson(staging / "menu.json", json{{"version", kMenuVersion}, {"source", utf8(source)}, {"textures", textures}, {"nodes", nodes}, {"cameras", cameras},
                                          {"bloom", bloom}, {"tuning", tuning}, {"font", font}, {"gui", gui}, {"audio", audio}, {"files", files}});
    fs::remove_all(dir, ec);
    fs::rename(staging, dir, ec);
    if (ec) throw std::runtime_error(tr("The Sly Collection's menu could not be saved."));
    cleanup.keep = true;
    fs::remove_all(legacyDir(profile), ec);
    step(1.0f);
}
}  // namespace ps3menu
}  // namespace cane::collection
