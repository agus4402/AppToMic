#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "capture.h"

// Mixes the selected apps + the real mic and plays the result into "CABLE Input".
class Engine {
public:
    ~Engine() { Stop(); }

    bool Start(std::wstring& error);
    void Stop();
    bool Running() const { return running_; }
    bool RenderFailed() const { return renderDead_; }

    // Attach/detach process captures so they match the given exe names (lowercase).
    // Also re-attaches apps that were restarted. Call periodically while running.
    void SyncApps(const std::set<std::wstring>& exeNames);
    void SetMic(const std::wstring& deviceId);  // empty = no mic

    void SetAppGain(float g) { appGain_ = g; }
    void SetMicGain(float g) { micGain_ = g; }

    size_t ActiveApps();
    bool MicActive();
    bool DefaultOutputIsCable() const;

private:
    static DWORD WINAPI RenderEntry(LPVOID self);
    void RenderLoop();
    void Mix(float* out, UINT32 frames);
    void StartMic();

    std::mutex mu_;  // guards apps_ and mic_ (UI thread vs render thread)
    std::vector<std::unique_ptr<ProcessCapture>> apps_;
    std::unique_ptr<DeviceCapture> mic_;
    std::set<DWORD> failedPids_;

    std::wstring micId_;
    std::wstring renderId_;
    std::atomic<float> appGain_{1.0f};
    std::atomic<float> micGain_{1.0f};
    std::atomic<bool> running_{false};
    std::atomic<bool> renderDead_{false};

    HANDLE thread_ = nullptr;
    HANDLE stop_ = nullptr;
    HANDLE ready_ = nullptr;
    HRESULT initHr_ = E_FAIL;
    std::vector<float> scratch_;
};
