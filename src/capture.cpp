#include "capture.h"

#include <mmdeviceapi.h>
#include <audioclientactivationparams.h>
#include <avrt.h>

void FillMixFormat(WAVEFORMATEX& wf) {
    wf = {};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = kChannels;
    wf.nSamplesPerSec = kSampleRate;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
}

// ---------------------------------------------------------------- CaptureSource

CaptureSource::CaptureSource() : ring_(kSampleRate * kChannels) {}  // 1 s of headroom

CaptureSource::~CaptureSource() { Stop(); }

bool CaptureSource::Start() {
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
    if (!thread_) return false;
    WaitForSingleObject(ready_, 15000);
    return SUCCEEDED(initHr_);
}

void CaptureSource::Stop() {
    if (thread_) {
        SetEvent(stop_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
    if (ready_) { CloseHandle(ready_); ready_ = nullptr; }
}

DWORD WINAPI CaptureSource::ThreadEntry(LPVOID self) {
    static_cast<CaptureSource*>(self)->Run();
    return 0;
}

void CaptureSource::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    HANDLE dataEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    WAVEFORMATEX wf;
    FillMixFormat(wf);
    HRESULT hr = CreateClient(&client);
    if (SUCCEEDED(hr))
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                StreamFlags() | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                200000 /* 20 ms */, 0, &wf, nullptr);
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(dataEvent);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(hr)) hr = client->Start();

    initHr_ = hr;
    SetEvent(ready_);

    if (SUCCEEDED(hr)) {
        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        HANDLE waits[3] = {stop_, dataEvent, extraWait_};
        DWORD waitCount = extraWait_ ? 3 : 2;
        for (;;) {
            DWORD w = WaitForMultipleObjects(waitCount, waits, FALSE, 1000);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_OBJECT_0 + 2) { dead_ = true; break; }  // target process exited

            UINT32 packet = 0;
            while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet) {
                BYTE* data;
                UINT32 frames;
                DWORD flags;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                    ring_.WriteSilence(frames * kChannels);
                else
                    ring_.Write(reinterpret_cast<float*>(data), frames * kChannels);
                capture->ReleaseBuffer(frames);
            }
            if (FAILED(hr)) { dead_ = true; break; }  // e.g. AUDCLNT_E_DEVICE_INVALIDATED
        }

        client->Stop();
        if (task) AvRevertMmThreadCharacteristics(task);
    } else {
        dead_ = true;
    }

    if (capture) capture->Release();
    if (client) client->Release();
    CloseHandle(dataEvent);
    CoUninitialize();
}

// --------------------------------------------------------------- ProcessCapture

namespace {

// Completion handler for ActivateAudioInterfaceAsync. Must be agile (free-threaded).
class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_FAIL;
    IAudioClient* client = nullptr;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *ppv = static_cast<IAgileObject*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hrActivate = E_FAIL;
        IUnknown* unk = nullptr;
        HRESULT hr = op->GetActivateResult(&hrActivate, &unk);
        if (SUCCEEDED(hr)) hr = hrActivate;
        if (SUCCEEDED(hr) && unk) hr = unk->QueryInterface(IID_PPV_ARGS(&client));
        if (unk) unk->Release();
        result = hr;
        SetEvent(done);
        return S_OK;
    }

private:
    ~ActivateHandler() {
        if (client) client->Release();
        CloseHandle(done);
    }
    LONG ref_ = 1;
};

}  // namespace

ProcessCapture::ProcessCapture(DWORD pid) : pid_(pid) {
    extraWait_ = OpenProcess(SYNCHRONIZE, FALSE, pid);
}

ProcessCapture::~ProcessCapture() {
    Stop();
    if (extraWait_) CloseHandle(extraWait_);
}

HRESULT ProcessCapture::CreateClient(IAudioClient** out) {
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid_;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv = {};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivateHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                             __uuidof(IAudioClient), &pv, handler, &op);
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObject(handler->done, 5000) == WAIT_OBJECT_0) {
            hr = handler->result;
            if (SUCCEEDED(hr)) {
                *out = handler->client;
                handler->client = nullptr;
            }
        } else {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }
    if (op) op->Release();
    handler->Release();
    return hr;
}

// ---------------------------------------------------------------- DeviceCapture

HRESULT DeviceCapture::CreateClient(IAudioClient** out) {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDevice(id_.c_str(), &device);
    if (SUCCEEDED(hr))
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(out));
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    return hr;
}
