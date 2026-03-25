// audio_recorder.cpp - NVGT plugin for recording mic and system audio to WAV
// Windows only - uses WASAPI (Windows Audio Session API)
// No external libraries required.

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <cstring>
#include "../../src/nvgt_plugin.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

// WAV file header structure
#pragma pack(push, 1)
struct WAVHeader {
    char     riff[4];        // "RIFF"
    uint32_t chunkSize;      // file size - 8
    char     wave[4];        // "WAVE"
    char     fmt[4];         // "fmt "
    uint32_t subchunk1Size;  // 16 for PCM
    uint16_t audioFormat;    // 1 = PCM
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;       // sampleRate * numChannels * bitsPerSample/8
    uint16_t blockAlign;     // numChannels * bitsPerSample/8
    uint16_t bitsPerSample;
    char     data[4];        // "data"
    uint32_t dataSize;       // number of bytes in data
};
#pragma pack(pop)

// ---- Recorder class --------------------------------------------------------

class AudioRecorder {
public:
    AudioRecorder()
        : m_recording(false), m_isLoopback(false),
          m_sampleRate(0), m_channels(0), m_bitsPerSample(0),
          m_pEnumerator(nullptr), m_pDevice(nullptr),
          m_pAudioClient(nullptr), m_pCaptureClient(nullptr) {}

    ~AudioRecorder() {
        stop();
        cleanup();
    }

    // source: 0 = microphone, 1 = system audio (loopback)
    bool start(const std::string& filepath, int source) {
        if (m_recording) return false;

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            m_lastError = "CoInitializeEx failed";
            return false;
        }

        m_isLoopback = (source == 1);
        m_filepath = filepath;

        // Create device enumerator
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              (void**)&m_pEnumerator);
        if (FAILED(hr)) { m_lastError = "Failed to create device enumerator"; return false; }

        // Get default device
        EDataFlow flow = m_isLoopback ? eRender : eCapture;
        hr = m_pEnumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_pDevice);
        if (FAILED(hr)) { m_lastError = "Failed to get default audio endpoint"; cleanup(); return false; }

        // Activate audio client
        hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                 nullptr, (void**)&m_pAudioClient);
        if (FAILED(hr)) { m_lastError = "Failed to activate audio client"; cleanup(); return false; }

        // Get mix format
        WAVEFORMATEX* pwfx = nullptr;
        hr = m_pAudioClient->GetMixFormat(&pwfx);
        if (FAILED(hr)) { m_lastError = "Failed to get mix format"; cleanup(); return false; }

        // Normalize to 16-bit PCM if needed
        WAVEFORMATEX wfx = {};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = pwfx->nChannels;
        wfx.nSamplesPerSec  = pwfx->nSamplesPerSec;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        CoTaskMemFree(pwfx);

        m_sampleRate    = wfx.nSamplesPerSec;
        m_channels      = wfx.nChannels;
        m_bitsPerSample = wfx.wBitsPerSample;

        // Try to initialize with PCM; fall back to mix format float if rejected
        DWORD streamFlags = m_isLoopback
            ? (AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY)
            : (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);

        REFERENCE_TIME hnsBufferDuration = 10000000; // 1 second buffer
        hr = m_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                        hnsBufferDuration, 0, &wfx, nullptr);
        if (FAILED(hr)) {
            m_lastError = "Failed to initialize audio client (hr=" + std::to_string(hr) + ")";
            cleanup();
            return false;
        }

        hr = m_pAudioClient->GetService(__uuidof(IAudioCaptureClient),
                                        (void**)&m_pCaptureClient);
        if (FAILED(hr)) { m_lastError = "Failed to get capture client"; cleanup(); return false; }

        hr = m_pAudioClient->Start();
        if (FAILED(hr)) { m_lastError = "Failed to start audio client"; cleanup(); return false; }

        m_recording = true;
        m_pcmBuffer.clear();
        m_captureThread = std::thread(&AudioRecorder::captureLoop, this);
        return true;
    }

    bool stop() {
        if (!m_recording) return false;
        m_recording = false;
        if (m_captureThread.joinable()) m_captureThread.join();
        if (m_pAudioClient) m_pAudioClient->Stop();
        writeWAVFile();
        cleanup();
        return true;
    }

    bool isRecording() const { return m_recording.load(); }
    std::string getLastError() const { return m_lastError; }

    // Returns recorded size in bytes so far
    int getRecordedBytes() {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        return (int)m_pcmBuffer.size();
    }

private:
    void captureLoop() {
        while (m_recording) {
            UINT32 packetLength = 0;
            HRESULT hr = m_pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            while (packetLength != 0) {
                BYTE* pData = nullptr;
                UINT32 numFramesAvailable = 0;
                DWORD flags = 0;

                hr = m_pCaptureClient->GetBuffer(&pData, &numFramesAvailable,
                                                 &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                size_t bytes = numFramesAvailable * m_channels * (m_bitsPerSample / 8);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && pData && bytes > 0) {
                    std::lock_guard<std::mutex> lock(m_bufferMutex);
                    size_t offset = m_pcmBuffer.size();
                    m_pcmBuffer.resize(offset + bytes);
                    memcpy(m_pcmBuffer.data() + offset, pData, bytes);
                } else if (flags & AUDCLNT_BUFFERFLAGS_SILENT && bytes > 0) {
                    // Write silence
                    std::lock_guard<std::mutex> lock(m_bufferMutex);
                    size_t offset = m_pcmBuffer.size();
                    m_pcmBuffer.resize(offset + bytes, 0);
                }

                m_pCaptureClient->ReleaseBuffer(numFramesAvailable);
                m_pCaptureClient->GetNextPacketSize(&packetLength);
            }

            Sleep(10);
        }
    }

    void writeWAVFile() {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        if (m_pcmBuffer.empty()) return;

        std::ofstream f(m_filepath, std::ios::binary);
        if (!f) { m_lastError = "Failed to open output file: " + m_filepath; return; }

        WAVHeader hdr = {};
        memcpy(hdr.riff,  "RIFF", 4);
        memcpy(hdr.wave,  "WAVE", 4);
        memcpy(hdr.fmt,   "fmt ", 4);
        memcpy(hdr.data,  "data", 4);
        hdr.subchunk1Size = 16;
        hdr.audioFormat   = 1; // PCM
        hdr.numChannels   = (uint16_t)m_channels;
        hdr.sampleRate    = m_sampleRate;
        hdr.bitsPerSample = (uint16_t)m_bitsPerSample;
        hdr.blockAlign    = hdr.numChannels * (hdr.bitsPerSample / 8);
        hdr.byteRate      = hdr.sampleRate * hdr.blockAlign;
        hdr.dataSize      = (uint32_t)m_pcmBuffer.size();
        hdr.chunkSize     = 36 + hdr.dataSize;

        f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        f.write(reinterpret_cast<const char*>(m_pcmBuffer.data()), m_pcmBuffer.size());
    }

    void cleanup() {
        if (m_pCaptureClient) { m_pCaptureClient->Release(); m_pCaptureClient = nullptr; }
        if (m_pAudioClient)   { m_pAudioClient->Release();   m_pAudioClient   = nullptr; }
        if (m_pDevice)        { m_pDevice->Release();        m_pDevice        = nullptr; }
        if (m_pEnumerator)    { m_pEnumerator->Release();    m_pEnumerator    = nullptr; }
    }

    std::atomic<bool>        m_recording;
    bool                     m_isLoopback;
    std::string              m_filepath;
    std::string              m_lastError;
    uint32_t                 m_sampleRate;
    uint16_t                 m_channels;
    uint16_t                 m_bitsPerSample;
    std::vector<uint8_t>     m_pcmBuffer;
    std::mutex               m_bufferMutex;
    std::thread              m_captureThread;

    IMMDeviceEnumerator*     m_pEnumerator;
    IMMDevice*               m_pDevice;
    IAudioClient*            m_pAudioClient;
    IAudioCaptureClient*     m_pCaptureClient;
};

// ---- Static instance (simple single-recorder model) -----------------------
// You can extend this to a map of named recorders if you need simultaneous recordings.

static AudioRecorder* g_recorder = nullptr;

static bool audio_recorder_start(const std::string& filepath, int source) {
    if (!g_recorder) g_recorder = new AudioRecorder();
    return g_recorder->start(filepath, source);
}

static bool audio_recorder_stop() {
    if (!g_recorder) return false;
    return g_recorder->stop();
}

static bool audio_recorder_is_recording() {
    if (!g_recorder) return false;
    return g_recorder->isRecording();
}

static int audio_recorder_get_recorded_bytes() {
    if (!g_recorder) return 0;
    return g_recorder->getRecordedBytes();
}

static std::string audio_recorder_get_last_error() {
    if (!g_recorder) return "";
    return g_recorder->getLastError();
}

#else
// Stub implementations for non-Windows builds
static bool audio_recorder_start(const std::string&, int) { return false; }
static bool audio_recorder_stop() { return false; }
static bool audio_recorder_is_recording() { return false; }
static int  audio_recorder_get_recorded_bytes() { return 0; }
static std::string audio_recorder_get_last_error() { return "audio_recorder is Windows only"; }
#endif

// ---- Plugin entry point ----------------------------------------------------

plugin_main(nvgt_plugin_shared* shared) {
    prepare_plugin(shared);

    // bool audio_recorder_start(const string &in filepath, int source)
    // source: 0 = microphone, 1 = system audio (loopback)
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
