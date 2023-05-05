#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/widgets/file_select.h>
#include <filesystem>
#include <regex>
#include <gui/tuner.h>

#include "baseband_interface.h"
#include "detect_header.h"
#include <chrono>

#include "gui/style.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "baseband_source",
    /* Description:     */ "Baseband file source module for SDR++",
    /* Author:          */ "Aang23",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class BasebandSourceModule : public ModuleManager::Instance {
public:
    BasebandSourceModule(std::string name) : fileSelect("", { "ZIQ Files (*.ziq)", "*.ziq", "All Files", "*" }) {
        this->name = name;

        if (core::args["server"].b()) { return; }

        config.acquire();
        fileSelect.setPath(config.conf["path"], true);
        config.release();

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        sigpath::sourceManager.registerSource("Baseband", &handler);
    }

    ~BasebandSourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource("Baseband");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void menuSelected(void* ctx) {
        BasebandSourceModule* _this = (BasebandSourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", _this->centerFreq);
        sigpath::iqFrontEnd.setBuffering(false);
        gui::waterfall.centerFrequencyLocked = true;
        // gui::freqSelect.minFreq = _this->centerFreq - (_this->sampleRate/2);
        // gui::freqSelect.maxFreq = _this->centerFreq + (_this->sampleRate/2);
        // gui::freqSelect.limitFreq = true;
        flog::info("BasebandSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        BasebandSourceModule* _this = (BasebandSourceModule*)ctx;
        sigpath::iqFrontEnd.setBuffering(true);
        // gui::freqSelect.limitFreq = false;
        gui::waterfall.centerFrequencyLocked = false;
        flog::info("BasebandSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        BasebandSourceModule* _this = (BasebandSourceModule*)ctx;
        if (_this->running) { return; }
        _this->running = true;
        _this->workerThread = std::thread(worker, _this);
        flog::info("BasebandSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        BasebandSourceModule* _this = (BasebandSourceModule*)ctx;
        if (!_this->running) { return; }
        _this->stream.stopWriter();
        _this->workerThread.join();
        _this->stream.clearWriteStop();
        _this->running = false;
        // _this->reader->rewind();
        flog::info("BasebandSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        BasebandSourceModule* _this = (BasebandSourceModule*)ctx;
        flog::info("BasebandSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        BasebandSourceModule* _this = (BasebandSourceModule*)ctx;

        if (_this->running)
            style::beginDisabled();
        if (_this->fileSelect.render("##baseband_source_" + _this->name)) {
            if (_this->fileSelect.pathIsValid()) {
                try {
                    HeaderInfo hdr = try_parse_header(_this->fileSelect.path);

                    if (hdr.valid) {
                        if (hdr.type == "u8")
                            _this->select_sample_format = 3;
                        else if (hdr.type == "s16")
                            _this->select_sample_format = 1;
                        else if (hdr.type == "ziq")
                            _this->select_sample_format = 4;
                        else if (hdr.type == "ziq2")
                            _this->select_sample_format = 5;

                        _this->sampleRate = hdr.samplerate;

                        flog::debug("Setting file to {:s}", _this->fileSelect.path.c_str());
                        flog::debug("Setting samplerate to {:d}", hdr.samplerate);

                        _this->update_format = true;
                    }
                }
                catch (std::exception e) {
                    flog::error("Error: {0}", e.what());
                }
                config.acquire();
                config.conf["path"] = _this->fileSelect.path;
                config.release(true);
            }
        }

        ImGui::InputFloat("Samplerate", &_this->sampleRate, 0);

        if (ImGui::Combo("Format###basebandplayerformat", &_this->select_sample_format, "f32\0"
                                                                                        "s16\0"
                                                                                        "s8\0"
                                                                                        "u8\0"
                                                                                        // #ifdef BUILD_ZIQ
                                                                                        "ziq\0"
                                                                                        // #endif
                                                                                        "ziq2\0") ||
            _this->update_format) {
            if (_this->select_sample_format == 0)
                _this->baseband_type = "f32";
            else if (_this->select_sample_format == 1)
                _this->baseband_type = "s16";
            else if (_this->select_sample_format == 2)
                _this->baseband_type = "s8";
            else if (_this->select_sample_format == 3)
                _this->baseband_type = "u8";
            else if (_this->select_sample_format == 4)
                _this->baseband_type = "ziq";
            else if (_this->select_sample_format == 5)
                _this->baseband_type = "ziq2";
        }
        if (_this->running)
            style::endDisabled();

        if (_this->select_sample_format == 4)
            style::beginDisabled();
        float file_progress = (float(_this->baseband_reader.progress) / float(_this->baseband_reader.filesize)) * 100.0;
        if (ImGui::SliderFloat("Progress", &file_progress, 0, 100))
            _this->baseband_reader.set_progress(file_progress);
        if (_this->select_sample_format == 4) {
            ImGui::TextColored(ImColor(255, 0, 0), "Scrolling not\navailable on ZIQ!");
            style::endDisabled();
        }

        if (_this->running)
            style::beginDisabled();
        ImGui::Checkbox("Enforce Realtime", &_this->enforce_realtime_samplerate);
        if (_this->running)
            style::endDisabled();
    }

    static void worker(void* ctx) {
        BasebandSourceModule* _this = (BasebandSourceModule*)ctx;
        double sampleRate = _this->sampleRate;
        int blockSize = sampleRate / 200.0f;

        _this->baseband_reader.set_file(_this->fileSelect.path, dsp::basebandTypeFromString(_this->baseband_type));
        _this->baseband_reader.should_repeat = true;

        uint64_t total_samples = 0;
        auto start_time_point = std::chrono::steady_clock::now();
        auto sample_time_period = std::chrono::duration<double>(1.0 / (double)sampleRate);

        while (true) {
            int nsamp = _this->baseband_reader.read_samples(_this->stream.writeBuf, blockSize);

            if (!_this->stream.swap(nsamp)) { break; };

            total_samples += nsamp;

            if (_this->enforce_realtime_samplerate) {
                auto now = std::chrono::steady_clock::now();
                auto expected_time = start_time_point + sample_time_period * total_samples;

                if (expected_time > now)
                    std::this_thread::sleep_until(expected_time);
            }
        }
    }


    double getFrequency(std::string filename) {
        std::regex expr("[0-9]+Hz");
        std::smatch matches;
        std::regex_search(filename, matches, expr);
        if (matches.empty()) { return 0; }
        std::string freqStr = matches[0].str();
        return std::atof(freqStr.substr(0, freqStr.size() - 2).c_str());
    }

    FileSelect fileSelect;
    std::string name;
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    bool running = false;
    bool enabled = true;
    float sampleRate = 1000000;
    std::thread workerThread;

    double centerFreq = 100000000;

    bool enforce_realtime_samplerate = false;
    bool update_format = true;
    int select_sample_format = 0;
    std::string baseband_type = "f32";
    dsp::BasebandReader baseband_reader;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["path"] = "";
    config.setPath(core::args["root"].s() + "/baseband_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    return new BasebandSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (BasebandSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}