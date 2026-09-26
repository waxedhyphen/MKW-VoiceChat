#include "mkwvc/AppConfig.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace mkwvc {

namespace {

std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::wstring buffer(32768,L'\0');
    const auto length=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(length>0 && length<buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
    }
#elif defined(__APPLE__)
    std::vector<char> buffer(4096);
    auto size=static_cast<std::uint32_t>(buffer.size());

    if(_NSGetExecutablePath(buffer.data(),&size)!=0) {
        buffer.resize(size);
        if(_NSGetExecutablePath(buffer.data(),&size)!=0) return std::filesystem::current_path();
    }

    return std::filesystem::path(buffer.data()).parent_path();
#endif

    return std::filesystem::current_path();
}

}

std::filesystem::path configPath() {
    return executableDirectory()/"config.ini";
}

AppConfig loadConfig() {
    AppConfig config;
    std::ifstream file(configPath());
    if(!file) return config;

    std::string line;
    while(std::getline(file,line)) {
        const auto separator=line.find('=');
        if(separator==std::string::npos) continue;
        const auto key=line.substr(0,separator);
        const auto value=line.substr(separator+1);

        if(key=="peer") config.peerAddress=value;
        else if(key=="player_name" && !value.empty()) config.playerName=value.substr(0,63);
        else if(key=="port") {
            try {
                const auto parsed=std::stoi(value);
                if(parsed>=1 && parsed<=65535) config.port=parsed;
            } catch(...) {}
        } else if(key=="input_device") config.inputDevice=value;
        else if(key=="output_device") config.outputDevice=value;
        else if(key=="mic_gain") {
            try {
                config.microphoneGain=std::clamp(std::stof(value),0.25f,5.0f);
            } catch(...) {}
        } else if(key=="playback_volume") {
            try {
                config.playbackVolume=std::clamp(std::stof(value),0.0f,3.0f);
            } catch(...) {}
        } else if(key=="automatic_normalization") config.automaticNormalization=value!="0";
        else if(key=="noise_suppression") config.noiseSuppression=value!="0";
        else if(key=="noise_suppression_strength") {
            try {
                config.noiseSuppressionStrength=std::clamp(std::stoi(value),0,100);
            } catch(...) {}
        } else if(key=="signaling_server" && !value.empty()) config.signalingServer=value;
    }

    return config;
}

void saveConfig(const AppConfig& config) {
    std::ofstream file(configPath(),std::ios::trunc);
    if(!file) throw std::runtime_error("Failed to save config file");

    file<<"peer="<<config.peerAddress<<"\n";
    file<<"player_name="<<config.playerName<<"\n";
    file<<"port="<<config.port<<"\n";
    file<<"input_device="<<config.inputDevice<<"\n";
    file<<"output_device="<<config.outputDevice<<"\n";
    file<<"mic_gain="<<config.microphoneGain<<"\n";
    file<<"playback_volume="<<config.playbackVolume<<"\n";
    file<<"automatic_normalization="<<(config.automaticNormalization ? 1 : 0)<<"\n";
    file<<"noise_suppression="<<(config.noiseSuppression ? 1 : 0)<<"\n";
    file<<"noise_suppression_strength="<<config.noiseSuppressionStrength<<"\n";
    file<<"signaling_server="<<config.signalingServer<<"\n";
}

}
