#include <imgui.h>
#include <module.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/bench/peak_level_meter.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/routing/splitter.h>
#include <dsp/audio/volume.h>
#include <dsp/convert/stereo_to_mono.h>
#include <thread>
#include <ctime>
#include <gui/gui.h>
#include <filesystem>
#include <signal_path/signal_path.h>
#include <config.h>
#include <gui/style.h>
#include <gui/widgets/volume_meter.h>
#include <regex>
#include <gui/widgets/folder_select.h>
#include <core.h>
#include <utils/optionlist.h>
#include <utils/wav.h>
#include "baseband_sink_interface.h"

#include "baseband_interface.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

#define SILENCE_LVL 10e-6

SDRPP_MOD_INFO{
    /* Name:            */ "baseband_sink",
    /* Description:     */ "Baseband Sink module for SDR++",
    /* Author:          */ "Aang23",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

class BasebandSinkModule : public ModuleManager::Instance {
public:
    BasebandSinkModule(std::string name) : folderSelect("%ROOT%/recordings") {
        this->name = name;
        root = (std::string)core::args["root"];
        strcpy(nameTemplate, "$t_$f_$h-$m-$s_$d-$M-$y");

        // Define option lists
        sampleTypes.define(dsp::CF_32, "f32", dsp::CF_32);
        sampleTypes.define(dsp::IS_16, "s16", dsp::IS_16);
        sampleTypes.define(dsp::IS_8, "s8", dsp::IS_8);
        // sampleTypes.define(dsp::IU_8, "u8", dsp::IU_8);
        sampleTypes.define(dsp::WAV_16, "w16", dsp::WAV_16);
        sampleTypes.define(dsp::ZIQ, "ziq", dsp::ZIQ);
        sampleTypes.define(dsp::ZIQ2, "ziq2", dsp::ZIQ2);


        // Load default config for option lists
        sampleTypeId = sampleTypes.valueId(dsp::ZIQ2);

        // Load config
        config.acquire();
        if (config.conf[name].contains("mode")) {
            recMode = config.conf[name]["mode"];
        }
        if (config.conf[name].contains("recPath")) {
            folderSelect.setPath(config.conf[name]["recPath"]);
        }
        if (config.conf[name].contains("sampleType") && sampleTypes.keyExists(config.conf[name]["sampleType"])) {
            sampleTypeId = sampleTypes.keyId(config.conf[name]["sampleType"]);
        }
        if (config.conf[name].contains("audioStream")) {
            selectedStreamName = config.conf[name]["audioStream"];
        }
        if (config.conf[name].contains("audioVolume")) {
            audioVolume = config.conf[name]["audioVolume"];
        }
        if (config.conf[name].contains("stereo")) {
            stereo = config.conf[name]["stereo"];
        }
        if (config.conf[name].contains("nameTemplate")) {
            std::string _nameTemplate = config.conf[name]["nameTemplate"];
            if (_nameTemplate.length() > sizeof(nameTemplate) - 1) {
                _nameTemplate = _nameTemplate.substr(0, sizeof(nameTemplate) - 1);
            }
            strcpy(nameTemplate, _nameTemplate.c_str());
        }
        config.release();

        // Init audio path
        volume.init(NULL, audioVolume, false);
        splitter.init(&volume.out);

        // Init sinks
        basebandSink.init(NULL, complexHandler, this);
        stereoSink.init(&stereoStream, stereoHandler, this);

        gui::menu.registerEntry(name, menuHandler, this);
        core::modComManager.registerInterface("baseband_sink", name, moduleInterfaceHandler, this);
    }

    ~BasebandSinkModule() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        core::modComManager.unregisterInterface(name);
        gui::menu.removeEntry(name);
        stop();
        deselectStream();
        sigpath::sinkManager.onStreamRegistered.unbindHandler(&onStreamRegisteredHandler);
        sigpath::sinkManager.onStreamUnregister.unbindHandler(&onStreamUnregisterHandler);
    }

    void postInit() {
        // Enumerate streams
        audioStreams.clear();
        auto names = sigpath::sinkManager.getStreamNames();
        for (const auto& name : names) {
            audioStreams.define(name, name, name);
        }

        // Bind stream register/unregister handlers
        onStreamRegisteredHandler.ctx = this;
        onStreamRegisteredHandler.handler = streamRegisteredHandler;
        sigpath::sinkManager.onStreamRegistered.bindHandler(&onStreamRegisteredHandler);
        onStreamUnregisterHandler.ctx = this;
        onStreamUnregisterHandler.handler = streamUnregisterHandler;
        sigpath::sinkManager.onStreamUnregister.bindHandler(&onStreamUnregisterHandler);

        // Select the stream
        selectStream(selectedStreamName);
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void start() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (recording) { return; }

        // Configure the writer
        if (recMode == BASEBAND_SINK_MODE_AUDIO) {
            if (selectedStreamName.empty()) { return; }
            samplerate = sigpath::sinkManager.getStreamSampleRate(selectedStreamName);
        }
        else {
            samplerate = sigpath::iqFrontEnd.getSampleRate();
        }

#if 0
        writer.setFormat(containers[containerId]);
        writer.setChannels((recMode == BASEBAND_SINK_MODE_AUDIO && !stereo) ? 1 : 2);
        writer.setSampleType(sampleTypes[sampleTypeId]);
        writer.setSamplerate(samplerate);

        // Open file
        std::string type = (recMode == BASEBAND_SINK_MODE_AUDIO) ? "audio" : "baseband";
        std::string vfoName = (recMode == BASEBAND_SINK_MODE_AUDIO) ? gui::waterfall.selectedVFO : "";
        std::string extension = ".wav";
        std::string expandedPath = expandString(folderSelect.path + "/" + genFileName(nameTemplate, type, vfoName) + extension);
        if (!writer.open(expandedPath)) {
            flog::error("Failed to open file for recording: {0}", expandedPath);
            return;
        }
#endif

        flog::info("Recording with samplerate {:d}", samplerate);

        std::string type = (recMode == BASEBAND_SINK_MODE_AUDIO) ? "audio" : "baseband";
        std::string vfoName = (recMode == BASEBAND_SINK_MODE_AUDIO) ? gui::waterfall.selectedVFO : "";
        std::string expandedPath = expandString(folderSelect.path + "/" + genFileName(nameTemplate, type, vfoName));

        baseband_writer.set_output_sample_type(sampleTypes[sampleTypeId]);
        baseband_writer.start_recording(expandedPath, samplerate, actual_bit_depth);

        // Open audio stream or baseband
        if (recMode == BASEBAND_SINK_MODE_AUDIO) {
            // Start correct path depending on
            if (stereo) {
                stereoSink.start();
            }
            else {
                flog::error("MONO NOT SUPPORTED");
            }
            splitter.bindStream(&stereoStream);
        }
        else {
            // Create and bind IQ stream
            basebandStream = new dsp::stream<dsp::complex_t>();
            basebandSink.setInput(basebandStream);
            basebandSink.start();
            sigpath::iqFrontEnd.bindIQStream(basebandStream);
        }

        recording = true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (!recording) { return; }

        // Close audio stream or baseband
        if (recMode == BASEBAND_SINK_MODE_AUDIO) {
            splitter.unbindStream(&stereoStream);
            stereoSink.stop();
        }
        else {
            // Unbind and destroy IQ stream
            sigpath::iqFrontEnd.unbindIQStream(basebandStream);
            basebandSink.stop();
            delete basebandStream;
        }

        // Close file
        baseband_writer.stop_recording();

        recording = false;
    }

private:
    static void menuHandler(void* ctx) {
        BasebandSinkModule* _this = (BasebandSinkModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // Recording mode
        if (_this->recording) { style::beginDisabled(); }
        ImGui::BeginGroup();
        ImGui::Columns(2, CONCAT("RecorderModeColumns##_", _this->name), false);
        if (ImGui::RadioButton(CONCAT("Baseband##_BASEBAND_SINK_mode_", _this->name), _this->recMode == BASEBAND_SINK_MODE_BASEBAND)) {
            _this->recMode = BASEBAND_SINK_MODE_BASEBAND;
            config.acquire();
            config.conf[_this->name]["mode"] = _this->recMode;
            config.release(true);
        }
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("Audio##_BASEBAND_SINK_mode_", _this->name), _this->recMode == BASEBAND_SINK_MODE_AUDIO)) {
            _this->recMode = BASEBAND_SINK_MODE_AUDIO;
            config.acquire();
            config.conf[_this->name]["mode"] = _this->recMode;
            config.release(true);
        }
        ImGui::Columns(1, CONCAT("EndRecorderModeColumns##_", _this->name), false);
        ImGui::EndGroup();
        if (_this->recording) { style::endDisabled(); }

        // Recording path
        if (_this->folderSelect.render("##_BASEBAND_SINK_fold_" + _this->name)) {
            if (_this->folderSelect.pathIsValid()) {
                config.acquire();
                config.conf[_this->name]["recPath"] = _this->folderSelect.path;
                config.release(true);
            }
        }

        ImGui::LeftLabel("Name template");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##_BASEBAND_SINK_name_template_", _this->name), _this->nameTemplate, 1023)) {
            config.acquire();
            config.conf[_this->name]["nameTemplate"] = _this->nameTemplate;
            config.release(true);
        }


        ImGui::LeftLabel("Sample type");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_BASEBAND_SINK_st_", _this->name), &_this->sampleTypeId, _this->sampleTypes.txt)) {
            config.acquire();
            config.conf[_this->name]["sampleType"] = _this->sampleTypes.key(_this->sampleTypeId);
            config.release(true);
        }

        if (!(_this->sampleTypes[_this->sampleTypeId] == dsp::ZIQ || _this->sampleTypes[_this->sampleTypeId] == dsp::ZIQ2))
            style::beginDisabled();
        ImGui::LeftLabel("Bit Depth");
        ImGui::FillWidth();
        if (ImGui::Combo("##baseband_sink_bit_depth", &_this->selected_bit_depth, "8\0"
                                                                                  "16\0")) {
            if (_this->selected_bit_depth == 0)
                _this->actual_bit_depth = 8;
            else if (_this->selected_bit_depth == 1)
                _this->actual_bit_depth = 16;
        }
        if (!(_this->sampleTypes[_this->sampleTypeId] == dsp::ZIQ || _this->sampleTypes[_this->sampleTypeId] == dsp::ZIQ2))
            style::endDisabled();

        // Show additional audio options
        if (_this->recMode == BASEBAND_SINK_MODE_AUDIO) {
            ImGui::LeftLabel("Stream");
            ImGui::FillWidth();
            if (ImGui::Combo(CONCAT("##_BASEBAND_SINK_stream_", _this->name), &_this->streamId, _this->audioStreams.txt)) {
                _this->selectStream(_this->audioStreams.value(_this->streamId));
                config.acquire();
                config.conf[_this->name]["audioStream"] = _this->audioStreams.key(_this->streamId);
                config.release(true);
            }

            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_BASEBAND_SINK_vol_", _this->name), &_this->audioVolume, 0, 1, "")) {
                _this->volume.setVolume(_this->audioVolume);
                config.acquire();
                config.conf[_this->name]["audioVolume"] = _this->audioVolume;
                config.release(true);
            }

            if (_this->recording) { style::beginDisabled(); }
            // if (ImGui::Checkbox(CONCAT("Stereo##_BASEBAND_SINK_stereo_", _this->name), &_this->stereo))
            if (!_this->stereo) {
                _this->stereo = true;
                config.acquire();
                config.conf[_this->name]["stereo"] = _this->stereo;
                config.release(true);
            }
            if (_this->recording) { style::endDisabled(); }
        }

        // Record button
        bool canRecord = _this->folderSelect.pathIsValid();
        if (_this->recMode == BASEBAND_SINK_MODE_AUDIO) { canRecord &= !_this->selectedStreamName.empty(); }
        if (!_this->recording) {
            if (ImGui::Button(CONCAT("Record##_BASEBAND_SINK_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->start();
            }
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "Idle --:--:--");
        }
        else {
            if (ImGui::Button(CONCAT("Stop##_BASEBAND_SINK_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->stop();
            }


            if (_this->baseband_writer.get_written() < 1e9)
                ImGui::Text("Size : %.2f MB", _this->baseband_writer.get_written() / 1e6);
            else
                ImGui::Text("Size : %.2f GB", _this->baseband_writer.get_written() / 1e9);

            if (_this->sampleTypes[_this->sampleTypeId] == dsp::ZIQ) {
                if (_this->baseband_writer.get_written_raw() < 1e9)
                    ImGui::Text("Size (raw) : %.2f MB", _this->baseband_writer.get_written_raw() / 1e6);
                else
                    ImGui::Text("Size (raw) : %.2f GB", _this->baseband_writer.get_written_raw() / 1e9);
            }
        }
    }

    void selectStream(std::string name) {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        deselectStream();

        if (audioStreams.empty()) {
            selectedStreamName.clear();
            return;
        }
        else if (!audioStreams.keyExists(name)) {
            selectStream(audioStreams.key(0));
            return;
        }

        audioStream = sigpath::sinkManager.bindStream(name);
        if (!audioStream) { return; }
        selectedStreamName = name;
        streamId = audioStreams.keyId(name);
        volume.setInput(audioStream);
        startAudioPath();
    }

    void deselectStream() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (selectedStreamName.empty() || !audioStream) {
            selectedStreamName.clear();
            return;
        }
        if (recording && recMode == BASEBAND_SINK_MODE_AUDIO) { stop(); }
        stopAudioPath();
        sigpath::sinkManager.unbindStream(selectedStreamName, audioStream);
        selectedStreamName.clear();
        audioStream = NULL;
    }

    void startAudioPath() {
        volume.start();
        splitter.start();
    }

    void stopAudioPath() {
        volume.stop();
        splitter.stop();
    }

    static void streamRegisteredHandler(std::string name, void* ctx) {
        BasebandSinkModule* _this = (BasebandSinkModule*)ctx;

        // Add new stream to the list
        _this->audioStreams.define(name, name, name);

        // If no stream is selected, select new stream. If not, update the menu ID.
        if (_this->selectedStreamName.empty()) {
            _this->selectStream(name);
        }
        else {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }

    static void streamUnregisterHandler(std::string name, void* ctx) {
        BasebandSinkModule* _this = (BasebandSinkModule*)ctx;

        // Remove stream from list
        _this->audioStreams.undefineKey(name);

        // If the stream is in used, deselect it and reselect default. Otherwise, update ID.
        if (_this->selectedStreamName == name) {
            _this->selectStream("");
        }
        else {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }


    std::string genFileName(std::string templ, std::string type, std::string name) {
        // Get data
        time_t now = time(0);
        tm* ltm = localtime(&now);
        char buf[1024];
        double freq = gui::waterfall.getCenterFrequency();
        if (gui::waterfall.vfos.find(name) != gui::waterfall.vfos.end()) {
            freq += gui::waterfall.vfos[name]->generalOffset;
        }

        // Format to string
        char freqStr[128];
        char hourStr[128];
        char minStr[128];
        char secStr[128];
        char dayStr[128];
        char monStr[128];
        char yearStr[128];
        sprintf(freqStr, "%.0lfHz", freq);
        sprintf(hourStr, "%02d", ltm->tm_hour);
        sprintf(minStr, "%02d", ltm->tm_min);
        sprintf(secStr, "%02d", ltm->tm_sec);
        sprintf(dayStr, "%02d", ltm->tm_mday);
        sprintf(monStr, "%02d", ltm->tm_mon + 1);
        sprintf(yearStr, "%02d", ltm->tm_year + 1900);

        // Replace in template
        templ = std::regex_replace(templ, std::regex("\\$t"), type);
        templ = std::regex_replace(templ, std::regex("\\$f"), freqStr);
        templ = std::regex_replace(templ, std::regex("\\$h"), hourStr);
        templ = std::regex_replace(templ, std::regex("\\$m"), minStr);
        templ = std::regex_replace(templ, std::regex("\\$s"), secStr);
        templ = std::regex_replace(templ, std::regex("\\$d"), dayStr);
        templ = std::regex_replace(templ, std::regex("\\$M"), monStr);
        templ = std::regex_replace(templ, std::regex("\\$y"), yearStr);
        return templ;
    }

    std::string expandString(std::string input) {
        input = std::regex_replace(input, std::regex("%ROOT%"), root);
        return std::regex_replace(input, std::regex("//"), "/");
    }

    static void complexHandler(dsp::complex_t* data, int count, void* ctx) {
        BasebandSinkModule* _this = (BasebandSinkModule*)ctx;
        _this->baseband_writer.feed_samples(data, count);
    }

    static void stereoHandler(dsp::stereo_t* data, int count, void* ctx) {
        BasebandSinkModule* _this = (BasebandSinkModule*)ctx;
        _this->baseband_writer.feed_samples((dsp::complex_t*)data, count);
    }

    static void monoHandler(float* data, int count, void* ctx) {
        BasebandSinkModule* _this = (BasebandSinkModule*)ctx;
        flog::error("Mono NOT supported!");
    }

    static void moduleInterfaceHandler(int code, void* in, void* out, void* ctx) {
        BasebandSinkModule* _this = (BasebandSinkModule*)ctx;
        std::lock_guard lck(_this->recMtx);
        if (code == BASEBAND_SINK_IFACE_CMD_GET_MODE) {
            int* _out = (int*)out;
            *_out = _this->recMode;
        }
        else if (code == BASEBAND_SINK_IFACE_CMD_SET_MODE) {
            if (_this->recording) { return; }
            int* _in = (int*)in;
            _this->recMode = std::clamp<int>(*_in, 0, 1);
        }
        else if (code == BASEBAND_SINK_IFACE_CMD_START) {
            if (!_this->recording) { _this->start(); }
        }
        else if (code == BASEBAND_SINK_IFACE_CMD_STOP) {
            if (_this->recording) { _this->stop(); }
        }
    }

    std::string name;
    bool enabled = true;
    std::string root;
    char nameTemplate[1024];

    OptionList<int, dsp::BasebandType> sampleTypes;
    FolderSelect folderSelect;

    int recMode = BASEBAND_SINK_MODE_BASEBAND;
    int sampleTypeId;
    bool stereo = true;
    std::string selectedStreamName = "";
    float audioVolume = 1.0f;
    dsp::stereo_t audioLvl = { -100.0f, -100.0f };

    bool recording = false;
    std::recursive_mutex recMtx;
    dsp::stream<dsp::complex_t>* basebandStream;
    dsp::stream<dsp::stereo_t> stereoStream;
    dsp::sink::Handler<dsp::complex_t> basebandSink;
    dsp::sink::Handler<dsp::stereo_t> stereoSink;


    OptionList<std::string, std::string> audioStreams;
    int streamId = 0;
    dsp::stream<dsp::stereo_t>* audioStream = NULL;
    dsp::audio::Volume volume;
    dsp::routing::Splitter<dsp::stereo_t> splitter;


    uint64_t samplerate = 48000;

    EventHandler<std::string> onStreamRegisteredHandler;
    EventHandler<std::string> onStreamUnregisterHandler;

    int selected_bit_depth = 0;
    int actual_bit_depth = 8;

    dsp::BasebandWriter baseband_writer;
};

MOD_EXPORT void _INIT_() {
    // Create default recording directory
    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root + "/recordings")) {
        flog::warn("Recordings directory does not exist, creating it");
        if (!std::filesystem::create_directory(root + "/recordings")) {
            flog::error("Could not create recordings directory");
        }
    }
    json def = json({});
    config.setPath(root + "/baseband_sink_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new BasebandSinkModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete (BasebandSinkModule*)inst;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}