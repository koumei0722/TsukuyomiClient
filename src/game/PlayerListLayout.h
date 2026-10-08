#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::playerlist {

inline constexpr int kMaxPlayers = 80, kMaxRows = 20, kMaxColumns = 4;
inline constexpr int kFirstBool = 256, kFirstFloat = 256, kFirstText = 96;
inline constexpr int kVisibleSlot = kFirstBool;
inline constexpr int kColumnSlot = kFirstBool + 1, kGapSlot = kFirstBool + 5;
inline constexpr int kScorePaddingSlot = kFirstBool + 8, kRowSlot = kFirstBool + 9;
inline constexpr int kBoxAlphaSlot = kFirstFloat, kStripeAlphaSlot = kFirstFloat + 1;
inline constexpr int kGhostAlphaSlot = kFirstFloat + 2, kNameAlphaSlot = kFirstFloat + 3;
inline constexpr int kNameSlot = kFirstText, kScoreSlot = kFirstText + kMaxPlayers;
inline constexpr int kWidthNameSlot = kScoreSlot + kMaxPlayers, kWidthScoreSlot = kWidthNameSlot + 1;
inline constexpr int kHeadRowSlot = kRowSlot + kMaxPlayers;
inline constexpr int kHeadColumnSlot = kHeadRowSlot + kMaxPlayers;
inline constexpr int kHeartsModeSlot = kHeadColumnSlot + 1;
inline constexpr int kHealthSlot = kNameAlphaSlot + kMaxPlayers;
inline constexpr int kFaceSlot = kWidthScoreSlot + 1;
inline constexpr int kHearts = 10;
inline constexpr int kHeartsColumn = 90;

struct Arrangement { int rows; int columns; };
Arrangement arrange(int count);
int playerAt(const Arrangement& a, int column, int row, int count);
struct Entry {
    std::string name; bool spectator; bool hasScore; int score;
    int health = -1;
    std::vector<std::uint8_t> face;
    int faceSide = 0;
};
void sortEntries(std::vector<Entry>& entries);
int estimateWidth(std::string_view utf8);
std::string displayName(const Entry& e);
std::string scoreText(int score);
void bindName(char kind, int index, char* out, std::size_t cap);
int healthPoints(float current);
std::string faceFileName(const std::vector<std::uint8_t>& rgba, int side);
std::string layoutJson();

}
