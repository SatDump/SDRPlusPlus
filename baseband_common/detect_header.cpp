#include "detect_header.h"

#include "wav.h"
#include "ziq.h"

#include <utils/flog.h>

#include "ziq2.h"

HeaderInfo try_parse_header(std::string file) {
    HeaderInfo info;

    if (wav::isValidWav(wav::parseHeaderFromFileWav(file))) {
        flog::debug("File is wav!");
        info.samplerate = wav::parseHeaderFromFileWav(file).samplerate;
        if (wav::parseHeaderFromFileWav(file).bits_per_sample == 8)
            info.type = "u8";
        else if (wav::parseHeaderFromFileWav(file).bits_per_sample == 16)
            info.type = "s16";
        info.valid = true;
    }
    else if (wav::isValidRF64(wav::parseHeaderFromFileWav(file))) {
        flog::debug("File is RF64!");
        info.samplerate = wav::parseHeaderFromFileRF64(file).samplerate;
        if (wav::parseHeaderFromFileRF64(file).bits_per_sample == 8)
            info.type = "u8";
        else if (wav::parseHeaderFromFileRF64(file).bits_per_sample == 16)
            info.type = "s16";
        info.valid = true;
    }
    else if (ziq::isValidZIQ(file)) {
        flog::debug("File is ZIQ!");
        info.type = "ziq";
        info.valid = true;
        info.samplerate = ziq::getCfgFromFile(file).samplerate;
    }
    else if (ziq2::ziq2_is_valid_ziq2(file)) {
        flog::debug("File is ZIQ2!");
        info.type = "ziq2";
        info.valid = true;
        info.samplerate = ziq2::ziq2_try_parse_header(file);
    }

    return info;
}