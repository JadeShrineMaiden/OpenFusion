#pragma once

#include "Player.hpp"
#include "core/Core.hpp"
#include "servers/CNShardServer.hpp"

#include <map>
#include <list>

namespace Groups {
    void init();

    int32_t getGroupFlags(Player* plr);
    void sendPacketToGroup(CNSocket* sock, void* buf, uint32_t type, size_t size);
    bool addPlayerToGroup(Player* leadPlr, Player* plr);
    bool kickPlayerFromGroup(Player* plr);
    void groupTickInfo(Player* plr);
    void addNPCToGroup(CNSocket* sock, BaseNPC* npc, int taskNum);
    void kickNPCFromGroup(CNSocket* sock, BaseNPC* npc);
}
