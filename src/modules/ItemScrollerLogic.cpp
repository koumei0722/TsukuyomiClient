#include "modules/ItemScrollerLogic.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace tsukuyomi::itemscrollerlogic {

bool readRecipes(const nlohmann::json& root, Recipes& recipes, int& selected)
{
    try {
        if (!root.is_object()) return false;
        Recipes next = recipes;
        auto readIng = [](const nlohmann::json& j) {
            Ingredient ing;
            if (j.is_object()) {
                ing.name = j.value("name", std::string());
                ing.aux = j.value("aux", 0);
                const std::string tag = j.value("tag", std::string());
                if (!tag.empty()) {
                    char* end = nullptr;
                    ing.tag = std::strtoull(tag.c_str(), &end, 16);
                    ing.tagKnown = end != tag.c_str() && *end == '\0';
                }
            }
            return ing;
        };
        if (root.contains("recipes")) {
            if (!root["recipes"].is_array()) return false;
            for (const auto& r : root["recipes"]) {
                if (!r.is_object()) continue;
                const int index = r.value("index", -1);
                if (index < 0 || index >= kRecipeCount) continue;
                Recipe rec;
                rec.result = readIng(r.value("result", nlohmann::json()));
                rec.resultCount = r.value("count", 1);
                rec.gridSize = r.value("grid", 9);
                if (r.contains("items")) {
                    if (!r["items"].is_array()) return false;
                    std::size_t i = 0;
                    for (const auto& it : r["items"]) {
                        if (i >= rec.items.size()) break;
                        rec.items[i++] = readIng(it);
                    }
                }
                next[static_cast<std::size_t>(index)] = std::move(rec);
            }
        }
        const int nextSelected = std::clamp(root.value("selected", 0), 0, kRecipeCount - 1);
        recipes = std::move(next);
        selected = nextSelected;
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

bool readFavorites(const nlohmann::json& root, std::set<std::string>& global,
                   std::map<std::string, std::set<std::string>>& villagers)
{
    try {
        if (!root.is_object()) return false;
        auto nextGlobal = global;
        auto nextVillagers = villagers;
        if (root.contains("global") && root["global"].is_array()) {
            for (const auto& k : root["global"]) {
                if (k.is_string()) nextGlobal.insert(k.get<std::string>());
            }
        }
        if (root.contains("villagers") && root["villagers"].is_object()) {
            for (auto it = root["villagers"].begin(); it != root["villagers"].end(); ++it) {
                if (!it.value().is_array()) continue;
                for (const auto& k : it.value()) {
                    if (k.is_string()) nextVillagers[it.key()].insert(k.get<std::string>());
                }
            }
        }
        global = std::move(nextGlobal);
        villagers = std::move(nextVillagers);
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

int compare(const SortKey& a, const SortKey& b)
{
    auto cmp = [](const auto& x, const auto& y) { return x < y ? -1 : (y < x ? 1 : 0); };
    int c = 0;
    if ((c = cmp(a.box, b.box)) != 0) return c;
    if ((c = cmp(a.bundle, b.bundle)) != 0) return c;
    if ((c = cmp(a.priority, b.priority)) != 0) return c;
    if ((c = cmp(a.empty, b.empty)) != 0) return c;
    if (a.empty) return 0;
    if (a.box && b.box) return cmp(a.fill, b.fill);
    if (a.bundle && b.bundle) return cmp(a.fill, b.fill);
    if ((c = cmp(a.category, b.category)) != 0) return c;
    if ((c = cmp(a.name, b.name)) != 0) return c;
    if ((c = cmp(a.aux, b.aux)) != 0) return c;
    return cmp(b.count, a.count);
}

}
