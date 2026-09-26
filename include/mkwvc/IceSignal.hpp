#pragma once

#include "mkwvc/IcePeerTransport.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mkwvc {

struct IceSignalBundle {
    IceDescriptionSignal description;
    std::vector<IceCandidateSignal> candidates;
};

std::string encodeIceSignal(const IceSignalBundle& bundle);
IceSignalBundle decodeIceSignal(std::string_view text);

}
