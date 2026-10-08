#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace tsukuyomi::itemscrollerlogic {

struct Ingredient {
    std::string name;
    int aux = 0;
    std::uint64_t tag = 0;
    bool tagKnown = false;
    bool empty() const { return name.empty(); }
    bool same(const Ingredient& o) const
    {
        return name == o.name && aux == o.aux && tagKnown == o.tagKnown && tag == o.tag;
    }
};

struct Recipe {
    Ingredient result;
    int resultCount = 0;
    std::array<Ingredient, 9> items{};
    int gridSize = 0;
    bool empty() const { return result.empty(); }
};

constexpr int kRecipeCount = 144;
using Recipes = std::array<Recipe, kRecipeCount>;
bool readRecipes(const nlohmann::json& root, Recipes& recipes, int& selected);
bool readFavorites(const nlohmann::json& root, std::set<std::string>& global,
                   std::map<std::string, std::set<std::string>>& villagers);

struct SortKey {
    std::string name;
    bool empty = false;
    bool box = false;
    bool bundle = false;
    int priority = -1;
    int category = 0;
    int count = 0;
    int aux = 0;
    int fill = 0;
};

int compare(const SortKey& a, const SortKey& b);

inline bool canTakeCraftOutput(int clicks, int cap)
{
    return cap == 1 ? clicks < cap : clicks + 1 < cap;
}

}
