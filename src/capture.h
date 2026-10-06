#pragma once
#include <windows.h>
#include <audioclient.h>
#include <atomic>
#include <string>
#include "ringbuffer.h"

// Internal format used by every stream: 48 kHz, float32, stereo interleaved.
constexpr UINT32 kSampleRate = 48000;
constexpr UINT32 kChannels = 2;
void FillMixFormat(WAVEFORMATEX& wf);

// A WASAPI capture stream running on its own thread, feeding a ring buffer.
class CaptureSource {
public:
    CaptureSource();
    virtual ~CaptureSource();
    CaptureSource(const CaptureSource&) = delete;
    CaptureSource& operator=(const CaptureSource&) = delete;

    bool Start();  // blocks until the stream is running (or failed)
    void Stop();
    bool IsDead() const { return dead_; }
    RingBuffer& Ring() { return ring_; }

    bool primed = false;  // owned by the render thread (jitter buffer state)

protected:
    virtual HRESULT CreateClient(IAudioClient** out) = 0;
    virtual DWORD StreamFlags() const = 0;
    HANDLE extraWait_ = nullptr;  // optional handle that, when signaled, kills the stream

private:
    static DWORD WINAPI ThreadEntry(LPVOID self);
    void Run();

    RingBuffer ring_;
    HANDLE thread_ = nullptr;
    HANDLE stop_ = nullptr;
    HANDLE ready_ = nullptr;
    HRESULT initHr_ = E_FAIL;
    std::atomic<bool> dead_{false};
};

// Captures everything a process (and its child processes) plays.
class ProcessCapture : public CaptureSource {
public:
    explicit ProcessCapture(DWORD pid);
    ~ProcessCapture() override;
    DWORD Pid() const { return pid_; }

protected:
    HRESULT CreateClient(IAudioClient** out) override;
    DWORD StreamFlags() const override { return AUDCLNT_STREAMFLAGS_LOOPBACK; }

private:
    DWORD pid_;
};

// Captures a regular input device (the real microphone).
class DeviceCapture : public CaptureSource {
public:
    explicit DeviceCapture(std::wstring deviceId) : id_(std::move(deviceId)) {}
    ~DeviceCapture() override { Stop(); }

protected:
    HRESULT CreateClient(IAudioClient** out) override;
    DWORD StreamFlags() const override { return AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY; }

private:
    std::wstring id_;
};
