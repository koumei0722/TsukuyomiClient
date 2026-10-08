#include "game/SettingsCommandLogic.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <unordered_set>
#include <utility>

#include "core/Strings.h"
#include "input/Keys.h"
#include "input/PadKeys.h"

#include <nlohmann/json.hpp>

namespace tsukuyomi::settingscommand {

std::string slug(std::wstring_view label)
{
    std::string result;
    bool separator = false;
    for (wchar_t c : label) {
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) {
            if (separator && !result.empty()) result += '_';
            result += static_cast<char>(c);
            separator = false;
        } else {
            separator = true;
        }
    }
    return result;
}

namespace {
bool supported(const MenuItem& item)
{
    switch (item.kind) {
    case MenuItemKind::Toggle: return item.isOn && item.activate;
    case MenuItemKind::Number: return item.getNumber && item.setNumber;
    case MenuItemKind::Text: return item.getText && item.setText;
    case MenuItemKind::Keybind: return item.getKeys && item.setKeys;
    case MenuItemKind::Action: return !!item.activate;
    case MenuItemKind::Cycle:
        return (!item.choices.empty() && item.getChoice && item.setChoice)
               || (item.value && item.activate);
    default: return false;
    }
}

void collect(const std::vector<MenuItem>& children, const std::string& prefix,
             std::vector<SettingRef>& result, std::unordered_set<std::string>& used, bool includeHidden)
{
    for (std::size_t i = 0; i < children.size(); ++i) {
        const auto& item = children[i];
        if ((!includeHidden && item.hidden) || item.opensPage) continue;
        if (item.kind == MenuItemKind::Submenu) {
            collect(item.children, prefix + slug(item.labelText()) + "_", result, used, includeHidden);
            continue;
        }
        if (!supported(item)) continue;
        std::string name = item.commandName.empty() ? slug(item.labelText()) : item.commandName;
        if (name.empty()) name = "setting" + std::to_string(i);
        const std::string base = prefix + name;
        name = base;
        for (std::size_t suffix = 2; !used.insert(name).second; ++suffix)
            name = base + "_" + std::to_string(suffix);
        result.push_back({std::move(name), &item});
    }
}

struct BindingValue {
    bool hasKey = false;
    bool keyDefault = false;
    std::vector<int> key;
    bool hasPad = false;
    std::vector<int> pad;
};

bool parseBinding(const std::string& text, BindingValue& out, std::string& error)
{
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.empty()) {
        error = "Invalid binding: " + text;
        return false;
    }
    for (const auto& [name, value] : json.items()) {
        std::string joined;
        if (value.is_string()) {
            joined = value.get<std::string>();
        } else if (value.is_array()) {
            for (const auto& part : value) {
                if (!part.is_string()) { error = "\"" + name + "\" must be a name or a list of names"; return false; }
                if (!joined.empty()) joined += " + ";
                joined += part.get<std::string>();
            }
        } else {
            error = "\"" + name + "\" must be a name or a list of names";
            return false;
        }
        if (name == "key") {
            out.hasKey = true;
            std::string lowered = joined;
            for (char& c : lowered) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            if (lowered == "default" || lowered == "reset") out.keyDefault = true;
            else if (!keys::parseCombo(toUtf16(joined), out.key)) { error = "Unknown key: " + joined; return false; }
        } else if (name == "pad") {
            out.hasPad = true;
            if (!padkeys::parseCombo(joined, out.pad)) { error = "Unknown controller button: " + joined; return false; }
        } else {
            error = "Unknown field: " + name + " (use \"key\" and \"pad\")";
            return false;
        }
    }
    return true;
}

std::string describe(const MenuItem& item)
{
    if (item.kind == MenuItemKind::Toggle) return item.isOn() ? "true" : "false";
    if (item.kind == MenuItemKind::Keybind) {
        const auto combo = item.getKeys();
        std::string text = combo.empty() ? "unassigned" : toUtf8(keys::comboName(combo));
        if (item.getPadKeys) {
            const auto pad = item.getPadKeys();
            if (!pad.empty()) text += " / pad: " + padkeys::comboName(pad);
        }
        return text;
    }
    if (item.kind == MenuItemKind::Action && !item.value) return "action";
    return toUtf8(item.valueText());
}

std::string joined(const std::vector<std::string>& values, std::string_view separator,
                   std::size_t first = 0)
{
    std::string result;
    for (std::size_t i = first; i < values.size(); ++i) {
        if (i != first) result += separator;
        result += values[i];
    }
    return result;
}

int resolve(const std::vector<std::string>& names, std::string_view input)
{
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == input) return static_cast<int>(i);
    }
    return -1;
}

void packed(Result& result, const std::vector<std::string>& entries, std::string prefix = {})
{
    std::vector<std::string> lines;
    std::vector<std::size_t> counts;
    std::string line = std::move(prefix);
    std::size_t count = 0;
    auto flush = [&] {
        lines.push_back(std::move(line));
        counts.push_back(count);
        line.clear();
        count = 0;
    };
    for (const auto& entry : entries) {
        if (!line.empty() && line.size() + (count ? 2 : 0) + entry.size() > 100) flush();
        if (count) line += ", ";
        std::string_view rest = entry;
        while (line.size() + rest.size() > 100) {
            std::size_t take = 100 - line.size();
            while (take > 0 && (static_cast<unsigned char>(rest[take]) & 0xc0) == 0x80) --take;
            line += rest.substr(0, take);
            rest.remove_prefix(take);
            flush();
        }
        line += rest;
        ++count;
    }
    if (!line.empty()) flush();
    const std::size_t room = 20 - result.lines.size();
    if (lines.size() <= room) {
        result.lines.insert(result.lines.end(), lines.begin(), lines.end());
    } else if (room > 0) {
        result.lines.insert(result.lines.end(), lines.begin(), lines.begin() + (room - 1));
        std::size_t remaining = 0;
        for (std::size_t i = room - 1; i < counts.size(); ++i) remaining += counts[i];
        result.lines.push_back("... and " + std::to_string(remaining) + " more");
    }
}

std::vector<std::string> choiceNames(const MenuItem& item)
{
    std::vector<std::string> names;
    for (const auto& value : item.choices) names.push_back(slug(value));
    return names;
}

std::vector<std::string> choiceDisplayNames(const MenuItem& item)
{
    std::vector<std::string> names;
    for (const auto& value : item.choices) {
        auto name = toUtf8(value);
        for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        names.push_back(std::move(name));
    }
    return names;
}

std::string lower(std::string value)
{
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

bool choiceValue(const std::string& value, std::string& decoded, bool& quoted)
{
    quoted = !value.empty() && value.front() == '"';
    if (!quoted) {
        decoded = lower(value);
        return chatcommand::isEnumWord(decoded);
    }
    if (value.size() < 2 || value.back() != '"') return false;
    decoded.clear();
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        char c = value[i];
        if (c == '\\') {
            if (++i + 1 >= value.size() || (value[i] != '\\' && value[i] != '"')) return false;
            c = value[i];
        } else if (c == '"') return false;
        decoded += c;
    }
    decoded = lower(decoded);
    return true;
}

std::string range(const MenuItem& item)
{
    return std::format("{:g}..{:g}", item.numberMin, item.numberMax);
}

std::string hint(const MenuItem& item)
{
    switch (item.kind) {
    case MenuItemKind::Toggle: return " (true|false|toggle)";
    case MenuItemKind::Cycle: {
        if (item.choices.empty()) return " (next or a value it cycles through; quote names containing spaces or symbols)";
        auto names = choiceNames(item);
        const auto display = choiceDisplayNames(item);
        for (std::size_t i = 0; i < names.size(); ++i) {
            auto& name = names[i];
            if (chatcommand::isEnumWord(name)) continue;
            std::string quoted = "\"";
            for (char c : display[i]) {
                if (c == '\\' || c == '"') quoted += '\\';
                quoted += c;
            }
            name = quoted + "\"";
        }
        return " (choices: " + joined(names, ", ") + "; next)";
    }
    case MenuItemKind::Number:
        return std::string(item.numberIsInteger ? " (whole number" : " (number")
               + (item.numberMin < item.numberMax ? " " + range(item) : "") + ")";
    case MenuItemKind::Text: return " (int)";
    case MenuItemKind::Keybind:
        return " (f6, pad_lb, none, default, pad_none or {\"key\":[\"ctrl\",\"r\"],\"pad\":[\"lb\",\"x\"]})";
    case MenuItemKind::Action: return " (action: run)";
    default: return {};
    }
}
}

std::vector<SettingRef> settingsOf(const MenuItem& module, bool includeHidden)
{
    std::vector<SettingRef> result;
    std::unordered_set<std::string> used;
    collect(module.children, {}, result, used, includeHidden);
    return result;
}

Result run(const std::vector<MenuItem>& tree, const std::vector<std::string>& args)
{
    Result result;
    result.error = true;
    std::vector<std::string> modules;
    for (const auto& module : tree) modules.push_back(slug(module.labelText()));
    if (args.empty()) {
        std::vector<std::string> visible;
        for (std::size_t i = 0; i < tree.size(); ++i) if (!tree[i].hidden) visible.push_back(modules[i]);
        packed(result, visible, "Modules: ");
        result.lines.push_back("Usage: /tk set <module> <setting> <value>");
        return result;
    }
    const auto moduleName = lower(args[0]);
    const int moduleAt = resolve(modules, moduleName);
    if (moduleAt < 0) {
        result.lines.push_back("Unknown module: " + args[0]);
        return result;
    }
    const auto& module = tree[static_cast<std::size_t>(moduleAt)];
    const std::string label = toUtf8(module.labelText());
    if (module.hidden) {
        result.lines.push_back(label + " is hidden: a hook or patch it needs is turned off in hooks.json");
        return result;
    }
    const auto settings = settingsOf(module);
    if (args.size() == 1) {
        if (!module.isAvailable()) {
            result.lines.push_back(label + " is not available");
            return result;
        }
        result.lines.push_back(label + " (" + std::to_string(settings.size()) + " settings) - /tk set "
                               + modules[static_cast<std::size_t>(moduleAt)] + " <setting> <value>");
        std::vector<std::string> entries;
        for (const auto& ref : settings) entries.push_back(ref.name + ": " + describe(*ref.item));
        packed(result, entries);
        result.error = false;
        return result;
    }
    std::vector<std::string> names;
    for (const auto& ref : settings) names.push_back(ref.name);
    const std::string settingName = lower(args[1]);
    const int at = resolve(names, settingName);
    if (at < 0) {
        result.lines.push_back("Unknown setting: " + args[1]);
        return result;
    }
    const auto& ref = settings[static_cast<std::size_t>(at)];
    const auto& item = *ref.item;
    const std::string title = label + " " + ref.name;
    if (!module.isAvailable() || !item.isAvailable()) {
        result.lines.push_back(title + " is not available");
        return result;
    }
    const std::string before = describe(item);
    if (args.size() == 2) {
        result.lines.push_back(title + ": " + before + hint(item));
        result.error = false;
        return result;
    }
    const std::string value = joined(args, " ", 2);
    const std::string normalized = lower(value);
    std::string after;
    std::string suffix;
    switch (item.kind) {
    case MenuItemKind::Toggle: {
        const bool current = item.isOn();
        bool desired = current;
        if (normalized == "true") desired = true;
        else if (normalized == "false") desired = false;
        else if (normalized == "toggle") desired = !current;
        else { result.lines.push_back("Unknown value: " + value + " (true|false|toggle)"); return result; }
        if (current != desired) item.activate();
        if (item.isOn() != desired) {
            result.lines.push_back("could not turn " + title + (desired ? " on" : " off") + " (see Tsukuyomi.log)");
            return result;
        }
        result.changed = current != desired;
        break;
    }
    case MenuItemKind::Cycle: {
        std::string target;
        bool quoted = false;
        if (!choiceValue(value, target, quoted)) {
            result.lines.push_back("Unknown value: " + value + " (use a choice name, next or a quoted display name)");
            return result;
        }
        if (!item.choices.empty() && item.getChoice && item.setChoice) {
            const auto choices = choiceNames(item);
            int selected = -1;
            if (!quoted && normalized == "next") {
                const int current = item.getChoice();
                selected = current >= 0 && static_cast<std::size_t>(current) < choices.size()
                           ? static_cast<int>((static_cast<std::size_t>(current) + 1) % choices.size()) : 0;
            } else {
                selected = resolve(quoted ? choiceDisplayNames(item) : choices, target);
            }
            if (selected < 0) {
                result.lines.push_back("Unknown value: " + value + " (choices: " + joined(choices, ", ") + ")");
                return result;
            }
            const int previous = item.getChoice();
            item.setChoice(selected);
            result.changed = previous != item.getChoice();
        } else if (!quoted && normalized == "next") {
            item.activate();
            result.changed = before != describe(item);
        } else {
            std::vector<std::string> seen{before};
            const auto same = [&](const std::string& shown) {
                return quoted ? lower(shown) == target : slug(toUtf16(shown)) == target;
            };
            bool matched = same(before);
            for (int step = 0; !matched && step < 64; ++step) {
                item.activate();
                const std::string current = describe(item);
                matched = same(current);
                if (current == before) break;
                seen.push_back(current);
            }
            result.changed = before != describe(item);
            if (!matched) {
                result.lines.push_back("Unknown value: " + value + " (choices: " + joined(seen, ", ") + ")");
                return result;
            }
        }
        break;
    }
    case MenuItemKind::Number: {
        std::string_view typed = value;
        if (typed.starts_with('+')) typed.remove_prefix(1);
        float number = 0;
        const auto parsed = std::from_chars(typed.data(), typed.data() + typed.size(), number);
        if (typed.empty() || typed.starts_with('+') || (typed.starts_with('-') && value.starts_with('+'))
            || parsed.ec != std::errc{} || parsed.ptr != typed.data() + typed.size() || !std::isfinite(number)) {
            result.lines.push_back("Unknown number: " + value); return result;
        }
        if (item.numberIsInteger && std::trunc(number) != number) {
            result.lines.push_back(title + " needs a whole number"); return result;
        }
        if (item.numberMin < item.numberMax) {
            const float clamped = std::clamp(number, item.numberMin, item.numberMax);
            if (clamped != number) suffix = " (clamped to " + range(item) + ")";
            number = clamped;
        }
        const float previous = item.getNumber();
        item.setNumber(number);
        result.changed = previous != item.getNumber();
        break;
    }
    case MenuItemKind::Text: {
        std::string_view numberText = value;
        if (numberText.starts_with('+')) numberText.remove_prefix(1);
        int number = 0;
        const auto parsed = std::from_chars(numberText.data(), numberText.data() + numberText.size(), number);
        if (numberText.empty() || (value.starts_with('+') && numberText.starts_with('-'))
            || parsed.ec != std::errc{} || parsed.ptr != numberText.data() + numberText.size()) {
            result.lines.push_back("Unknown integer: " + value); return result;
        }
        const auto previous = item.getText();
        const auto typed = toUtf16(value);
        item.setText(typed);
        const auto current = item.getText();
        if (current == previous && current != typed) {
            result.lines.push_back("Value not accepted: " + value); return result;
        }
        after = toUtf8(current);
        result.changed = previous != current;
        break;
    }
    case MenuItemKind::Keybind: {
        std::vector<int> combo;
        if (!value.empty() && value.front() == '{') {
            BindingValue binding;
            std::string error;
            if (!parseBinding(value, binding, error)) {
                result.lines.push_back(error + " (e.g. {\"key\":\"C\",\"pad\":[\"lb\",\"x\"]}, {\"pad\":[]})");
                return result;
            }
            if (binding.hasPad && (!item.setPadKeys || !item.getPadKeys)) {
                result.lines.push_back(ref.name + " has no controller binding");
                return result;
            }
            const auto beforeKeys = item.getKeys();
            const auto beforePad = item.getPadKeys ? item.getPadKeys() : std::vector<int>{};
            if (binding.hasKey) {
                item.setKeys(binding.keyDefault ? item.defaultKeys : binding.key);
                result.keysChanged = true;
            }
            if (binding.hasPad) {
                item.setPadKeys(binding.pad);
                result.padChanged = true;
            }
            result.changed = beforeKeys != item.getKeys() || (item.getPadKeys && beforePad != item.getPadKeys());
            break;
        }
        const auto values = keys::commandNames();
        const bool keyboardValue = std::find(values.begin(), values.end(), normalized) != values.end();
        const bool padValue = std::any_of(padkeys::kButtons.begin(), padkeys::kButtons.end(), [&](const auto& b) {
            return normalized == "pad_" + lower(std::string(b.name));
        });
        if (!keyboardValue && !padValue && normalized != "none" && normalized != "default" && normalized != "pad_none") {
            result.lines.push_back("Unknown key: " + value + " (use f6, pad_lb or a json binding)");
            return result;
        }
        if (padkeys::isPadValue(value)) {
            if (!item.setPadKeys || !item.getPadKeys) {
                result.lines.push_back(ref.name + " has no controller binding");
                return result;
            }
            if (!padkeys::parseCombo(value, combo)) {
                result.lines.push_back("Unknown controller button: " + value
                    + " (use pad_lb, pad_rt, pad_none or a json binding)");
                return result;
            }
            const auto previous = item.getPadKeys();
            item.setPadKeys(combo);
            result.padChanged = true;
            result.changed = previous != item.getPadKeys();
            break;
        }
        if (normalized == "default") combo = item.defaultKeys;
        else if (!keys::parseCombo(toUtf16(value), combo)) {
            result.lines.push_back("Unknown key: " + value + " (use f6, mouse4, none, default or a json binding)");
            return result;
        }
        const auto previous = item.getKeys();
        item.setKeys(combo);
        result.keysChanged = true;
        result.changed = previous != item.getKeys();
        break;
    }
    case MenuItemKind::Action:
        if (normalized != "run") {
            result.lines.push_back(ref.name + " is an action: /tk set " + modules[static_cast<std::size_t>(moduleAt)] + " " + ref.name + " run");
            return result;
        }
        item.activate();
        result.changed = true;
        break;
    default: break;
    }
    if (item.kind != MenuItemKind::Text) after = describe(item);
    result.lines.push_back(result.changed ? title + ": " + before + " -> " + after + suffix
                                         : title + " is already " + after + suffix);
    result.error = false;
    return result;
}

std::vector<ModuleInfo> buildIndex(const std::vector<MenuItem>& tree)
{
    std::vector<ModuleInfo> index;
    for (const auto& module : tree) {
        if (module.hidden) continue;
        ModuleInfo info{slug(module.labelText()), {}};
        for (const auto& ref : settingsOf(module)) {
            SettingInfo setting{ref.name, {}, ref.item->kind};
            switch (ref.item->kind) {
            case MenuItemKind::Toggle: setting.values = {"true", "false", "toggle"}; break;
            case MenuItemKind::Keybind:
                setting.values = keys::commandNames();
                setting.values.insert(setting.values.end(), {"none", "default"});
                if (ref.item->setPadKeys) {
                    setting.values.push_back("pad_none");
                    for (const auto& button : padkeys::kButtons) {
                        std::string value = "pad_";
                        for (char c : button.name) value += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
                        setting.values.push_back(std::move(value));
                    }
                }
                break;
            case MenuItemKind::Action: setting.values = {"run"}; break;
            case MenuItemKind::Cycle:
                setting.values = choiceNames(*ref.item);
                std::erase_if(setting.values, [](const auto& value) { return !chatcommand::isEnumWord(value); });
                setting.values.push_back("next");
                break;
            default: break;
            }
            info.settings.push_back(std::move(setting));
        }
        index.push_back(std::move(info));
    }
    return index;
}

chatcommand::SettingsCompletion completionOf(const std::vector<ModuleInfo>& index)
{
    chatcommand::SettingsCompletion result;
    for (const auto& module : index) {
        result.modules.push_back(module.name);
        for (const auto& setting : module.settings) {
            result.settings.push_back(setting.name);
            switch (setting.kind) {
            case MenuItemKind::Toggle: result.toggle.push_back(setting.name); break;
            case MenuItemKind::Number: result.number.push_back(setting.name); break;
            case MenuItemKind::Text: result.integer.push_back(setting.name); break;
            case MenuItemKind::Cycle:
                result.choice.push_back(setting.name);
                result.choiceValues.insert(result.choiceValues.end(), setting.values.begin(), setting.values.end());
                break;
            case MenuItemKind::Keybind: result.key.push_back(setting.name); break;
            case MenuItemKind::Action: result.action.push_back(setting.name); break;
            default: break;
            }
        }
    }
    if (!result.key.empty()) {
        result.keyValues = keys::commandNames();
        result.keyValues.insert(result.keyValues.end(), {"none", "default", "pad_none"});
        for (const auto& button : padkeys::kButtons) result.keyValues.push_back("pad_" + lower(std::string(button.name)));
    }
    for (auto* values : {&result.settings, &result.toggle, &result.number, &result.integer,
                        &result.choice, &result.key, &result.action, &result.choiceValues, &result.keyValues}) {
        std::sort(values->begin(), values->end());
        values->erase(std::unique(values->begin(), values->end()), values->end());
    }
    return result;
}

bool allowSuggestion(const std::vector<ModuleInfo>& index, const std::vector<std::string>& words,
                     std::size_t argument, std::string_view suggestion)
{
    if ((argument != 3 && argument != 4) || words.size() < 3) return true;
    std::vector<std::string> modules;
    for (const auto& module : index) modules.push_back(module.name);
    const int moduleAt = resolve(modules, words[2]);
    if (moduleAt < 0) return true;
    const auto& settings = index[static_cast<std::size_t>(moduleAt)].settings;
    if (argument == 3) return std::any_of(settings.begin(), settings.end(), [&](const auto& item) { return item.name == suggestion; });
    if (words.size() < 4) return true;
    std::vector<std::string> names;
    for (const auto& setting : settings) names.push_back(setting.name);
    const int at = resolve(names, words[3]);
    if (at < 0) return true;
    const auto& setting = settings[static_cast<std::size_t>(at)];
    if (!suggestion.empty() && ((setting.kind == MenuItemKind::Keybind
        && std::string_view("{}[]\"").find(suggestion.front()) != std::string_view::npos)
        || (setting.kind == MenuItemKind::Cycle && suggestion.front() == '"'))) return true;
    const auto& values = setting.values;
    return std::find(values.begin(), values.end(), suggestion) != values.end();
}

}
