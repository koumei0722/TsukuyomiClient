#pragma once

#include <atomic>

namespace tsukuyomi {

class InventoryScreen {
public:
    static InventoryScreen& instance();

    void onController();

    bool screenLooksOpen() const;

private:
    InventoryScreen() = default;

    static constexpr unsigned long long kFreshMs = 500;

    std::atomic<unsigned long long> m_seenAtMs{0};
};

}
