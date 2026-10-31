#include "BotModule.h"
#include "bot/BotCommandSyntax.h"
#include "bot/BotManager.h"
#include "bot/BotNet.h"
#include "game/ChatCommand.h"

namespace tsukuyomi {
Bot& Bot::instance() { static Bot value; return value; }
bool Bot::available() const { return bot::net::available(); }
MenuItem Bot::buildMenu()
{
    MenuItem item = Module::buildMenu();
    item.hidden = true;
    return item;
}
void Bot::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    enabledWasInConfig_ = section.contains("enabled");
}
void Bot::onScansReady()
{
    chatcommand::registerModuleCommand({"bot"}, bot::syntax::kSummary,
        [this](const std::vector<std::string>& args) -> chatcommand::Reply {
            if (!enabled()) return {{"Bot is disabled"}, true};
            if (!available()) return {{"Bot is unavailable"}, true};
            return bot::BotManager::instance().command(args);
        });
    chatcommand::registerModuleCommand({"skin"}, "/tk skin reload|steal <gamertag> [name]",
        [this](const std::vector<std::string>& args) -> chatcommand::Reply {
            if (!enabled()) return {{"Bot is disabled"}, true};
            if (!available()) return {{"Bot is unavailable"}, true};
            return bot::BotManager::instance().skin(args);
        });
    if (available()) {
        if (!enabledWasInConfig_) setEnabled(true);
        bot::BotManager::instance().initialize();
    }
}
void Bot::onPlayerViewUpdate() { bot::BotManager::instance().onPlayerViewUpdate(enabled()); }
void Bot::shutdown() { bot::BotManager::instance().shutdown(); }
}
