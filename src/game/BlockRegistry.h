#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "game/Structure.h"

namespace tsukuyomi::blocks {

using Table = std::unordered_map<std::string, const void*>;

bool resolve(const std::vector<std::string>& wanted, Table& out);

struct WantedState {
    std::string name;
    bool isText = false;
    std::int64_t number = 0;
    std::string text;
};

std::string stateNamesOf(const void* block);

const void* stateVariant(const void* block, const std::vector<WantedState>& want,
                         std::size_t* missing = nullptr);

inline constexpr std::size_t kStateTextBytes = 96;

bool nameOfBlock(const void* block, char* out, std::size_t cap);
void forgetWorldLookups();
int statesOfBlock(const void* block, char out[][kStateTextBytes], int maxStates);
bool typedStatesOfBlock(const void* block, std::vector<structure::StateValue>& out);

int tagsOfBlock(const void* block, char out[][kStateTextBytes], int maxTags);

bool materialFlags(const void* block, bool& blocksMotion, bool& liquid);

int usedByClick(const void* block);

int clickUses(void* gameMode, const void* block, const std::int32_t pos[3], int face, const float click[3]);

const void* transformedBlock(const void* block, int quarters, int mirror, bool* ok = nullptr);

std::size_t lastEntryCount();

std::size_t appendStateVariants(const void* block, std::vector<const void*>& out);

const void* legacyOfFast(const void* block);

bool hasBlockEntity(const void* block);

bool calibrateItemIds();

bool blockItemIdAux(const void* block, std::int32_t& out);

const void* itemByName(const std::string& name);

std::vector<std::string> registeredItemNames();

bool maxStackSizeOf(const char* name, int& out);

void setAirBlock(const void* air);
const void* airBlock();

void setSimThread(unsigned long id);
unsigned long simThread();

}
