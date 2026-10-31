#include "game/PasteCommands.h"

#include <algorithm>
#include <tuple>

namespace tsukuyomi::paste {
namespace {
bool quote(const std::string& text, std::string& out)
{
    out += '"';
    for (const unsigned char ch : text) {
        if (ch < 32) return false;
        if (ch == '"' || ch == '\\') out += '\\';
        out += static_cast<char>(ch);
    }
    out += '"';
    return true;
}
std::string pos(const Cell& cell)
{
    return std::to_string(cell.x) + " " + std::to_string(cell.y) + " " + std::to_string(cell.z);
}
}

bool shouldSend(Replace rule, bool wantAir, bool realAir, bool same)
{
    if (same) return false;
    switch (rule) {
    case Replace::None: return realAir && !wantAir;
    case Replace::All: return true;
    case Replace::NonAir: return !wantAir;
    }
    return false;
}

bool blockArgument(const std::string& name, const std::vector<structure::StateValue>& states, std::string& out)
{
    out.clear();
    if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_:.-/") != std::string::npos) return false;
    std::string result = name;
    if (!states.empty()) {
        result += " [";
        bool first = true;
        for (const auto& state : states) {
            if (!first) result += ',';
            first = false;
            if (!quote(state.name, result)) return false;
            result += '=';
            switch (state.tag) {
            case nbt::Tag::String:
                if (!quote(state.text, result)) return false;
                break;
            case nbt::Tag::Byte:
                if (state.number != 0 && state.number != 1) return false;
                result += state.number ? "true" : "false";
                break;
            case nbt::Tag::Short: case nbt::Tag::Int: case nbt::Tag::Long:
                result += std::to_string(state.number);
                break;
            default: return false;
            }
        }
        result += ']';
    }
    out = std::move(result);
    return true;
}

std::vector<Cell> runs(std::vector<Cell> cells)
{
    std::sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) {
        return std::tie(a.y, a.z, a.x) < std::tie(b.y, b.z, b.x);
    });
    std::vector<Cell> result;
    for (auto& cell : cells) {
        std::size_t left = cell.count;
        auto x = std::int64_t(cell.x);
        while (left != 0) {
            if (!result.empty()) {
                auto& previous = result.back();
                if (previous.y == cell.y && previous.z == cell.z && previous.block == cell.block
                    && previous.count < 32768 && std::int64_t(previous.x) + static_cast<std::int64_t>(previous.count) == x) {
                    const auto count = (std::min)(left, 32768 - previous.count);
                    previous.count += count; left -= count; x += static_cast<std::int64_t>(count);
                    continue;
                }
            }
            const auto count = (std::min)(left, std::size_t{32768});
            result.push_back({static_cast<std::int32_t>(x), cell.y, cell.z, cell.block, count});
            left -= count; x += static_cast<std::int64_t>(count);
        }
    }
    return result;
}

std::vector<Command> commands(std::vector<Cell> cells)
{
    std::vector<Command> result;
    for (const auto& run : runs(std::move(cells))) {
        auto end = run;
        end.x = static_cast<std::int32_t>(std::int64_t(run.x) + static_cast<std::int64_t>(run.count) - 1);
        result.push_back({(run.count == 1 ? "/setblock " + pos(run) : "/fill " + pos(run) + " " + pos(end))
            + " " + run.block + " replace", run.count, run.x, run.y, run.z});
    }
    return result;
}
}
