// audio_recorder.cpp - NVGT plugin for recording mic and/or system audio to WAV
// Windows only - uses WASAPI (Windows Audio Session API)
// No external libraries required.
//
// Source mode constants (also registered as NVGT globals):
//   AUDIO_SOURCE_MIC    = 0  - microphone only
//   AUDIO_SOURCE_SYSTEM = 1  - system / loopback audio only
//   AUDIO_SOURCE_BOTH   = 2  - mic + system mixed into one WAV file

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <memory>
#include "../../src/nvgt_plugin.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

// ---------------------------------------------------------------------------
// WAV file header (packed so it can be written directly to disk)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct WAVHeader {
    char     riff[4];         // "RIFF"
    uint32_t chunkSize;       // file size - 8
    char     wave[4];         // "WAVE"
    char     fmt_[4];         // "fmt "
    uint32_t subchunk1Size;   // 16 for PCM
    uint16_t audioFormat;     // 1 = PCM
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char     data[4];         // "data"
    uint32_t dataSize;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Fixed output format.
// Both capture streams are coerced to this via AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
// so mixing is trivial (same sample rate, channel count, and bit depth).
// ---------------------------------------------------------------------------
static const uint32_t TARGET_SAMPLE_RATE = 48000;
static const uint16_t TARGET_CHANNELS    = 2;
static const uint16_t TARGET_BITS        = 16;
static const uint16_t TARGET_BLOCK_ALIGN = TARGET_CHANNELS * (TARGET_BITS / 8); // 4

// ---------------------------------------------------------------------------
// Saturating mix of two int16 PCM sample vectors.
// If the vectors differ in length the shorter one is treated as zero-padded.
// ---------------------------------------------------------------------------
static std::vector<int16_t> mixBuffers(const std::vector<int16_t>& a,
                                       const std::vector<int16_t>& b)
{
    size_t len = std::max(a.size(), b.size());
    std::vector<int16_t> out(len, 0);
    for (size_t i = 0; i < len; ++i) {
        int32_t va    = (i < a.size()) ? static_cast<int32_t>(a[i]) : 0;
        int32_t vb    = (i < b.size()) ? static_cast<int32_t>(b[i]) : 0;
        int32_t mixed = va + vb;
        if (mixed >  32767) mixed =  32767;
        if (mixed < -32768) mixed = -32768;
        out[i] = static_cast<int16_t>(mixed);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Write a 16-bit stereo PCM buffer to a WAV file.
// ---------------------------------------------------------------------------
static bool writePCMToWAV(const std::string& path,
                           const std::vector<int16_t>& samples)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));

    WAVHeader hdr = {};
    memcpy(hdr.riff,  "RIFF", 4);
    memcpy(hdr.wave,  "WAVE", 4);
    memcpy(hdr.fmt_,  "fmt ", 4);
    memcpy(hdr.data,  "data", 4);
    hdr.subchunk1Size = 16;
    hdr.audioFormat   = 1;
    hdr.numChannels   = TARGET_CHANNELS;
    hdr.sampleRate    = TARGET_SAMPLE_RATE;
    hdr.bitsPerSample = TARGET_BITS;
    hdr.blockAlign    = TARGET_BLOCK_ALIGN;
    hdr.byteRate      = TARGET_SAMPLE_RATE * TARGET_BLOCK_ALIGN;
    hdr.dataSize      = dataBytes;
    hdr.chunkSize     = 36 + dataBytes;

    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
    return f.good();
}

// ---------------------------------------------------------------------------
// WASAPICapture - one self-contained WASAPI capture stream.
// Call start(false) for microphone, start(true) for system/loopback audio.
// After stopping, call drainBuffer() to take ownership of the PCM data.
// ---------------------------------------------------------------------------
class WASAPICapture {
public:
    WASAPICapture()
        : m_running(false), m_isLoopback(false),
          m_pEnumerator(nullptr), m_pDevice(nullptr),
          m_pAudioClient(nullptr), m_pCaptureClient(nullptr) {}

    ~WASAPICapture() { stop(); }

    bool start(bool loopback) {
        if (m_running) return false;
        m_isLoopback = loopback;
        m_pcmBuffer.clear();
        m_lastError.clear();

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            m_lastError = "CoInitializeEx failed";
            return false;
        }

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&m_pEnumerator));
        if (FAILED(hr)) { m_lastError = "Failed to create MMDeviceEnumerator"; return false; }

        EDataFlow flow = m_isLoopback ? eRender : eCapture;
        hr = m_pEnumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_pDevice);
        if (FAILED(hr)) {
            m_lastError = m_isLoopback
                ? "Failed to get default render endpoint (system audio)"
                : "Failed to get default capture endpoint (microphone)";
            releaseAll(); return false;
        }

        hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                 nullptr, reinterpret_cast<void**>(&m_pAudioClient));
        if (FAILED(hr)) { m_lastError = "Failed to activate IAudioClient"; releaseAll(); return false; }

        // Request our fixed target format; Windows handles format conversion for us.
        WAVEFORMATEX wfx    = {};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = TARGET_CHANNELS;
        wfx.nSamplesPerSec  = TARGET_SAMPLE_RATE;
        wfx.wBitsPerSample  = TARGET_BITS;
        wfx.nBlockAlign     = TARGET_BLOCK_ALIGN;
        wfx.nAvgBytesPerSec = TARGET_SAMPLE_RATE * TARGET_BLOCK_ALIGN;

        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                    | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (m_isLoopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

        REFERENCE_TIME bufDuration = 10000000; // 1-second internal WASAPI buffer
        hr = m_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        flags, bufDuration, 0, &wfx, nullptr);
        if (FAILED(hr)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "0x%08X", static_cast<unsigned>(hr));
            m_lastError = std::string("IAudioClient::Initialize failed (hr=") + tmp + ")";
            releaseAll(); return false;
        }

        hr = m_pAudioClient->GetService(__uuidof(IAudioCaptureClient),
                                        reinterpret_cast<void**>(&m_pCaptureClient));
        if (FAILED(hr)) { m_lastError = "Failed to get IAudioCaptureClient"; releaseAll(); return false; }

        hr = m_pAudioClient->Start();
        if (FAILED(hr)) { m_lastError = "IAudioClient::Start failed"; releaseAll(); return false; }

        m_running = true;
        m_thread  = std::thread(&WASAPICapture::captureLoop, this);
        return true;
    }

    // Signal stop and wait for the capture thread to finish.
    // Always call drainBuffer() BEFORE stop() while the thread is still running
    // so you don't miss the last few milliseconds of audio.
    void stop() {
        if (!m_running.exchange(false)) return;
        if (m_thread.joinable()) m_thread.join();
        if (m_pAudioClient) m_pAudioClient->Stop();
        releaseAll();
    }

    bool isRunning() const { return m_running.load(); }
    std::string getLastError() const { return m_lastError; }

    // Approximate captured data size in bytes (for progress reporting).
    size_t getCapturedBytes() const {
        std::lock_guard<std::mutex> lk(m_bufMutex);
        return m_pcmBuffer.size() * sizeof(int16_t);
    }

    // Take ownership of the internal PCM buffer.
    // Safe to call while the capture thread is still running - uses the mutex.
    std::vector<int16_t> drainBuffer() {
        std::lock_guard<std::mutex> lk(m_bufMutex);
        return std::move(m_pcmBuffer);
    }

private:
    void captureLoop() {
        while (m_running) {
            UINT32 pktLen = 0;
            if (FAILED(m_pCaptureClient->GetNextPacketSize(&pktLen))) break;

            while (pktLen != 0) {
                BYTE*  pData  = nullptr;
                UINT32 frames = 0;
                DWORD  flags  = 0;

                if (FAILED(m_pCaptureClient->GetBuffer(
                        &pData, &frames, &flags, nullptr, nullptr))) break;

                size_t samples = static_cast<size_t>(frames) * TARGET_CHANNELS;
                {
                    std::lock_guard<std::mutex> lk(m_bufMutex);
                    size_t off = m_pcmBuffer.size();
                    m_pcmBuffer.resize(off + samples);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !pData) {
                        memset(m_pcmBuffer.data() + off, 0, samples * sizeof(int16_t));
                    } else {
                        memcpy(m_pcmBuffer.data() + off, pData, samples * sizeof(int16_t));
                    }
                }

                m_pCaptureClient->ReleaseBuffer(frames);
                m_pCaptureClient->GetNextPacketSize(&pktLen);
            }
            Sleep(10); // poll every 10 ms
        }
    }

    void releaseAll() {
        if (m_pCaptureClient) { m_pCaptureClient->Release(); m_pCaptureClient = nullptr; }
        if (m_pAudioClient)   { m_pAudioClient->Release();   m_pAudioClient   = nullptr; }
        if (m_pDevice)        { m_pDevice->Release();        m_pDevice        = nullptr; }
        if (m_pEnumerator)    { m_pEnumerator->Release();    m_pEnumerator    = nullptr; }
    }

    std::atomic<bool>      m_running;
    bool                   m_isLoopback;
    std::string            m_lastError;
    std::vector<int16_t>   m_pcmBuffer;
    mutable std::mutex     m_bufMutex;
    std::thread            m_thread;

    IMMDeviceEnumerator*   m_pEnumerator;
    IMMDevice*             m_pDevice;
    IAudioClient*          m_pAudioClient;
    IAudioCaptureClient*   m_pCaptureClient;
};

// ---------------------------------------------------------------------------
// Source mode constants
// ---------------------------------------------------------------------------
static const int AUDIO_SOURCE_MIC    = 0;
static const int AUDIO_SOURCE_SYSTEM = 1;
static const int AUDIO_SOURCE_BOTH   = 2;

// ---------------------------------------------------------------------------
// AudioRecorder - high-level recorder that owns up to two WASAPICapture streams.
// In BOTH mode, both streams run in parallel and are mixed on stop().
// ---------------------------------------------------------------------------
class AudioRecorder {
public:
    AudioRecorder() : m_recording(false), m_mode(AUDIO_SOURCE_MIC) {}
    ~AudioRecorder() { stop(); }

    bool start(const std::string& filepath, int mode) {
        if (m_recording) { m_lastError = "Already recording"; return false; }
        if (mode < 0 || mode > 2) { m_lastError = "Invalid source mode (use 0, 1, or 2)"; return false; }

        m_filepath  = filepath;
        m_mode      = mode;
        m_lastError.clear();

        bool wantMic    = (mode == AUDIO_SOURCE_MIC    || mode == AUDIO_SOURCE_BOTH);
        bool wantSystem = (mode == AUDIO_SOURCE_SYSTEM || mode == AUDIO_SOURCE_BOTH);

        if (wantMic) {
            m_mic.reset(new WASAPICapture());
            if (!m_mic->start(false /* not loopback */)) {
                m_lastError = "Microphone init failed: " + m_mic->getLastError();
                m_mic.reset();
                return false;
            }
        }

        if (wantSystem) {
            m_system.reset(new WASAPICapture());
            if (!m_system->start(true /* loopback */)) {
                m_lastError = "System audio init failed: " + m_system->getLastError();
                // Roll back mic if it was already started
                if (m_mic) { m_mic->stop(); m_mic.reset(); }
                m_system.reset();
                return false;
            }
        }

        m_recording = true;
        return true;
    }

    bool stop() {
        if (!m_recording) return false;
        m_recording = false;

        // Drain BEFORE stopping threads to capture the final milliseconds.
        std::vector<int16_t> micBuf, sysBuf;
        if (m_mic)    micBuf = m_mic->drainBuffer();
        if (m_system) sysBuf = m_system->drainBuffer();

        if (m_mic)    { m_mic->stop();    m_mic.reset(); }
        if (m_system) { m_system->stop(); m_system.reset(); }

        // Combine buffers according to the active mode.
        std::vector<int16_t> finalBuf;
        if (!micBuf.empty() && !sysBuf.empty()) {
            // Both streams active: saturating mix into one file.
            finalBuf = mixBuffers(micBuf, sysBuf);
        } else if (!micBuf.empty()) {
            finalBuf = std::move(micBuf);
        } else if (!sysBuf.empty()) {
            finalBuf = std::move(sysBuf);
        } else {
            m_lastError = "No audio was captured";
            return false;
        }

        if (!writePCMToWAV(m_filepath, finalBuf)) {
            m_lastError = "Failed to write WAV file: " + m_filepath;
            return false;
        }
        return true;
    }

    bool isRecording() const { return m_recording.load(); }

    // Returns total raw PCM bytes buffered across all active streams.
    int getRecordedBytes() const {
        size_t total = 0;
        if (m_mic)    total += m_mic->getCapturedBytes();
        if (m_system) total += m_system->getCapturedBytes();
        return static_cast<int>(total);
    }

    std::string getLastError() const {
        if (!m_lastError.empty()) return m_lastError;
        if (m_mic    && !m_mic->getLastError().empty())    return m_mic->getLastError();
        if (m_system && !m_system->getLastError().empty()) return m_system->getLastError();
        return "";
    }

private:
    std::atomic<bool>              m_recording;
    int                            m_mode;
    std::string                    m_filepath;
    std::string                    m_lastError;
    std::unique_ptr<WASAPICapture> m_mic;
    std::unique_ptr<WASAPICapture> m_system;
};

// ---------------------------------------------------------------------------
// Plugin-level singleton + thin C-callable wrappers
// ---------------------------------------------------------------------------
static AudioRecorder* g_recorder = nullptr;

static AudioRecorder& getRecorder() {
    if (!g_recorder) g_recorder = new AudioRecorder();
    return *g_recorder;
}

static bool        audio_recorder_start(const std::string& fp, int src) { return getRecorder().start(fp, src); }
static bool        audio_recorder_stop()                                 { return getRecorder().stop(); }
static bool        audio_recorder_is_recording()                         { return getRecorder().isRecording(); }
static int         audio_recorder_get_recorded_bytes()                   { return getRecorder().getRecordedBytes(); }
static std::string audio_recorder_get_last_error()                       { return getRecorder().getLastError(); }

#else // -----------------------------------------------------------------------
// Non-Windows stubs — the plugin still compiles everywhere; functions just no-op.
// -----------------------------------------------------------------------
static const int AUDIO_SOURCE_MIC    = 0;
static const int AUDIO_SOURCE_SYSTEM = 1;
static const int AUDIO_SOURCE_BOTH   = 2;
static bool        audio_recorder_start(const std::string&, int) { return false; }
static bool        audio_recorder_stop()                          { return false; }
static bool        audio_recorder_is_recording()                  { return false; }
static int         audio_recorder_get_recorded_bytes()            { return 0; }
static std::string audio_recorder_get_last_error()                { return "audio_recorder is Windows only"; }
#endif

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------
plugin_main(nvgt_plugin_shared* shared) {
    prepare_plugin(shared);

    // Expose source-mode constants so scripts don't need bare magic numbers.
    shared->script_engine->RegisterGlobalProperty(
        "const int AUDIO_SOURCE_MIC",    const_cast<int*>(&AUDIO_SOURCE_MIC));
    shared->script_engine->RegisterGlobalProperty(
        "const int AUDIO_SOURCE_SYSTEM", const_cast<int*>(&AUDIO_SOURCE_SYSTEM));
    shared->script_engine->RegisterGlobalProperty(
        "const int AUDIO_SOURCE_BOTH",   const_cast<int*>(&AUDIO_SOURCE_BOTH));

    // bool audio_recorder_start(const string &in filepath, int source = AUDIO_SOURCE_MIC)
    shared->script_engine->RegisterGlobalFunction(
        "bool audio_recorder_start(const string&in filepath, int source = 0)",
        asFUNCTION(audio_recorder_start), asCALL_CDECL);

    // bool audio_recorder_stop()
    shared->script_engine->RegisterGlobalFunction(
        "bool audio_recorder_stop()",
        asFUNCTION(audio_recorder_stop), asCALL_CDECL);

    // bool audio_recorder_is_recording()
    shared->script_engine->RegisterGlobalFunction(
        "bool audio_recorder_is_recording()",
        asFUNCTION(audio_recorder_is_recording), asCALL_CDECL);

    // int audio_recorder_get_recorded_bytes()
    shared->script_engine->RegisterGlobalFunction(
        "int audio_recorder_get_recorded_bytes()",
        asFUNCTION(audio_recorder_get_recorded_bytes), asCALL_CDECL);

    // string audio_recorder_get_last_error()
    shared->script_engine->RegisterGlobalFunction(
        "string audio_recorder_get_last_error()",
        asFUNCTION(audio_recorder_get_last_error), asCALL_CDECL);

    return true;
}