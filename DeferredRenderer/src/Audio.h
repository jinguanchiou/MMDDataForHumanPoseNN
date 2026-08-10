#pragma once
#include <memory>
#include <string>

namespace dr {

// Minimal looping WAV player (XAudio2). Used for the dance BGM: it loops forever, can be
// paused/resumed in lockstep with the animation, and restarted from the top on replay so
// the music and the motion stay frame-aligned. All XAudio2 detail lives in the .cpp.
class Audio {
public:
    Audio();
    ~Audio();
    Audio(const Audio&)            = delete;
    Audio& operator=(const Audio&) = delete;

    bool Init();                          // create the XAudio2 engine + mastering voice
    bool Load(const std::wstring& wav);   // parse the .wav and build a looping source voice
    void Play();                          // start (first call submits the loop) or resume
    void Pause();                         // pause, keeping the playback cursor
    void Restart();                       // flush + resubmit + play from the beginning
    void Stop();                          // stop and drop the queued buffer

    bool   Loaded() const;
    float  LengthSeconds() const;         // effective loop period in seconds
    void   SetLoopSeconds(double seconds);// clamp the loop period (0 = whole clip); for equal-length sync
    double PositionSeconds() const;       // playback position within the current loop [0,length)
    bool   Playing() const;               // started and not paused

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dr
