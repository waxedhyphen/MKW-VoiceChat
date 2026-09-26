#include "mkwvc/IceSignal.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mkwvc {

namespace {

constexpr std::string_view Alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(std::string_view input) {
    std::string output;
    output.reserve((input.size()+2)/3*4);

    std::size_t i=0;
    while(i<input.size()) {
        const auto remaining=input.size()-i;
        const auto count=std::min<std::size_t>(3,remaining);
        const auto a=static_cast<unsigned char>(input[i++]);
        const auto b=count>1 ? static_cast<unsigned char>(input[i++]) : 0;
        const auto c=count>2 ? static_cast<unsigned char>(input[i++]) : 0;

        const unsigned value=(static_cast<unsigned>(a)<<16)|(static_cast<unsigned>(b)<<8)|c;
        output.push_back(Alphabet[(value>>18)&63]);
        output.push_back(Alphabet[(value>>12)&63]);
        output.push_back(count>1 ? Alphabet[(value>>6)&63] : '=');
        output.push_back(count>2 ? Alphabet[value&63] : '=');
    }

    return output;
}

int base64Value(char ch) {
    const auto pos=Alphabet.find(ch);
    return pos==std::string_view::npos ? -1 : static_cast<int>(pos);
}

std::string base64Decode(std::string_view input) {
    if(input.size()%4!=0) throw std::runtime_error("Invalid ICE signal encoding");

    std::string output;
    output.reserve(input.size()/4*3);

    for(std::size_t i=0;i<input.size();i+=4) {
        const int a=base64Value(input[i]);
        const int b=base64Value(input[i+1]);
        const int c=input[i+2]=='=' ? 0 : base64Value(input[i+2]);
        const int d=input[i+3]=='=' ? 0 : base64Value(input[i+3]);
        if(a<0 || b<0 || c<0 || d<0) throw std::runtime_error("Invalid ICE signal encoding");

        const unsigned value=(static_cast<unsigned>(a)<<18)|(static_cast<unsigned>(b)<<12)|
                             (static_cast<unsigned>(c)<<6)|static_cast<unsigned>(d);

        output.push_back(static_cast<char>((value>>16)&0xFF));
        if(input[i+2]!='=') output.push_back(static_cast<char>((value>>8)&0xFF));
        if(input[i+3]!='=') output.push_back(static_cast<char>(value&0xFF));
    }

    return output;
}

std::string_view takeLine(std::string_view& text) {
    const auto end=text.find('\n');
    if(end==std::string_view::npos) {
        const auto line=text;
        text={};
        return line;
    }
    const auto line=text.substr(0,end);
    text.remove_prefix(end+1);
    return line;
}

}

std::string encodeIceSignal(const IceSignalBundle& bundle) {
    if(bundle.description.sdp.empty() || bundle.description.type.empty()) {
        throw std::runtime_error("ICE signal has no description");
    }

    std::string output="MKWVC-ICE-1\n";
    output+="TYPE "+bundle.description.type+"\n";
    output+="SDP "+base64Encode(bundle.description.sdp)+"\n";

    for(const auto& candidate:bundle.candidates) {
        output+="CAND "+base64Encode(candidate.mid)+" "+base64Encode(candidate.candidate)+"\n";
    }

    return output;
}

IceSignalBundle decodeIceSignal(std::string_view text) {
    if(takeLine(text)!="MKWVC-ICE-1") throw std::runtime_error("Not an MKW VoiceChat ICE signal");

    IceSignalBundle bundle;
    while(!text.empty()) {
        const auto line=takeLine(text);
        if(line.empty()) continue;

        if(line.starts_with("TYPE ")) {
            bundle.description.type=std::string(line.substr(5));
        } else if(line.starts_with("SDP ")) {
            bundle.description.sdp=base64Decode(line.substr(4));
        } else if(line.starts_with("CAND ")) {
            const auto payload=line.substr(5);
            const auto split=payload.find(' ');
            if(split==std::string_view::npos) throw std::runtime_error("Invalid ICE candidate signal");
            bundle.candidates.push_back({
                base64Decode(payload.substr(split+1)),
                base64Decode(payload.substr(0,split))
            });
        }
    }

    if(bundle.description.type!="offer" && bundle.description.type!="answer") {
        throw std::runtime_error("ICE signal has invalid description type");
    }
    if(bundle.description.sdp.empty()) throw std::runtime_error("ICE signal has no SDP");
    return bundle;
}

}
