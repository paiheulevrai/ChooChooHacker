#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cerrno>
#include <charconv>
#include <limits.h>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "audio_test.h"

extern "C" const uint8_t font12x16[];
extern char** environ;

static std::string controlScript;

static bool locateBackend() {
    char executable[PATH_MAX];
    const ssize_t count = readlink("/proc/self/exe", executable, sizeof(executable));
    if (count < 0 || static_cast<size_t>(count) == sizeof(executable)) {
        std::cerr << "ERROR: Cannot locate the application executable.\n";
        return false;
    }
    const std::string path(executable, static_cast<size_t>(count));
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) {
        std::cerr << "ERROR: Cannot locate the application directory.\n";
        return false;
    }
    controlScript = path.substr(0, slash + 1) + "control.sh";
    if (access(controlScript.c_str(), R_OK) != 0) {
        std::cerr << "ERROR: Cannot read backend script: " << controlScript << '\n';
        return false;
    }
    return true;
}

static std::vector<std::string> backendCommand(const std::string& action, const std::vector<std::string>& arguments) {
    std::vector<std::string> command{"timeout", action == "discover" || action == "controls" ? "120" :
        action == "diagnostics" || action == "export-log" ? "180" :
        action == "status" ? "25" : "360", "sh", controlScript, action};
    command.insert(command.end(), arguments.begin(), arguments.end());
    return command;
}

[[noreturn]] static void execControl(const std::string& action, const std::vector<std::string>& arguments = {}) {
    auto command = backendCommand(action, arguments);
    std::vector<char*> argv;
    for (auto& argument : command) argv.push_back(argument.data());
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    std::perror("ERROR: Cannot start backend");
    _exit(127);
}

static void usage(std::ostream& out) {
    out << "Usage: choochoohacker.aarch64 [command]\n"
        << "No command: open the interface.\n\n"
        << "  status       Print read-only KEY=VALUE device settings.\n"
        << "  mic-on       Enable the mic, preserving the persistence choice.\n"
        << "  mic-off      Disable the mic, preserving the persistence choice.\n"
        << "  restore      Undo temporary changes and remove the reset hook.\n"
        << "  persist-on   Keep the chosen mic state at every NextUI boot.\n"
        << "  persist-off  Remove the boot hook and restore original settings.\n"
        << "  diagnostics  Print a read-only audio/MIDI report.\n"
        << "  export-log   Save that report; print REPORT=absolute-path.\n"
        << "  inputs       List named ALSA capture inputs (read-only).\n"
        << "  discover     Briefly capture each input; save discovery.log.\n"
        << "  controls     List safely editable ALSA input controls.\n"
        << "  experiment-set CARD NUMID VALUES  Change input temporarily.\n"
        << "  experiment-restore               Undo all input experiments.\n"
        << "  --help       Show this help.\n\n"
        << "Diagnostics: --smoke-test PREFIX, --self-test, --probe-input\n"
        << "Capture probes: --probe-input or --probe-device hw:CARD=ID,DEV=N.\n"
        << "Discover and capture probes open inputs; no audio is saved.\n";
}

struct ControlProcess {
    std::mutex mutex;
    pid_t pid = 0;
    bool cancelled = false;
};

struct Reply {
    int code = 1;
    std::string output;
};

static Reply control(const std::string& action, const std::vector<std::string>& arguments = {},
                     ControlProcess* process = nullptr) {
    int channel[2];
    if (pipe(channel) != 0) return {1, "ERROR: Cannot create backend pipe."};
    auto command = backendCommand(action, arguments);
    std::vector<char*> argv;
    for (auto& argument : command) argv.push_back(argument.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        close(channel[0]); close(channel[1]);
        return {1, "ERROR: Cannot prepare backend process."};
    }
    if ((result = posix_spawn_file_actions_adddup2(&actions, channel[1], STDOUT_FILENO)) == 0 &&
        (result = posix_spawn_file_actions_adddup2(&actions, channel[1], STDERR_FILENO)) == 0 &&
        (result = posix_spawn_file_actions_addclose(&actions, channel[0])) == 0)
        result = posix_spawn_file_actions_addclose(&actions, channel[1]);
    pid_t pid = 0;
    if (result == 0) result = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        close(channel[0]); close(channel[1]);
        return {1, "ERROR: Cannot start backend (error " + std::to_string(result) + ")."};
    }
    if (process) {
        std::lock_guard<std::mutex> lock(process->mutex);
        process->pid = pid;
        if (process->cancelled && kill(pid, SIGTERM) != 0 && errno != ESRCH)
            std::perror("ERROR: Cannot stop discovery");
    }
    close(channel[1]);
    std::string output;
    bool truncated = false;
    char buffer[1024];
    ssize_t count;
    while ((count = read(channel[0], buffer, sizeof(buffer))) != 0) {
        if (count < 0) {
            if (errno == EINTR) continue;
            output += "\nERROR: Backend output read failed.";
            break;
        }
        const size_t available = 32768 - output.size();
        const size_t copied = std::min(available, static_cast<size_t>(count));
        output.append(buffer, copied);
        if (copied != static_cast<size_t>(count)) truncated = true;
    }
    if (truncated) output += "\n[More details in the exported log.]\n";
    close(channel[0]);
    int status = 0;
    std::unique_lock<std::mutex> lock;
    if (process) lock = std::unique_lock<std::mutex>(process->mutex);
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            if (process) process->pid = 0;
            return {1, output + "\nERROR: Cannot collect backend result."};
        }
    }
    if (process) process->pid = 0;
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 1, output};
}

static std::map<std::string, std::string> parse(const std::string& output) {
    std::map<std::string, std::string> result;
    std::istringstream input(output);
    std::string line;
    while (std::getline(input, line)) {
        const auto at = line.find('=');
        if (at != std::string::npos) result[line.substr(0, at)] = line.substr(at + 1);
    }
    return result;
}

static std::vector<std::string> wrap(const std::string& input, size_t width) {
    std::vector<std::string> lines;
    std::istringstream words(input);
    std::string word, line;
    while (words >> word) {
        while (word.size() > width) {
            if (!line.empty()) { lines.push_back(line); line.clear(); }
            lines.push_back(word.substr(0, width));
            word.erase(0, width);
        }
        if (!line.empty() && line.size() + word.size() + 1 > width) {
            lines.push_back(line); line.clear();
        }
        if (!line.empty()) line += ' ';
        line += word;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// ChooChooTracker's Dubwise.cth palette; the disco ball keeps its neon colours.
constexpr SDL_Color ink{0x10, 0x13, 0x0f, 255};
constexpr SDL_Color white{0xd8, 0xd6, 0xc8, 255};
constexpr SDL_Color title{0xf0, 0xe9, 0xd6, 255};
constexpr SDL_Color green{0x56, 0xb4, 0x5c, 255};
constexpr SDL_Color gold{0xe2, 0xbb, 0x3d, 255};
constexpr SDL_Color muted{0x7c, 0x86, 0x75, 255};
constexpr SDL_Color grid{0x25, 0x2a, 0x22, 255};
constexpr SDL_Color selection{0x30, 0x34, 0x26, 255};
constexpr SDL_Color warning{0xdf, 0x49, 0x3c, 255};

class Screen {
public:
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* font = nullptr;
    SDL_Surface* offscreenSurface = nullptr;

    bool init(bool offscreen) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (SDL_Init(offscreen ? 0 : SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return false;
        if (offscreen) {
            offscreenSurface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_ARGB8888);
            if (!offscreenSurface) return false;
            renderer = SDL_CreateSoftwareRenderer(offscreenSurface);
        } else {
            window = SDL_CreateWindow("ChooChooHacker", SDL_WINDOWPOS_CENTERED,
                SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_FULLSCREEN_DESKTOP);
            if (!window) return false;
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
            if (!renderer) {
                SDL_Log("Accelerated renderer unavailable: %s; trying software.", SDL_GetError());
                renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
            }
        }
        if (!renderer || SDL_RenderSetLogicalSize(renderer, 800, 450) != 0) return false;
        SDL_Surface* atlas = SDL_CreateRGBSurfaceWithFormat(0, 192, 96, 32, SDL_PIXELFORMAT_RGBA32);
        if (!atlas) return false;
        SDL_FillRect(atlas, nullptr, SDL_MapRGBA(atlas->format, 0, 0, 0, 0));
        for (int ch = 0; ch < 95; ++ch) {
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 12; ++x) {
                    if (font12x16[ch * 32 + y * 2 + x / 8] & (0x80 >> (x % 8))) {
                        auto* row = reinterpret_cast<Uint32*>(static_cast<Uint8*>(atlas->pixels) +
                            ((ch / 16) * 16 + y) * atlas->pitch);
                        row[(ch % 16) * 12 + x] = SDL_MapRGBA(atlas->format, 255, 255, 255, 255);
                    }
                }
            }
        }
        font = SDL_CreateTextureFromSurface(renderer, atlas);
        SDL_FreeSurface(atlas);
        return font && SDL_SetTextureBlendMode(font, SDL_BLENDMODE_BLEND) == 0;
    }

    ~Screen() {
        SDL_DestroyTexture(font); SDL_DestroyRenderer(renderer);
        SDL_FreeSurface(offscreenSurface);
        SDL_DestroyWindow(window); SDL_Quit();
    }

    void rect(int x, int y, int w, int h, SDL_Color color) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_Rect area{x, y, w, h};
        SDL_RenderFillRect(renderer, &area);
    }

    void text(int x, int y, const std::string& str, SDL_Color color, int scale = 1, int limit = 62) {
        SDL_SetTextureColorMod(font, color.r, color.g, color.b);
        int n = 0;
        for (unsigned char ch : str) {
            if (n++ >= limit) break;
            if (ch < 32 || ch > 126) ch = '?';
            const int index = ch - 32;
            SDL_Rect source{(index % 16) * 12, (index / 16) * 16, 12, 16};
            SDL_Rect dest{x, y, 12 * scale, 16 * scale};
            SDL_RenderCopy(renderer, font, &source, &dest);
            x += 12 * scale;
        }
    }

    void disco(Uint32 tick) {
        rect(712, 0, 1, 21, muted);
        for (int y = -36; y < 36; y += 6) {
            int width = static_cast<int>(std::sqrt(36 * 36 - y * y));
            for (int x = -width; x < width; x += 7) {
                double light = std::sin(x * 0.13 + y * 0.1 + tick * 0.0015);
                SDL_Color color = light > 0.72 ? SDL_Color{224, 234, 237, 255} :
                    light > 0.25 ? SDL_Color{0, 245, 220, 255} :
                    light > -0.3 ? SDL_Color{255, 42, 149, 255} : grid;
                rect(712 + x, 57 + y, 5, 4, color);
            }
        }
    }

    bool save(const std::string& path) {
        int w, h;
        if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0) return false;
        SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!shot) return false;
        bool ok = SDL_RenderReadPixels(renderer, nullptr, shot->format->format, shot->pixels, shot->pitch) == 0;
        if (ok) ok = SDL_SaveBMP(shot, path.c_str()) == 0;
        SDL_FreeSurface(shot);
        return ok;
    }
};

enum class Key { none, left, right, up, down, accept, cancel, quit, refresh, explore };

static Key keyboard(SDL_Keycode k) {
    switch (k) {
        case SDLK_LEFT: return Key::left;
        case SDLK_RIGHT: case SDLK_TAB: return Key::right;
        case SDLK_UP: return Key::up;
        case SDLK_DOWN: return Key::down;
        case SDLK_RETURN: case SDLK_SPACE: return Key::accept;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: return Key::cancel;
        case SDLK_q: return Key::quit;
        case SDLK_r: return Key::refresh;
        case SDLK_d: return Key::explore;
        default: return Key::none;
    }
}

static Key button(Uint8 b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return Key::left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Key::right;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return Key::up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Key::down;
        case SDL_CONTROLLER_BUTTON_A: return Key::accept;
        case SDL_CONTROLLER_BUTTON_B: return Key::cancel;
        case SDL_CONTROLLER_BUTTON_START: return Key::quit;
        case SDL_CONTROLLER_BUTTON_X: return Key::refresh;
        case SDL_CONTROLLER_BUTTON_Y: return Key::explore;
        default: return Key::none;
    }
}

class App {
    Screen& screen;
    AudioTest audio;
    std::map<std::string, std::string> info;
    std::thread worker;
    ControlProcess process;
    std::atomic<bool> done{false};
    bool busy = false, quitAfter = false;
    Reply reply;
    std::string job, confirm, notice;
    Uint32 refreshed = 0;
    int selected = 0, infoOffset = 0;
    bool needReport = false;
    std::vector<std::string> reportLines;
    enum class TakePhase { closed, recording, playing, done };
    TakePhase takePhase = TakePhase::closed;
    std::vector<std::pair<float, float>> wave;
    struct InputControl {
        std::string card, numid, name, type, values;
        int minimum = 0, maximum = 0, channels = 0;
    };
    std::vector<InputControl> inputControls;
    int controlIndex = 0;
    std::string proposedValue, selectedInput;
    bool closingExperiments = false;

    std::string get(const std::string& key) const {
        const auto it = info.find(key);
        return it == info.end() || it->second.empty() ? "unavailable" : it->second;
    }
    void start(const std::string& action, const std::vector<std::string>& arguments = {}) {
        if (action != "status") audio.stop();
        job = action; busy = true; done = false;
        {
            std::lock_guard<std::mutex> lock(process.mutex);
            process.cancelled = false;
        }
        worker = std::thread([this, action, arguments] {
            reply = control(action, arguments, &process); done.store(true);
        });
    }
    void line(int row, const std::string& str, SDL_Color color = white) {
        screen.text(40, 150 + row * 24, str, color, 1, 60);
    }
    void menu(int row, const std::string& label) {
        const int y = 202 + row * 42;
        if (row == selected) screen.rect(32, y - 9, 736, 36, selection);
        screen.text(48, y, row == selected ? ">" : " ", gold);
        screen.text(80, y, label, row == selected ? gold : white, 1, 55);
    }
    bool persistent() const { return get("PERSIST") == "enabled"; }
    bool enabled() const { return get("MIC_STATE") == "on"; }
    void setReport(const std::string& text) {
        reportLines.clear();
        std::istringstream input(text);
        std::string row;
        while (std::getline(input, row)) {
            const auto lines = wrap(row, 60);
            if (lines.empty()) reportLines.emplace_back();
            else reportLines.insert(reportLines.end(), lines.begin(), lines.end());
        }
        infoOffset = 0;
    }
    void audioError() {
        SDL_Log("Audio error: %s", audio.error.c_str());
        audio.stop();
        takePhase = TakePhase::closed;
        notice = "Could not test the mic. Details are in Info.";
    }
    void drawWave() {
        screen.rect(60, 274, 680, 1, selection);
        float peak = 0;
        for (const auto& bin : wave) peak = std::max({peak, -bin.first, bin.second});
        const float scale = peak > 0.001f ? 52.0f / peak : 52.0f;
        for (size_t i = 0; i < wave.size(); ++i) {
            const int x = 60 + static_cast<int>(i * 680 / wave.size());
            const int top = 274 - static_cast<int>(wave[i].second * scale);
            const int bottom = 274 - static_cast<int>(wave[i].first * scale);
            screen.rect(x, top, 2, std::max(1, bottom - top), green);
        }
    }
    static bool integer(const std::string& text, int& value) {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    }
    bool readControls(const std::string& output) {
        inputControls.clear();
        std::istringstream input(output);
        std::string row;
        while (std::getline(input, row)) {
            if (row.compare(0, 8, "CONTROL_") != 0) continue;
            const auto equals = row.find('=');
            if (equals == std::string::npos) return false;
            std::istringstream fields(row.substr(equals + 1));
            std::array<std::string, 8> values;
            for (auto& value : values) if (!std::getline(fields, value, '|')) return false;
            InputControl item;
            item.card = values[0]; item.numid = values[1]; item.name = values[2];
            item.type = values[3]; item.values = values[7];
            if (!integer(values[4], item.minimum) || !integer(values[5], item.maximum) ||
                !integer(values[6], item.channels) || item.minimum > item.maximum ||
                item.channels < 1 || item.channels > 8 ||
                (item.type != "BOOLEAN" && item.type != "INTEGER" && item.type != "ENUMERATED"))
                return false;
            inputControls.push_back(item);
        }
        controlIndex = std::clamp(controlIndex, 0, std::max(0, static_cast<int>(inputControls.size()) - 1));
        if (!inputControls.empty()) proposedValue = inputControls[controlIndex].values;
        return true;
    }
    void changeValue(int direction) {
        if (inputControls.empty()) return;
        const auto& item = inputControls[controlIndex];
        const auto first = proposedValue.substr(0, proposedValue.find(','));
        std::string next;
        if (item.type == "BOOLEAN") next = first == "on" ? "off" : "on";
        else {
            int current = 0;
            if (!integer(first, current)) {
                notice = "Invalid control value. Refresh the input controls.";
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot parse mixer value: %s", first.c_str());
                return;
            }
            const long long value = std::clamp(static_cast<long long>(current) + direction,
                static_cast<long long>(item.minimum), static_cast<long long>(item.maximum));
            next = std::to_string(value);
        }
        proposedValue = next;
        for (int channel = 1; channel < item.channels; ++channel) proposedValue += "," + next;
    }
public:
    int page = 0;
    bool running = true;
    explicit App(Screen& target) : screen(target) {}
    ~App() { if (worker.joinable()) worker.join(); }

    bool initial() {
        Reply initial = control("status");
        std::cout << initial.output << std::flush;
        if (initial.code != 0) {
            notice = "Could not read the device settings. Try Refresh in Info.";
            return false;
        }
        info = parse(initial.output);
        refreshed = SDL_GetTicks();
        return true;
    }
    void update(bool autoRefresh = true) {
        audio.update();
        if (takePhase == TakePhase::recording && !audio.listening()) {
            wave = audio.waveform();
            if (wave.empty() || !audio.play()) audioError();
            else takePhase = TakePhase::playing;
        }
        if (takePhase == TakePhase::playing && !audio.playing()) takePhase = TakePhase::done;
        if (busy && done.load()) {
            worker.join(); busy = false;
            std::cout << job << " [" << reply.code << "]\n" << reply.output << std::flush;
            if (job == "status") {
                if (reply.code == 0) info = parse(reply.output);
                else notice = "Could not read the device settings. Try Refresh in Info.";
                refreshed = SDL_GetTicks();
            } else if (job == "diagnostics") {
                setReport(reply.output);
                if (reply.code != 0) notice = "Could not finish the report. Try Refresh.";
            } else if (job == "export-log") {
                const auto result = parse(reply.output);
                const auto path = result.find("REPORT");
                notice = reply.code == 0 && path != result.end() ?
                    "Saved: " + path->second : "Could not export the report. Check free space.";
            } else if (job == "discover") {
                setReport(reply.output.empty() ? "Discovery stopped. No results saved." : reply.output);
                page = 1;
                const auto result = parse(reply.output);
                const auto input = result.find("DISCOVERED_INPUT");
                if (process.cancelled) notice = "Discovery stopped.";
                else if (reply.code == 0 && input != result.end() && audio.selectInput(input->second)) {
                    selectedInput = input->second;
                    notice = "Input selected for Test. Check it with sound.";
                } else notice = "No input opened. See Info for the results.";
                if (quitAfter) start("cleanup");
            } else if (job == "controls") {
                page = 3;
                if (reply.code != 0 || !readControls(reply.output))
                    notice = "Could not read the input controls. Details are in the log.";
            } else if (job == "experiment-set") {
                if (reply.code == 0) start("controls");
                else notice = "Input change failed. The recovery snapshot was kept if needed.";
            } else if (job == "experiment-restore") {
                if (reply.code == 0) {
                    if (closingExperiments) { page = 2; selected = 1; closingExperiments = false; }
                    else start("controls");
                } else {
                    closingExperiments = false;
                    notice = "Could not restore every input. Keep the device connected.";
                }
            } else {
                if (reply.code != 0) {
                    notice = "Could not change the mic settings. Open Info for details.";
                    quitAfter = false;
                } else if (quitAfter) {
                    running = false;
                } else {
                    notice.clear();
                }
                if (running) start("status");
            }
        }
        if (needReport && !busy) { needReport = false; start("diagnostics"); }
        if (autoRefresh && !busy && page == 0 && takePhase == TakePhase::closed &&
            confirm.empty() && notice.empty() && SDL_GetTicks() - refreshed > 5000) start("status");
    }
    void input(Key key) {
        if (key == Key::none) return;
        if (takePhase != TakePhase::closed) {
            if (key == Key::cancel || key == Key::quit) {
                audio.stop(); takePhase = TakePhase::closed;
                if (key == Key::quit) { quitAfter = true; start("cleanup"); }
            } else if (key == Key::accept && takePhase == TakePhase::done) {
                if (!audio.play()) audioError();
                else takePhase = TakePhase::playing;
            }
            return;
        }
        if (busy) {
            if (job == "discover" && (key == Key::cancel || key == Key::quit)) {
                std::lock_guard<std::mutex> lock(process.mutex);
                process.cancelled = true;
                if (process.pid > 0 && kill(process.pid, SIGTERM) != 0 && errno != ESRCH) {
                    std::perror("ERROR: Cannot stop discovery");
                    notice = "Could not stop discovery. Waiting for its time limit.";
                }
                if (key == Key::quit) quitAfter = true;
            }
            return;
        }
        if (!notice.empty()) {
            if (key == Key::accept || key == Key::cancel) notice.clear();
            return;
        }
        if (!confirm.empty()) {
            if (key == Key::cancel) confirm.clear();
            else if (key == Key::accept) {
                std::string action = confirm; confirm.clear();
                if (action == "audio-test") {
                    if (!audio.record()) audioError();
                    else { wave.clear(); takePhase = TakePhase::recording; }
                } else if (action == "experiment-set") {
                    const auto& item = inputControls[controlIndex];
                    start(action, {item.card, item.numid, proposedValue});
                } else start(action);
            }
            return;
        }
        if (page == 1) {
            if (key == Key::cancel) { page = 0; selected = 3; needReport = false; }
            else if (key == Key::up) infoOffset = std::max(0, infoOffset - 1);
            else if (key == Key::down) infoOffset = std::min(std::max(0, static_cast<int>(reportLines.size()) - 10), infoOffset + 1);
            else if (key == Key::left) infoOffset = std::max(0, infoOffset - 10);
            else if (key == Key::right) infoOffset = std::min(std::max(0, static_cast<int>(reportLines.size()) - 10), infoOffset + 10);
            else if (key == Key::accept) start("export-log");
            else if (key == Key::refresh) needReport = true;
            else if (key == Key::explore) { page = 2; selected = 0; }
            else if (key == Key::quit) { quitAfter = true; start("cleanup"); }
            return;
        }
        if (page == 2) {
            if (key == Key::cancel) page = 1;
            else if (key == Key::up) selected = (selected + 3) % 4;
            else if (key == Key::down) selected = (selected + 1) % 4;
            else if (key == Key::accept) {
                if (selected == 0) confirm = "discover";
                else if (selected == 1) start("controls");
                else if (selected == 2) { closingExperiments = true; confirm = "experiment-restore"; }
                else page = 1;
            } else if (key == Key::quit) { quitAfter = true; start("cleanup"); }
            return;
        }
        if (page == 3) {
            if (key == Key::cancel) {
                closingExperiments = true;
                start("experiment-restore");
            } else if (key == Key::quit) { quitAfter = true; start("cleanup"); }
            else if (key == Key::refresh) { closingExperiments = false; confirm = "experiment-restore"; }
            else if (!inputControls.empty()) {
                if (key == Key::up || key == Key::down) {
                    const int count = static_cast<int>(inputControls.size());
                    controlIndex = (controlIndex + (key == Key::up ? count - 1 : 1)) % count;
                    proposedValue = inputControls[controlIndex].values;
                } else if (key == Key::left || key == Key::right) changeValue(key == Key::left ? -1 : 1);
                else if (key == Key::accept) confirm = "experiment-set";
            }
            return;
        }
        switch (key) {
            case Key::up: selected = (selected + 3) % 4; break;
            case Key::down: selected = (selected + 1) % 4; break;
            case Key::accept:
                if (selected == 0) confirm = enabled() ? "mic-off" : "mic-on";
                else if (selected == 1) confirm = persistent() ? "persist-off" : "persist-on";
                else if (selected == 2) confirm = "audio-test";
                else { page = 1; needReport = true; }
                break;
            case Key::refresh: start("status"); break;
            case Key::cancel: case Key::quit: quitAfter = true; start("cleanup"); break;
            default: break;
        }
    }
    void draw(Uint32 tick) {
        screen.rect(0, 0, 800, 450, ink);
        screen.rect(0, 0, 800, 4, gold);
        screen.text(32, 32, "CHOOCHOOHACKER", title, 2, 14);
        screen.disco(tick);
        if (page == 0) {
            screen.text(40, 130, "MIC STATUS", muted);
            const bool externalInput = !selectedInput.empty() && selectedInput != "hw:CARD=audiocodec,DEV=0";
            const std::string state = enabled() ? "Enabled" : get("MIC_STATE") == "off" ? "Disabled" :
                get("MIC_STATE") == "inactive" ? "Found, inactive" :
                get("AUDIO") == "unavailable" ? "Not found" : "Unknown";
            screen.text(220, 130, state, enabled() ? green : white);
            if (externalInput) screen.text(40, 165, "Test: " + selectedInput, muted, 1, 60);
            menu(0, enabled() ? (externalInput ? "Disable internal mic" : "Disable mic") :
                (externalInput ? "Enable internal mic" : "Enable mic"));
            menu(1, std::string("Keep after restart: ") + (persistent() ? "Yes" :
                get("PERSIST") == "disabled" ? "No" : "Unknown"));
            menu(2, "Test");
            menu(3, "Info");
        } else if (page == 1) {
            screen.text(40, 108, "Info / Audio + MIDI", gold);
            if (reportLines.empty()) line(0, "Loading...", muted);
            for (size_t row = static_cast<size_t>(infoOffset);
                 row < reportLines.size() && row < static_cast<size_t>(infoOffset + 10); ++row)
                line(static_cast<int>(row) - infoOffset, reportLines[row]);
            if (reportLines.size() > 10) {
                screen.rect(772, 150, 3, 240, selection);
                screen.rect(772, 150 + infoOffset * 220 / static_cast<int>(reportLines.size() - 10), 3, 20, gold);
            }
        } else if (page == 2) {
            screen.text(40, 108, "Explore / Advanced", gold);
            menu(0, "Discover inputs");
            menu(1, "Try input controls");
            menu(2, "Undo input experiments");
            menu(3, "Back");
        } else {
            screen.text(40, 108, "Input controls / Temporary", gold);
            if (inputControls.empty()) line(0, "No editable input controls found.", muted);
            const int first = std::max(0, controlIndex - 7);
            for (int index = first; index < static_cast<int>(inputControls.size()) && index < first + 8; ++index) {
                const int row = index - first;
                const bool active = index == controlIndex;
                if (active) screen.rect(32, 145 + row * 24, 736, 23, selection);
                line(row, (active ? "> " : "  ") + inputControls[index].name, active ? gold : white);
            }
            if (!inputControls.empty()) {
                const auto& item = inputControls[controlIndex];
                screen.text(40, 358, item.card + " / " + item.type, muted, 1, 60);
                screen.text(40, 382, "Value: " + proposedValue, green, 1, 60);
            }
        }
        screen.rect(32, 406, 736, 1, grid);
        screen.text(32, 423, busy ? (job == "discover" ? "Discovering...   B Stop" : "Please wait...") :
            page == 1 ? "A Export  X Refresh  Y Explore  B Back" :
            page == 3 ? "L/R Value  A Apply  X Undo  B Back" :
            "UP/DOWN Choose   A Select   B Exit", muted);
        if (takePhase != TakePhase::closed) {
            screen.rect(36, 159, 728, 238, gold);
            screen.rect(38, 161, 724, 234, ink);
            screen.text(60, 182, takePhase == TakePhase::recording ? "Recording 3 seconds" :
                takePhase == TakePhase::playing ? "Playing..." : "Your recording", title);
            if (takePhase == TakePhase::recording) {
                screen.text(60, 218, "Make noise.", green);
                const int progress = static_cast<int>(audio.frames() * 680 / (AudioTest::rate * AudioTest::seconds));
                screen.rect(60, 274, 680, 8, grid);
                screen.rect(60, 274, progress, 8, gold);
            } else drawWave();
            screen.text(60, 366, takePhase == TakePhase::done ? "A Replay   B Close" : "B Stop", gold);
        }
        if (!confirm.empty() || !notice.empty()) {
            screen.rect(36, 159, 728, 238, gold);
            screen.rect(38, 161, 724, 234, ink);
            screen.text(60, 182, confirm.empty() ? "Notice" : "Confirm", title);
            std::string body = notice;
            if (!confirm.empty()) {
                body = confirm == "discover" ?
                    "Try each audio input? No sound is saved or sent." :
                    confirm == "experiment-set" ?
                    "Change all channels of this input? Undo before unplugging or restarting." :
                    confirm == "experiment-restore" ? "Undo all input experiments?" :
                    confirm == "audio-test" ?
                    "Record 3 seconds and play it back? Keep volume low." :
                    confirm == "mic-on" ?
                    "Enable the mic?" :
                    confirm == "mic-off" ? "Disable the mic?" :
                    confirm == "persist-on" ?
                    "Keep this mic setting after every restart?" :
                    confirm == "restore" ?
                    "Undo the mic changes?" :
                    "Stop keeping this setting and restore the original values?";
            }
            auto lines = wrap(body, 53);
            for (size_t i = 0; i < std::min<size_t>(7, lines.size()); ++i)
                screen.text(60, 230 + static_cast<int>(i) * 24, lines[i], white, 1, 53);
            screen.text(60, 366, confirm.empty() ? "A/B Close" : "A Yes   B Cancel", gold);
        }
    }
    bool navigationTest() {
        input(Key::left);
        if (page != 0 || selected != 0) return false;
        input(Key::up);
        input(Key::accept);
        if (page != 1 || !confirm.empty()) return false;
        input(Key::cancel);
        if (page != 0 || selected != 3 || busy || needReport) return false;
        input(Key::down);
        input(Key::accept);
        if (confirm != "mic-on") return false;
        input(Key::cancel);
        info["MIC_STATE"] = "on";
        info["PERSIST"] = "enabled";
        input(Key::accept);
        if (confirm != "mic-off") return false;
        input(Key::cancel);
        input(Key::down);
        input(Key::accept);
        if (confirm != "persist-off") return false;
        input(Key::cancel);
        info["PERSIST"] = "disabled";
        input(Key::accept);
        if (confirm != "persist-on") return false;
        input(Key::cancel);
        input(Key::down);
        input(Key::accept);
        if (confirm != "audio-test") return false;
        input(Key::cancel);
        page = 1;
        input(Key::explore);
        if (page != 2 || selected != 0) return false;
        input(Key::accept);
        if (confirm != "discover") return false;
        input(Key::cancel);
        input(Key::cancel);
        if (page != 1) return false;
        if (!readControls("CONTROL_1=card|5|MIC gain|INTEGER|0|31|2|4,7\n")) return false;
        changeValue(1);
        if (proposedValue != "5,5") return false;
        if (readControls("CONTROL_1=card|5|MIC gain|INTEGER|bad|31|2|4,7\n")) return false;
        page = 0;
        takePhase = TakePhase::recording;
        input(Key::cancel);
        return !busy && confirm.empty() && !audio.listening() && takePhase == TakePhase::closed;
    }
    bool previews(const std::string& prefix) {
        auto save = [&](const std::string& suffix) {
            draw(1000);
            const bool ok = screen.save(prefix + suffix + ".bmp");
            SDL_RenderPresent(screen.renderer);
            return ok;
        };
        page = 0; selected = 0;
        if (!save("-main")) return false;
        page = 1;
        setReport(control("diagnostics").output);
        if (!save("-info")) return false;
        page = 0;
        confirm = "audio-test";
        if (!save("-confirm")) return false;
        confirm.clear();
        takePhase = TakePhase::recording;
        if (!save("-recording")) return false;
        // Synthetic popup fixture only; smoke tests never open an audio stream.
        wave.resize(256);
        for (size_t i = 0; i < wave.size(); ++i) {
            const float amplitude = 0.03f + 0.7f * std::fabs(std::sin(i * 0.09f)) *
                std::fabs(std::sin(i * 0.023f));
            wave[i] = {-amplitude, amplitude * 0.8f};
        }
        takePhase = TakePhase::playing;
        if (!save("-waveform")) return false;
        takePhase = TakePhase::closed;
        wave.clear();
        return true;
    }
};

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "";
    if (argc == 2 && (command == "--help" || command == "help")) {
        usage(std::cout);
        return 0;
    }
    const std::array<const char*, 12> cliActions{{
        "status", "mic-on", "mic-off", "restore", "persist-on", "persist-off", "diagnostics", "export-log",
        "inputs", "discover", "controls", "experiment-restore"}};
    if (argc == 2 && std::find(cliActions.begin(), cliActions.end(), command) != cliActions.end()) {
        if (!locateBackend()) return 1;
        execControl(command);
    }
    if (argc == 5 && command == "experiment-set") {
        if (!locateBackend()) return 1;
        execControl(command, {argv[2], argv[3], argv[4]});
    }
    if (argc == 2 && command == "--runtime-info") {
        SDL_version compiled, linked;
        SDL_VERSION(&compiled);
        SDL_GetVersion(&linked);
        std::cout << "SDL compiled " << int(compiled.major) << "." << int(compiled.minor) << "." << int(compiled.patch)
                  << "; linked " << int(linked.major) << "." << int(linked.minor) << "." << int(linked.patch)
                  << "\nCapture: 48000 Hz, S16 native endian, 2 channels, 512-frame buffer; 3-second takes."
                  << "\nSDL audio drivers (not opened):";
        for (int i = 0; i < SDL_GetNumAudioDrivers(); ++i) std::cout << " " << SDL_GetAudioDriver(i);
        std::cout << '\n';
        return 0;
    }
    if ((argc == 2 && command == "--probe-input") || (argc == 3 && command == "--probe-device")) {
        SDL_LogSetPriority(SDL_LOG_CATEGORY_AUDIO, SDL_LOG_PRIORITY_DEBUG);
        AudioTest audio;
        if (argc == 3 && !audio.selectInput(argv[2])) return 2;
        if (!audio.meter()) { std::cerr << audio.error << '\n'; return 1; }
        SDL_Delay(1000);
        std::cout << "Capture stream opened for 1s; no recording or playback. Peaks L="
                  << audio.left() << " R=" << audio.right() << '\n';
        audio.stop();
        return 0;
    }
    bool smoke = argc == 3 && std::string(argv[1]) == "--smoke-test";
    bool selfTest = argc == 2 && std::string(argv[1]) == "--self-test";
    if (argc != 1 && !smoke && !selfTest) {
        usage(std::cerr);
        return 2;
    }
    if (!selfTest && !locateBackend()) return 1;
    Screen screen;
    if (!screen.init(smoke || selfTest)) { std::cerr << "SDL init failed: " << SDL_GetError() << '\n'; return 1; }
    App app(screen);
    if (selfTest) {
        if (!app.navigationTest()) { std::cerr << "Navigation checks failed.\n"; return 1; }
        std::cout << "Navigation checks passed; no mixer writes or audio capture.\n";
        return 0;
    }
    bool diagnostic = app.initial();
    if (smoke) {
        if (!diagnostic) return 1;
        if (!app.previews(argv[2])) {
            std::cerr << "Screenshot failed: " << SDL_GetError() << '\n'; return 1;
        }
        std::cout << "SDL smoke: main, Info and synthetic test popup states rendered; no mixer writes or audio capture.\n";
        return 0;
    }
    SDL_GameController* pad = nullptr;
    SDL_Joystick* raw = nullptr;
    auto connect = [&] {
        if (pad || raw) return;
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);
            else raw = SDL_JoystickOpen(i);
            if (pad || raw) {
                SDL_Log("Input: %s", pad ? SDL_GameControllerName(pad) : SDL_JoystickName(raw));
                break;
            }
            SDL_Log("Input open failed: %s", SDL_GetError());
        }
    };
    connect();
    while (app.running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            Key key = Key::none;
            if (event.type == SDL_QUIT) key = Key::quit;
            else if (event.type == SDL_KEYDOWN && !event.key.repeat) key = keyboard(event.key.keysym.sym);
            else if (event.type == SDL_CONTROLLERBUTTONDOWN) key = button(event.cbutton.button);
            else if (event.type == SDL_JOYBUTTONDOWN && raw) {
                switch (event.jbutton.button) {
                    case 1: key = Key::accept; break;
                    case 0: key = Key::cancel; break;
                    case 3: key = Key::refresh; break;
                    case 2: key = Key::explore; break;
                    case 4: key = Key::left; break;
                    case 5: key = Key::right; break;
                    case 7: key = Key::quit; break;
                }
            } else if (event.type == SDL_JOYHATMOTION && raw) {
                if (event.jhat.value & SDL_HAT_UP) key = Key::up;
                else if (event.jhat.value & SDL_HAT_DOWN) key = Key::down;
                else if (event.jhat.value & SDL_HAT_LEFT) key = Key::left;
                else if (event.jhat.value & SDL_HAT_RIGHT) key = Key::right;
            } else if (event.type == SDL_JOYDEVICEADDED) connect();
            else if (event.type == SDL_JOYDEVICEREMOVED) {
                if (pad && !SDL_GameControllerGetAttached(pad)) { SDL_GameControllerClose(pad); pad = nullptr; }
                if (raw && !SDL_JoystickGetAttached(raw)) { SDL_JoystickClose(raw); raw = nullptr; }
                connect();
            }
            app.input(key);
        }
        app.update();
        app.draw(SDL_GetTicks());
        SDL_RenderPresent(screen.renderer);
        SDL_Delay(66);
    }
    SDL_GameControllerClose(pad); SDL_JoystickClose(raw);
    return 0;
}
