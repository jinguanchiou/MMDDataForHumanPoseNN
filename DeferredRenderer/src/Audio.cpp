#include "Audio.h"

#include <xaudio2.h>
#include <wrl/client.h>
// Media Foundation: used to decode compressed formats (e.g. the rabbit-hole .mp3) to raw PCM
// so they can be played through the same XAudio2 path as the .wav BGM.
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <combaseapi.h>
#include <atomic>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cwctype>
#include <thread>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace dr {

struct Audio::Impl {
    ComPtr<IXAudio2>          xa;
    IXAudio2MasteringVoice*   master = nullptr;
    IXAudio2SourceVoice*      src    = nullptr;
    WAVEFORMATEX              wfx{};
    std::vector<std::uint8_t> pcm;        // kept alive — the source voice references it
    // These are read by the animation worker thread (PositionSeconds/Playing) while the main
    // thread writes them (Play/Pause/Restart), so they are atomic.
    std::atomic<bool>          started{ false };
    std::atomic<bool>          playing{ false };   // started and not paused
    std::atomic<std::uint64_t> baseline{ 0 };      // SamplesPlayed at the last (re)start = loop origin
    float                      lengthSec = 0.0f;   // full decoded buffer length (seconds)
    float                      loopSec   = 0.0f;   // effective loop period (0 = whole buffer)

    float EffectiveLoop() const { return (loopSec > 0.0f && loopSec < lengthSec) ? loopSec : lengthSec; }

    ~Impl() {
        if (src)    { src->Stop(0); src->FlushSourceBuffers(); src->DestroyVoice(); src = nullptr; }
        if (master) { master->DestroyVoice(); master = nullptr; }
        // xa is released by ComPtr
    }

    void Submit() {
        XAUDIO2_BUFFER b{};
        b.pAudioData = pcm.data();
        b.AudioBytes = static_cast<UINT32>(pcm.size());
        b.Flags      = XAUDIO2_END_OF_STREAM;
        // Loop the whole clip by default; if a shorter loop period was requested (e.g. clamp the
        // music to the dance length so they stay equal), loop just [0, loopSec).
        if (loopSec > 0.0f && loopSec < lengthSec && wfx.nSamplesPerSec) {
            b.LoopBegin  = 0;
            b.LoopLength = static_cast<UINT32>(loopSec * static_cast<float>(wfx.nSamplesPerSec));
        }
        b.LoopCount  = XAUDIO2_LOOP_INFINITE;   // loop forever
        src->SubmitSourceBuffer(&b);
    }
};

Audio::Audio() : m_impl(std::make_unique<Impl>()) {}
Audio::~Audio() = default;

bool Audio::Init() {
    if (m_impl->xa) return true;
    HRESULT hr = XAudio2Create(m_impl->xa.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[Audio] XAudio2Create failed (0x%08lX)\n", static_cast<unsigned long>(hr));
        return false;
    }
    hr = m_impl->xa->CreateMasteringVoice(&m_impl->master);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[Audio] CreateMasteringVoice failed (0x%08lX)\n", static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

// RIFF/WAVE chunk scanner: walk every chunk (the file may carry a LIST/INFO chunk between
// 'fmt ' and 'data', so we can't assume 'data' follows 'fmt ' directly).
static bool ParseWav(const std::wstring& path, WAVEFORMATEX& wfx, std::vector<std::uint8_t>& pcm) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12) return false;
    if (std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) return false;

    bool haveFmt = false, haveData = false;
    size_t off = 12;
    while (off + 8 <= b.size()) {
        char id[4];
        std::memcpy(id, b.data() + off, 4);
        std::uint32_t sz;
        std::memcpy(&sz, b.data() + off + 4, 4);
        const size_t body = off + 8;
        if (body + sz > b.size()) sz = static_cast<std::uint32_t>(b.size() - body);

        if (std::memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            std::memset(&wfx, 0, sizeof(wfx));
            std::memcpy(&wfx, b.data() + body, 16);  // first 16 bytes == WAVEFORMATEX minus cbSize
            wfx.cbSize = 0;
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            pcm.assign(b.begin() + body, b.begin() + body + sz);
            haveData = true;
        }
        off = body + sz + (sz & 1u);   // chunks are word-aligned
    }
    return haveFmt && haveData;
}

// Decode body. MUST run in the COM MTA: a synchronous Source Reader created on an STA thread
// deadlocks/faults (no message pump for its async internals), and the app's main thread is in
// an STA via WIC/DirectXTK texture loading. DecodeMediaFoundation runs this on its own thread.
static bool DecodeMF_OnMtaThread(const std::wstring& path, WAVEFORMATEX& wfx,
                                 std::vector<std::uint8_t>& pcm) {
    const HRESULT coHr  = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // fresh thread → real MTA
    const bool coInited = SUCCEEDED(coHr);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        if (coInited) CoUninitialize();
        return false;
    }

    // Stream selectors are signed enum constants; cast once so /W4 doesn't flag signed/unsigned.
    const DWORD kAllStreams  = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
    const DWORD kAudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

    bool ok = false;
    {
        ComPtr<IMFSourceReader> reader;
        HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader.GetAddressOf());
        if (SUCCEEDED(hr)) {
            reader->SetStreamSelection(kAllStreams, FALSE);
            reader->SetStreamSelection(kAudioStream, TRUE);

            // Ask the reader to give us uncompressed 16-bit PCM; it inserts the decoder.
            ComPtr<IMFMediaType> want;
            if (SUCCEEDED(MFCreateMediaType(want.GetAddressOf()))) {
                want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                want->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_PCM);
                hr = reader->SetCurrentMediaType(kAudioStream, nullptr, want.Get());
            }

            ComPtr<IMFMediaType> actual;
            WAVEFORMATEX* pwfx = nullptr; UINT32 wfxSize = 0;
            if (SUCCEEDED(hr) &&
                SUCCEEDED(reader->GetCurrentMediaType(kAudioStream, actual.GetAddressOf())) &&
                SUCCEEDED(MFCreateWaveFormatExFromMFMediaType(actual.Get(), &pwfx, &wfxSize)) && pwfx) {
                wfx.cbSize = 0;
                CoTaskMemFree(pwfx);

                pcm.clear();
                for (;;) {
                    DWORD flags = 0;
                    ComPtr<IMFSample> sample;
                    hr = reader->ReadSample(kAudioStream, 0, nullptr,
                                            &flags, nullptr, sample.GetAddressOf());
                    if (FAILED(hr)) break;
                    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                    if (!sample) continue;   // gap / format-change tick with no data
                    ComPtr<IMFMediaBuffer> buf;
                    if (FAILED(sample->ConvertToContiguousBuffer(buf.GetAddressOf()))) continue;
                    BYTE* data = nullptr; DWORD cur = 0;
                    if (SUCCEEDED(buf->Lock(&data, nullptr, &cur))) {
                        pcm.insert(pcm.end(), data, data + cur);
                        buf->Unlock();
                    }
                }
                ok = !pcm.empty();
            }
        }
    }

    MFShutdown();
    if (coInited) CoUninitialize();
    return ok;
}

// Decode any Media-Foundation-supported file (mp3, m4a/aac, wma, flac, also wav) to 16-bit PCM
// in its native channel count / sample rate. Runs the actual decode on a dedicated MTA thread
// (see above) and blocks until it finishes. The decoded interleaved samples land in `pcm` and
// the negotiated format in `wfx`, so the XAudio2 path is identical to the WAV case afterwards.
static bool DecodeMediaFoundation(const std::wstring& path, WAVEFORMATEX& wfx,
                                  std::vector<std::uint8_t>& pcm) {
    bool ok = false;
    std::thread worker([&] { ok = DecodeMF_OnMtaThread(path, wfx, pcm); });
    worker.join();
    return ok;
}

bool Audio::Load(const std::wstring& wav) {
    if (!m_impl->xa && !Init()) return false;

    // Destroy any existing source voice BEFORE touching m_impl->pcm. A playing voice references
    // that buffer directly; decoding the new clip reallocates it, and freeing the old storage
    // underneath a live voice is a use-after-free (heap corruption / crash). Switching clips
    // calls Load while the previous clip is still playing, so this ordering is essential.
    if (m_impl->src) {
        m_impl->src->Stop(0);
        m_impl->src->FlushSourceBuffers();
        m_impl->src->DestroyVoice();
        m_impl->src = nullptr;
    }
    m_impl->started = false;
    m_impl->playing = false;

    // Decode by extension: .wav goes through the lightweight RIFF parser; everything else
    // (mp3/m4a/…) is decoded to PCM by Media Foundation. If the .wav parse fails, fall back to MF.
    std::wstring ext;
    if (auto dot = wav.find_last_of(L'.'); dot != std::wstring::npos) {
        ext = wav.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    }
    bool decoded = false;
    if (ext == L".wav") {
        decoded = ParseWav(wav, m_impl->wfx, m_impl->pcm);
        if (!decoded) decoded = DecodeMediaFoundation(wav, m_impl->wfx, m_impl->pcm);
    } else {
        decoded = DecodeMediaFoundation(wav, m_impl->wfx, m_impl->pcm);
    }
    if (!decoded) {
        std::fprintf(stderr, "[Audio] failed to decode audio file\n");
        return false;
    }

    HRESULT hr = m_impl->xa->CreateSourceVoice(&m_impl->src, &m_impl->wfx);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[Audio] CreateSourceVoice failed (0x%08lX)\n", static_cast<unsigned long>(hr));
        return false;
    }
    if (m_impl->wfx.nAvgBytesPerSec)
        m_impl->lengthSec = static_cast<float>(m_impl->pcm.size()) /
                            static_cast<float>(m_impl->wfx.nAvgBytesPerSec);
    return true;
}

void Audio::Play() {
    if (!m_impl->src) return;
    if (!m_impl->started) {
        m_impl->Submit();
        XAUDIO2_VOICE_STATE st{}; m_impl->src->GetState(&st);
        m_impl->baseline = st.SamplesPlayed;   // this is the loop origin (position 0)
        m_impl->started  = true;
    }
    m_impl->src->Start(0);
    m_impl->playing = true;
}

void Audio::Pause() {
    if (m_impl->src) m_impl->src->Stop(0);     // SamplesPlayed freezes; position holds
    m_impl->playing = false;
}

void Audio::Stop() {
    if (!m_impl->src) return;
    m_impl->src->Stop(0);
    m_impl->src->FlushSourceBuffers();
    m_impl->started = false;
    m_impl->playing = false;
}

void Audio::Restart() {
    if (!m_impl->src) return;
    m_impl->src->Stop(0);
    m_impl->src->FlushSourceBuffers();
    m_impl->Submit();
    XAUDIO2_VOICE_STATE st{}; m_impl->src->GetState(&st);
    m_impl->baseline = st.SamplesPlayed;       // reset the loop origin to "now" -> position 0
    m_impl->started  = true;
    m_impl->src->Start(0);
    m_impl->playing  = true;
}

bool  Audio::Loaded() const        { return m_impl->src != nullptr; }
float Audio::LengthSeconds() const { return m_impl->EffectiveLoop(); }
bool  Audio::Playing() const       { return m_impl->playing; }

void Audio::SetLoopSeconds(double seconds) {
    // Clamp the loop period (e.g. to the dance length so the music and motion stay equal-length
    // and restart together). 0 or >= the buffer length restores looping the whole clip. Takes
    // effect on the next Submit (Play/Restart).
    m_impl->loopSec = (seconds > 0.0) ? static_cast<float>(seconds) : 0.0f;
}

double Audio::PositionSeconds() const {
    if (!m_impl->src || !m_impl->started || m_impl->lengthSec <= 0.0f ||
        m_impl->wfx.nSamplesPerSec == 0)
        return 0.0;
    XAUDIO2_VOICE_STATE st{};
    m_impl->src->GetState(&st);
    double played = static_cast<double>(st.SamplesPlayed - m_impl->baseline) /
                    static_cast<double>(m_impl->wfx.nSamplesPerSec);
    if (played < 0.0) played = 0.0;
    return std::fmod(played, static_cast<double>(m_impl->EffectiveLoop()));   // position inside the loop
}

} // namespace dr
