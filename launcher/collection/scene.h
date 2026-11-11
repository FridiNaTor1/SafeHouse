#pragma once
// The Sly Collection's menu scene as the Collection Style Launcher shows it: the converted menu (ps3menu.h: menu.json,
// tex/*.png) drawn through Dear ImGui's draw lists, so it runs wherever the launcher's own screens do.
//
// The scene is flat: textured quads at small depths (z 0 .. 0.47) in front of a camera at z 2.44 that looks down -Z
// with a 45 degree vertical field of view and glides along X from page to page. Everything is drawn back to front.
//   - the three scrolling strips per half (stars / map / silhouettes, Paris at dusk): 27 units wide, their texture
//     repeats and scrolls by itself; they are cut at the texture's seams here (no repeat sampling needed) and carried
//     on past their ends, so any window width is filled;
//   - logos, arrows, icons: quads with a half-transparent black copy behind as their shadow; some sway;
//   - the van and Sly ride with the camera; their emitters puff exhaust and dust.
// A window narrower than 4:3 widens the view instead of cutting the page; the sky and the ground are carried on
// above and below the strips.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <glad/glad.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "ps3menu.h"

namespace cane::collection {
// ---------------------------------------------------------------- GL textures
struct Tex {
    GLuint id = 0;
    int w = 0, h = 0;
    Tex() = default;
    explicit Tex(const Image& source) {
        if (source.empty()) return;
        const Image* img = &source;
        Image half;
        GLint limit = 2048;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
        // (a strip is 4048 wide: halved on a device that cannot hold it)
        while (img->width > limit || img->height > limit) {
            Image next;
            next.width = std::max(1, img->width / 2), next.height = std::max(1, img->height / 2);
            next.rgba.resize(size_t(next.width) * size_t(next.height) * 4);
            for (int y = 0; y < next.height; ++y)
                for (int x = 0; x < next.width; ++x)
                    for (int k = 0; k < 4; ++k) {
                        const int x2 = std::min(img->width - 1, x * 2 + 1), y2 = std::min(img->height - 1, y * 2 + 1);
                        auto at = [&](int px, int py) { return int(img->rgba[(size_t(py) * size_t(img->width) + size_t(px)) * 4 + size_t(k)]); };
                        next.rgba[(size_t(y) * size_t(next.width) + size_t(x)) * 4 + size_t(k)] = uint8_t((at(x * 2, y * 2) + at(x2, y * 2) + at(x * 2, y2) + at(x2, y2) + 2) / 4);
                    }
            half = std::move(next);
            img = &half;
        }
        w = img->width, h = img->height;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    Tex(const Tex&) = delete;
    Tex& operator=(const Tex&) = delete;
    Tex(Tex&& o) noexcept : id(o.id), w(o.w), h(o.h) { o.id = 0; }
    Tex& operator=(Tex&& o) noexcept {
        std::swap(id, o.id), std::swap(w, o.w), std::swap(h, o.h);
        return *this;
    }
    ~Tex() {
        if (id) glDeleteTextures(1, &id);
    }
    explicit operator bool() const { return id != 0; }
    ImTextureID im() const { return reinterpret_cast<ImTextureID>(static_cast<intptr_t>(id)); }
    float aspect() const { return h ? float(w) / float(h) : 1.0f; }
};

// ---------------------------------------------------------------- the scene
class MenuScene {
public:
    struct UvAnimation {
        int channel = 0, mode = 0;
        float rate = 0, frames = 0, range = 1;
    };
    struct Mesh {
        std::string texture;
        ImTextureID external = nullptr;  // (a picture of the launcher's own, see addImage)
        bool particle = false;
        std::vector<float> v;  // x y z u v r g b a
        std::vector<uint16_t> i;
        std::vector<UvAnimation> uv;
        // a scrolling strip (one wide quad whose texture repeats): cut at the seams, carried on past the outer end
        bool strip = false, carryLeft = false, carryRight = false, sky = false, ground = false;
        int half = 0;  // (a strip: 1 of the world's first half, the night drive; 2 of the second, Paris)
        uint32_t edgeColour = 0;  // (sky: the top row's colour, ground: the bottom row's)
    };
    struct Sway {
        float amplitude = 0, period = 0;
        bool pingPong = false, randomStart = false;
    };
    struct Emitter {
        std::string texture;
        float life = 0.5f, speed = 0.25f, rate = 25, sizeFrom = 1, sizeTo = 1, spin = 0, half = 0.06f;
        float gravity[3] = {}, spread[3] = {};
        float colour[4] = {1, 1, 1, 1};
        float carry = 0;  // (fractions of a particle between frames)
    };
    struct Node {
        std::string name;
        int parent = -1;
        float pos[3] = {}, x[3] = {1, 0, 0}, z[3] = {0, 0, 1}, scale[3] = {1, 1, 1};
        Sway move[3], turn[3];
        std::vector<Mesh> meshes;
        std::unique_ptr<Emitter> emitter;
        // what the menu sets
        bool show = true;
        float alpha = 1, shade = 1;  // (shade: the colour's multiplier: a locked logo is dark)
        float offsetX = 0;           // (the van and Sly ride with the camera)
        float phase = 0;
    };
    struct Particle {
        int node = 0;
        float x = 0, y = 0, z = 0, vx = 0, vy = 0, age = 0, life = 1, turn = 0;
    };

    // Reads the converted menu's scene. Textures load when first drawn (preload() for the ones known to show).
    void load(const fs::path& dir, const json& menu) {
        m_dir = dir;
        m_textureFiles.clear(), m_textures.clear(), m_nodes.clear(), m_cameras.clear(), m_particles.clear();
        for (const auto& [name, t] : menu.at("textures").items()) m_textureFiles[name] = t.value("file", std::string{});
        for (const auto& [name, c] : menu.at("cameras").items()) m_cameras[name] = {c.at("pos").at(0).get<float>(), c.at("pos").at(2).get<float>(), c.value("fov", 45.0f)};
        for (const auto& j : menu.at("nodes")) {
            Node n;
            n.name = j.value("name", std::string{});
            n.parent = j.value("parent", -1);
            for (size_t k = 0; k < 3; ++k) {
                n.pos[k] = j.at("pos").at(k).get<float>(), n.x[k] = j.at("x").at(k).get<float>();
                n.z[k] = j.at("z").at(k).get<float>(), n.scale[k] = j.at("scale").at(k).get<float>();
            }
            auto sway = [&](const char* key, Sway* out) {
                if (!j.contains(key)) return;
                for (size_t k = 0; k < 3; ++k) {
                    const auto& a = j.at(key).at(k);
                    out[k] = {a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<int>() != 0, a.at(3).get<int>() != 0};
                }
            };
            sway("move", n.move), sway("turn", n.turn);
            uint32_t h = 2166136261u;
            for (char c : n.name) h = (h ^ uint8_t(c)) * 16777619u;
            n.phase = float(h & 0xFFFF) / 65536.0f;
            for (const auto& m : j.at("meshes")) {
                Mesh mesh;
                mesh.texture = m.value("tex", std::string{});
                mesh.particle = m.value("particle", false);
                mesh.v = m.at("v").get<std::vector<float>>();
                mesh.i = m.at("i").get<std::vector<uint16_t>>();
                for (const auto& a : m.at("uv")) mesh.uv.push_back({a.at(0).get<int>(), a.at(1).get<int>(), a.at(2).get<float>(), a.at(3).get<float>(), a.at(4).get<float>()});
                if (mesh.v.size() % 9 || mesh.i.size() % 3) continue;
                bool ok = true;
                for (uint16_t i : mesh.i) ok = ok && size_t(i) * 9 < mesh.v.size();
                if (ok) n.meshes.push_back(std::move(mesh));
            }
            if (j.contains("emitter")) {
                const auto& e = j.at("emitter");
                auto em = std::make_unique<Emitter>();
                em->texture = e.value("tex", std::string{});
                em->life = std::clamp(e.value("life", 0.5f), 0.1f, 4.0f), em->speed = e.value("speed", 0.25f), em->rate = e.value("rate", 25.0f);
                em->sizeFrom = e.at("size").at(0).get<float>(), em->sizeTo = e.at("size").at(1).get<float>();
                em->spin = e.value("spin", 0.0f), em->half = e.value("half", 0.06f);
                for (size_t k = 0; k < 3; ++k) em->gravity[k] = e.at("gravity").at(k).get<float>(), em->spread[k] = e.at("spread").at(k).get<float>();
                for (size_t k = 0; k < 4; ++k) em->colour[k] = e.at("colour").at(k).get<float>() / 255.0f;
                n.emitter = std::move(em);
            }
            m_nodes.push_back(std::move(n));
        }
        for (auto& n : m_nodes)
            if (n.parent >= int(m_nodes.size())) n.parent = -1;
        findStrips();
        m_bloom = menu.value("bloom", json::object());
    }
    std::vector<Node>& nodes() { return m_nodes; }
    // A picture that is not the menu's (a logo the launcher ships) placed in the scene as the menu places its own
    // logos: the picture and, behind it, a half-transparent black copy as its drop shadow; drawn in depth order with
    // everything else, inside the glow. width, height: its size in the world.
    void addImage(const std::string& name, ImTextureID picture, float x, float y, float z, float width, float height) {
        Node n;
        n.name = name;
        n.pos[0] = x, n.pos[1] = y, n.pos[2] = z;
        auto quad = [&](float dx, float dy, float dz, float r, float g, float b, float a) {
            Mesh m;
            m.external = picture;
            const float hw = width * 0.5f, hh = height * 0.5f;
            const float corners[4][4] = {{-hw, -hh, 0, 1}, {hw, -hh, 1, 1}, {hw, hh, 1, 0}, {-hw, hh, 0, 0}};
            for (const auto& c : corners) m.v.insert(m.v.end(), {c[0] + dx, c[1] + dy, dz, c[2], c[3], r, g, b, a});
            m.i = {0, 1, 2, 0, 2, 3};
            return m;
        };
        // (the scene's own logos: vertex colour 0.8, the shadow 0.034 down and left and 0.056 behind at half black)
        n.meshes.push_back(quad(0, 0, 0, 0.8f, 0.8f, 0.8f, 1.0f));
        n.meshes.push_back(quad(-0.034f, -0.034f, -0.056f, 0, 0, 0, 0.5f));
        m_nodes.push_back(std::move(n));
    }
    // A copy of a root node (an arrow for a page the menu has none for) at another place.
    bool clone(std::string_view name, const std::string& as, float x) {
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            if (m_nodes[i].name != name || m_nodes[i].parent >= 0) continue;
            Node n;
            const Node& from = m_nodes[i];
            n.name = as, n.meshes = from.meshes;
            std::copy(from.pos, from.pos + 3, n.pos), std::copy(from.x, from.x + 3, n.x), std::copy(from.z, from.z + 3, n.z), std::copy(from.scale, from.scale + 3, n.scale);
            std::copy(from.move, from.move + 3, n.move), std::copy(from.turn, from.turn + 3, n.turn);
            n.phase = from.phase + 0.37f;
            n.pos[0] = x;
            m_nodes.push_back(std::move(n));
            return true;
        }
        return false;
    }
    Node* node(std::string_view name) {
        for (auto& n : m_nodes)
            if (n.name == name) return &n;
        return nullptr;
    }
    bool hasCamera(const std::string& name) const { return m_cameras.count(name) != 0; }
    float cameraX(const std::string& name, float fallback = 0) const {
        auto it = m_cameras.find(name);
        return it == m_cameras.end() ? fallback : it->second.x;
    }
    // The seam between the two halves of the world (the night drive, Paris at dusk): where the first half's strips end.
    float seamX() const { return m_seam; }
    // Moves the change from the first half's strips to the second's to x: the first half's repeat on (their texture
    // repeats anyway) up to there and the second's begin there, cross-faded over x +- halfWidth, so no edge shows.
    // halfWidth 0: the scene's own seam, as the file has it.
    void setTransition(float x, float halfWidth) { m_transition = x, m_transitionHalf = std::max(0.0f, halfWidth); }
    const Tex& texture(const std::string& name) {
        auto it = m_textures.find(name);
        if (it != m_textures.end()) return it->second;
        Tex tex;
        auto f = m_textureFiles.find(name);
        if (f != m_textureFiles.end()) {
            const auto bytes = readBytes(m_dir / path(f->second));
            const Image img = bytes.empty() ? Image{} : decodeImage(bytes.data(), bytes.size());
            noteEdges(name, img);
            tex = Tex(img);
        }
        return m_textures.emplace(name, std::move(tex)).first->second;
    }
    // Loads the textures of every node shown now.
    void preload() {
        for (const auto& n : m_nodes) {
            if (!n.show) continue;
            for (const auto& m : n.meshes)
                if (!m.external) texture(m.texture);
            if (n.emitter) texture(n.emitter->texture);
        }
    }
    json bloom() const { return m_bloom; }

    // ---- the view
    struct View {
        ImVec2 size{1280, 720};
        float x = 0, z = 2.4384f, focal = 2.4142f;  // (focal: 1 / tan(fov / 2), over half the window's height)
        float worldAt(float sx, float depth = 0) const { return x + (sx - size.x * 0.5f) * (z - depth) / (focal * size.y * 0.5f); }
        ImVec2 project(float wx, float wy, float wz) const {
            const float k = focal * size.y * 0.5f / (z - wz);
            return {size.x * 0.5f + (wx - x) * k, size.y * 0.5f - wy * k};
        }
        // The window's pixels per world unit at this depth.
        float scale(float depth = 0) const { return focal * size.y * 0.5f / (z - depth); }
    };
    View view(ImVec2 size, float cameraAtX) const {
        View v;
        v.size = size, v.x = cameraAtX;
        auto it = m_cameras.find("Camera_Title");
        const float fov = it == m_cameras.end() ? 45.0f : it->second.fov;
        if (it != m_cameras.end()) v.z = it->second.z;
        v.focal = 1.0f / std::tan(fov * 3.14159265f / 360.0f);
        // narrower than 4:3: the page keeps its width (the view grows upwards and downwards instead)
        const float aspect = size.y > 0 ? size.x / size.y : 16.0f / 9.0f;
        if (aspect < 4.0f / 3.0f) v.focal *= aspect / (4.0f / 3.0f);
        return v;
    }

    // ---- a frame
    void update(float dt, float cameraX) {
        m_time += dt;
        // emitters
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            Node& n = m_nodes[i];
            if (!n.emitter || !shown(int(i))) continue;
            Emitter& e = *n.emitter;
            e.carry += dt * e.rate;
            float m[6];
            world(int(i), cameraX, m);
            while (e.carry >= 1) {
                e.carry -= 1;
                Particle p;
                p.node = int(i);
                const float r1 = random() - 0.5f, r2 = random() - 0.5f;
                p.x = m[4] + r1 * e.spread[0], p.y = m[5] + r2 * e.spread[1], p.z = worldZ(int(i));
                // along the emitter's own X axis (the exhaust pipe points up and back)
                p.vx = m[0] * e.speed, p.vy = m[1] * e.speed;
                p.life = e.life, p.turn = random() * 6.2831853f;
                m_particles.push_back(p);
            }
        }
        for (auto& p : m_particles) {
            const Emitter& e = *m_nodes[size_t(p.node)].emitter;
            p.age += dt;
            p.vx += e.gravity[0] * dt, p.vy += e.gravity[1] * dt;
            // (the world runs past under the wheels: the puffs are left behind)
            p.x += (p.vx - kDrift) * dt, p.y += p.vy * dt;
        }
        m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(), [](const Particle& p) { return p.age >= p.life; }), m_particles.end());
    }
    // Draws the scene. Returns nothing; rect() gives a node's place afterwards.
    void draw(ImDrawList* dl, const View& view, float fade = 1.0f) {
        struct Item {
            float z;
            int node, mesh;  // mesh -1: the node's particles
        };
        std::vector<Item> items;
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            if (!shown(int(i))) continue;
            const float z = worldZ(int(i));
            for (size_t m = 0; m < m_nodes[i].meshes.size(); ++m) {
                const Mesh& mesh = m_nodes[i].meshes[m];
                float local = 0;
                for (size_t k = 2; k < mesh.v.size(); k += 9) local += mesh.v[k];
                items.push_back({z + local / float(std::max<size_t>(1, mesh.v.size() / 9)), int(i), int(m)});
            }
            if (m_nodes[i].emitter) items.push_back({z - 0.001f, int(i), -1});
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.z < b.z; });
        for (const Item& it : items) {
            if (it.mesh < 0) drawParticles(dl, view, it.node, fade);
            else if (m_nodes[size_t(it.node)].meshes[size_t(it.mesh)].strip) drawStrip(dl, view, it.node, it.mesh, fade);
            else drawMesh(dl, view, it.node, it.mesh, fade);
        }
    }
    // A node's box in the window (its first mesh), for the mouse and for notes placed beside it.
    ImRect rect(const View& view, std::string_view name) {
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            if (m_nodes[i].name != name || m_nodes[i].meshes.empty()) continue;
            float m[6];
            world(int(i), view.x, m);
            const float z = worldZ(int(i));
            ImRect r({1e9f, 1e9f}, {-1e9f, -1e9f});
            const Mesh& mesh = m_nodes[i].meshes.front();
            for (size_t k = 0; k + 8 < mesh.v.size(); k += 9) {
                const ImVec2 p = view.project(m[0] * mesh.v[k] + m[2] * mesh.v[k + 1] + m[4], m[1] * mesh.v[k] + m[3] * mesh.v[k + 1] + m[5], z + mesh.v[k + 2]);
                r.Add(p);
            }
            return r;
        }
        return {};
    }
    double time() const { return m_time; }

private:
    // The speed the road runs under the van, in units per second (the map strip's own scroll: 0.05 widths of 12.15).
    static constexpr float kDrift = 0.6f;
    struct CameraInfo {
        float x = 0, z = 2.4384f, fov = 45;
    };
    float random() {
        m_seed = m_seed * 1664525u + 1013904223u;
        return float((m_seed >> 8) & 0xFFFF) / 65536.0f;
    }
    bool shown(int i) const {
        for (int n = i; n >= 0; n = m_nodes[size_t(n)].parent)
            if (!m_nodes[size_t(n)].show || m_nodes[size_t(n)].alpha <= 0) return false;
        return true;
    }
    float alphaOf(int i) const {
        float a = 1;
        for (int n = i; n >= 0; n = m_nodes[size_t(n)].parent) a *= m_nodes[size_t(n)].alpha;
        return a;
    }
    float shadeOf(int i) const {
        float a = 1;
        for (int n = i; n >= 0; n = m_nodes[size_t(n)].parent) a *= m_nodes[size_t(n)].shade;
        return a;
    }
    float worldZ(int i) const {
        float z = 0;
        for (int n = i; n >= 0; n = m_nodes[size_t(n)].parent) z += m_nodes[size_t(n)].pos[2];
        return z;
    }
    static float swing(const Sway& s, double time, float phase) {
        if (s.amplitude == 0 || s.period <= 0) return 0;
        const double t = time / double(s.period) + double(s.randomStart ? phase : 0);
        if (s.pingPong) return s.amplitude * float(std::sin(t * 6.283185307179586));
        return s.amplitude * float(t - std::floor(t));
    }
    // The node's 2D transform in the world: [a b; c d] (columns: its X axis, its Y axis) and its origin (m[4], m[5]).
    void world(int i, float cameraX, float m[6]) const {
        const Node& n = m_nodes[size_t(i)];
        // (the axes stored are X and Z; every node of the menu lies in the picture plane, so Y is Z x X)
        const float ax = n.x[0], ay = n.x[1];
        float yx = n.z[1] * n.x[2] - n.z[2] * n.x[1], yy = n.z[2] * n.x[0] - n.z[0] * n.x[2];
        const float turn = swing(n.turn[2], m_time, n.phase) * 3.14159265f / 180.0f;
        const float c = std::cos(turn), s = std::sin(turn);
        float local[6] = {(ax * c + yx * s) * n.scale[0], (ay * c + yy * s) * n.scale[0], (-ax * s + yx * c) * n.scale[1], (-ay * s + yy * c) * n.scale[1],
                          n.pos[0] + n.offsetX + swing(n.move[0], m_time, n.phase), n.pos[1] + swing(n.move[1], m_time, n.phase)};
        if (n.parent < 0) {
            if (n.offsetX != 0 || m_rides.count(i)) local[4] = cameraX + n.offsetX + swing(n.move[0], m_time, n.phase);
            std::copy(local, local + 6, m);
            return;
        }
        float p[6];
        world(n.parent, cameraX, p);
        m[0] = p[0] * local[0] + p[2] * local[1], m[1] = p[1] * local[0] + p[3] * local[1];
        m[2] = p[0] * local[2] + p[2] * local[3], m[3] = p[1] * local[2] + p[3] * local[3];
        m[4] = p[0] * local[4] + p[2] * local[5] + p[4], m[5] = p[1] * local[4] + p[3] * local[5] + p[5];
    }
    // The texture-coordinate offset of a mesh now.
    void uvOffset(const Mesh& mesh, float& du, float& dv) const {
        du = dv = 0;
        for (const auto& a : mesh.uv) {
            float o = 0;
            if (a.mode == 1 && a.frames == 0 && a.rate != 0) {
                o = a.range * float(std::sin(m_time * double(a.rate) / double(a.range) * 6.283185307179586));  // a sway (the van's engine)
            } else if (a.frames > 0) {
                // a flipbook: the offset runs as a scroll does (rate: the range per second) and steps in `frames` steps
                // (Sly's run: 8 x 8 frames, U 9.6 rows per second, V 1.2 sheets per second: 76.8 frames per second)
                const double range = a.range > 0 ? a.range : 1.0, run = std::fmod(m_time * double(a.rate), range) / range;
                o = float(std::floor(run * double(a.frames)) / double(a.frames) * range);
            } else {
                o = float(std::fmod(m_time * double(a.rate), double(a.range > 0 ? a.range : 1.0f)));  // a scroll
            }
            (a.channel == 0 ? du : dv) += o;
        }
    }
    static ImU32 colour(const float* rgba, float shade, float alpha) {
        auto c = [](float v) { return int(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return IM_COL32(c(rgba[0] * shade), c(rgba[1] * shade), c(rgba[2] * shade), c(rgba[3] * alpha));
    }
    void drawMesh(ImDrawList* dl, const View& view, int ni, int mi, float fade) {
        const Node& n = m_nodes[size_t(ni)];
        const Mesh& mesh = n.meshes[size_t(mi)];
        ImTextureID picture = mesh.external;
        if (!picture) {
            const Tex& tex = texture(mesh.texture);
            if (!tex) return;
            picture = tex.im();
        }
        float m[6], du, dv;
        world(ni, view.x, m);
        uvOffset(mesh, du, dv);
        const float z = worldZ(ni), alpha = alphaOf(ni) * fade, shade = shadeOf(ni);
        // (off the window: skipped)
        float minX = 1e9f, maxX = -1e9f;
        std::vector<ImVec2>& pts = m_points;
        pts.clear();
        for (size_t k = 0; k + 8 < mesh.v.size(); k += 9) {
            const ImVec2 p = view.project(m[0] * mesh.v[k] + m[2] * mesh.v[k + 1] + m[4], m[1] * mesh.v[k] + m[3] * mesh.v[k + 1] + m[5], z + mesh.v[k + 2]);
            minX = std::min(minX, p.x), maxX = std::max(maxX, p.x);
            pts.push_back(p);
        }
        if (maxX < 0 || minX > view.size.x) return;
        dl->PushTextureID(picture);
        dl->PrimReserve(int(mesh.i.size()), int(pts.size()));
        const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
        for (size_t k = 0; k < pts.size(); ++k) dl->PrimWriteVtx(pts[k], {mesh.v[k * 9 + 3] + du, mesh.v[k * 9 + 4] + dv}, colour(&mesh.v[k * 9 + 5], shade, alpha));
        for (uint16_t i : mesh.i) dl->PrimWriteIdx(ImDrawIdx(base + i));
        dl->PopTextureID();
    }
    // A strip: a wide quad whose texture repeats along U and scrolls. Cut at every seam of the texture; the outer end
    // is carried on to the window's edge; the sky above / the ground below take the strip's edge colour.
    void drawStrip(ImDrawList* dl, const View& view, int ni, int mi, float fade) {
        const Node& n = m_nodes[size_t(ni)];
        const Mesh& mesh = n.meshes[size_t(mi)];
        const Tex& tex = texture(mesh.texture);
        if (!tex) return;
        float m[6], du, dv;
        world(ni, view.x, m);
        uvOffset(mesh, du, dv);
        const float z = worldZ(ni), alpha = alphaOf(ni) * fade, shade = shadeOf(ni);
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, u0 = 0, u1 = 0, vTop = 0, vBottom = 1;
        for (size_t k = 0; k + 8 < mesh.v.size(); k += 9) {
            const float wx = m[0] * mesh.v[k] + m[4], wy = m[3] * mesh.v[k + 1] + m[5];
            if (wx < x0) x0 = wx, u0 = mesh.v[k + 3];
            if (wx > x1) x1 = wx, u1 = mesh.v[k + 3];
            if (wy < y0) y0 = wy, vBottom = mesh.v[k + 4];
            if (wy > y1) y1 = wy, vTop = mesh.v[k + 4];
        }
        if (x1 <= x0 || u1 == u0) return;
        const float perUnit = (u1 - u0) / (x1 - x0);  // texture widths per world unit
        float left = x0, right = x1;
        // the change of halves moved (setTransition): the first half's strips run on to it, the second's start there
        const bool moved = m_transitionHalf > 0 && mesh.half != 0;
        const float fadeFrom = m_transition - m_transitionHalf, fadeTo = m_transition + m_transitionHalf;
        if (moved && mesh.half == 1) right = fadeTo;
        if (moved && mesh.half == 2) left = fadeFrom;
        const float viewLeft = view.worldAt(0, z) - 0.01f, viewRight = view.worldAt(view.size.x, z) + 0.01f;
        if (mesh.carryLeft) left = std::min(left, viewLeft);
        if (mesh.carryRight) right = std::max(right, viewRight);
        left = std::max(left, viewLeft), right = std::min(right, viewRight);
        if (right <= left) return;
        // how much of the strip shows at x: across the change the second half is laid over the first (it reaches full
        // cover halfway, where the first begins to fade out under it), so the two always cover the picture together
        auto cover = [&](float wx) {
            if (!moved) return 1.0f;
            const float t = std::clamp((wx - fadeFrom) / (fadeTo - fadeFrom), 0.0f, 1.0f);
            return std::min(1.0f, 2.0f * (mesh.half == 1 ? 1.0f - t : t));
        };
        auto tint = [&](float a) { return colour(&mesh.v[5], shade, alpha * a); };
        const float top = view.project(0, y1, z).y, bottom = view.project(0, y0, z).y;
        const ImU32 edge = IM_COL32(int(float(mesh.edgeColour & 255) * mesh.v[5] * shade), int(float((mesh.edgeColour >> 8) & 255) * mesh.v[6] * shade),
                                    int(float((mesh.edgeColour >> 16) & 255) * mesh.v[7] * shade), 0);
        auto edgeTint = [&](float a) { return edge | (ImU32(std::clamp(int(255 * alpha * a + 0.5f), 0, 255)) << IM_COL32_A_SHIFT); };
        auto screenX = [&](float wx, bool last) { return last ? std::ceil(view.project(wx, 0, z).x) : std::floor(view.project(wx, 0, z).x); };
        // half a texel is kept off each seam (the two sides of a seam are different ends of the picture)
        const float inset = 0.5f / float(std::max(1, tex.w));
        auto uAt = [&](float wx) { return u0 + (wx - x0) * perUnit + du; };
        // pieces of constant slope of the cover: cut where the fade begins, peaks and ends
        float cuts[5] = {left, fadeFrom, m_transition, fadeTo, right};
        int count = 0;
        float pieces[5];
        for (float c : cuts)
            if (c >= left && c <= right && (count == 0 || c > pieces[count - 1] + 1e-4f)) pieces[count++] = c;
        if (pieces[count - 1] < right - 1e-4f) pieces[count++] = right;
        for (int p = 0; p + 1 < count; ++p) {
            const float pa = pieces[p], pb = pieces[p + 1], ca = cover(pa), cb = cover(pb);
            if (ca <= 0 && cb <= 0) continue;
            auto coverAt = [&](float wx) { return ca + (cb - ca) * (wx - pa) / (pb - pa); };
            const bool lastPiece = pb >= right - 1e-4f;
            dl->PushTextureID(tex.im());
            for (float wx = pa; wx < pb - 1e-4f;) {
                const float u = uAt(wx), tile = std::floor(u + 1e-5f);
                // the world x where this tile ends
                float end = perUnit > 0 ? x0 + (tile + 1 - du - u0) / perUnit : x0 + (tile - du - u0) / perUnit;
                if (end <= wx + 1e-4f) end = wx + 1.0f / std::fabs(perUnit);
                end = std::min(end, pb);
                float ua = u - tile, ub = uAt(end) - tile;
                ua = std::clamp(ua, inset, 1 - inset), ub = std::clamp(ub, inset, 1 - inset);
                const float sa = screenX(wx, false), sb = screenX(end, lastPiece && end >= pb);
                const ImU32 cl = tint(coverAt(wx)), cr = tint(coverAt(end));
                dl->PrimReserve(6, 4);
                const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
                dl->PrimWriteIdx(base), dl->PrimWriteIdx(ImDrawIdx(base + 1)), dl->PrimWriteIdx(ImDrawIdx(base + 2));
                dl->PrimWriteIdx(base), dl->PrimWriteIdx(ImDrawIdx(base + 2)), dl->PrimWriteIdx(ImDrawIdx(base + 3));
                dl->PrimWriteVtx({sa, top}, {ua, vTop + dv}, cl), dl->PrimWriteVtx({sb, top}, {ub, vTop + dv}, cr);
                dl->PrimWriteVtx({sb, bottom}, {ub, vBottom + dv}, cr), dl->PrimWriteVtx({sa, bottom}, {ua, vBottom + dv}, cl);
                wx = end;
            }
            dl->PopTextureID();
            // the sky above / the ground below: the strip's edge colour carried on to the window's edge
            const float sl = screenX(pa, false), sr = screenX(pb, lastPiece);
            const ImU32 el = edgeTint(ca), er = edgeTint(cb);
            if (mesh.sky && top > 0) dl->AddRectFilledMultiColor({sl, 0}, {sr, top + 1}, el, er, er, el);
            if (mesh.ground && bottom < view.size.y) dl->AddRectFilledMultiColor({sl, bottom - 1}, {sr, view.size.y}, el, er, er, el);
        }
    }
    void drawParticles(ImDrawList* dl, const View& view, int ni, float fade) {
        const Node& n = m_nodes[size_t(ni)];
        const Emitter& e = *n.emitter;
        const Tex& tex = texture(e.texture);
        if (!tex) return;
        const float alpha = alphaOf(ni) * fade;
        for (const Particle& p : m_particles) {
            if (p.node != ni) continue;
            const float t = p.age / p.life;
            // fades in over the first tenth, out over the rest; grows; turns
            const float a = std::min(t / 0.1f, 1.0f) * (1 - t) * e.colour[3] * alpha;
            const float half = e.half * (e.sizeFrom + (e.sizeTo - e.sizeFrom) * t) * view.scale(p.z);
            const float turn = p.turn + e.spin * t * 3.14159265f / 180.0f, c = std::cos(turn) * half, s = std::sin(turn) * half;
            const ImVec2 o = view.project(p.x, p.y, p.z);
            const float rgba[4] = {e.colour[0], e.colour[1], e.colour[2], 1};
            dl->AddImageQuad(tex.im(), {o.x - c + s, o.y - s - c}, {o.x + c + s, o.y + s - c}, {o.x + c - s, o.y + s + c}, {o.x - c - s, o.y - s + c}, {0, 0}, {1, 0}, {1, 1},
                             {0, 1}, colour(rgba, 1, a));
        }
    }
    // Which meshes are strips, which end of each depth's row is the outer one, and which are the sky and the ground.
    void findStrips() {
        struct Found {
            int node, mesh;
            float left, right, z;
        };
        std::vector<Found> found;
        m_rides.clear();
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            Node& n = m_nodes[i];
            if (n.parent >= 0) continue;
            for (size_t m = 0; m < n.meshes.size(); ++m) {
                Mesh& mesh = n.meshes[m];
                if (mesh.v.size() != 36) continue;
                float x0 = 1e9f, x1 = -1e9f;
                for (size_t k = 0; k < 36; k += 9) x0 = std::min(x0, mesh.v[k]), x1 = std::max(x1, mesh.v[k]);
                if (x1 - x0 < 10) continue;
                mesh.strip = true;
                found.push_back({int(i), int(m), n.pos[0] + x0, n.pos[0] + x1, n.pos[2]});
            }
        }
        float zMin = 1e9f, zMax = -1e9f, xMin = 1e9f, xMax = -1e9f;
        for (const auto& f : found) zMin = std::min(zMin, f.z), zMax = std::max(zMax, f.z), xMin = std::min(xMin, f.left), xMax = std::max(xMax, f.right);
        m_seam = 1e9f;
        for (const auto& f : found) {
            Mesh& mesh = m_nodes[size_t(f.node)].meshes[size_t(f.mesh)];
            mesh.carryLeft = f.left <= xMin + 0.01f, mesh.carryRight = f.right >= xMax - 0.01f;
            mesh.sky = f.z <= zMin + 0.001f, mesh.ground = f.z >= zMax - 0.001f;
            if (f.right < xMax - 0.01f) m_seam = std::min(m_seam, f.right);
        }
        if (m_seam > 1e8f) m_seam = xMax;
        // which half each strip belongs to (the ones that end at the seam, the ones that begin there)
        for (const auto& f : found) {
            Mesh& mesh = m_nodes[size_t(f.node)].meshes[size_t(f.mesh)];
            mesh.half = std::fabs(f.right - m_seam) < 0.01f ? 1 : std::fabs(f.left - m_seam) < 0.01f ? 2 : 0;
        }
    }
    // A strip's edge colour from its picture: the top row (the sky carried on upwards), the bottom row (the ground).
    void noteEdges(const std::string& name, const Image& img) {
        if (img.empty()) return;
        for (auto& n : m_nodes)
            for (auto& mesh : n.meshes) {
                if (!mesh.strip || mesh.texture != name || !(mesh.sky || mesh.ground)) continue;
                const int row = mesh.sky ? 0 : img.height - 1;
                uint64_t sum[3] = {};
                for (int x = 0; x < img.width; ++x)
                    for (int k = 0; k < 3; ++k) sum[k] += img.rgba[(size_t(row) * size_t(img.width) + size_t(x)) * 4 + size_t(k)];
                mesh.edgeColour = uint32_t(sum[0] / uint64_t(img.width)) | uint32_t(sum[1] / uint64_t(img.width)) << 8 | uint32_t(sum[2] / uint64_t(img.width)) << 16;
            }
    }

public:
    // The van and Sly ride with the camera: their root nodes take the camera's X (plus offsetX).
    void ride(std::string_view name) {
        for (size_t i = 0; i < m_nodes.size(); ++i)
            if (m_nodes[i].name == name && m_nodes[i].parent < 0) m_rides.insert(int(i));
    }

private:
    fs::path m_dir;
    std::map<std::string, std::string> m_textureFiles;
    std::map<std::string, Tex> m_textures;
    std::map<std::string, CameraInfo> m_cameras;
    std::vector<Node> m_nodes;
    std::vector<Particle> m_particles;
    std::vector<ImVec2> m_points;
    std::set<int> m_rides;
    json m_bloom;
    double m_time = 0;
    float m_seam = 18.9f, m_transition = 18.9f, m_transitionHalf = 0;
    uint32_t m_seed = 12345;
};

// ---------------------------------------------------------------- the comic font
// The menu's own letters (ps3menu.h readGlyphMap): pages of glyph boxes in reading order from a first character.
class MenuFont {
public:
    void load(const fs::path& dir, const json& menu) {
        m_glyphs.clear(), m_pages.clear();
        if (!menu.contains("font")) return;
        for (const auto& p : menu.at("font")) {
            const auto bytes = readBytes(dir / path(p.value("file", std::string{})));
            Tex tex(bytes.empty() ? Image{} : decodeImage(bytes.data(), bytes.size()));
            if (!tex) continue;
            const int page = int(m_pages.size()), first = p.value("first", 32);
            const float w = float(std::max(1, p.value("w", 512))), h = float(std::max(1, p.value("h", 512)));
            int code = first;
            for (const auto& g : p.at("glyphs")) {
                Glyph glyph{page, g.at(0).get<float>() / w, g.at(1).get<float>() / h, g.at(2).get<float>() / w, g.at(3).get<float>() / h,
                            g.at(2).get<float>(), g.at(3).get<float>(), float(g.at(4).get<int>() - g.at(1).get<int>())};
                m_glyphs.emplace(code++, glyph);
                m_line = std::max(m_line, glyph.h);
            }
            m_pages.push_back(std::move(tex));
        }
    }
    bool ok() const { return !m_pages.empty() && m_line > 0; }
    // Every character of the text has a glyph (else the caller writes it in the launcher's own typeface).
    bool covers(const char* text) const {
        if (!ok()) return false;
        for (const char* p = text; *p;) {
            unsigned int c = 0;
            p += ImTextCharFromUtf8(&c, p, nullptr);
            if (c != '\n' && !m_glyphs.count(int(c))) return false;
        }
        return true;
    }
    // size: the height of a capital letter's box in pixels.
    ImVec2 measure(float size, const char* text) const {
        const float k = size / kCap;
        float x = 0, widest = 0, lines = 1;
        for (const char* p = text; *p;) {
            unsigned int c = 0;
            p += ImTextCharFromUtf8(&c, p, nullptr);
            if (c == '\n') {
                widest = std::max(widest, x), x = 0, ++lines;
                continue;
            }
            auto it = m_glyphs.find(int(c));
            if (it != m_glyphs.end()) x += advance(it->second) * k;
        }
        return {std::max(widest, x), lines * kLine * k};
    }
    void draw(ImDrawList* dl, float size, ImVec2 at, ImU32 colour, const char* text) const {
        const float k = size / kCap;
        float x = at.x, y = at.y;
        for (const char* p = text; *p;) {
            unsigned int c = 0;
            p += ImTextCharFromUtf8(&c, p, nullptr);
            if (c == '\n') {
                x = at.x, y += kLine * k;
                continue;
            }
            auto it = m_glyphs.find(int(c));
            if (it == m_glyphs.end()) continue;
            const Glyph& g = it->second;
            if (g.h > 2) {
                // the baseline sits kCap under the line's top
                const float top = y + (kCap - g.base) * k;
                dl->AddImage(m_pages[size_t(g.page)].im(), {x, top}, {x + g.w * k, top + g.h * k}, {g.u, g.v}, {g.u + g.uw, g.v + g.vh}, colour);
            }
            x += advance(g) * k;
        }
    }

private:
    struct Glyph {
        int page;
        float u, v, uw, vh, w, h, base;
    };
    // (the boxes include the letters' black outline: neighbours overlap by it)
    static float advance(const Glyph& g) { return g.h > 2 ? g.w - 3.0f : g.w; }
    static constexpr float kCap = 44.0f, kLine = 58.0f;  // a capital's box and the line pitch on the font's page, in its pixels
    std::map<int, Glyph> m_glyphs;
    std::vector<Tex> m_pages;
    float m_line = 0;
};
}  // namespace cane::collection
