#include "wuwa/race.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace wuwa {
namespace {

Tile tileFromCode(int code) {
    switch (code) {
    case 0:
        return Tile{TileType::Normal, 0, -1};
    case 1:
        return Tile{TileType::Advance, 1, -1};
    case 2:
        return Tile{TileType::Delay, 1, -1};
    case 3:
        return Tile{TileType::Rift, 0, -1};
    case 4:
        return Tile{TileType::Finish, 0, -1};
    default:
        throw std::runtime_error("track tile code must be 0..4");
    }
}

std::vector<int> parseTrackArray(const std::string& text) {
    std::vector<int> values;
    bool inNumber = false;
    int value = 0;

    for (char ch : text) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            inNumber = true;
            value = value * 10 + (ch - '0');
            continue;
        }

        if (inNumber) {
            values.push_back(value);
            value = 0;
            inNumber = false;
        }

        if (ch == '[' || ch == ']' || ch == ',' || std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }

        throw std::runtime_error("track file should be an array like [0,1,2,3,4]");
    }

    if (inNumber) {
        values.push_back(value);
    }
    return values;
}

} // namespace

void loadTrackFile(Scenario& scenario, const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open track file: " + path);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::vector<int> values = parseTrackArray(buffer.str());

    if (values.empty()) {
        throw std::runtime_error("track file is empty");
    }
    if (values.size() > kMaxTrackTiles) {
        throw std::runtime_error("track is longer than kMaxTrackTiles");
    }
    if (values.back() != 4) {
        throw std::runtime_error("track file must end with 4 (Finish)");
    }

    scenario.trackLength = static_cast<int>(values.size());
    scenario.finishIndex = scenario.trackLength - 1;
    scenario.startIndex = 0;

    for (int i = 0; i < scenario.trackLength; ++i) {
        scenario.track[i] = tileFromCode(values[i]);
    }
}

void loadRunnerNamesFile(Scenario& scenario, const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open runner names file: " + path);
    }

    std::string line;
    int index = 0;
    while (index < scenario.runnerCount && std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            scenario.runners[index].name = line;
        }
        ++index;
    }
}

} // namespace wuwa
