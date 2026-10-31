#include "BotHooks.h"
#include "BotCompletion.h"
#include "BotManager.h"
#include "BotNet.h"
#include "ContainerScreen.h"
#include "FriendWorld.h"
#include "GameConnection.h"
#include "GameIdentity.h"
#include "GameKey.h"
#include "GameTerrain.h"
#include "NetherNetLink.h"
#include "PlayerInteract.h"
#include "RakNetLink.h"
#include "core/Logger.h"
#include "hooks/HookManager.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace tsukuyomi::bot {
namespace {
net::ReceiveFn g_networkReceivePacket = nullptr;
net::SendInternalFn g_sendInternal = nullptr;
gameidentity::CtorFn g_identityCtor = nullptr;
gamekey::SecretFn g_computeSecret = nullptr;
gameterrain::EnqueueFn g_terrainEnqueue = nullptr;
gameterrain::ConsumeFn g_terrainConsume = nullptr;
gameterrain::FinishFn g_terrainFinish = nullptr;
playerinteract::GetInteractionFn g_getInteraction = nullptr;
containerscreen::ContainerAtFn g_containerAt = nullptr;
containerscreen::InRangeFn g_containerInRange = nullptr;
containerscreen::WeakRefFn g_containerWeakRef = nullptr;
containerscreen::NameAtFn g_containerNameAt = nullptr;
containerscreen::CustomNameAtFn g_containerCustomNameAt = nullptr;
using GameUpdateFn = void(__fastcall*)(void* game);
GameUpdateFn g_gameUpdate = nullptr;
void __fastcall detourGameUpdate(void* game)
{
    g_gameUpdate(game);
    BotManager::instance().onGameUpdate(game);
}
using U = std::uintptr_t;
using ClientDataBuildFn = U(__fastcall*)(U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U);
ClientDataBuildFn g_clientDataBuild = nullptr;
struct OptionalString { std::string value; bool engaged = true; };
static_assert(sizeof(std::string) == 0x20 && offsetof(OptionalString, engaged) == 0x20);
U __fastcall detourClientDataBuild(U a1, U a2, U a3, U a4, U a5, U a6, U a7, U a8, U a9, U a10, U a11, U a12, U a13, U a14, U a15, U a16,
    U a17, U a18, U a19, U a20, U a21, U a22, U a23, U a24, U a25, U nonce)
{
    OptionalString replaced;
    if (auto armed = net::armedLoginNonce()) {
        replaced.value = std::move(*armed);
        nonce = reinterpret_cast<U>(&replaced);
        static int logged = 0;
        if (logged++ < 8) log().info(L"Bot: the game's login carries the bot's join nonce");
    }
    const auto result = g_clientDataBuild(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, nonce);
    gamekey::noteClientData(reinterpret_cast<const void*>(a3));
    return result;
}
friendworld::FindFn g_friendWorldFind = nullptr;
void __fastcall detourFriendWorldFind(void* finder, const std::string* id, void* callback)
{
    friendworld::onFind(g_friendWorldFind, finder, id, callback);
}
void* __fastcall detourContainerAt(void* player, std::uint8_t type, const void* position)
{
    return containerscreen::onContainerAt(g_containerAt, player, type, position);
}
bool __fastcall detourContainerInRange(void* model, float pickRange)
{
    return containerscreen::onInRange(g_containerInRange, model, pickRange);
}
void* __fastcall detourContainerWeakRef(void* model, void* out)
{
    return containerscreen::onWeakRef(g_containerWeakRef, model, out);
}
void* __fastcall detourContainerNameAt(void* model, void* out, const void* position)
{
    return containerscreen::onNameAt(g_containerNameAt, model, out, position);
}
bool __fastcall detourContainerCustomNameAt(void* controller, const void* position)
{
    return containerscreen::onCustomNameAt(g_containerCustomNameAt, controller, position);
}
void* __fastcall detourGetInteraction(void* self, void* out, void* player, void* position, std::uintptr_t fifth)
{
    return playerinteract::onGetInteraction(g_getInteraction, self, out, player, position, fifth);
}
void __fastcall detourTerrainEnqueue(void* list, void** job) { gameterrain::onEnqueue(g_terrainEnqueue, list, job); }
void __fastcall detourTerrainConsume(void* list) { gameterrain::onConsume(g_terrainConsume, list); }
void* __fastcall detourTerrainFinish(void* task, void* a, void* b, void* c) { return gameterrain::onFinish(g_terrainFinish, task, a, b, c); }
void __fastcall detourSendInternal(void* system, void* id, void* packet, const std::string* data)
{
    net::sendInternal(g_sendInternal, system, id, packet, data);
}
void* __fastcall detourIdentityCtor(void* self, void* key, void* token)
{
    return gameidentity::onConstruct(g_identityCtor, self, key, token);
}
void* __fastcall detourComputeSecret(void* self, void* out, const void* peer)
{
    return gamekey::onComputeSecret(g_computeSecret, self, out, peer);
}
int __fastcall detourNetworkReceivePacket(void* peer, void* out, void* context)
{
    return net::receive(g_networkReceivePacket, peer, out, context);
}
}

void installHooks(HookManager& hooks, bool packetSendHooked)
{
    net::initialize();
    gameconn::initialize();
    nethernet::initialize();
    raknet::initialize();
    const bool receiveReady = hooks.create(net::receiveTarget(), reinterpret_cast<void*>(&detourNetworkReceivePacket),
                                           reinterpret_cast<void**>(&g_networkReceivePacket), L"NetworkReceivePacket");
    net::hooksReady(receiveReady, packetSendHooked);
    if (receiveReady) net::setReceiveOriginal(g_networkReceivePacket);
    if (!net::sendInternalTarget()
        || !hooks.create(net::sendInternalTarget(), reinterpret_cast<void*>(&detourSendInternal),
                         reinterpret_cast<void**>(&g_sendInternal), L"NetworkSendInternal"))
        log().warn(L"Bot: NetworkSendInternal hook is unavailable - switching accounts is unavailable");
    if (gameidentity::initialize()
        && !hooks.create(gameidentity::ctorTarget(), reinterpret_cast<void*>(&detourIdentityCtor),
                         reinterpret_cast<void**>(&g_identityCtor), L"NetherNetIdentity"))
        log().warn(L"Bot: NetherNetIdentity hook is unavailable - switching to a bot on its own connection is unavailable");
    if (gamekey::initialize()
        && !hooks.create(gamekey::secretTarget(), reinterpret_cast<void*>(&detourComputeSecret),
                         reinterpret_cast<void**>(&g_computeSecret), L"GameKeySecret"))
        log().warn(L"Bot: GameKeySecret hook is unavailable - switching to a bot on its own connection is unavailable on RakNet");
    else if (g_computeSecret) gamekey::setOriginal(g_computeSecret);
    if (gameterrain::initialize()) {
        const bool hooked = hooks.create(gameterrain::enqueueTarget(), reinterpret_cast<void*>(&detourTerrainEnqueue), reinterpret_cast<void**>(&g_terrainEnqueue), L"TerrainJobEnqueue")
            && hooks.create(gameterrain::consumeTarget(), reinterpret_cast<void*>(&detourTerrainConsume), reinterpret_cast<void**>(&g_terrainConsume), L"TerrainJobConsume")
            && hooks.create(gameterrain::finishTarget(), reinterpret_cast<void*>(&detourTerrainFinish), reinterpret_cast<void**>(&g_terrainFinish), L"TerrainJobFinish");
        if (!hooked) {
            gameterrain::disable();
            log().warn(L"Bot: the sub-chunk job hooks are unavailable - far bots and bots in other dimensions cannot walk along a path");
        }
    }
    containerscreen::setHooked(containerscreen::initialize()
        && hooks.create(containerscreen::containerAtTarget(), reinterpret_cast<void*>(&detourContainerAt),
                        reinterpret_cast<void**>(&g_containerAt), L"ContainerAtPosition")
        && hooks.create(containerscreen::inRangeTarget(), reinterpret_cast<void*>(&detourContainerInRange),
                        reinterpret_cast<void**>(&g_containerInRange), L"LevelContainerInRange")
        && hooks.create(containerscreen::weakRefTarget(), reinterpret_cast<void*>(&detourContainerWeakRef),
                        reinterpret_cast<void**>(&g_containerWeakRef), L"LevelContainerWeakRef")
        && hooks.create(containerscreen::nameAtTarget(), reinterpret_cast<void*>(&detourContainerNameAt),
                        reinterpret_cast<void**>(&g_containerNameAt), L"ContainerNameAt")
        && hooks.create(containerscreen::customNameAtTarget(), reinterpret_cast<void*>(&detourContainerCustomNameAt),
                        reinterpret_cast<void**>(&g_containerCustomNameAt), L"ContainerCustomNameAt"));
    if (!containerscreen::available())
        log().warn(L"Bot: the chest screen hooks are unavailable - /tk bot inventory and right-clicking players do not open");
    if (!playerinteract::initialize()
        || !hooks.create(playerinteract::target(), reinterpret_cast<void*>(&detourGetInteraction),
                         reinterpret_cast<void**>(&g_getInteraction), L"ActorGetInteraction"))
        log().warn(L"Bot: ActorGetInteraction hook is unavailable - right-clicking players does not show their inventory");
    if (void* const update = net::site(Site::MinecraftGameUpdate);
        !update || !hooks.create(update, reinterpret_cast<void*>(&detourGameUpdate), reinterpret_cast<void**>(&g_gameUpdate), L"GameUpdate"))
        log().warn(L"Bot: GameUpdate hook is unavailable - bots on their own connections stay after you leave until the game closes");
    if (!friendworld::initialize()) log().warn(L"Bot: the game's friend world joiner is unavailable - switching in a friend's world is unavailable");
    else if (void* const find = net::site(Site::FriendWorldFind);
             !find || !hooks.create(find, reinterpret_cast<void*>(&detourFriendWorldFind), reinterpret_cast<void**>(&g_friendWorldFind), L"FriendWorldFind"))
        log().warn(L"Bot: FriendWorldFind hook is unavailable - switching in a friend's world is unavailable");
    else friendworld::setGuarded(true);
    auto* const build = net::site(Site::ClientDataBuild);
    auto* const nonceSite = net::site(Site::ClientDataNonce);
    if (!build || !nonceSite || nonceSite < build || nonceSite - build > 0xc00
        || !hooks.create(build, reinterpret_cast<void*>(&detourClientDataBuild), reinterpret_cast<void**>(&g_clientDataBuild), L"ClientDataBuild"))
        log().warn(L"Bot: ClientDataBuild hook is unavailable - switching to a bot on its own connection is unavailable in a friend's world");
}
void onPacketSend(void*, void* sender) { net::observeSender(sender); }
void addTkCompletion(chatcommand::TkCommandSpec& spec, std::uint32_t firstEnumIndex, std::optional<std::uint32_t> booleanIndex)
{
    addCompletion(spec, firstEnumIndex, completionNames(), booleanIndex);
    noteAccountCompletion();
}
}
