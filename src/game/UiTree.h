#pragma once

#include <cstddef>
#include <cstdint>

namespace tsukuyomi::uitree {

inline constexpr std::size_t kValueWords = 8;
inline constexpr std::size_t kVectorWords = 4;
inline constexpr std::size_t kNodeWords = 4;
inline constexpr std::size_t kRecordBytes = 16;

inline constexpr std::uintptr_t kTagString = 4;
inline constexpr std::uintptr_t kTagArray = 6;
inline constexpr std::uintptr_t kTagObject = 7;

#pragma pack(push, 8)
struct TreeNode {
    std::uintptr_t left;
    std::uintptr_t parent;
    std::uintptr_t right;
    unsigned char color;
    unsigned char isnil;
    unsigned char pad[6];
    std::uintptr_t key;
    std::uintptr_t value;
    std::uintptr_t tag;
    std::uintptr_t seq;
};
#pragma pack(pop)
static_assert(sizeof(TreeNode) == 0x40);

struct KeyValue {
    const char* key = nullptr;
    std::uintptr_t value = 0;
    std::uintptr_t tag = 0;
    std::uintptr_t seq = 0;
};

std::uintptr_t linkBalanced(TreeNode* nodes, TreeNode* head, int lo, int hi,
                            std::uintptr_t parent);

void finishMap(TreeNode* head, TreeNode* nodes, int count, std::uintptr_t* node);

class Arena {
public:
    Arena(TreeNode* nodes, std::size_t nodeCap, std::uintptr_t* words, std::size_t wordCap,
          unsigned char* records, std::size_t recordCap, char* text, std::size_t textCap);

    void reset();

    TreeNode* takeNodes(std::size_t count);
    std::uintptr_t* takeWords(std::size_t count);
    unsigned char* takeRecord();
    const char* takeText(const char* utf8);

    bool overflowed() const { return m_overflowed; }

    std::size_t usedNodes() const { return m_nodeAt; }
    std::size_t usedWords() const { return m_wordAt; }
    std::size_t usedRecords() const { return m_recordAt; }
    std::size_t usedText() const { return m_textAt; }

    std::size_t capacityNodes() const { return m_nodeCap; }
    std::size_t capacityWords() const { return m_wordCap; }
    std::size_t capacityRecords() const { return m_recordCap; }
    std::size_t capacityText() const { return m_textCap; }

    std::uintptr_t* makeMap(KeyValue* items, std::size_t count);

    std::uintptr_t* makeValue(const std::uintptr_t* donorWords, const std::uintptr_t* node);

    unsigned char* makeStringRecord(const unsigned char* donorRecord, const char* text);

    std::uintptr_t* makeVector(const std::uintptr_t* donorVector,
                               const std::uintptr_t* const* elements, std::size_t count);

    std::uintptr_t* makeStringElement(const std::uintptr_t* donorElement,
                                      const unsigned char* record);

private:
    TreeNode* m_nodes;
    std::size_t m_nodeCap;
    std::size_t m_nodeAt = 0;
    std::uintptr_t* m_words;
    std::size_t m_wordCap;
    std::size_t m_wordAt = 0;
    unsigned char* m_records;
    std::size_t m_recordCap;
    std::size_t m_recordAt = 0;
    char* m_text;
    std::size_t m_textCap;
    std::size_t m_textAt = 0;
    bool m_overflowed = false;
};

}
