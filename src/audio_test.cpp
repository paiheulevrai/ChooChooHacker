#include "audio_test.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

AudioTest::~AudioTest() { stop(); }

bool AudioTest::selectInput(const std::string& pcm) {
    const std::string prefix = "hw:CARD=";
    const auto separator = pcm.find(",DEV=", prefix.size());
    if (pcm.compare(0, prefix.size(), prefix) != 0 || separator == std::string::npos ||
        separator == prefix.size() || separator + 5 == pcm.size() ||
        pcm.substr(prefix.size(), separator - prefix.size()).find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
        pcm.substr(separator + 5).find_first_not_of("0123456789") != std::string::npos) {
        error = "Invalid named ALSA capture input.";
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s", error.c_str());
        return false;
    }
    stop();
    capturePcm = pcm;
    return true;
}

bool AudioTest::init() {
    error.clear();
    if (SDL_WasInit(SDL_INIT_AUDIO)) return true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) return true;
    error = std::string("Audio initialization failed: ") + SDL_GetError();
    return false;
}

void AudioTest::capture(void* data, Uint8* buffer, int bytes) {
    auto& test = *static_cast<AudioTest*>(data);
    auto* samples = reinterpret_cast<const int16_t*>(buffer);
    const size_t count = static_cast<size_t>(bytes) / (2 * sizeof(int16_t));
    float l = test.peakLeft.load() * 0.944f;
    float r = test.peakRight.load() * 0.944f;
    for (size_t i = 0; i < count; ++i) {
        l = std::max(l, std::fabs(static_cast<float>(samples[i * 2])) / 32768.0f);
        r = std::max(r, std::fabs(static_cast<float>(samples[i * 2 + 1])) / 32768.0f);
    }
    test.peakLeft.store(l); test.peakRight.store(r);
    if (!test.recordingFlag.load()) return;
    const size_t offset = test.written.load();
    const size_t available = test.take.size() / 2 - offset;
    const size_t copy = std::min(count, available);
    std::memcpy(test.take.data() + offset * 2, samples, copy * 2 * sizeof(int16_t));
    test.written.store(offset + copy);
    if (copy == available) {
        test.recordingFlag.store(false);
        test.full.store(true);
    }
}

bool AudioTest::openInput() {
    if (input) return true;
    if (!init()) return false;
    SDL_AudioSpec spec{};
    spec.freq = rate; spec.format = AUDIO_S16SYS; spec.channels = 2;
    spec.samples = 512; spec.callback = capture; spec.userdata = this;
    const char* name = nullptr;
    for (int i = 0; capturePcm.empty() && i < SDL_GetNumAudioDevices(SDL_TRUE); ++i) {
        const char* candidate = SDL_GetAudioDeviceName(i, SDL_TRUE);
        if (candidate && std::strstr(candidate, "audiocodec")) { name = candidate; break; }
    }
    const char* driver = SDL_GetCurrentAudioDriver();
    const bool dummy = driver && std::strcmp(driver, "dummy") == 0;
    SDL_Log("Capture request: device=%s driver=%s rate=%d channels=%d samples=%d",
        name ? name : capturePcm.empty() ? "(none)" : capturePcm.c_str(),
        driver ? driver : "(none)", spec.freq, spec.channels, spec.samples);
    if (name || dummy) input = SDL_OpenAudioDevice(name, SDL_TRUE, &spec, nullptr, 0);
    // NextUI's sysdefault/plughw can reject capture even when the raw PCM accepts it.
    if (!input && driver && std::strcmp(driver, "alsa") == 0) {
        const char* before = std::getenv("AUDIODEV");
        const bool existed = before != nullptr;
        const std::string saved = existed ? before : "";
        const std::string raw = capturePcm.empty() ? "hw:CARD=audiocodec,DEV=0" : capturePcm;
        const std::string devices[]{raw, "plug" + raw};
        for (const std::string& device : devices) {
            SDL_Log("Capture unavailable: %s; trying %s.", SDL_GetError(), device.c_str());
            if (setenv("AUDIODEV", device.c_str(), 1) != 0) {
                error = "Cannot select the internal capture device.";
                break;
            }
            input = SDL_OpenAudioDevice(nullptr, SDL_TRUE, &spec, nullptr, 0);
            if (input) break;
        }
        const int restored = existed ? setenv("AUDIODEV", saved.c_str(), 1) : unsetenv("AUDIODEV");
        if (restored != 0) {
            closeInput(); error = "Cannot restore the AUDIODEV environment."; return false;
        }
        if (!error.empty()) return false;
    }
    if (!input) {
        error = std::string("Cannot capture selected input (48k/S16/stereo): ") + SDL_GetError();
        return false;
    }
    SDL_PauseAudioDevice(input, 0);
    SDL_Log("Capture opened: %s", driver ? driver : "unknown driver");
    return true;
}

void AudioTest::closeInput() {
    if (input) SDL_CloseAudioDevice(input);
    input = 0;
    recordingFlag.store(false); full.store(false);
    peakLeft.store(0); peakRight.store(0);
}

bool AudioTest::meter() {
    stop();
    return openInput();
}

bool AudioTest::record() {
    stop();
    take.resize(static_cast<size_t>(rate) * seconds * 2);
    written.store(0); full.store(false);
    if (!openInput()) return false;
    recordingFlag.store(true);
    return true;
}

bool AudioTest::play() {
    stop();
    const size_t count = written.load() * 2;
    if (count == 0) { error = "No take in memory. Record first."; return false; }
    if (!init()) return false;
    SDL_AudioSpec spec{};
    spec.freq = rate; spec.format = AUDIO_S16SYS; spec.channels = 2; spec.samples = 512;
    output = SDL_OpenAudioDevice(nullptr, SDL_FALSE, &spec, nullptr, 0);
    if (!output) { error = std::string("Playback open failed: ") + SDL_GetError(); return false; }
    std::vector<int16_t> attenuated(count);
    for (size_t i = 0; i < count; ++i) attenuated[i] = take[i] / 4;
    if (SDL_QueueAudio(output, attenuated.data(), static_cast<Uint32>(count * sizeof(int16_t))) != 0) {
        error = std::string("Playback queue failed: ") + SDL_GetError();
        stop(); return false;
    }
    SDL_PauseAudioDevice(output, 0);
    return true;
}

void AudioTest::stop() {
    closeInput();
    if (output) SDL_CloseAudioDevice(output);
    output = 0;
}

void AudioTest::update() {
    if (full.exchange(false)) closeInput();
    if (output && SDL_GetQueuedAudioSize(output) == 0) {
        SDL_CloseAudioDevice(output); output = 0;
    }
}

std::vector<std::pair<float, float>> AudioTest::waveform() {
    if (listening()) {
        error = "Stop capture before reading the waveform.";
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s", error.c_str());
        return {};
    }
    const size_t count = frames();
    const size_t columns = std::min<size_t>(256, count);
    std::vector<std::pair<float, float>> result(columns, {0, 0});
    for (size_t column = 0; column < columns; ++column) {
        for (size_t frame = column * count / columns; frame < (column + 1) * count / columns; ++frame) {
            for (size_t channel = 0; channel < 2; ++channel) {
                const float sample = static_cast<float>(take[frame * 2 + channel]) / 32768.0f;
                result[column].first = std::min(result[column].first, sample);
                result[column].second = std::max(result[column].second, sample);
            }
        }
    }
    return result;
}

bool AudioTest::selfTest() {
    AudioTest test;
    if (!test.selectInput("hw:CARD=USB_Audio-1,DEV=12") ||
        test.capturePcm != "hw:CARD=USB_Audio-1,DEV=12" ||
        test.selectInput("hw:CARD=bad;command,DEV=0") ||
        test.selectInput("hw:CARD=audiocodec,DEV=0,unsafe=yes") ||
        test.selectInput("hw:CARD=,DEV=0")) return false;
    test.take.resize(8);
    test.recordingFlag.store(true);
    int16_t samples[]{-32768, 16384, 0, 0, 8192, -32768, 1, 2, 222, 333};
    capture(&test, reinterpret_cast<Uint8*>(samples), sizeof(samples));
    if (test.frames() != 4 || test.recording() || !test.full.load()) return false;
    if (test.left() != 1.0f || test.right() != 1.0f) return false;
    if (!std::equal(test.take.begin(), test.take.end(), samples)) return false;
    capture(&test, reinterpret_cast<Uint8*>(samples), sizeof(samples));
    const auto wave = test.waveform();
    return test.frames() == 4 && wave.size() == 4 &&
        wave[0].first == -1.0f && wave[0].second == 0.5f &&
        wave[2].first == -1.0f && wave[2].second == 0.25f;
}
