#include "../src/audio_test.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    const bool hardware = argc == 2 && std::strcmp(argv[1], "--hardware") == 0;
    if (argc != 1 && !hardware) {
        std::fputs("Usage: audio-test [--hardware]\n", stderr); return 2;
    }
    if (!AudioTest::selfTest()) {
        std::fputs("Peak normalization, stereo copy or bounded-take check failed.\n", stderr);
        return 1;
    }
    if (!hardware) SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    AudioTest audio;
    if (!audio.record()) { std::fprintf(stderr, "%s\n", audio.error.c_str()); return 1; }
    SDL_Delay(100);
    audio.stop();
    if (audio.frames() == 0 || !audio.play()) {
        std::fprintf(stderr, "Record/play failed: %s\n", audio.error.c_str()); return 1;
    }
    SDL_Delay(250);
    audio.update();
    if (audio.playing() || audio.listening()) {
        std::fputs("Audio devices did not stop.\n", stderr); return 1;
    }
    if (!audio.record()) { std::fprintf(stderr, "%s\n", audio.error.c_str()); return 1; }
    const Uint32 started = SDL_GetTicks();
    float left = 0, right = 0;
    if (hardware) { std::puts("Recording for three seconds. Make a sound near the console."); std::fflush(stdout); }
    while (audio.listening() && SDL_GetTicks() - started < 12000) {
        SDL_Delay(20);
        left = std::max(left, audio.left());
        right = std::max(right, audio.right());
        audio.update();
    }
    if (audio.listening() || audio.recording() ||
        audio.frames() != static_cast<size_t>(AudioTest::rate) * AudioTest::seconds) {
        std::fputs("Recording did not stop at exactly three seconds.\n", stderr); return 1;
    }
    const auto wave = audio.waveform();
    if (wave.size() != 256) { std::fputs("Completed take has no bounded waveform.\n", stderr); return 1; }
    if (hardware) {
        std::printf("Captured %zu frames. Peak L=%.4f R=%.4f. Playing at 25%% digital level.\n",
            audio.frames(), left, right);
        std::fflush(stdout);
        if (!audio.play()) { std::fprintf(stderr, "%s\n", audio.error.c_str()); return 1; }
        const Uint32 playback = SDL_GetTicks();
        while (audio.playing() && SDL_GetTicks() - playback < 12000) {
            SDL_Delay(20);
            audio.update();
        }
        if (audio.playing() || audio.listening()) {
            std::fputs("Hardware playback did not stop.\n", stderr); return 1;
        }
    }
    std::puts("Audio checks passed: stereo peaks, partial take, waveform, replay and three-second auto-stop.");
    return 0;
}
