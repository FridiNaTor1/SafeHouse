// See model.h. glTF through cgltf (MIT, Libraries/cgltf), FBX through ufbx (MIT / public domain, Libraries/ufbx),
// images through stb_image / stb_image_write. The .glb writer is our own (nlohmann/json for the JSON chunk).
#include "model.h"

#include <nlohmann/json.hpp>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <ufbx.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb/stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>

namespace cane::editor::model {

std::size_t Scene::triangles() const
{
    std::size_t c = 0;
    for (const Mesh& m : meshes)
        for (const Primitive& p : m.prims) c += p.idx.size() / 3;
    return c;
}
std::size_t Scene::vertices() const
{
    std::size_t c = 0;
    for (const Mesh& m : meshes)
        for (const Primitive& p : m.prims) c += p.pos.size();
    return c;
}

namespace {

std::string lower(std::string s)
{
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool readAll(const fs::path& p, std::vector<uint8_t>& ab)
{
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff cb = in.tellg();
    if (cb < 0 || cb > (std::streamoff(1) << 31)) return false;
    ab.resize(std::size_t(cb));
    in.seekg(0);
    return bool(in.read(reinterpret_cast<char*>(ab.data()), cb));
}

bool decodeImage(const uint8_t* pb, std::size_t cb, Image& img)
{
    int w = 0, h = 0, n = 0;
    stbi_uc* p = stbi_load_from_memory(pb, int(cb), &w, &h, &n, 4);
    if (!p) return false;
    img.w = w;
    img.h = h;
    img.rgba.assign(p, p + std::size_t(w) * std::size_t(h) * 4);
    stbi_image_free(p);
    return true;
}

// ---- glTF

glm::mat4 matOf(const float a[16]) { return glm::make_mat4(a); }

bool loadGltf(const fs::path& path, Scene& sc, std::string& error)
{
    cgltf_options opt{};
    cgltf_data* data = nullptr;
    const std::string spath = path.string();
    if (cgltf_parse_file(&opt, spath.c_str(), &data) != cgltf_result_success) { error = "not a glTF file"; return false; }
    struct Free { cgltf_data* p; ~Free() { cgltf_free(p); } } guard{data};
    if (cgltf_load_buffers(&opt, data, spath.c_str()) != cgltf_result_success) { error = "the glTF buffers could not be read"; return false; }

    // images
    std::map<const cgltf_image*, int> mpimage;
    for (cgltf_size i = 0; i < data->images_count; ++i)
    {
        const cgltf_image& im = data->images[i];
        Image img;
        img.name = im.name ? im.name : ("image" + std::to_string(i));
        bool ok = false;
        if (im.buffer_view && im.buffer_view->buffer->data)
            ok = decodeImage(static_cast<const uint8_t*>(im.buffer_view->buffer->data) + im.buffer_view->offset, im.buffer_view->size, img);
        else if (im.uri && std::strncmp(im.uri, "data:", 5) == 0)
        {
            const char* comma = std::strchr(im.uri, ',');
            if (comma)
            {
                const std::size_t c64 = std::strlen(comma + 1);
                void* pv = nullptr;
                if (cgltf_load_buffer_base64(&opt, c64 * 3 / 4, comma + 1, &pv) == cgltf_result_success)
                {
                    ok = decodeImage(static_cast<const uint8_t*>(pv), c64 * 3 / 4, img);
                    std::free(pv);
                }
            }
        }
        else if (im.uri)
        {
            std::string uri = im.uri;
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            std::vector<uint8_t> ab;
            if (readAll(path.parent_path() / fs::u8path(uri), ab)) ok = decodeImage(ab.data(), ab.size(), img);
        }
        if (!ok) continue;   // (a missing image: the material keeps its colour)
        mpimage[&im] = int(sc.images.size());
        sc.images.push_back(std::move(img));
    }
    // materials
    std::map<const cgltf_material*, int> mpmat;
    for (cgltf_size i = 0; i < data->materials_count; ++i)
    {
        const cgltf_material& m = data->materials[i];
        Material mat;
        mat.name = m.name ? m.name : ("material" + std::to_string(i));
        if (m.has_pbr_metallic_roughness)
        {
            mat.base = glm::make_vec4(m.pbr_metallic_roughness.base_color_factor);
            const cgltf_texture* t = m.pbr_metallic_roughness.base_color_texture.texture;
            if (t && t->image && mpimage.count(t->image)) mat.image = mpimage[t->image];
        }
        mat.blend = m.alpha_mode != cgltf_alpha_mode_opaque;
        mat.doubleSided = m.double_sided;
        mpmat[&m] = int(sc.materials.size());
        sc.materials.push_back(mat);
    }
    // the skeleton: the first skin (later skins' joints are added to it)
    std::map<const cgltf_node*, int> mpjoint;
    for (cgltf_size is = 0; is < data->skins_count; ++is)
    {
        const cgltf_skin& skin = data->skins[is];
        for (cgltf_size j = 0; j < skin.joints_count; ++j)
        {
            const cgltf_node* n = skin.joints[j];
            if (mpjoint.count(n)) continue;
            Joint jt;
            jt.name = n->name ? n->name : ("joint" + std::to_string(sc.joints.size()));
            float a[16];
            cgltf_node_transform_local(n, a);
            jt.local = matOf(a);
            if (skin.inverse_bind_matrices) { cgltf_accessor_read_float(skin.inverse_bind_matrices, j, a, 16); jt.inverseBind = matOf(a); }
            else { cgltf_node_transform_world(n, a); jt.inverseBind = glm::inverse(matOf(a)); }
            mpjoint[n] = int(sc.joints.size());
            sc.joints.push_back(jt);
        }
    }
    for (auto& [n, ij] : mpjoint)
        for (const cgltf_node* p = n->parent; p; p = p->parent)
            if (auto it = mpjoint.find(p); it != mpjoint.end()) { sc.joints[size_t(ij)].parent = it->second; break; }
    // parents first (a stable order: by depth)
    {
        std::vector<int> depth(sc.joints.size());
        for (std::size_t i = 0; i < sc.joints.size(); ++i)
            for (int p = sc.joints[i].parent; p >= 0 && depth[i] < 1000; p = sc.joints[size_t(p)].parent) ++depth[i];
        std::vector<int> order(sc.joints.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = int(i);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return depth[size_t(a)] < depth[size_t(b)]; });
        std::vector<int> newOf(order.size());
        for (std::size_t i = 0; i < order.size(); ++i) newOf[size_t(order[i])] = int(i);
        std::vector<Joint> aj(order.size());
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            aj[i] = sc.joints[size_t(order[i])];
            if (aj[i].parent >= 0) aj[i].parent = newOf[size_t(aj[i].parent)];
        }
        sc.joints = std::move(aj);
        for (auto& [n, ij] : mpjoint) ij = newOf[size_t(ij)];
    }
    // meshes
    for (cgltf_size in = 0; in < data->nodes_count; ++in)
    {
        const cgltf_node& node = data->nodes[in];
        if (!node.mesh) continue;
        Mesh mesh;
        mesh.name = node.name ? node.name : (node.mesh->name ? node.mesh->name : "mesh");
        mesh.skinned = node.skin != nullptr;
        float aw[16];
        cgltf_node_transform_world(&node, aw);
        const glm::mat4 world = matOf(aw);
        const glm::mat3 nworld = glm::transpose(glm::inverse(glm::mat3(world)));
        for (cgltf_size ip = 0; ip < node.mesh->primitives_count; ++ip)
        {
            const cgltf_primitive& prim = node.mesh->primitives[ip];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            Primitive p;
            p.material = prim.material && mpmat.count(prim.material) ? mpmat[prim.material] : -1;
            const cgltf_accessor* apos = nullptr;
            for (cgltf_size ia = 0; ia < prim.attributes_count; ++ia)
                if (prim.attributes[ia].type == cgltf_attribute_type_position) apos = prim.attributes[ia].data;
            if (!apos) continue;
            const std::size_t cv = apos->count;
            p.pos.resize(cv);
            for (cgltf_size ia = 0; ia < prim.attributes_count; ++ia)
            {
                const cgltf_attribute& at = prim.attributes[ia];
                const cgltf_accessor* acc = at.data;
                if (acc->count != cv) continue;
                float v[4] = {0, 0, 0, 1};
                switch (at.type)
                {
                case cgltf_attribute_type_position:
                    for (std::size_t k = 0; k < cv; ++k) { cgltf_accessor_read_float(acc, k, v, 3); p.pos[k] = glm::vec3(v[0], v[1], v[2]); }
                    break;
                case cgltf_attribute_type_normal:
                    p.nrm.resize(cv);
                    for (std::size_t k = 0; k < cv; ++k) { cgltf_accessor_read_float(acc, k, v, 3); p.nrm[k] = glm::vec3(v[0], v[1], v[2]); }
                    break;
                case cgltf_attribute_type_texcoord:
                    if (at.index != 0) break;
                    p.uv.resize(cv);
                    for (std::size_t k = 0; k < cv; ++k) { cgltf_accessor_read_float(acc, k, v, 2); p.uv[k] = glm::vec2(v[0], v[1]); }
                    break;
                case cgltf_attribute_type_color:
                    if (at.index != 0) break;
                    p.color.resize(cv);
                    for (std::size_t k = 0; k < cv; ++k)
                    {
                        float c[4] = {1, 1, 1, 1};
                        cgltf_accessor_read_float(acc, k, c, cgltf_num_components(acc->type));
                        p.color[k] = glm::make_vec4(c);
                    }
                    break;
                case cgltf_attribute_type_joints:
                    if (at.index != 0 || !node.skin) break;
                    p.joints.resize(cv);
                    for (std::size_t k = 0; k < cv; ++k)
                    {
                        cgltf_uint u[4] = {};
                        cgltf_accessor_read_uint(acc, k, u, 4);
                        for (int j = 0; j < 4; ++j)
                        {
                            const cgltf_node* jn = u[j] < node.skin->joints_count ? node.skin->joints[u[j]] : nullptr;
                            p.joints[k][j] = uint16_t(jn && mpjoint.count(jn) ? mpjoint[jn] : 0);
                        }
                    }
                    break;
                case cgltf_attribute_type_weights:
                    if (at.index != 0 || !node.skin) break;
                    p.weights.resize(cv);
                    for (std::size_t k = 0; k < cv; ++k) { float w[4] = {}; cgltf_accessor_read_float(acc, k, w, 4); p.weights[k] = glm::make_vec4(w); }
                    break;
                default: break;
                }
            }
            if (prim.indices)
            {
                p.idx.resize(prim.indices->count);
                for (std::size_t k = 0; k < p.idx.size(); ++k) p.idx[k] = uint32_t(cgltf_accessor_read_index(prim.indices, k));
            }
            else
            {
                p.idx.resize(cv);
                for (std::size_t k = 0; k < cv; ++k) p.idx[k] = uint32_t(k);
            }
            p.idx.resize(p.idx.size() / 3 * 3);
            for (uint32_t i : p.idx)
                if (i >= cv) { error = "a mesh index outside its vertices"; return false; }
            if (!mesh.skinned)
            {
                for (glm::vec3& v : p.pos) v = glm::vec3(world * glm::vec4(v, 1.0f));
                for (glm::vec3& n : p.nrm) { n = nworld * n; const float l = glm::length(n); if (l > 0) n /= l; }
                if (glm::determinant(glm::mat3(world)) < 0)
                    for (std::size_t k = 0; k < p.idx.size(); k += 3) std::swap(p.idx[k + 1], p.idx[k + 2]);
            }
            mesh.prims.push_back(std::move(p));
        }
        if (!mesh.prims.empty()) sc.meshes.push_back(std::move(mesh));
    }
    // animations on the joints
    for (cgltf_size ia = 0; ia < data->animations_count; ++ia)
    {
        const cgltf_animation& an = data->animations[ia];
        Animation anim;
        anim.name = an.name ? an.name : ("animation" + std::to_string(ia));
        for (cgltf_size ic = 0; ic < an.channels_count; ++ic)
        {
            const cgltf_animation_channel& c = an.channels[ic];
            if (!c.target_node || !mpjoint.count(c.target_node) || !c.sampler) continue;
            Channel ch;
            ch.joint = mpjoint[c.target_node];
            if (c.target_path == cgltf_animation_path_type_translation) ch.path = 0;
            else if (c.target_path == cgltf_animation_path_type_rotation) ch.path = 1;
            else if (c.target_path == cgltf_animation_path_type_scale) ch.path = 2;
            else continue;
            const cgltf_accessor* in = c.sampler->input;
            const cgltf_accessor* out = c.sampler->output;
            const bool cubic = c.sampler->interpolation == cgltf_interpolation_type_cubic_spline;
            ch.t.resize(in->count);
            ch.v.resize(in->count);
            for (std::size_t k = 0; k < in->count; ++k)
            {
                cgltf_accessor_read_float(in, k, &ch.t[k], 1);
                float v[4] = {0, 0, 0, 1};
                cgltf_accessor_read_float(out, cubic ? 3 * k + 1 : k, v, ch.path == 1 ? 4 : 3);
                ch.v[k] = glm::make_vec4(v);
            }
            anim.ch.push_back(std::move(ch));
        }
        if (!anim.ch.empty()) sc.anims.push_back(std::move(anim));
    }
    if (sc.meshes.empty()) { error = "the file has no triangle meshes"; return false; }
    return true;
}

// ---- FBX

glm::mat4 matOf(const ufbx_matrix& m)
{
    glm::mat4 r(1.0f);
    for (int c = 0; c < 4; ++c) r[c] = glm::vec4(float(m.cols[c].x), float(m.cols[c].y), float(m.cols[c].z), c == 3 ? 1.0f : 0.0f);
    return r;
}

bool loadFbx(const fs::path& path, Scene& sc, std::string& error)
{
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_y_up;   // (as glTF: the game side converts from it)
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.generate_missing_normals = true;
    ufbx_error err{};
    const std::string spath = path.string();
    ufbx_scene* scene = ufbx_load_file(spath.c_str(), &opts, &err);
    if (!scene) { error = "FBX: " + std::string(err.description.data, err.description.length); return false; }
    struct Free { ufbx_scene* p; ~Free() { ufbx_free_scene(p); } } guard{scene};

    std::map<const ufbx_texture*, int> mptex;
    auto imageOf = [&](const ufbx_texture* t) -> int {
        if (!t) return -1;
        if (auto it = mptex.find(t); it != mptex.end()) return it->second;
        Image img;
        img.name = std::string(t->name.data, t->name.length);
        bool ok = false;
        if (t->content.size) ok = decodeImage(static_cast<const uint8_t*>(t->content.data), t->content.size, img);
        for (const ufbx_string* ps : {&t->filename, &t->relative_filename, &t->absolute_filename})
        {
            if (ok || !ps->length) continue;
            std::vector<uint8_t> ab;
            fs::path p = fs::u8path(std::string(ps->data, ps->length));
            if (p.is_relative()) p = path.parent_path() / p;
            if (readAll(p, ab) || readAll(path.parent_path() / p.filename(), ab)) ok = decodeImage(ab.data(), ab.size(), img);
        }
        const int i = ok ? int(sc.images.size()) : -1;
        if (ok) sc.images.push_back(std::move(img));
        mptex[t] = i;
        return i;
    };
    std::map<const ufbx_material*, int> mpmat;
    auto materialOf = [&](const ufbx_material* m) -> int {
        if (!m) return -1;
        if (auto it = mpmat.find(m); it != mpmat.end()) return it->second;
        Material mat;
        mat.name = std::string(m->name.data, m->name.length);
        const ufbx_material_map& bc = m->pbr.base_color.has_value ? m->pbr.base_color : m->fbx.diffuse_color;
        if (bc.has_value) mat.base = glm::vec4(float(bc.value_vec4.x), float(bc.value_vec4.y), float(bc.value_vec4.z), 1.0f);
        if (m->pbr.base_factor.has_value && m->pbr.base_factor.value_real > 0) mat.base = glm::vec4(glm::vec3(mat.base) * float(m->pbr.base_factor.value_real), 1.0f);
        mat.image = imageOf(m->pbr.base_color.texture ? m->pbr.base_color.texture : m->fbx.diffuse_color.texture);
        if (mat.image >= 0) mat.base = glm::vec4(1.0f);
        if (m->pbr.opacity.has_value && m->pbr.opacity.value_real < 1.0) { mat.base.a = float(m->pbr.opacity.value_real); mat.blend = true; }
        const int i = int(sc.materials.size());
        sc.materials.push_back(mat);
        mpmat[m] = i;
        return i;
    };
    std::map<const ufbx_node*, int> mpjoint;
    auto jointOf = [&](const ufbx_node* n, const ufbx_matrix& geometryToBone) -> int {
        if (auto it = mpjoint.find(n); it != mpjoint.end()) return it->second;
        Joint jt;
        jt.name = std::string(n->name.data, n->name.length);
        jt.local = matOf(n->node_to_parent);
        jt.inverseBind = matOf(geometryToBone);
        const int i = int(sc.joints.size());
        sc.joints.push_back(jt);
        mpjoint[n] = i;
        return i;
    };
    for (std::size_t in = 0; in < scene->nodes.count; ++in)
    {
        const ufbx_node* node = scene->nodes.data[in];
        const ufbx_mesh* m = node->mesh;
        if (!m) continue;
        Mesh mesh;
        mesh.name = std::string(node->name.data, node->name.length);
        const ufbx_skin_deformer* skin = m->skin_deformers.count ? m->skin_deformers.data[0] : nullptr;
        mesh.skinned = skin != nullptr;
        const glm::mat4 world = matOf(node->geometry_to_world);
        const glm::mat3 nworld = glm::transpose(glm::inverse(glm::mat3(world)));
        std::vector<int> ajointOfCluster;
        if (skin)
            for (std::size_t c = 0; c < skin->clusters.count; ++c)
            {
                const ufbx_skin_cluster* cl = skin->clusters.data[c];
                ajointOfCluster.push_back(cl->bone_node ? jointOf(cl->bone_node, cl->geometry_to_bone) : 0);
            }
        const std::size_t cmat = std::max<std::size_t>(1, m->materials.count);
        std::vector<Primitive> aprim(cmat);
        for (std::size_t im = 0; im < cmat; ++im)
            aprim[im].material = materialOf(im < m->materials.count ? m->materials.data[im] : nullptr);
        std::vector<uint32_t> tri(m->max_face_triangles * 3);
        for (std::size_t f = 0; f < m->faces.count; ++f)
        {
            const ufbx_face face = m->faces.data[f];
            const std::size_t im = m->face_material.count ? std::min<std::size_t>(m->face_material.data[f], cmat - 1) : 0;
            Primitive& p = aprim[im];
            const uint32_t ct = ufbx_triangulate_face(tri.data(), tri.size(), m, face);
            for (uint32_t k = 0; k < ct * 3; ++k)
            {
                const uint32_t ix = tri[k];
                glm::vec3 pos(float(ufbx_get_vertex_vec3(&m->vertex_position, ix).x), float(ufbx_get_vertex_vec3(&m->vertex_position, ix).y),
                              float(ufbx_get_vertex_vec3(&m->vertex_position, ix).z));
                glm::vec3 nrm(0, 0, 1);
                if (m->vertex_normal.exists)
                {
                    const ufbx_vec3 n = ufbx_get_vertex_vec3(&m->vertex_normal, ix);
                    nrm = glm::vec3(float(n.x), float(n.y), float(n.z));
                }
                if (!skin)
                {
                    pos = glm::vec3(world * glm::vec4(pos, 1.0f));
                    nrm = nworld * nrm;
                    const float l = glm::length(nrm);
                    if (l > 0) nrm /= l;
                }
                p.pos.push_back(pos);
                p.nrm.push_back(nrm);
                if (m->vertex_uv.exists)
                {
                    const ufbx_vec2 uv = ufbx_get_vertex_vec2(&m->vertex_uv, ix);
                    p.uv.push_back(glm::vec2(float(uv.x), 1.0f - float(uv.y)));   // (FBX: v up; glTF: v down)
                }
                if (m->vertex_color.exists)
                {
                    const ufbx_vec4 c = ufbx_get_vertex_vec4(&m->vertex_color, ix);
                    p.color.push_back(glm::vec4(float(c.x), float(c.y), float(c.z), float(c.w)));
                }
                if (skin)
                {
                    const uint32_t iv = m->vertex_indices.data[ix];
                    glm::u16vec4 j(0);
                    glm::vec4 w(0.0f);
                    if (iv < skin->vertices.count)
                    {
                        const ufbx_skin_vertex sv = skin->vertices.data[iv];
                        for (uint32_t q = 0; q < std::min<uint32_t>(sv.num_weights, 4); ++q)
                        {
                            const ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + q];
                            j[int(q)] = uint16_t(sw.cluster_index < ajointOfCluster.size() ? ajointOfCluster[sw.cluster_index] : 0);
                            w[int(q)] = float(sw.weight);
                        }
                    }
                    p.joints.push_back(j);
                    p.weights.push_back(w);
                }
                p.idx.push_back(uint32_t(p.pos.size() - 1));
            }
        }
        if (!skin && glm::determinant(glm::mat3(world)) < 0)
            for (Primitive& p : aprim)
                for (std::size_t k = 0; k + 2 < p.idx.size(); k += 3) std::swap(p.idx[k + 1], p.idx[k + 2]);
        for (Primitive& p : aprim)
            if (!p.idx.empty()) mesh.prims.push_back(std::move(p));
        if (!mesh.prims.empty()) sc.meshes.push_back(std::move(mesh));
    }
    // joint parents (nearest ancestor that is a joint); FBX animations are not read (glTF carries them)
    for (auto& [n, ij] : mpjoint)
        for (const ufbx_node* p = n->parent; p; p = p->parent)
            if (auto it = mpjoint.find(p); it != mpjoint.end()) { sc.joints[size_t(ij)].parent = it->second; break; }
    if (sc.meshes.empty()) { error = "the file has no meshes"; return false; }
    return true;
}

// ---- .glb writer

struct GlbBuilder {
    nlohmann::json j = nlohmann::json::object();
    std::vector<uint8_t> bin;
    std::map<std::string, int> mpacc;   // (accessorShared: the same contents and form once)
    int view(const void* pv, std::size_t cb, int target)
    {
        while (bin.size() % 4) bin.push_back(0);
        nlohmann::json v = {{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", cb}};
        if (target) v["target"] = target;
        const uint8_t* pb = static_cast<const uint8_t*>(pv);
        bin.insert(bin.end(), pb, pb + cb);
        j["bufferViews"].push_back(v);
        return int(j["bufferViews"].size()) - 1;
    }
    int accessor(const void* pv, std::size_t count, int comp, const char* type, int ncomp, int target, bool minmax = false, bool normalized = false)
    {
        const std::size_t cbComp = comp == 5126 || comp == 5125 ? 4 : comp == 5123 ? 2 : 1;
        const int bv = view(pv, count * cbComp * std::size_t(ncomp), target);
        nlohmann::json a = {{"bufferView", bv}, {"componentType", comp}, {"count", count}, {"type", type}};
        if (normalized) a["normalized"] = true;
        if (minmax && count)
        {
            const float* pf = static_cast<const float*>(pv);
            std::vector<float> mn(pf, pf + ncomp), mx(pf, pf + ncomp);
            for (std::size_t i = 0; i < count; ++i)
                for (int c = 0; c < ncomp; ++c) { mn[size_t(c)] = std::min(mn[size_t(c)], pf[i * size_t(ncomp) + size_t(c)]); mx[size_t(c)] = std::max(mx[size_t(c)], pf[i * size_t(ncomp) + size_t(c)]); }
            a["min"] = mn;
            a["max"] = mx;
        }
        j["accessors"].push_back(a);
        return int(j["accessors"].size()) - 1;
    }
    // an accessor of float data written once for the same contents (the animations' key times and constant values)
    int accessorShared(const float* pf, std::size_t count, const char* type, int ncomp, bool minmax)
    {
        std::string key(type);
        key += minmax ? "+" : "-";
        key.append(reinterpret_cast<const char*>(pf), count * std::size_t(ncomp) * sizeof(float));
        if (const auto it = mpacc.find(key); it != mpacc.end()) return it->second;
        const int i = accessor(pf, count, 5126, type, ncomp, 0, minmax);
        mpacc.emplace(std::move(key), i);
        return i;
    }
};

}  // namespace

bool loadImage(const fs::path& path, Image& img, std::string& error)
{
    std::vector<uint8_t> ab;
    if (!readAll(path, ab)) { error = "cannot read " + path.string(); return false; }
    if (!decodeImage(ab.data(), ab.size(), img)) { error = "not a PNG / JPEG image: " + path.string(); return false; }
    img.name = path.stem().string();
    return true;
}

std::vector<uint8_t> pngBytes(const Image& img)
{
    std::vector<uint8_t> out;
    if (img.w <= 0 || img.h <= 0 || img.rgba.size() < std::size_t(img.w) * std::size_t(img.h) * 4) return out;
    stbi_write_png_to_func([](void* ctx, void* data, int size) {
        auto* p = static_cast<std::vector<uint8_t>*>(ctx);
        p->insert(p->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    }, &out, img.w, img.h, 4, img.rgba.data(), img.w * 4);
    return out;
}

bool savePng(const fs::path& path, const Image& img, std::string& error)
{
    const std::vector<uint8_t> ab = pngBytes(img);
    std::ofstream out(path, std::ios::binary);
    if (ab.empty() || !out || !out.write(reinterpret_cast<const char*>(ab.data()), std::streamsize(ab.size()))) { error = "cannot write " + path.string(); return false; }
    return true;
}

bool load(const fs::path& path, Scene& scene, std::string& error)
{
    scene = Scene();
    const std::string ext = lower(path.extension().string());
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) { error = "no such file: " + path.string(); return false; }
    if (ext == ".glb" || ext == ".gltf") return loadGltf(path, scene, error);
    if (ext == ".fbx") return loadFbx(path, scene, error);
    error = "unknown model type " + ext + " (.glb, .gltf, .fbx)";
    return false;
}

bool saveGlb(const fs::path& path, const Scene& sc, std::string& error)
{
    GlbBuilder b;
    b.j["asset"] = {{"version", "2.0"}, {"generator", "SafeHouse map editor"}};
    b.j["scene"] = 0;
    b.j["scenes"] = nlohmann::json::array({nlohmann::json::object({{"nodes", nlohmann::json::array()}})});
    b.j["bufferViews"] = nlohmann::json::array();
    b.j["accessors"] = nlohmann::json::array();
    b.j["nodes"] = nlohmann::json::array();
    for (const Image& img : sc.images)
    {
        const std::vector<uint8_t> png = pngBytes(img);
        const int bv = b.view(png.data(), png.size(), 0);
        b.j["images"].push_back({{"name", img.name}, {"bufferView", bv}, {"mimeType", "image/png"}});
        b.j["textures"].push_back({{"source", int(b.j["images"].size()) - 1}, {"sampler", 0}});
    }
    if (!sc.images.empty()) b.j["samplers"] = nlohmann::json::array({{{"magFilter", 9729}, {"minFilter", 9987}}});
    for (const Material& m : sc.materials)
    {
        nlohmann::json pbr = {{"baseColorFactor", {m.base.r, m.base.g, m.base.b, m.base.a}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}};
        if (m.image >= 0) pbr["baseColorTexture"] = {{"index", m.image}};
        nlohmann::json jm = {{"name", m.name}, {"pbrMetallicRoughness", pbr}};
        if (m.blend) jm["alphaMode"] = "BLEND";
        if (m.doubleSided) jm["doubleSided"] = true;
        b.j["materials"].push_back(jm);
    }
    // the skeleton's nodes
    const int ijointNode0 = 0;
    for (const Joint& jt : sc.joints)
    {
        nlohmann::json n = {{"name", jt.name}};
        if (jt.local != glm::mat4(1.0f))
        {
            // (TRS: glTF forbids a matrix on an animated node; a mirrored frame keeps its sign in the x scale)
            glm::vec3 s(glm::length(glm::vec3(jt.local[0])), glm::length(glm::vec3(jt.local[1])), glm::length(glm::vec3(jt.local[2])));
            if (glm::determinant(glm::mat3(jt.local)) < 0) s.x = -s.x;
            glm::mat3 r(glm::vec3(jt.local[0]) / s.x, glm::vec3(jt.local[1]) / s.y, glm::vec3(jt.local[2]) / s.z);
            const glm::quat q = glm::normalize(glm::quat_cast(r));
            const glm::vec3 t(jt.local[3]);
            if (t != glm::vec3(0.0f)) n["translation"] = {t.x, t.y, t.z};
            if (std::fabs(q.w) < 1.0f - 1e-7f) n["rotation"] = {q.x, q.y, q.z, q.w};
            if (glm::any(glm::greaterThan(glm::abs(s - glm::vec3(1.0f)), glm::vec3(1e-6f)))) n["scale"] = {s.x, s.y, s.z};
        }
        if (!jt.extras.empty())
        {
            nlohmann::json e = nlohmann::json::parse(jt.extras, nullptr, false);
            if (e.is_object()) n["extras"] = e;
        }
        b.j["nodes"].push_back(n);
    }
    for (std::size_t i = 0; i < sc.joints.size(); ++i)
    {
        if (sc.joints[i].parent >= 0) b.j["nodes"][size_t(ijointNode0 + sc.joints[i].parent)]["children"].push_back(ijointNode0 + int(i));
        else b.j["scenes"][0]["nodes"].push_back(ijointNode0 + int(i));
    }
    bool fSkin = false;
    if (!sc.joints.empty())
    {
        std::vector<float> aibm;
        nlohmann::json joints = nlohmann::json::array();
        for (std::size_t i = 0; i < sc.joints.size(); ++i)
        {
            aibm.insert(aibm.end(), glm::value_ptr(sc.joints[i].inverseBind), glm::value_ptr(sc.joints[i].inverseBind) + 16);
            joints.push_back(ijointNode0 + int(i));
        }
        const int acc = b.accessor(aibm.data(), sc.joints.size(), 5126, "MAT4", 16, 0);
        nlohmann::json skin = {{"joints", joints}, {"inverseBindMatrices", acc}};
        int croot = 0, iroot = -1;
        for (std::size_t i = 0; i < sc.joints.size(); ++i) if (sc.joints[i].parent < 0) { ++croot; iroot = int(i); }
        if (croot == 1) skin["skeleton"] = ijointNode0 + iroot;
        b.j["skins"].push_back(skin);
        fSkin = true;
    }
    for (const Mesh& mesh : sc.meshes)
    {
        nlohmann::json jm = {{"name", mesh.name}, {"primitives", nlohmann::json::array()}};
        for (const Primitive& p : mesh.prims)
        {
            if (p.pos.empty() || p.idx.empty()) continue;
            nlohmann::json attrs;
            attrs["POSITION"] = b.accessor(p.pos.data(), p.pos.size(), 5126, "VEC3", 3, 34962, true);
            if (p.nrm.size() == p.pos.size()) attrs["NORMAL"] = b.accessor(p.nrm.data(), p.nrm.size(), 5126, "VEC3", 3, 34962);
            if (p.uv.size() == p.pos.size()) attrs["TEXCOORD_0"] = b.accessor(p.uv.data(), p.uv.size(), 5126, "VEC2", 2, 34962);
            if (p.color.size() == p.pos.size()) attrs["COLOR_0"] = b.accessor(p.color.data(), p.color.size(), 5126, "VEC4", 4, 34962);
            if (mesh.skinned && fSkin && p.joints.size() == p.pos.size() && p.weights.size() == p.pos.size())
            {
                attrs["JOINTS_0"] = b.accessor(p.joints.data(), p.joints.size(), 5123, "VEC4", 4, 34962);
                attrs["WEIGHTS_0"] = b.accessor(p.weights.data(), p.weights.size(), 5126, "VEC4", 4, 34962);
            }
            nlohmann::json jp = {{"attributes", attrs}, {"indices", b.accessor(p.idx.data(), p.idx.size(), 5125, "SCALAR", 1, 34963)}};
            if (p.material >= 0 && std::size_t(p.material) < sc.materials.size()) jp["material"] = p.material;
            jm["primitives"].push_back(jp);
        }
        if (jm["primitives"].empty()) continue;
        b.j["meshes"].push_back(jm);
        nlohmann::json n = {{"name", mesh.name}, {"mesh", int(b.j["meshes"].size()) - 1}};
        if (mesh.skinned && fSkin) n["skin"] = 0;
        b.j["nodes"].push_back(n);
        b.j["scenes"][0]["nodes"].push_back(int(b.j["nodes"].size()) - 1);
    }
    for (const Animation& an : sc.anims)
    {
        nlohmann::json ja = {{"name", an.name}, {"channels", nlohmann::json::array()}, {"samplers", nlohmann::json::array()}};
        for (const Channel& ch : an.ch)
        {
            if (ch.t.empty() || ch.t.size() != ch.v.size() || ch.joint < 0 || std::size_t(ch.joint) >= sc.joints.size()) continue;
            const int in = b.accessorShared(ch.t.data(), ch.t.size(), "SCALAR", 1, true);
            std::vector<float> av;
            for (const glm::vec4& v : ch.v)
            {
                const glm::vec4 u = ch.path == 1 && glm::length(v) > 0 ? glm::normalize(v) : v;   // (unit quaternions)
                for (int c = 0; c < (ch.path == 1 ? 4 : 3); ++c) av.push_back(u[c]);
            }
            const int out = b.accessorShared(av.data(), ch.v.size(), ch.path == 1 ? "VEC4" : "VEC3", ch.path == 1 ? 4 : 3, false);
            ja["samplers"].push_back({{"input", in}, {"output", out}, {"interpolation", "LINEAR"}});
            ja["channels"].push_back({{"sampler", int(ja["samplers"].size()) - 1},
                                      {"target", {{"node", ijointNode0 + ch.joint}, {"path", ch.path == 0 ? "translation" : ch.path == 1 ? "rotation" : "scale"}}}});
        }
        if (!ja["channels"].empty()) b.j["animations"].push_back(ja);
    }
    while (b.bin.size() % 4) b.bin.push_back(0);
    b.j["buffers"] = nlohmann::json::array({{{"byteLength", b.bin.size()}}});
    std::string js = b.j.dump();
    while (js.size() % 4) js.push_back(' ');
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); };
    u32(0x46546C67u); u32(2); u32(uint32_t(12 + 8 + js.size() + 8 + b.bin.size()));
    u32(uint32_t(js.size())); u32(0x4E4F534Au);
    out.insert(out.end(), js.begin(), js.end());
    u32(uint32_t(b.bin.size())); u32(0x004E4942u);
    out.insert(out.end(), b.bin.begin(), b.bin.end());
    std::ofstream f(path, std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()))) { error = "cannot write " + path.string(); return false; }
    return true;
}

}  // namespace cane::editor::model
