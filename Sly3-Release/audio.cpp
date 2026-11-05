#include "audio.h"

// FAudio on every platform (on Windows too, over SDL2), so the mixer behaves the same everywhere.
#include "../platform/audio_linux.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

IXAudio2* g_pxaudio2 = nullptr;
IXAudio2MasteringVoice* g_pmastervoice = nullptr;
std::recursive_mutex g_mtxSound;

namespace
{
bool s_fAudioFailed = false;
}

bool FEnsureAudio()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (g_pxaudio2) return true;
    if (s_fAudioFailed) return false;
    if (FAILED(XAudio2Create(&g_pxaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR)))
    {
        g_pxaudio2 = nullptr;
        s_fAudioFailed = true;
        std::fprintf(stderr, "audio: no audio engine\n");
        return false;
    }
    if (FAILED(g_pxaudio2->CreateMasteringVoice(&g_pmastervoice)))
    {
        g_pxaudio2->Release();
        g_pxaudio2 = nullptr;
        g_pmastervoice = nullptr;
        s_fAudioFailed = true;
        std::fprintf(stderr, "audio: no audio device\n");
        return false;
    }
    g_pmastervoice->SetVolume(kOutputGain);
    return true;
}

namespace { float s_gOutput = 1.0f; bool s_fMute = false; }
void SetOutputVolume(float g)
{
    s_gOutput = std::clamp(g, 0.0f, 1.0f);
    if (FEnsureAudio()) g_pmastervoice->SetVolume(s_fMute ? 0.0f : s_gOutput * kOutputGain);
}
void SetOutputMute(bool fMute)
{
    if (s_fMute == fMute) return;
    s_fMute = fMute;
    if (g_pmastervoice) g_pmastervoice->SetVolume(s_fMute ? 0.0f : s_gOutput * kOutputGain);
}

void ShutdownAudio()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (g_pmastervoice) g_pmastervoice->DestroyVoice();
    g_pmastervoice = nullptr;
    if (g_pxaudio2) g_pxaudio2->Release();
    g_pxaudio2 = nullptr;
}

void SetVoiceLevels(IXAudio2SourceVoice* pvoice, int cchSource, float gLeft, float gRight)
{
    if (!pvoice || !g_pmastervoice) return;
    // The mastering voice is stereo (audio_linux.h opens 2 channels at 48 kHz).
    if (cchSource == 1)
    {
        const float matrix[2] = {gLeft, gRight};
        pvoice->SetOutputMatrix(g_pmastervoice, 1, 2, matrix);
    }
    else
    {
        // Row-major [destination][source]: left from the left channel, right from the right one.
        const float matrix[4] = {gLeft, 0.0f, 0.0f, gRight};
        pvoice->SetOutputMatrix(g_pmastervoice, 2, 2, matrix);
    }
}

bool CPcmStream::FOpen(int cch, int rate, float frqMax)
{
    Close();
    if (!FEnsureAudio() || cch < 1 || cch > 2 || rate <= 0) return false;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<uint16_t>(cch);
    format.nSamplesPerSec = static_cast<uint32_t>(rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<uint16_t>(2 * cch);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    if (FAILED(g_pxaudio2->CreateSourceVoice(&m_pvoice, &format, 0, frqMax)) || !m_pvoice)
    {
        m_pvoice = nullptr;
        return false;
    }
    m_cch = cch;
    m_rate = rate;
    m_iab = 0;
    SetLevels(1.0f, 1.0f);
    return true;
}

void CPcmStream::Close()
{
    if (!m_pvoice) return;
    m_pvoice->Stop();
    m_pvoice->FlushSourceBuffers();
    m_pvoice->DestroyVoice();  // waits for the mixer: the buffers are free after it
    m_pvoice = nullptr;
}

bool CPcmStream::FPush(const int16_t* asample, std::size_t cframe, bool fEnd)
{
    if (!m_pvoice || CBufferQueued() >= kcBuffer) return false;
    std::vector<int16_t>& ab = m_aab[m_iab];
    m_iab = (m_iab + 1) % kcBuffer;
    ab.assign(asample, asample + cframe * static_cast<std::size_t>(m_cch));
    if (ab.empty()) ab.assign(static_cast<std::size_t>(m_cch), 0);  // an end marker needs a sample
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(ab.size() * sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(ab.data());
    buffer.Flags = fEnd ? XAUDIO2_END_OF_STREAM : 0;
    return SUCCEEDED(m_pvoice->SubmitSourceBuffer(&buffer));
}

int CPcmStream::CBufferQueued() const
{
    if (!m_pvoice) return 0;
    XAUDIO2_VOICE_STATE state{};
    m_pvoice->GetState(&state);
    return static_cast<int>(state.BuffersQueued);
}

uint64_t CPcmStream::CFramePlayed() const
{
    if (!m_pvoice) return 0;
    XAUDIO2_VOICE_STATE state{};
    m_pvoice->GetState(&state);
    return state.SamplesPlayed;
}

void CPcmStream::Start()
{
    if (m_pvoice) m_pvoice->Start();
}

void CPcmStream::Stop()
{
    if (m_pvoice) m_pvoice->Stop();
}

void CPcmStream::SetLevels(float gLeft, float gRight) { SetVoiceLevels(m_pvoice, m_cch, gLeft, gRight); }

void CPcmStream::SetMatrix(const float matrix[4])
{
    if (!m_pvoice || !g_pmastervoice) return;
    if (m_cch == 1)
    {
        const float m[2] = {matrix[0], matrix[2]};
        m_pvoice->SetOutputMatrix(g_pmastervoice, 1, 2, m);
    }
    else
        m_pvoice->SetOutputMatrix(g_pmastervoice, 2, 2, matrix);
}

void CPcmStream::SetFrequencyRatio(float r)
{
    if (m_pvoice) m_pvoice->SetFrequencyRatio(std::clamp(r, XAUDIO2_MIN_FREQ_RATIO, 4.0f));
}

// orig: (SPU2 hardware) the ADPCM decoder; Sly1-Release sound.cpp DecodeSpuAdpcm
void DecodeAdpcmBlock(const uint8_t* pb, int16_t* asample, int* ph1, int* ph2)
{
    // The SPU2 takes both header nibbles as 4-bit fields; filters above 4 use the zero tail of its table.
    static constexpr int s_acoef[16][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    const int shift = pb[0] & 0x0F;
    const int filter = (pb[0] >> 4) & 0x0F;
    int h1 = *ph1, h2 = *ph2;
    for (int i = 0; i < 28; ++i)
    {
        int nibble = (i & 1) ? pb[2 + i / 2] >> 4 : pb[2 + i / 2] & 0x0F;
        if (nibble & 8) nibble -= 16;
        int v = (nibble << 12) >> shift;
        v += (s_acoef[filter][0] * h1 + s_acoef[filter][1] * h2 + 32) >> 6;
        v = std::clamp(v, -32768, 32767);
        h2 = h1;
        h1 = v;
        asample[i] = static_cast<int16_t>(v);
    }
    *ph1 = h1;
    *ph2 = h2;
}
