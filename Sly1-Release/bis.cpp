#include "bis.h"
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
    if (n <= 1)
        return;

    if (m_bisk == BISK_Nil)
    {
        throw std::runtime_error(
            "CBinaryInputStream is not open");
    }

    const std::size_t alignment =
        static_cast<std::size_t>(n);

    if (m_ib > m_cb)
    {
        throw std::runtime_error(
            "Binary memory stream position exceeds its size");
    }

    const std::size_t remainder =
        m_ib % alignment;

    const std::size_t padding =
        remainder == 0
        ? 0
        : alignment - remainder;

    if (padding > m_cb - m_ib)
    {
        throw std::runtime_error(
            "Binary memory stream alignment exceeded buffer size");
    }

    m_ib += padding;
}

glm::vec2 CBinaryInputStream::ReadVector2()
{
    // Separate statements guarantee stream read order.
    const float x = F32Read();
    const float y = F32Read();

    return glm::vec2(x, y);
}

glm::vec3 CBinaryInputStream::ReadVector()
{
    // Separate statements guarantee stream read order.
    const float x = F32Read();
    const float y = F32Read();
    const float z = F32Read();

    return glm::vec3(x, y, z);
}

glm::vec4 CBinaryInputStream::ReadVector4()
{
    // Separate statements guarantee stream read order.
    const float x = F32Read();
    const float y = F32Read();
    const float z = F32Read();
    const float w = F32Read();

    return glm::vec4(x, y, z, w);
}

glm::mat3 CBinaryInputStream::ReadMatrix()
{
    // The BRX MATRIX3 contains three consecutive VECTOR values:
    // 3 columns * 3 floats = 36 bytes.
    const glm::vec3 column0 = ReadVector();
    const glm::vec3 column1 = ReadVector();
    const glm::vec3 column2 = ReadVector();

    glm::mat3 matrix(1.0f);

    matrix[0] = column0;
    matrix[1] = column1;
    matrix[2] = column2;

    return matrix;
}

glm::mat4 CBinaryInputStream::ReadMatrix4()
{
    // The BRX MATRIX4 stores four consecutive three-float vectors:
    // 4 columns * 3 floats = 48 bytes.
    const glm::vec3 column0 = ReadVector();
    const glm::vec3 column1 = ReadVector();
    const glm::vec3 column2 = ReadVector();
    const glm::vec3 column3 = ReadVector();

    glm::mat4 matrix(1.0f);

    matrix[0] = glm::vec4(column0, 0.0f);
    matrix[1] = glm::vec4(column1, 0.0f);
    matrix[2] = glm::vec4(column2, 0.0f);
    matrix[3] = glm::vec4(column3, 1.0f);

    return matrix;
}

std::shared_ptr<char[]>
CBinaryInputStream::ReadStringAlloc()
{
    const uint16_t cch = U16Read();

    std::shared_ptr<char[]> text(
        new char[static_cast<std::size_t>(cch) + 1]());

    if (cch > 0)
    {
        ReadBytes(
            text.get(),
            static_cast<std::size_t>(cch));
    }

    text[cch] = '\0';

    return text;
}

std::string CBinaryInputStream::ReadStringSw()
{
    const uint16_t cch = U16Read();

    std::string text(static_cast<std::size_t>(cch), '\0');

    if (cch > 0)
        ReadBytes(text.data(), static_cast<std::size_t>(cch));

    return text;
}

std::string CBinaryInputStream::ReadFixedString32NoAlloc()
{
    char buffer[32]{};

    ReadBytes(
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

    ReadBytes(
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

std::size_t CBinaryInputStream::GetSize()
{
    // Both kinds view a buffer: OpenFile reads the whole file into m_abFile.
    return m_bisk == BISK_Nil ? 0 : m_cb;
}

std::size_t CBinaryInputStream::Remaining()
{
    return m_bisk == BISK_Nil ? 0 : m_cb - m_ib;
}

void CBinaryInputStream::Seek(std::size_t offset)
{
    if (m_bisk == BISK_Nil)
        throw std::runtime_error("CBinaryInputStream is not open");
    if (offset > m_cb)
        throw std::runtime_error("Binary input seek exceeded the stream size");
    m_ib = offset;
}
