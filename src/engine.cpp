#include "engine.h"

#include <mmdeviceapi.h>
#include <avrt.h>
#include <algorithm>
#include <cstdio>

#include "sessions.h"

namespace {
constexpr size_t kPrimeFrames = kSampleRate * 20 / 1000;     // wait for 20 ms before playing a source
constexpr size_t kMaxLatencyFrames = kSampleRate * 100 / 1000; // drop audio if a source lags > 100 ms
}  // namespace

bool Engine::Start(std::wstring& error) {
    if (running_) return true;

    renderId_ = FindDeviceByName(eRender, L"CABLE Input");
    if (renderId_.empty()) {
        error = L"No se encontró VB-Cable (\"CABLE Input\"). Instalalo desde vb-audio.com/Cable y reiniciá.";
        return false;
    }

    renderDead_ = false;
    initHr_ = E_FAIL;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, RenderEntry, this, 0, nullptr);
    if (thread_) WaitForSingleObject(ready_, 10000);
    if (FAILED(initHr_)) {
        wchar_t msg[128];
        swprintf_s(msg, L"No se pudo abrir CABLE Input (error 0x%08X).", static_cast<unsigned>(initHr_));
        error = msg;
        Stop();
        return false;
    }

    running_ = true;
    StartMic();
    return true;
}

void Engine::Stop() {
    if (thread_) {
        SetEvent(stop_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
    if (ready_) { CloseHandle(ready_); ready_ = nullptr; }

    std::vector<std::unique_ptr<ProcessCapture>> apps;
    std::unique_ptr<DeviceCapture> mic;
    {
        std::lock_guard<std::mutex> lock(mu_);
        apps.swap(apps_);
        mic.swap(mic_);
    }
    // Destructors stop the capture threads, outside the lock.
    apps.clear();
    mic.reset();
    failedPids_.clear();
    running_ = false;
}

void Engine::SyncApps(const std::set<std::wstring>& exeNames) {
    if (!running_) return;

    std::vector<DWORD> wanted = FindRootPids(exeNames);
    std::vector<std::unique_ptr<ProcessCapture>> removed;
    std::set<DWORD> have;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = apps_.begin(); it != apps_.end();) {
            bool keep = !(*it)->IsDead() &&
                        std::find(wanted.begin(), wanted.end(), (*it)->Pid()) != wanted.end();
            if (keep) {
                have.insert((*it)->Pid());
                ++it;
            } else {
                removed.push_back(std::move(*it));
                it = apps_.erase(it);
            }
        }
    }
    removed.clear();

    for (DWORD pid : wanted) {
        if (have.count(pid) || failedPids_.count(pid)) continue;
        auto capture = std::make_unique<ProcessCapture>(pid);
        if (capture->Start()) {
            std::lock_guard<std::mutex> lock(mu_);
            apps_.push_back(std::move(capture));
        } else {
            failedPids_.insert(pid);
        }
    }

    // Retry the mic if it was unplugged and came back.
    bool micDead;
    {
        std::lock_guard<std::mutex> lock(mu_);
        micDead = !micId_.empty() && (!mic_ || mic_->IsDead());
    }
    if (micDead) SetMic(micId_);
}

void Engine::SetMic(const std::wstring& deviceId) {
    micId_ = deviceId;
    if (!running_) return;
    std::unique_ptr<DeviceCapture> old;
    {
        std::lock_guard<std::mutex> lock(mu_);
        old.swap(mic_);
    }
    old.reset();
    StartMic();
}

void Engine::StartMic() {
    if (micId_.empty()) return;
    auto mic = std::make_unique<DeviceCapture>(micId_);
    if (mic->Start()) {
        std::lock_guard<std::mutex> lock(mu_);
        mic_ = std::move(mic);
    }
}

size_t Engine::ActiveApps() {
    std::lock_guard<std::mutex> lock(mu_);
    return std::count_if(apps_.begin(), apps_.end(), [](const auto& a) { return !a->IsDead(); });
}

bool Engine::MicActive() {
    std::lock_guard<std::mutex> lock(mu_);
    return mic_ && !mic_->IsDead();
}

bool Engine::DefaultOutputIsCable() const {
    return !renderId_.empty() && DefaultDeviceId(eRender, eConsole) == renderId_;
}

DWORD WINAPI Engine::RenderEntry(LPVOID self) {
    static_cast<Engine*>(self)->RenderLoop();
    return 0;
}

void Engine::RenderLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT32 bufferFrames = 0;

    WAVEFORMATEX wf;
    FillMixFormat(wf);
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDevice(renderId_.c_str(), &device);
    if (SUCCEEDED(hr))
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr))
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                200000 /* 20 ms */, 0, &wf, nullptr);
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
    if (SUCCEEDED(hr)) {
        BYTE* data;
        if (SUCCEEDED(render->GetBuffer(bufferFrames, &data)))
            render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        hr = client->Start();
    }

    initHr_ = hr;
    SetEvent(ready_);

    if (SUCCEEDED(hr)) {
        scratch_.resize(static_cast<size_t>(bufferFrames) * kChannels);
        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        HANDLE waits[2] = {stop_, event};
        for (;;) {
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, 1000);
            if (w == WAIT_OBJECT_0) break;

            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;
            UINT32 frames = bufferFrames - padding;
            if (!frames) continue;

            BYTE* data;
            hr = render->GetBuffer(frames, &data);
            if (FAILED(hr)) break;
            Mix(reinterpret_cast<float*>(data), frames);
            render->ReleaseBuffer(frames, 0);
        }
        if (FAILED(hr)) renderDead_ = true;

        client->Stop();
        if (task) AvRevertMmThreadCharacteristics(task);
    }

    if (render) render->Release();
    if (client) client->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    CloseHandle(event);
    CoUninitialize();
}

void Engine::Mix(float* out, UINT32 frames) {
    const size_t samples = static_cast<size_t>(frames) * kChannels;
    std::fill(out, out + samples, 0.0f);
    float* tmp = scratch_.data();

    auto add = [&](CaptureSource& src, float gain) {
        RingBuffer& ring = src.Ring();
        size_t avail = ring.Available() / kChannels;
        if (!src.primed) {
            // Small jitter buffer so bursty sources don't crackle.
            if (avail < kPrimeFrames + frames) return;
            src.primed = true;
        }
        if (avail > kMaxLatencyFrames + frames) {
            // Clock drift / stall made this source lag behind: drop the oldest audio.
            ring.Skip((avail - kPrimeFrames - frames) * kChannels);
            avail = kPrimeFrames + frames;
        }
        size_t n = std::min<size_t>(frames, avail) * kChannels;
        ring.Read(tmp, n);
        for (size_t i = 0; i < n; ++i) out[i] += tmp[i] * gain;
        if (n < samples) src.primed = false;  // underrun: re-buffer
    };

    const float appGain = appGain_;
    const float micGain = micGain_;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& app : apps_) add(*app, appGain);
        if (mic_) add(*mic_, micGain);
    }

    for (size_t i = 0; i < samples; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}
