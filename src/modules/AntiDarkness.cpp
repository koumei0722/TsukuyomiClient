#include "modules/AntiDarkness.h"

#include "core/Logger.h"
#include "memory/Scanner.h"

namespace tsukuyomi {

AntiDarkness& AntiDarkness::instance()
{
    static AntiDarkness module;
    return module;
}

bool AntiDarkness::available() const
{
    return Scanner::instance().found(Target::AntiDarkness);
}

void AntiDarkness::onScansReady()
{
    std::byte* const address = Scanner::instance().address(Target::AntiDarkness);
    if (address == nullptr) {
        if (enabled()) {
            log().warn(L"AntiDarkness: patch site not found, disabling");
            setEnabled(false);
        }
        return;
    }

    m_patch = makeNopPatch(address + kPatchOffset, kPatchSize);

    if (enabled() && !m_patch.apply()) {
        log().error(L"AntiDarkness: failed to apply the patch");
        setEnabled(false);
    }
}

void AntiDarkness::onEnabledChanged(bool enabled)
{
    if (!m_patch.valid()) {
        return;
    }

    if (!m_patch.setEnabled(enabled)) {
        log().error(L"AntiDarkness: failed to {} the patch", enabled ? L"apply" : L"restore");
    }
}

void AntiDarkness::shutdown()
{

    m_patch.restore();
}

}
