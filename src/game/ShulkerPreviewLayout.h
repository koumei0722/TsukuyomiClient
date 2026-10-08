#pragma once

#include <string>

namespace tsukuyomi::spreview {

inline constexpr char kCollection[] = "tk_sp_items";
static_assert(sizeof(kCollection) - 1 <= 15);

inline constexpr char kBindCurrent[] = "#tk_sp_cur";
inline constexpr char kBindVisible[] = "#tk_sp_visible";

inline constexpr int kColumns = 9;
inline constexpr int kRows = 3;
inline constexpr int kCell = 18;

std::string recorderJson();

inline constexpr char kBindName[] = "#tk_sp_name";
inline constexpr int kTextSlotName = 344;
inline constexpr char kBindNormal[] = "#tk_sp_normal";
inline constexpr char kBindSelectedPrefix[] = "#tk_sp_sel";
inline constexpr char kBindHasSelected[] = "#tk_sp_hassel";
inline constexpr char kBindSelectedName[] = "#tk_sp_selname";
inline constexpr int kTextSlotSelectedName = 345;
std::string selectedBinding(int index);
inline constexpr int kBundleCell = 20;
inline constexpr int kBundleMargin = 5;
inline constexpr int kBundleHeader = 18;
inline constexpr int kBundleWidth = kColumns * kBundleCell + 2 * kBundleMargin;
inline constexpr int kBundleHeight = kBundleHeader + kRows * kBundleCell + kBundleMargin;

std::string bundleJson();
std::string hoverPanelControlsJson();

}
