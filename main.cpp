#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <RtAudio.h>
#include "asio.h"

#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cctype>
#include <cmath>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

// ============================================================================
// Lock-free Single-Producer Single-Consumer (SPSC) Ring Buffer
// ============================================================================
template <typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static constexpr size_t Mask = Capacity - 1;

    alignas(64) std::atomic<size_t> m_writeIndex{0};
    alignas(64) std::atomic<size_t> m_readIndex{0};
    T m_buffer[Capacity];

public:
    SPSCRingBuffer() {
        std::memset(m_buffer, 0, sizeof(m_buffer));
    }

    void reset() {
        m_writeIndex.store(0, std::memory_order_relaxed);
        m_readIndex.store(0, std::memory_order_relaxed);
        std::memset(m_buffer, 0, sizeof(m_buffer));
    }

    size_t write(const T* src, size_t count) {
        const size_t currentWrite = m_writeIndex.load(std::memory_order_relaxed);
        const size_t currentRead = m_readIndex.load(std::memory_order_acquire);
        const size_t available = Capacity - (currentWrite - currentRead) - 1;
        const size_t toWrite = (count < available) ? count : available;

        for (size_t i = 0; i < toWrite; ++i) {
            m_buffer[(currentWrite + i) & Mask] = src[i];
        }
        m_writeIndex.store(currentWrite + toWrite, std::memory_order_release);
        return toWrite;
    }

    size_t read(T* dst, size_t count) {
        const size_t currentWrite = m_writeIndex.load(std::memory_order_acquire);
        const size_t currentRead = m_readIndex.load(std::memory_order_relaxed);
        const size_t available = currentWrite - currentRead;
        const size_t toRead = (count < available) ? count : available;

        for (size_t i = 0; i < toRead; ++i) {
            dst[i] = m_buffer[(currentRead + i) & Mask];
        }
        for (size_t i = toRead; i < count; ++i) {
            dst[i] = T(0);
        }
        m_readIndex.store(currentRead + toRead, std::memory_order_release);
        return toRead;
    }

    size_t availableRead() const {
        const size_t currentWrite = m_writeIndex.load(std::memory_order_acquire);
        const size_t currentRead = m_readIndex.load(std::memory_order_relaxed);
        return currentWrite - currentRead;
    }
};

// 32768 floats = 16384 stereo frames (~341 ms buffer at 48000 Hz)
constexpr size_t RING_BUFFER_CAPACITY = 32768;
using AudioRingBuffer = SPSCRingBuffer<float, RING_BUFFER_CAPACITY>;

// ============================================================================
// Global State & Diagnostic Counters
// ============================================================================
std::atomic<bool> g_running{true};
bool g_headless = false;
std::atomic<float> g_systemVolumeScalar{1.0f};
std::atomic<bool> g_systemMuted{false};

std::atomic<float> g_currentInPeak{0.0f};
std::atomic<uint64_t> g_capturedPacketCount{0};
std::atomic<uint64_t> g_silentPacketCount{0};
std::atomic<uint64_t> g_activeAudioPacketCount{0};

#define LOG_INFO if (!g_headless) std::cout

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        LOG_INFO << "\n[System] Shutdown signal received. Exiting...\n";
        g_running.store(false, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

// Windows Core Audio endpoint volume callback
class EndpointVolumeCallback : public IAudioEndpointVolumeCallback {
public:
    EndpointVolumeCallback() : m_refCount(1) {}
    virtual ~EndpointVolumeCallback() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG ulRef = InterlockedDecrement(&m_refCount);
        if (ulRef == 0) {
            delete this;
        }
        return ulRef;
    }

    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify) override {
        if (pNotify) {
            g_systemVolumeScalar.store(pNotify->fMasterVolume, std::memory_order_relaxed);
            g_systemMuted.store(pNotify->bMuted != FALSE, std::memory_order_relaxed);
        }
        return S_OK;
    }

private:
    LONG m_refCount;
};

// Helper: case-insensitive string containment check
bool containsIgnoreCase(const std::string& str, const std::string& sub) {
    if (sub.empty()) return true;
    auto it = std::search(
        str.begin(), str.end(),
        sub.begin(), sub.end(),
        [](char ch1, char ch2) { return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2)); }
    );
    return it != str.end();
}

const char* asioSampleTypeToString(ASIOSampleType type) {
    switch (type) {
        case ASIOSTInt16LSB:   return "Int16LSB";
        case ASIOSTInt24LSB:   return "Int24LSB";
        case ASIOSTInt32LSB:   return "Int32LSB (32-bit int)";
        case ASIOSTFloat32LSB: return "Float32LSB (32-bit float)";
        case ASIOSTFloat64LSB: return "Float64LSB";
        case ASIOSTInt32LSB16: return "Int32LSB16";
        case ASIOSTInt32LSB18: return "Int32LSB18";
        case ASIOSTInt32LSB20: return "Int32LSB20";
        case ASIOSTInt32LSB24: return "Int32LSB24";
        default: return "Other/Unknown";
    }
}

// Print detailed breakdown of WAVEFORMATEX / WAVEFORMATEXTENSIBLE
void logMixFormatDetails(const WAVEFORMATEX* pwfx) {
    if (g_headless) return;
    std::cout << "========================================================\n"
              << " [WASAPI Mix Format from Windows Audio Engine]\n"
              << "  Format Tag:      0x" << std::hex << pwfx->wFormatTag;
    if (pwfx->wFormatTag == WAVE_FORMAT_PCM) std::cout << " (WAVE_FORMAT_PCM)\n";
    else if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) std::cout << " (WAVE_FORMAT_IEEE_FLOAT)\n";
    else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) std::cout << " (WAVE_FORMAT_EXTENSIBLE)\n";
    else std::cout << "\n";

    std::cout << std::dec
              << "  Channels:        " << pwfx->nChannels << "\n"
              << "  Sample Rate:     " << pwfx->nSamplesPerSec << " Hz\n"
              << "  Bits Per Sample: " << pwfx->wBitsPerSample << " bits\n"
              << "  Block Align:     " << pwfx->nBlockAlign << " bytes\n"
              << "  Avg Bytes/Sec:   " << pwfx->nAvgBytesPerSec << "\n";

    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22) {
        const auto* pExt = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        std::cout << "  Valid Bits:      " << pExt->Samples.wValidBitsPerSample << "\n"
                  << "  Channel Mask:    0x" << std::hex << pExt->dwChannelMask << std::dec << "\n";

        OLECHAR guidStr[64] = {};
        StringFromGUID2(pExt->SubFormat, guidStr, 64);
        char guidUtf8[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, guidStr, -1, guidUtf8, sizeof(guidUtf8), nullptr, nullptr);
        std::cout << "  SubFormat GUID:  " << guidUtf8;

        if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            std::cout << " [IEEE_FLOAT 32-bit]\n";
        } else if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            std::cout << " [PCM Integer]\n";
        } else {
            std::cout << "\n";
        }
    }
    std::cout << "========================================================\n\n";
}

// ============================================================================
// WASAPI Capture Thread (Supports both eCapture direct and eRender loopback)
// ============================================================================
void wasapiCaptureThreadFunc(AudioRingBuffer* ringBuffer, const std::string& captureDevicePattern) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        if (!g_headless) std::cerr << "[WASAPI Error] CoInitializeEx failed: hr=0x" << std::hex << hr << "\n";
        return;
    }

    DWORD taskIndex = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!hAvrt) {
        AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    }

    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;
    IAudioEndpointVolume* pEndpointVolume = nullptr;
    EndpointVolumeCallback* pVolCallback = nullptr;
    HANDLE hCaptureEvent = nullptr;

    auto cleanup = [&]() {
        if (pAudioClient) {
            pAudioClient->Stop();
        }
        if (pEndpointVolume && pVolCallback) {
            pEndpointVolume->UnregisterControlChangeNotify(pVolCallback);
            pVolCallback->Release();
            pVolCallback = nullptr;
        }
        if (pEndpointVolume) { pEndpointVolume->Release(); pEndpointVolume = nullptr; }
        if (pCaptureClient) { pCaptureClient->Release(); pCaptureClient = nullptr; }
        if (pAudioClient) { pAudioClient->Release(); pAudioClient = nullptr; }
        if (pDevice) { pDevice->Release(); pDevice = nullptr; }
        if (pEnumerator) { pEnumerator->Release(); pEnumerator = nullptr; }
        if (hCaptureEvent) { CloseHandle(hCaptureEvent); hCaptureEvent = nullptr; }
        if (hAvrt) { AvRevertMmThreadCharacteristics(hAvrt); hAvrt = nullptr; }
        CoUninitialize();
    };

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
        if (!g_headless) std::cerr << "[WASAPI Error] Failed to create MMDeviceEnumerator: hr=0x" << std::hex << hr << "\n";
        cleanup();
        return;
    }

    bool isCaptureEndpoint = false;

    // 1. Resolve target endpoint
    if (captureDevicePattern.empty()) {
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
        isCaptureEndpoint = false;
        if (FAILED(hr)) {
            if (!g_headless) std::cerr << "[WASAPI Error] Failed to get default render endpoint: hr=0x" << std::hex << hr << "\n";
            cleanup();
            return;
        }
    } else {
        // First check in eCapture (Recording devices, e.g. "CABLE Output")
        IMMDeviceCollection* pCapCol = nullptr;
        if (SUCCEEDED(pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCapCol)) && pCapCol) {
            UINT count = 0;
            pCapCol->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* pItem = nullptr;
                if (SUCCEEDED(pCapCol->Item(i, &pItem)) && pItem) {
                    IPropertyStore* pProps = nullptr;
                    if (SUCCEEDED(pItem->OpenPropertyStore(STGM_READ, &pProps))) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                            char utf8Name[256] = {};
                            WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, utf8Name, sizeof(utf8Name), nullptr, nullptr);
                            if (containsIgnoreCase(utf8Name, captureDevicePattern)) {
                                pDevice = pItem;
                                pDevice->AddRef();
                                isCaptureEndpoint = true;
                                PropVariantClear(&varName);
                                pProps->Release();
                                pItem->Release();
                                break;
                            }
                            PropVariantClear(&varName);
                        }
                        pProps->Release();
                    }
                    pItem->Release();
                }
            }
            pCapCol->Release();
        }

        // If not found in eCapture, check in eRender (Playback devices)
        if (!pDevice) {
            IMMDeviceCollection* pRenderCol = nullptr;
            if (SUCCEEDED(pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pRenderCol)) && pRenderCol) {
                UINT count = 0;
                pRenderCol->GetCount(&count);
                for (UINT i = 0; i < count; ++i) {
                    IMMDevice* pItem = nullptr;
                    if (SUCCEEDED(pRenderCol->Item(i, &pItem)) && pItem) {
                        IPropertyStore* pProps = nullptr;
                        if (SUCCEEDED(pItem->OpenPropertyStore(STGM_READ, &pProps))) {
                            PROPVARIANT varName;
                            PropVariantInit(&varName);
                            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                                char utf8Name[256] = {};
                                WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, utf8Name, sizeof(utf8Name), nullptr, nullptr);
                                if (containsIgnoreCase(utf8Name, captureDevicePattern)) {
                                    pDevice = pItem;
                                    pDevice->AddRef();
                                    isCaptureEndpoint = false;
                                    PropVariantClear(&varName);
                                    pProps->Release();
                                    pItem->Release();
                                    break;
                                }
                                PropVariantClear(&varName);
                            }
                            pProps->Release();
                        }
                        pItem->Release();
                    }
                }
                pRenderCol->Release();
            }
        }

        if (!pDevice) {
            LOG_INFO << "[WASAPI Warning] Device matching '" << captureDevicePattern
                     << "' not found in Recording or Playback devices. Falling back to default output device.\n";
            pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
            isCaptureEndpoint = false;
        }
    }

    if (!pDevice) {
        if (!g_headless) std::cerr << "[WASAPI Error] No valid audio endpoint available.\n";
        cleanup();
        return;
    }

    // Print friendly name & category
    IPropertyStore* pProps = nullptr;
    if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
            char utf8Name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, utf8Name, sizeof(utf8Name), nullptr, nullptr);
            LOG_INFO << "[WASAPI Source] Capturing from "
                     << (isCaptureEndpoint ? "[Recording Device (eCapture)]" : "[Playback Device (eRender Loopback)]")
                     << ": \"" << utf8Name << "\"\n";
            PropVariantClear(&varName);
        }
        pProps->Release();
    }

    // Setup volume sync
    IMMDevice* pVolDevice = nullptr;
    if (isCaptureEndpoint) {
        pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pVolDevice);
    } else {
        pVolDevice = pDevice;
        pVolDevice->AddRef();
    }

    if (pVolDevice) {
        hr = pVolDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&pEndpointVolume);
        if (SUCCEEDED(hr)) {
            float currentVol = 1.0f;
            BOOL isMuted = FALSE;
            pEndpointVolume->GetMasterVolumeLevelScalar(&currentVol);
            pEndpointVolume->GetMute(&isMuted);
            g_systemVolumeScalar.store(currentVol, std::memory_order_relaxed);
            g_systemMuted.store(isMuted != FALSE, std::memory_order_relaxed);

            pVolCallback = new EndpointVolumeCallback();
            pEndpointVolume->RegisterControlChangeNotify(pVolCallback);
            LOG_INFO << "[WASAPI Volume] System master volume sync active: " << std::fixed << std::setprecision(0)
                     << (currentVol * 100.0f) << "%" << (isMuted ? " [MUTED]" : "") << "\n";
        }
        pVolDevice->Release();
    }

    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
    if (FAILED(hr)) {
        if (!g_headless) std::cerr << "[WASAPI Error] Failed to activate IAudioClient: hr=0x" << std::hex << hr << "\n";
        cleanup();
        return;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr) || !pwfx) {
        if (!g_headless) std::cerr << "[WASAPI Error] Failed to get MixFormat: hr=0x" << std::hex << hr << "\n";
        cleanup();
        return;
    }

    logMixFormatDetails(pwfx);

    const unsigned int channels = pwfx->nChannels;
    const unsigned int sampleRate = pwfx->nSamplesPerSec;
    const unsigned int bitsPerSample = pwfx->wBitsPerSample;

    bool isFloatFormat = false;
    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloatFormat = true;
    } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* pExt = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            isFloatFormat = true;
        }
    }

    const DWORD baseFlags = isCaptureEndpoint ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;
    const REFERENCE_TIME bufferDuration = 200000; // 20ms

    bool useEventCallback = true;
    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        baseFlags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        bufferDuration,
        0,
        pwfx,
        nullptr
    );

    if (FAILED(hr)) {
        LOG_INFO << "[WASAPI Capture] Event callback mode not supported (hr=0x" << std::hex << hr
                 << "), falling back to timer-driven capture.\n";
        hr = pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            baseFlags,
            bufferDuration,
            0,
            pwfx,
            nullptr
        );
        useEventCallback = false;
    }

    if (FAILED(hr)) {
        if (!g_headless) std::cerr << "[WASAPI Error] Failed to initialize AudioClient: hr=0x" << std::hex << hr << "\n";
        CoTaskMemFree(pwfx);
        cleanup();
        return;
    }

    if (useEventCallback) {
        hCaptureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        hr = pAudioClient->SetEventHandle(hCaptureEvent);
        if (FAILED(hr)) {
            if (!g_headless) std::cerr << "[WASAPI Error] Failed to set event handle: hr=0x" << std::hex << hr << "\n";
            CoTaskMemFree(pwfx);
            cleanup();
            return;
        }
    }

    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
    if (FAILED(hr)) {
        if (!g_headless) std::cerr << "[WASAPI Error] Failed to get IAudioCaptureClient: hr=0x" << std::hex << hr << "\n";
        CoTaskMemFree(pwfx);
        cleanup();
        return;
    }

    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        if (!g_headless) std::cerr << "[WASAPI Error] Failed to start capture client: hr=0x" << std::hex << hr << "\n";
        CoTaskMemFree(pwfx);
        cleanup();
        return;
    }

    LOG_INFO << "[WASAPI Source] "
             << (isCaptureEndpoint ? "Direct capture active (continuous input)." : "Loopback capture active.")
             << "\n";

    std::vector<float> stereoBuffer;
    stereoBuffer.reserve(4096);
    std::vector<float> resampled48k;
    resampled48k.reserve(4096);

    const bool needsResampling = (sampleRate != 48000);
    const double resampleStep = static_cast<double>(sampleRate) / 48000.0;
    double resamplePos = 0.0;

    if (needsResampling) {
        LOG_INFO << "[WASAPI Resampler] Real-time resampling enabled: " << sampleRate
                 << " Hz -> 48000 Hz\n";
    }

    // Realtime capture loop: ZERO std::cout inside here!
    while (g_running.load(std::memory_order_relaxed)) {
        if (useEventCallback && hCaptureEvent) {
            WaitForSingleObject(hCaptureEvent, 20);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        UINT32 packetLength = 0;
        while (SUCCEEDED(pCaptureClient->GetNextPacketSize(&packetLength)) && packetLength > 0) {
            BYTE* pData = nullptr;
            UINT32 numFramesRead = 0;
            DWORD flags = 0;

            hr = pCaptureClient->GetBuffer(&pData, &numFramesRead, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                break;
            }

            g_capturedPacketCount.fetch_add(1, std::memory_order_relaxed);

            const float currentVol = g_systemMuted.load(std::memory_order_relaxed) ?
                                     0.0f : g_systemVolumeScalar.load(std::memory_order_relaxed);

            stereoBuffer.resize(numFramesRead * 2);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                g_silentPacketCount.fetch_add(1, std::memory_order_relaxed);
                std::fill(stereoBuffer.begin(), stereoBuffer.end(), 0.0f);
            } else if (pData) {
                if (isFloatFormat) {
                    const float* floatIn = reinterpret_cast<const float*>(pData);
                    for (UINT32 f = 0; f < numFramesRead; ++f) {
                        float left = floatIn[f * channels + 0] * currentVol;
                        float right = (channels > 1) ? (floatIn[f * channels + 1] * currentVol) : left;
                        stereoBuffer[f * 2 + 0] = left;
                        stereoBuffer[f * 2 + 1] = right;
                    }
                } else if (bitsPerSample == 16) {
                    const int16_t* intIn = reinterpret_cast<const int16_t*>(pData);
                    constexpr float scale16 = 1.0f / 32768.0f;
                    for (UINT32 f = 0; f < numFramesRead; ++f) {
                        float left = (intIn[f * channels + 0] * scale16) * currentVol;
                        float right = (channels > 1) ? ((intIn[f * channels + 1] * scale16) * currentVol) : left;
                        stereoBuffer[f * 2 + 0] = left;
                        stereoBuffer[f * 2 + 1] = right;
                    }
                } else if (bitsPerSample == 24 || bitsPerSample == 32) {
                    const int32_t* intIn = reinterpret_cast<const int32_t*>(pData);
                    constexpr float scale32 = 1.0f / 2147483648.0f;
                    for (UINT32 f = 0; f < numFramesRead; ++f) {
                        float left = (intIn[f * channels + 0] * scale32) * currentVol;
                        float right = (channels > 1) ? ((intIn[f * channels + 1] * scale32) * currentVol) : left;
                        stereoBuffer[f * 2 + 0] = left;
                        stereoBuffer[f * 2 + 1] = right;
                    }
                }
            }

            // Track input peak for live monitoring
            float maxIn = 0.0f;
            for (float s : stereoBuffer) {
                float absVal = std::abs(s);
                if (absVal > maxIn) maxIn = absVal;
            }
            if (maxIn > 0.0001f) {
                g_activeAudioPacketCount.fetch_add(1, std::memory_order_relaxed);
            }
            g_currentInPeak.store(maxIn, std::memory_order_relaxed);

            if (!needsResampling) {
                ringBuffer->write(stereoBuffer.data(), stereoBuffer.size());
            } else {
                resampled48k.clear();
                while (resamplePos < static_cast<double>(numFramesRead)) {
                    const size_t idx = static_cast<size_t>(resamplePos);
                    const float frac = static_cast<float>(resamplePos - idx);
                    const float l0 = stereoBuffer[idx * 2 + 0];
                    const float r0 = stereoBuffer[idx * 2 + 1];
                    const float l1 = (idx + 1 < numFramesRead) ? stereoBuffer[(idx + 1) * 2 + 0] : l0;
                    const float r1 = (idx + 1 < numFramesRead) ? stereoBuffer[(idx + 1) * 2 + 1] : r0;
                    resampled48k.push_back(l0 + frac * (l1 - l0));
                    resampled48k.push_back(r0 + frac * (r1 - r0));
                    resamplePos += resampleStep;
                }
                resamplePos -= static_cast<double>(numFramesRead);
                ringBuffer->write(resampled48k.data(), resampled48k.size());
            }

            pCaptureClient->ReleaseBuffer(numFramesRead);
        }
    }

    CoTaskMemFree(pwfx);
    cleanup();
    LOG_INFO << "\n[WASAPI Source] Capture stopped.\n";
}

// ============================================================================
// ASIO Audio Bridge Engine (RtAudio 6.x)
// ============================================================================
struct BridgeContext {
    AudioRingBuffer* ringBuffer = nullptr;
    std::atomic<uint64_t> underflowCount{0};
    std::atomic<uint64_t> framesProcessed{0};
    RtAudioFormat streamFormat = RTAUDIO_SINT32;
    std::vector<float> tempFloatBuffer;
    std::atomic<float> currentOutPeak{0.0f};
};

int asioPlaybackCallback(void* outputBuffer, void* /*inputBuffer*/, unsigned int nBufferFrames,
                         double /*streamTime*/, RtAudioStreamStatus status, void* userData)
{
    auto* ctx = static_cast<BridgeContext*>(userData);
    if (!ctx || !outputBuffer) return 0;

    if (status & RTAUDIO_OUTPUT_UNDERFLOW) {
        ctx->underflowCount.fetch_add(1, std::memory_order_relaxed);
    }

    const size_t samplesNeeded = static_cast<size_t>(nBufferFrames) * 2; // stereo

    if (ctx->streamFormat == RTAUDIO_SINT32) {
        int32_t* outInt32 = static_cast<int32_t*>(outputBuffer);

        ctx->ringBuffer->read(ctx->tempFloatBuffer.data(), samplesNeeded);

        float maxPeak = 0.0f;
        for (size_t i = 0; i < samplesNeeded; ++i) {
            float s = ctx->tempFloatBuffer[i];
            float absVal = std::abs(s);
            if (absVal > maxPeak) maxPeak = absVal;

            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;

            outInt32[i] = static_cast<int32_t>(s * 2147483647.0f);
        }
        ctx->currentOutPeak.store(maxPeak, std::memory_order_relaxed);
    } else {
        float* outFloat = static_cast<float*>(outputBuffer);
        ctx->ringBuffer->read(outFloat, samplesNeeded);

        float maxPeak = 0.0f;
        for (size_t i = 0; i < samplesNeeded; ++i) {
            float absVal = std::abs(outFloat[i]);
            if (absVal > maxPeak) maxPeak = absVal;
        }
        ctx->currentOutPeak.store(maxPeak, std::memory_order_relaxed);
    }

    ctx->framesProcessed.fetch_add(nBufferFrames, std::memory_order_relaxed);
    return 0;
}

class RtAudioError : public std::runtime_error {
public:
    RtAudioError(RtAudioErrorType type, const std::string& msg)
        : std::runtime_error(msg), m_type(type) {}
    RtAudioErrorType getType() const { return m_type; }
private:
    RtAudioErrorType m_type;
};

void asioErrorCallback(RtAudioErrorType type, const std::string& errorText) {
    if (!g_headless) {
        std::cerr << "\n[ASIO Runtime Error] (" << static_cast<int>(type) << "): " << errorText << "\n";
    }
}

// ============================================================================
// CLI & Device Discovery
// ============================================================================
void printUsage(const char* progName) {
    std::cout << "Usage: " << progName << " [options]\n\n"
              << "Options:\n"
              << "  -d, --device <pattern>   Substring match for ASIO device name (default: 'TerraTec' or 'Phase')\n"
              << "  -c, --capture <pattern>  Substring match for device to capture (searches Recording and Playback devices)\n"
              << "  -b, --buffer <samples>   ASIO buffer size (e.g. 256 or 512, default: 256)\n"
              << "  -ch, --channel <firstCh> First output channel index (default: 0 = Analog L, 1 = Analog R)\n"
              << "  -f, --format <fmt>       Output format: 'int32', 'float32', or 'auto' (default: 'auto')\n"
              << "  -s, --silent, --headless Run in background without console window / monitor output\n"
              << "  -l, --list               List available ASIO devices and exit\n"
              << "  --list-endpoints         List all Windows Playback (Render) and Recording (Capture) devices and exit\n"
              << "  -h, --help               Show this help message\n\n"
              << "Examples:\n"
              << "  " << progName << " --capture \"CABLE Output\" --device \"TerraTec\"\n"
              << "  " << progName << " --capture \"CABLE Output\" --device \"TerraTec\" --headless\n";
}

void listAsioDevices() {
    try {
        RtAudio asio(RtAudio::Api::WINDOWS_ASIO);
        std::vector<unsigned int> devIds = asio.getDeviceIds();
        std::cout << "\nDiscovered " << devIds.size() << " ASIO device(s):\n";
        std::cout << "--------------------------------------------------------\n";
        for (unsigned int id : devIds) {
            RtAudio::DeviceInfo info = asio.getDeviceInfo(id);
            std::cout << " [" << id << "] " << info.name << "\n"
                      << "     Output channels: " << info.outputChannels
                      << " | Input channels: " << info.inputChannels << "\n"
                      << "     Sample rates: ";
            for (auto sr : info.sampleRates) {
                std::cout << sr << " ";
            }
            std::cout << "\n";
        }
        std::cout << "--------------------------------------------------------\n";
    } catch (const std::exception& e) {
        std::cerr << "[Error] Failed to enumerate ASIO devices: " << e.what() << "\n";
    }
}

void listWindowsEndpoints() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr) || !pEnum) {
        std::cerr << "Failed to initialize MMDeviceEnumerator.\n";
        return;
    }

    auto printCategory = [&](EDataFlow dataFlow, const char* categoryTitle) {
        IMMDevice* pDef = nullptr;
        std::wstring defId;
        if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(dataFlow, eMultimedia, &pDef)) && pDef) {
            LPWSTR pStr = nullptr;
            if (SUCCEEDED(pDef->GetId(&pStr)) && pStr) {
                defId = pStr;
                CoTaskMemFree(pStr);
            }
            pDef->Release();
        }

        IMMDeviceCollection* pCol = nullptr;
        if (SUCCEEDED(pEnum->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &pCol)) && pCol) {
            UINT count = 0;
            pCol->GetCount(&count);
            std::cout << "\nActive " << categoryTitle << " (" << count << " found):\n";
            std::cout << "--------------------------------------------------------\n";
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* pDev = nullptr;
                if (SUCCEEDED(pCol->Item(i, &pDev)) && pDev) {
                    LPWSTR pDevId = nullptr;
                    bool isDef = false;
                    if (SUCCEEDED(pDev->GetId(&pDevId)) && pDevId) {
                        if (defId == pDevId) isDef = true;
                        CoTaskMemFree(pDevId);
                    }

                    IPropertyStore* pProps = nullptr;
                    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps))) {
                        PROPVARIANT var;
                        PropVariantInit(&var);
                        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &var)) && var.pwszVal) {
                            char utf8[256] = {};
                            WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, utf8, sizeof(utf8), nullptr, nullptr);
                            std::cout << (isDef ? " [*] [DEFAULT] " : " [ ]           ") << utf8 << "\n";
                            PropVariantClear(&var);
                        }
                        pProps->Release();
                    }
                    pDev->Release();
                }
            }
            std::cout << "--------------------------------------------------------\n";
            pCol->Release();
        }
    };

    printCategory(eRender, "Playback Devices (Render)");
    printCategory(eCapture, "Recording Devices (Capture)");
    std::cout << "\n";

    pEnum->Release();
    CoUninitialize();
}

// ============================================================================
// Main Application Routine
// ============================================================================
int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    std::string devicePattern = "";
    std::string capturePattern = "";
    unsigned int bufferFrames = 256;
    unsigned int firstChannel = 0;
    std::string formatChoice = "auto";

    // Parse CLI arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-l" || arg == "--list") {
            listAsioDevices();
            return 0;
        } else if (arg == "--list-endpoints") {
            listWindowsEndpoints();
            return 0;
        } else if (arg == "--headless" || arg == "--silent" || arg == "-s") {
            g_headless = true;
        } else if ((arg == "-d" || arg == "--device") && i + 1 < argc) {
            devicePattern = argv[++i];
        } else if ((arg == "-c" || arg == "--capture") && i + 1 < argc) {
            capturePattern = argv[++i];
        } else if ((arg == "-b" || arg == "--buffer") && i + 1 < argc) {
            bufferFrames = static_cast<unsigned int>(std::stoul(argv[++i]));
        } else if ((arg == "-ch" || arg == "--channel") && i + 1 < argc) {
            firstChannel = static_cast<unsigned int>(std::stoul(argv[++i]));
        } else if ((arg == "-f" || arg == "--format") && i + 1 < argc) {
            formatChoice = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (g_headless) {
        HWND hWnd = GetConsoleWindow();
        if (hWnd) {
            ShowWindow(hWnd, SW_HIDE);
        }
    } else {
        std::cout << "========================================================\n"
                  << " Windows 11 WASAPI -> ASIO Audio Bridge\n"
                  << " TerraTec Phase / FW Direct Low-Latency Sync\n"
                  << "========================================================\n";
    }

    AudioRingBuffer ringBuffer;
    BridgeContext bridgeContext;
    bridgeContext.ringBuffer = &ringBuffer;
    bridgeContext.tempFloatBuffer.resize(16384, 0.0f);

    // Launch WASAPI Capture thread (direct eCapture or eRender loopback)
    std::thread wasapiThread(wasapiCaptureThreadFunc, &ringBuffer, capturePattern);

    // Dedicated Live Monitor Thread (strictly once per 250 ms, fixed 57-char width, NEVER wraps)
    std::thread monitorThread;
    if (!g_headless) {
        monitorThread = std::thread([&]() {
            uint64_t lastTotalPkts = 0;
            uint64_t lastAudPkts = 0;

            while (g_running.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (!g_running.load(std::memory_order_relaxed)) break;

                float inPeak = g_currentInPeak.load(std::memory_order_relaxed);
                float outPeak = bridgeContext.currentOutPeak.load(std::memory_order_relaxed);
                float vol = g_systemMuted.load(std::memory_order_relaxed) ? 0.0f : g_systemVolumeScalar.load(std::memory_order_relaxed);
                size_t ringAvail = ringBuffer.availableRead();

                uint64_t totalPkts = g_capturedPacketCount.load(std::memory_order_relaxed);
                uint64_t audPkts = g_activeAudioPacketCount.load(std::memory_order_relaxed);

                uint64_t pktsDelta = (totalPkts >= lastTotalPkts) ? (totalPkts - lastTotalPkts) * 4 : 0;
                uint64_t audDelta = (audPkts >= lastAudPkts) ? (audPkts - lastAudPkts) * 4 : 0;
                lastTotalPkts = totalPkts;
                lastAudPkts = audPkts;

                // Decay peak levels smoothly for realistic meter visualization
                g_currentInPeak.store(inPeak * 0.70f, std::memory_order_relaxed);
                bridgeContext.currentOutPeak.store(outPeak * 0.70f, std::memory_order_relaxed);

                const char* stateMsg = "Idle  ";
                if (pktsDelta == 0) {
                    stateMsg = "Idle  ";
                } else if (audDelta == 0) {
                    stateMsg = "Silent";
                } else {
                    stateMsg = "Active";
                }

                // Exactly 58 chars (fits any 80-col console with 22 cols margin, NEVER wraps)
                char lineBuf[96];
                std::snprintf(lineBuf, sizeof(lineBuf),
                    "[Monitor] Vol:%3d%%%s | In:%4.2f | Out:%4.2f | Buf:%5zu | %s\r",
                    static_cast<int>(vol * 100.0f),
                    (g_systemMuted.load(std::memory_order_relaxed) ? " [M]" : "    "),
                    inPeak, outPeak, ringAvail, stateMsg);

                std::cout << lineBuf << std::flush;
            }
        });
    }

    // Main ASIO loop with fault-tolerance & auto-reconnect
    while (g_running.load(std::memory_order_relaxed)) {
        RtAudio asio(RtAudio::Api::WINDOWS_ASIO);
        unsigned int targetDeviceId = 0;
        bool deviceFound = false;
        std::string matchedDeviceName;
        RtAudio::DeviceInfo matchedDeviceInfo;

        try {
            std::vector<unsigned int> devIds = asio.getDeviceIds();
            for (unsigned int id : devIds) {
                RtAudio::DeviceInfo info = asio.getDeviceInfo(id);
                if (info.outputChannels < 2) continue;

                if (!devicePattern.empty()) {
                    if (containsIgnoreCase(info.name, devicePattern)) {
                        targetDeviceId = id;
                        matchedDeviceName = info.name;
                        matchedDeviceInfo = info;
                        deviceFound = true;
                        break;
                    }
                } else {
                    if (containsIgnoreCase(info.name, "TerraTec") || containsIgnoreCase(info.name, "Phase")) {
                        targetDeviceId = id;
                        matchedDeviceName = info.name;
                        matchedDeviceInfo = info;
                        deviceFound = true;
                        break;
                    }
                }
            }

            if (!deviceFound) {
                LOG_INFO << "\n[ASIO] Waiting for device matching '"
                         << (devicePattern.empty() ? "TerraTec / Phase" : devicePattern)
                         << "' to become available...\n";
                for (int s = 0; s < 4 && g_running.load(std::memory_order_relaxed); ++s) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                continue;
            }

            LOG_INFO << "\n[ASIO] Connecting to device [" << targetDeviceId << "]: "
                     << matchedDeviceName << "\n";

            RtAudio::StreamParameters oParams;
            oParams.deviceId = targetDeviceId;
            oParams.nChannels = 2; // Stereo
            oParams.firstChannel = firstChannel; // Strictly Channel 0 (Analog L) and Channel 1 (Analog R)

            unsigned int currentBufferFrames = bufferFrames;
            constexpr unsigned int sampleRate = 48000;

            RtAudio::StreamOptions options;
            options.flags = RTAUDIO_SCHEDULE_REALTIME | RTAUDIO_MINIMIZE_LATENCY;
            options.streamName = "TerraTec AudioBridge";

            RtAudioFormat activeFormat = RTAUDIO_SINT32;
            if (formatChoice == "float32") {
                activeFormat = RTAUDIO_FLOAT32;
            } else if (formatChoice == "int32") {
                activeFormat = RTAUDIO_SINT32;
            } else {
                if (matchedDeviceInfo.nativeFormats & RTAUDIO_SINT32 || containsIgnoreCase(matchedDeviceName, "TerraTec")) {
                    activeFormat = RTAUDIO_SINT32;
                } else if (matchedDeviceInfo.nativeFormats & RTAUDIO_FLOAT32) {
                    activeFormat = RTAUDIO_FLOAT32;
                }
            }
            bridgeContext.streamFormat = activeFormat;

            ringBuffer.reset();
            asio.setErrorCallback(asioErrorCallback);

            RtAudioErrorType err = asio.openStream(
                &oParams,
                nullptr,
                activeFormat,
                sampleRate,
                &currentBufferFrames,
                &asioPlaybackCallback,
                &bridgeContext,
                &options
            );

            if (err != RTAUDIO_NO_ERROR) {
                if (!g_headless) std::cerr << "[ASIO Error] openStream failed: " << asio.getErrorText() << "\n";
                throw RtAudioError(err, asio.getErrorText());
            }

            if (!g_headless) {
                std::cout << "\n========================================================\n"
                          << " [ASIO Output Channels Discovery]\n"
                          << " Driver: " << matchedDeviceName << " (" << matchedDeviceInfo.outputChannels << " output channels)\n"
                          << "========================================================\n";

                for (unsigned int ch = 0; ch < matchedDeviceInfo.outputChannels; ++ch) {
                    ASIOChannelInfo chInfo = {};
                    chInfo.channel = static_cast<long>(ch);
                    chInfo.isInput = ASIOFalse;
                    ASIOError asioErr = ASIOGetChannelInfo(&chInfo);

                    std::string chName = (asioErr == ASE_OK && chInfo.name[0] != '\0') ?
                                         chInfo.name : ("Output " + std::to_string(ch + 1));
                    std::string typeStr = (asioErr == ASE_OK) ? asioSampleTypeToString(chInfo.type) : "Unknown";

                    if (ch == firstChannel) {
                        std::cout << "  [*] Channel " << ch << ": \"" << chName << "\" [" << typeStr << "]  <--- [ACTIVE: Analog Out 1 / Left]\n";
                    } else if (ch == firstChannel + 1) {
                        std::cout << "  [*] Channel " << ch << ": \"" << chName << "\" [" << typeStr << "]  <--- [ACTIVE: Analog Out 2 / Right]\n";
                    } else {
                        std::cout << "  [ ] Channel " << ch << ": \"" << chName << "\" [" << typeStr << "]\n";
                    }
                }
                std::cout << "========================================================\n";

                std::cout << "[ASIO Format] " << (activeFormat == RTAUDIO_SINT32 ? "Int32 (32-bit integer PCM)" : "Float32")
                          << (activeFormat == RTAUDIO_SINT32 ? " -> Direct static_cast<int32_t>(float * 2147483647.0f) active\n" : "\n");
            }

            err = asio.startStream();
            if (err != RTAUDIO_NO_ERROR) {
                if (!g_headless) std::cerr << "[ASIO Error] startStream failed: " << asio.getErrorText() << "\n";
                throw RtAudioError(err, asio.getErrorText());
            }

            LOG_INFO << "[ASIO Output] Active: " << sampleRate << " Hz, Channels " << firstChannel << " & " << (firstChannel + 1)
                     << ", Buffer: " << currentBufferFrames << " samples ("
                     << std::fixed << std::setprecision(2)
                     << (static_cast<double>(currentBufferFrames) * 1000.0 / sampleRate) << " ms latency)\n\n";

            while (g_running.load(std::memory_order_relaxed) && asio.isStreamRunning()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }

        } catch (const RtAudioError& e) {
            if (!g_headless) std::cerr << "\n[ASIO Fault Detected] RtAudioError: " << e.what() << "\n";
        } catch (const std::exception& e) {
            if (!g_headless) std::cerr << "\n[ASIO Fault Detected] Exception: " << e.what() << "\n";
        } catch (...) {
            if (!g_headless) std::cerr << "\n[ASIO Fault Detected] Unknown error occurred.\n";
        }

        if (asio.isStreamOpen()) {
            try {
                if (asio.isStreamRunning()) {
                    asio.stopStream();
                }
                asio.closeStream();
            } catch (...) {}
        }

        if (g_running.load(std::memory_order_relaxed)) {
            LOG_INFO << "\n[Fault Tolerance] FireWire/ASIO bus recovery: waiting 2 seconds before retry...\n";
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    if (monitorThread.joinable()) {
        monitorThread.join();
    }
    if (wasapiThread.joinable()) {
        wasapiThread.join();
    }

    LOG_INFO << "\n[System] AudioBridge stopped cleanly. Goodbye!\n";
    return 0;
}
