#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int hexByte(std::string_view text) {
    int value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    std::from_chars(first, last, value, 16);
    return value;
}

std::vector<std::string_view> splitView(std::string_view text, char delim) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find(delim, start);
        if (pos == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::string actorName(int actor, int runnerCount, bool hasBuda) {
    if (hasBuda && actor == runnerCount) {
        return "Buda";
    }
    return "R" + std::to_string(actor);
}

std::string decodeOrder(std::string_view text, int entityCount, int runnerCount, bool hasBuda) {
    std::ostringstream out;
    out << "[";
    for (int i = 0; i < entityCount; ++i) {
        if (i > 0) {
            out << ",";
        }
        out << actorName(hexByte(text.substr(static_cast<std::size_t>(i) * 2, 2)), runnerCount, hasBuda);
    }
    out << "]";
    return out.str();
}

std::string decodeState(std::string_view text, int entityCount, int runnerCount, bool hasBuda) {
    std::ostringstream out;
    out << "{";
    for (int entity = 0; entity < entityCount; ++entity) {
        if (entity > 0) {
            out << " ";
        }
        const std::size_t off = static_cast<std::size_t>(entity) * 4;
        const int pos = hexByte(text.substr(off, 2));
        const int depth = hexByte(text.substr(off + 2, 2));
        out << actorName(entity, runnerCount, hasBuda) << "@P" << (pos + 1) << "#" << depth;
    }
    out << "}";
    return out.str();
}

void decodeRaceLine(
    std::string_view line,
    int runnerCount,
    int entityCount,
    bool hasBuda
) {
    const std::size_t stateLen = static_cast<std::size_t>(entityCount) * 4;
    const auto top = splitView(line, '|');
    if (top.size() != 3) {
        throw std::runtime_error("bad race line");
    }

    const int winner = hexByte(top[0]);
    std::cout << "Race winner=" << actorName(winner, runnerCount, hasBuda)
              << " order=" << decodeOrder(top[1], entityCount, runnerCount, hasBuda) << "\n";

    const auto rounds = splitView(top[2], ';');
    for (std::size_t r = 0; r < rounds.size(); ++r) {
        const auto chunks = splitView(rounds[r], '>');
        if (chunks.empty() || chunks[0].size() != stateLen) {
            throw std::runtime_error("bad round state");
        }

        std::cout << "  round " << (r + 1)
                  << " start " << decodeState(chunks[0], entityCount, runnerCount, hasBuda) << "\n";

        for (std::size_t i = 1; i < chunks.size(); ++i) {
            if (chunks[i].size() != 6 + stateLen) {
                throw std::runtime_error("bad move event");
            }

            const int actor = hexByte(chunks[i].substr(0, 2));
            const int roll = hexByte(chunks[i].substr(2, 2));
            const int move = hexByte(chunks[i].substr(4, 2)) - 128;
            const auto state = chunks[i].substr(6, stateLen);
            std::cout << "    " << actorName(actor, runnerCount, hasBuda)
                      << " roll=" << roll
                      << " move=" << move
                      << " -> " << decodeState(state, entityCount, runnerCount, hasBuda) << "\n";
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: wuwa_trace_decode <trace.wtrace>\n";
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "failed to open " << argv[1] << "\n";
        return 1;
    }

    int runnerCount = 0;
    int entityCount = 0;
    int trackLength = 0;
    bool hasBuda = false;
    bool inData = false;
    std::string line;

    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        if (line == "DATA") {
            inData = true;
            continue;
        }

        if (!inData) {
            std::istringstream header(line);
            std::string key;
            header >> key;
            if (key == "N") {
                int budaFlag = 0;
                header >> runnerCount >> entityCount >> trackLength >> budaFlag;
                hasBuda = budaFlag != 0;
            } else if (key == "TRACK") {
                std::string codes;
                header >> codes;
                std::cout << "Track(" << codes.size() << "): " << codes << "\n";
            } else if (key == "RUNNERS") {
                std::string runners;
                header >> runners;
                std::cout << "Runners: " << runners << "\n";
            }
            continue;
        }

        decodeRaceLine(line, runnerCount, entityCount, hasBuda);
    }

    (void)trackLength;
    return 0;
}
