#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>

using byte = std::uint8_t;

enum BISK
{
    BISK_Nil = -1,
    BISK_File = 0,
    BISK_Mem = 1
};

class CBinaryInputStream
{
public:
    CBinaryInputStream();
    explicit CBinaryInputStream(const std::string& filePath);

    CBinaryInputStream(const CBinaryInputStream&) = delete;
    CBinaryInputStream& operator=(const CBinaryInputStream&) = delete;

    // Reads the whole file with one read. A world file is parsed with
    // millions of 1-4 byte reads, which cost far more through an ifstream.
    void OpenFile(const std::string& filePath);
    void OpenMemory(int cb, const void* pv);

    [[nodiscard]] bool FIsOpen() const { return m_bisk != BISK_Nil; }

    void ReadBytes(void* destination, std::size_t cb)
    {
        // m_ib never exceeds m_cb, and m_cb is 0 while closed.
        if (cb != 0 && destination != nullptr && cb <= m_cb - m_ib)
        {
            std::memcpy(destination, m_pb + m_ib, cb);
            m_ib += cb;
            return;
        }

        ReadBytesSlow(destination, cb);
    }

    // Copies up to cb bytes at absolute offset ib without moving the read
    // position. Bytes past the end are left untouched, like a short read.
    std::size_t CbCopyAt(std::size_t ib, void* destination, std::size_t cb) const;

    void Align(int n);

    [[nodiscard]] std::size_t GetOffset();
	[[nodiscard]] std::size_t GetSize();
	[[nodiscard]] std::size_t Remaining();
	void Seek(std::size_t offset);

    byte U8Read() { byte value{}; ReadBytes(&value, sizeof(value)); return value; }
    uint16_t U16Read() { uint16_t value{}; ReadBytes(&value, sizeof(value)); return value; }
    uint32_t U32Read() { uint32_t value{}; ReadBytes(&value, sizeof(value)); return value; }

    int8_t S8Read() { int8_t value{}; ReadBytes(&value, sizeof(value)); return value; }
    int16_t S16Read() { int16_t value{}; ReadBytes(&value, sizeof(value)); return value; }
    int32_t S32Read() { int32_t value{}; ReadBytes(&value, sizeof(value)); return value; }

    float F32Read() { float value{}; ReadBytes(&value, sizeof(value)); return value; }

    glm::vec2 ReadVector2();
    glm::vec3 ReadVector();
    glm::vec4 ReadVector4();

    glm::mat3 ReadMatrix();
    glm::mat4 ReadMatrix4();

    std::shared_ptr<char[]> ReadStringAlloc();
    std::string ReadStringSw();

    std::string ReadFixedString32NoAlloc();
    std::shared_ptr<char[]> ReadFixedString32();

    void Close();

    ~CBinaryInputStream();

    BISK m_bisk = BISK_Nil;

    // OpenMemory uses a non-owning view of its buffer; OpenFile views m_abFile.
    const std::uint8_t* m_pb = nullptr;
    std::size_t m_cb = 0;
    std::size_t m_ib = 0;

private:
    void ReadBytesSlow(void* destination, std::size_t cb);

    std::vector<std::uint8_t> m_abFile;
};
