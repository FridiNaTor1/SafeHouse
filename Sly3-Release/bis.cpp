#include "bis.h"
#include "geom.h"
#include <algorithm>
#include "../platform/platform.h"

CBinaryInputStream::CBinaryInputStream()
{
}

CBinaryInputStream::CBinaryInputStream(const std::string& filePath)
{
    OpenFile(filePath);
}

void CBinaryInputStream::OpenFile(const std::string& filePath)
{
    Close();

    std::ifstream file(cane::path(filePath), std::ios::in | std::ios::binary | std::ios::ate);

    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open binary input file: " + filePath);
    }

    const std::streamoff cbFile = file.tellg();

    if (cbFile < 0 || !file.seekg(0, std::ios::beg))
    {
        throw std::runtime_error("Failed to query binary input file size: " + filePath);
    }

    m_abFile.resize(static_cast<std::size_t>(cbFile));

    if (cbFile > 0 &&
        (!file.read(reinterpret_cast<char*>(m_abFile.data()), static_cast<std::streamsize>(cbFile)) ||
            file.gcount() != static_cast<std::streamsize>(cbFile)))
    {
        std::vector<std::uint8_t>().swap(m_abFile);
        throw std::runtime_error("Failed to read binary input file: " + filePath);
    }

    m_bisk = BISK_File;
    m_pb = m_abFile.data();
    m_cb = m_abFile.size();
    m_ib = 0;
}

void CBinaryInputStream::OpenBuffer(std::vector<std::uint8_t>&& ab)
{
    Close();
    m_abFile = std::move(ab);
    m_bisk = BISK_Mem;
    m_pb = m_abFile.data();
    m_cb = m_abFile.size();
    m_ib = 0;
}

void CBinaryInputStream::OpenMemory(int cb, const void* pv)
{
    Close();

    if (cb < 0)
    {
        throw std::invalid_argument("CBinaryInputStream::OpenMemory received a negative size");
    }

    if (cb > 0 && !pv)
    {
        throw std::invalid_argument("CBinaryInputStream::OpenMemory received a null buffer");
    }

    m_bisk = BISK_Mem;
    m_pb = static_cast<const std::uint8_t*>(pv);
    m_cb = static_cast<std::size_t>(cb);
    m_ib = 0;
}

void CBinaryInputStream::ReadBytesSlow(
    void* destination,
    std::size_t cb)
{
    if (cb == 0)
        return;

    if (!destination)
    {
        throw std::invalid_argument("CBinaryInputStream::ReadBytes received a null destination");
    }

    switch (m_bisk)
    {
        case BISK_File:
            throw std::runtime_error("Unexpected end of binary input file");

        case BISK_Mem:
            throw std::runtime_error("Unexpected end of binary memory stream");

        default:
            throw std::runtime_error("CBinaryInputStream is not open");
    }
}

std::size_t CBinaryInputStream::CbCopyAt(std::size_t ib, void* destination, std::size_t cb) const
{
    if (ib >= m_cb || cb == 0 || destination == nullptr)
        return 0;

    const std::size_t cbCopy = (std::min)(cb, m_cb - ib);
    std::memcpy(destination, m_pb + ib, cbCopy);
    return cbCopy;
}

std::size_t CBinaryInputStream::GetOffset()
{
    if (m_bisk == BISK_Nil)
    {
        throw std::runtime_error(
            "CBinaryInputStream is not open");
    }

    return m_ib;
}

void CBinaryInputStream::Align(int n)
{
    // orig 0x117988
    Trace("Align");
    if (n <= 1)
        return;

    if (m_bisk == BISK_Nil)
        throw std::runtime_error("CBinaryInputStream is not open");

    const std::size_t alignment = static_cast<std::size_t>(n);
    const std::size_t remainder = m_ib % alignment;
    const std::size_t padding = remainder == 0 ? 0 : alignment - remainder;

    if (padding > m_cb - m_ib)
        throw std::runtime_error("Binary memory stream alignment exceeded buffer size");

    m_ib += padding;
}

void CBinaryInputStream::RawRead(void* pv, std::size_t cb)
{
    if (cb == 0) return;
    if (pv) { ReadRaw(pv, cb); return; }
    if (cb > m_cb - m_ib) throw std::runtime_error("Unexpected end of binary memory stream (skip)");
    m_ib += cb;
}

glm::vec2 CBinaryInputStream::ReadVector2()
{
    const float x = F32Read();
    const float y = F32Read();
    return glm::vec2(x, y);
}

glm::vec3 CBinaryInputStream::ReadVector()
{
    Trace("vec");
    float a[3];
    RawRead(a, sizeof(a));
    return glm::vec3(a[0], a[1], a[2]);
}

glm::vec4 CBinaryInputStream::ReadVector4()
{
    Trace("mat4");
    float a[4];
    RawRead(a, sizeof(a));
    return glm::vec4(a[0], a[1], a[2], a[3]);
}

glm::mat3 CBinaryInputStream::ReadMatrix()
{
    // Three consecutive VECTORs (rows of the original's MATRIX3, glm columns).
    Trace("vec4");
    float a[9];
    RawRead(a, sizeof(a));
    glm::mat3 m(1.0f);
    for (int i = 0; i < 3; ++i) m[i] = glm::vec3(a[3 * i], a[3 * i + 1], a[3 * i + 2]);
    return m;
}

glm::mat4 CBinaryInputStream::ReadMatrix4()
{
    // A MATRIX3 then the translation; w column (0, 0, 0, 1).
    Trace("mat");
    float a[12];
    RawRead(a, sizeof(a));
    glm::mat4 m(1.0f);
    for (int i = 0; i < 3; ++i) m[i] = glm::vec4(a[3 * i], a[3 * i + 1], a[3 * i + 2], 0.0f);
    m[3] = glm::vec4(a[9], a[10], a[11], 1.0f);
    return m;
}

void CBinaryInputStream::ReadGeom(GEOM* pgeom)
{
    // orig 0x117FD0: positions, surfaces (three position indices each, a plane from them, four floats, then
    // the surface's edges), edges.
    Trace("GEOM");
    const int cpos = Raw<uint16_t>();
    pgeom->apos.resize(cpos);
    for (int i = 0; i < cpos; ++i)
    {
        float a[3];
        RawRead(a, sizeof(a));
        pgeom->apos[i] = glm::vec3(a[0], a[1], a[2]);
    }
    const int csurf = Raw<uint16_t>();
    pgeom->asurf.assign(csurf, SURF{});
    const int cedge = Raw<uint16_t>();
    pgeom->aedge.assign(cedge, EDGE{});
    int iedge = 0;
    for (int isurf = 0; isurf < csurf; ++isurf)
    {
        SURF& surf = pgeom->asurf[isurf];
        const int i0 = Raw<uint16_t>(), i1 = Raw<uint16_t>(), i2 = Raw<uint16_t>();
        auto pos = [&](int i) { return i < cpos ? pgeom->apos[i] : glm::vec3(0.0f); };
        // orig 0x119308: the surface's plane
        const glm::vec3 n = glm::cross(pos(i1) - pos(i0), pos(i2) - pos(i0));
        const float len = glm::length(n);
        surf.normal = len > 0.0f ? n / len : glm::vec3(0.0f);
        surf.gDot = glm::dot(surf.normal, pos(i0));
        surf.ipos0 = i0;
        for (float& g : surf.ag) g = Raw<float>();
        const int c = RawU8();
        for (int j = 0; j < c; ++j)
        {
            const int ie = iedge++;
            if (ie >= cedge) throw std::runtime_error("ReadGeom: more edges than declared");
            EDGE& edge = pgeom->aedge[ie];
            edge.ipos0 = Raw<uint16_t>();
            edge.ipos1 = Raw<uint16_t>();
            edge.isurf = static_cast<uint16_t>(isurf);
            edge.isurfOther = Raw<uint16_t>();
            if (j == 0) surf.iedgeFirst = ie;
            else pgeom->aedge[ie - 1].iedgeNextInSurf = ie;
            if (edge.isurfOther < csurf)
            {
                SURF& surfOther = pgeom->asurf[edge.isurfOther];
                edge.iedgeNextOfOther = surfOther.iedgeOfOthers;
                surfOther.iedgeOfOthers = ie;
            }
        }
    }
}

void CBinaryInputStream::ReadBsp(const GEOM* pgeom, BSP* pbsp)
{
    // orig 0x118220
    Trace("BSP");
    pbsp->c = Raw<uint32_t>();
    pbsp->x4 = Raw<uint32_t>();
    pbsp->x8 = Raw<uint32_t>();
    pbsp->abspn.assign(pbsp->c, BSPN{});
    for (uint32_t i = 0; i < pbsp->c; ++i)
    {
        BSPN& bspn = pbsp->abspn[i];
        const uint16_t isurf = Raw<uint16_t>();
        if (isurf == 0xFFFF) continue;  // the sentinel surface, no subtrees
        bspn.isurf = isurf;
        (void)pgeom;
        const uint32_t iNeg = Raw<uint32_t>();
        if (iNeg != 0xFFFFFFFFu) bspn.ibspNeg = static_cast<int>(iNeg);
        const uint32_t iPos = Raw<uint32_t>();
        if (iPos != 0xFFFFFFFFu) bspn.ibspPos = static_cast<int>(iPos);
    }
}

std::shared_ptr<char[]>
CBinaryInputStream::ReadStringAlloc()
{
    Trace("str");
    const uint16_t cch = Raw<uint16_t>();
    std::shared_ptr<char[]> text(new char[static_cast<std::size_t>(cch) + 1]());
    RawRead(text.get(), cch);
    text[cch] = '\0';
    return text;
}

std::string CBinaryInputStream::ReadStringSw()
{
    Trace("str");
    const uint16_t cch = Raw<uint16_t>();
    std::string text(static_cast<std::size_t>(cch), '\0');
    RawRead(text.data(), cch);
    return text;
}

std::string CBinaryInputStream::ReadSkippedStringSw()
{
    Trace("118420");
    const uint16_t cch = Raw<uint16_t>();
    std::string text(static_cast<std::size_t>(cch), '\0');
    RawRead(text.data(), cch);
    return text;
}

void CBinaryInputStream::SkipStringSw()
{
    Trace("118420");
    const uint16_t cch = Raw<uint16_t>();
    RawRead(nullptr, cch);
}

std::string CBinaryInputStream::ReadFixedString32NoAlloc()
{
    char buffer[32]{};

    RawRead(
        buffer,
        sizeof(buffer));

    std::size_t length = 0;

    while (length < sizeof(buffer) &&
        buffer[length] != '\0')
    {
        ++length;
    }

    return std::string(buffer, length);
}

std::shared_ptr<char[]> CBinaryInputStream::ReadFixedString32()
{
    char buffer[32]{};

    RawRead(
        buffer,
        sizeof(buffer));

    if (buffer[0] == '\0')
        return nullptr;

    std::shared_ptr<char[]> text(new char[33]());

    std::memcpy(
        text.get(),
        buffer,
        sizeof(buffer));

    text[32] = '\0';

    return text;
}

void CBinaryInputStream::Close()
{
    std::vector<std::uint8_t>().swap(m_abFile);

    m_bisk = BISK_Nil;
    m_pb = nullptr;
    m_cb = 0;
    m_ib = 0;
}

CBinaryInputStream::~CBinaryInputStream()
{
    Close();
}