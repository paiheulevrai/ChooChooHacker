#pragma once
#include <SDL.h>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

class AudioTest {
public:
    static constexpr int rate = 48000;
    static constexpr int seconds = 3;
    ~AudioTest();
    bool meter();
    bool record();
    bool play();
    bool selectInput(const std::string& pcm);
    void stop();
    void update();
    bool listening() const { return input != 0; }
    bool playing() const { return output != 0; }
    bool recording() const { return recordingFlag.load(); }
    size_t frames() const { return written.load(); }
    float left() const { return peakLeft.load(); }
    float right() const { return peakRight.load(); }
    std::vector<std::pair<float, float>> waveform();
    std::string error;
    static bool selfTest();

private:
    SDL_AudioDeviceID input = 0, output = 0;
    std::string capturePcm;
    std::vector<int16_t> take;
    std::atomic<size_t> written{0};
    std::atomic<bool> recordingFlag{false}, full{false};
    std::atomic<float> peakLeft{0}, peakRight{0};
    bool init();
    bool openInput();
    void closeInput();
    static void capture(void* data, Uint8* buffer, int bytes);
};
