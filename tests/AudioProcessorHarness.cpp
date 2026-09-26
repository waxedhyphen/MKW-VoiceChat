#include "mkwvc/AudioProcessor.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <string>

int main(int argc,char** argv) {
    if(argc<4 || argc>5) return 2;
    mkwvc::AudioProcessor processor;
    mkwvc::AudioProcessingSettings settings;
    const int strength=std::stoi(argv[1]);
    settings.normalization=false;
    settings.compressor=false;
    settings.noiseSuppression=strength>=0;
    settings.noiseSuppressionStrength=std::max(strength,0);
    processor.setSettings(settings);
    std::ifstream input(argv[2],std::ios::binary);
    std::ofstream output(argv[3],std::ios::binary);
    if(!input || !output) return 3;
    std::array<std::int16_t,mkwvc::VoiceFormat::FrameSamples> frame{};
    std::size_t frameIndex=0;
    while(input.read(reinterpret_cast<char*>(frame.data()),sizeof(frame))) {
        if(argc==5 && std::string(argv[4])=="refresh") processor.setSettings(settings);
        if(argc==5 && std::string(argv[4])=="toggle") {
            settings.noiseSuppression=frameIndex%50<25;
            processor.setSettings(settings);
        }
        processor.processCapture(frame);
        processor.processPostGainSuppression(frame);
        output.write(reinterpret_cast<const char*>(frame.data()),sizeof(frame));
        ++frameIndex;
    }
    return input.bad() || !output ? 4 : 0;
}
