#include "game/UiTree.h"

#include <algorithm>
#include <cstring>

namespace tsukuyomi::uitree {

std::uintptr_t linkBalanced(TreeNode* nodes, TreeNode* head, int lo, int hi,
                            std::uintptr_t parent)
{
    if (lo >= hi) {
        return reinterpret_cast<std::uintptr_t>(head);
    }
    const int mid = lo + (hi - lo) / 2;
    TreeNode& node = nodes[mid];
    node.parent = parent;
    node.isnil = 0;
    node.color = 1;
    const std::uintptr_t self = reinterpret_cast<std::uintptr_t>(&node);
    node.left = linkBalanced(nodes, head, lo, mid, self);
    node.right = linkBalanced(nodes, head, mid + 1, hi, self);
    return self;
}

void finishMap(TreeNode* head, TreeNode* nodes, int count, std::uintptr_t* node)
{
    head->isnil = 1;
    head->color = 1;
    if (count <= 0) {
        const auto self = reinterpret_cast<std::uintptr_t>(head);
        head->parent = self;
        head->left = self;
        head->right = self;
        node[0] = self;
        node[1] = 0;
        node[2] = 0;
        node[3] = 0;
        return;
    }
    head->parent = linkBalanced(nodes, head, 0, count, reinterpret_cast<std::uintptr_t>(head));
    head->left = reinterpret_cast<std::uintptr_t>(&nodes[0]);
    head->right = reinterpret_cast<std::uintptr_t>(&nodes[count - 1]);
    node[0] = reinterpret_cast<std::uintptr_t>(head);
    node[1] = static_cast<std::uintptr_t>(count);
    node[2] = 0;
    node[3] = 0;
}

Arena::Arena(TreeNode* nodes, std::size_t nodeCap, std::uintptr_t* words, std::size_t wordCap,
             unsigned char* records, std::size_t recordCap, char* text, std::size_t textCap)
    : m_nodes(nodes)
    , m_nodeCap(nodeCap)
    , m_words(words)
    , m_wordCap(wordCap)
    , m_records(records)
    , m_recordCap(recordCap)
    , m_text(text)
    , m_textCap(textCap)
{
}

void Arena::reset()
{
    m_nodeAt = 0;
    m_wordAt = 0;
    m_recordAt = 0;
    m_textAt = 0;
    m_overflowed = false;
}

TreeNode* Arena::takeNodes(std::size_t count)
{
    if (count == 0 || count > m_nodeCap - m_nodeAt) {
        m_overflowed = true;
        return nullptr;
    }
    TreeNode* const at = m_nodes + m_nodeAt;
    m_nodeAt += count;
    std::memset(at, 0, count * sizeof(TreeNode));
    return at;
}

std::uintptr_t* Arena::takeWords(std::size_t count)
{
    if (count == 0 || count > m_wordCap - m_wordAt) {
        m_overflowed = true;
        return nullptr;
    }
    std::uintptr_t* const at = m_words + m_wordAt;
    m_wordAt += count;
    std::memset(at, 0, count * sizeof(std::uintptr_t));
    return at;
}

unsigned char* Arena::takeRecord()
{
    if (m_recordAt >= m_recordCap) {
        m_overflowed = true;
        return nullptr;
    }
    unsigned char* const at = m_records + m_recordAt * kRecordBytes;
    ++m_recordAt;
    std::memset(at, 0, kRecordBytes);
    return at;
}

const char* Arena::takeText(const char* utf8)
{
    if (utf8 == nullptr) {
        utf8 = "";
    }
    const std::size_t length = std::strlen(utf8);
    if (length + 1 > m_textCap - m_textAt) {
        m_overflowed = true;
        return nullptr;
    }
    char* const at = m_text + m_textAt;
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned char ch = static_cast<unsigned char>(utf8[i]);
        at[i] = (ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : '?';
    }
    at[length] = '\0';
    m_textAt += length + 1;
    return at;
}

std::uintptr_t* Arena::makeMap(KeyValue* items, std::size_t count)
{
    if (items == nullptr && count != 0) {
        return nullptr;
    }
    std::stable_sort(items, items + count, [](const KeyValue& a, const KeyValue& b) {
        const char* const ka = (a.key != nullptr) ? a.key : "";
        const char* const kb = (b.key != nullptr) ? b.key : "";
        return std::strcmp(ka, kb) < 0;
    });
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (items[i].key == nullptr) {
            continue;
        }
        if (kept > 0 && std::strcmp(items[kept - 1].key, items[i].key) == 0) {
            items[kept - 1] = items[i];
            continue;
        }
        items[kept++] = items[i];
    }

    TreeNode* const head = takeNodes(1);
    if (head == nullptr) {
        return nullptr;
    }
    TreeNode* body = nullptr;
    if (kept > 0) {
        body = takeNodes(kept);
        if (body == nullptr) {
            return nullptr;
        }
        for (std::size_t i = 0; i < kept; ++i) {
            body[i].key = reinterpret_cast<std::uintptr_t>(items[i].key);
            body[i].value = items[i].value;
            body[i].tag = items[i].tag;
            body[i].seq = items[i].seq;
        }
    }
    std::uintptr_t* const node = takeWords(kNodeWords);
    if (node == nullptr) {
        return nullptr;
    }
    finishMap(head, body, static_cast<int>(kept), node);
    return node;
}

std::uintptr_t* Arena::makeValue(const std::uintptr_t* donorWords, const std::uintptr_t* node)
{
    if (donorWords == nullptr || node == nullptr) {
        return nullptr;
    }
    std::uintptr_t* const value = takeWords(kValueWords);
    if (value == nullptr) {
        return nullptr;
    }
    std::memcpy(value, donorWords, kValueWords * sizeof(std::uintptr_t));
    value[0] = reinterpret_cast<std::uintptr_t>(node);
    return value;
}

unsigned char* Arena::makeStringRecord(const unsigned char* donorRecord, const char* text)
{
    if (donorRecord == nullptr || text == nullptr) {
        return nullptr;
    }
    unsigned char* const record = takeRecord();
    if (record == nullptr) {
        return nullptr;
    }
    std::memcpy(record, donorRecord, kRecordBytes);
    std::memcpy(record, &text, sizeof(text));
    return record;
}

std::uintptr_t* Arena::makeVector(const std::uintptr_t* donorVector,
                                  const std::uintptr_t* const* elements, std::size_t count)
{
    if (donorVector == nullptr || (elements == nullptr && count != 0)) {
        return nullptr;
    }
    std::uintptr_t* const slots = takeWords(count == 0 ? 1 : count);
    if (slots == nullptr) {
        return nullptr;
    }
    for (std::size_t i = 0; i < count; ++i) {
        slots[i] = reinterpret_cast<std::uintptr_t>(elements[i]);
    }
    std::uintptr_t* const vec = takeWords(kVectorWords);
    if (vec == nullptr) {
        return nullptr;
    }
    std::memcpy(vec, donorVector, kVectorWords * sizeof(std::uintptr_t));
    vec[0] = reinterpret_cast<std::uintptr_t>(slots);
    vec[1] = reinterpret_cast<std::uintptr_t>(slots + count);
    vec[2] = vec[1];
    return vec;
}

std::uintptr_t* Arena::makeStringElement(const std::uintptr_t* donorElement,
                                         const unsigned char* record)
{
    if (donorElement == nullptr || record == nullptr) {
        return nullptr;
    }
    std::uintptr_t* const elem = takeWords(kVectorWords);
    if (elem == nullptr) {
        return nullptr;
    }
    std::memcpy(elem, donorElement, kVectorWords * sizeof(std::uintptr_t));
    elem[0] = reinterpret_cast<std::uintptr_t>(record);
    elem[1] = kTagString;
    return elem;
}

}
