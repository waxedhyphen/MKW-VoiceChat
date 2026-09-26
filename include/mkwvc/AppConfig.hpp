#pragma once

#include <filesystem>
#include <string>

namespace mkwvc {

struct AppConfig {
    std::string peerAddress;
    int port=50000;
    std::string playerName="Player";
    std::string inputDevice;
    std::string outputDevice;
    float microphoneGain=1.0f;
    float playbackVolume=1.0f;
    bool automaticNormalization=true;
    bool noiseSuppression=true;
    int noiseSuppressionStrength=50;
    std::string signalingServer="ws://127.0.0.1:8765";
};

std::filesystem::path configPath();
AppConfig loadConfig();
void saveConfig(const AppConfig& config);

}
