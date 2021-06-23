#include "servers/CNShardServer.hpp"
#include "PlayerManager.hpp"
#include "Groups.hpp"
#include "Nanos.hpp"
#include "Abilities.hpp"
#include "Missions.hpp"
#include "TableData.hpp"
#include "MobAI.hpp"
#include "Transport.hpp"

#include <iostream>
#include <chrono>
#include <algorithm>
#include <thread>

/*
 * NOTE: Variadic response packets that list group members are technically
 * double-variadic, as they have two count members with trailing struct counts,
 * and are thus incompatible with the generic sendPacket() wrapper.
 * That means we still have to (carefully) use validOutVarPacket() in this
 * source file.
 */

using namespace Groups;

static Player* getGroupLeader(CNSocket* sock) {
    Player* plr = PlayerManager::getPlayer(sock);
    return PlayerManager::getPlayerFromID(plr->iIDGroup);
}

void Groups::sendPacketToGroup(CNSocket* sock, void* buf, uint32_t type, size_t size) {
    Player* plr = getGroupLeader(sock);

    if (plr == nullptr) {
        std::cout << "[WARN] Group leader is null\n";
        return;
    }

    for (int i = 0; i < plr->groupCnt; i++) {
        CNSocket* sock = PlayerManager::getSockFromID(plr->groupIDs[i]);

        if (sock == nullptr) {
            std::cout << "[WARN] Group member is null\n";
            continue;
        }

        // player leaving the group, reset them
        if (type == P_FE2CL_PC_GROUP_LEAVE_SUCC) {
            Player* leavingPlr = PlayerManager::getPlayer(sock);
            leavingPlr->iIDGroup = leavingPlr->iID;
        }

        sock->sendPacket(buf, type, size);
    }
}

// Prepares the variable length packets for various group functions
static void craftGroupMemberData(Player* plr, sPCGroupMemberInfo* respdata) {
    for (int i = 0; i < plr->groupCnt; i++) {
        Player* varPlr = PlayerManager::getPlayerFromID(plr->groupIDs[i]);

        if (varPlr == nullptr)
            continue;

        respdata[i].iPC_ID = varPlr->iID;
        respdata[i].iPCUID = varPlr->PCStyle.iPC_UID;
        respdata[i].iNameCheck = varPlr->PCStyle.iNameCheck;
        memcpy(respdata[i].szFirstName, varPlr->PCStyle.szFirstName, sizeof(varPlr->PCStyle.szFirstName));
        memcpy(respdata[i].szLastName, varPlr->PCStyle.szLastName, sizeof(varPlr->PCStyle.szLastName));
        respdata[i].iSpecialState = varPlr->iSpecialState;
        respdata[i].iLv = varPlr->level;
        respdata[i].iHP = varPlr->HP;
        respdata[i].iMaxHP = PC_MAXHEALTH(varPlr->level);
        respdata[i].iX = varPlr->x;
        respdata[i].iY = varPlr->y;
        respdata[i].iZ = varPlr->z;
        if (varPlr->activeNano > 0) {
            respdata[i].bNano = 1;
            respdata[i].Nano = varPlr->Nanos[varPlr->activeNano];
        }
    }
}

static void requestGroup(CNSocket* sock, CNPacketData* data) {
    sP_CL2FE_REQ_PC_GROUP_INVITE* recv = (sP_CL2FE_REQ_PC_GROUP_INVITE*)data->buf;

    Player* plr = PlayerManager::getPlayer(sock);
    Player* otherPlr = PlayerManager::getPlayerFromID(recv->iID_To);
    Player* leadPlr = getGroupLeader(sock);

    if (otherPlr == nullptr || leadPlr == nullptr) {
        std::cout << "[WARN] Group leader is null or requested player has left.\n";
        return;
    }

    // fail if the group is full or the other player is already in a group
    if (leadPlr->groupCnt >= 4 || otherPlr->iIDGroup != otherPlr->iID || otherPlr->groupCnt > 1) {
        INITSTRUCT(sP_FE2CL_PC_GROUP_INVITE_FAIL, resp);
        sock->sendPacket((void*)&resp, P_FE2CL_PC_GROUP_INVITE_FAIL, sizeof(sP_FE2CL_PC_GROUP_INVITE_FAIL));
        return;
    }

    CNSocket* otherSock = PlayerManager::getSockFromID(recv->iID_To);

    INITSTRUCT(sP_FE2CL_PC_GROUP_INVITE, resp);
    resp.iHostID = plr->iID;
    otherSock->sendPacket((void*)&resp, P_FE2CL_PC_GROUP_INVITE, sizeof(sP_FE2CL_PC_GROUP_INVITE));
}

static void refuseGroup(CNSocket* sock, CNPacketData* data) {
    sP_CL2FE_REQ_PC_GROUP_INVITE_REFUSE* recv = (sP_CL2FE_REQ_PC_GROUP_INVITE_REFUSE*)data->buf;

    CNSocket* otherSock = PlayerManager::getSockFromID(recv->iID_From);
    // possible disconnect scenario
    if (otherSock == nullptr) {
        std::cout << "[WARN] Group requester has left\n";
        return;
    }

    Player* plr = PlayerManager::getPlayer(sock);

    INITSTRUCT(sP_FE2CL_PC_GROUP_INVITE_REFUSE, resp);
    resp.iID_To = plr->iID;
    otherSock->sendPacket((void*)&resp, P_FE2CL_PC_GROUP_INVITE_REFUSE, sizeof(sP_FE2CL_PC_GROUP_INVITE_REFUSE));
}

bool Groups::addPlayerToGroup(Player* leadPlr, Player* plr) {
    // fail if the group is full or the other player is already in a group
    if (plr->groupCnt > 1 || plr->iIDGroup != plr->iID || leadPlr->groupCnt >= 4)
        return false;

    plr->iIDGroup = leadPlr->iID;
    leadPlr->groupIDs[leadPlr->groupCnt] = plr->iID;
    leadPlr->groupCnt += 1;

    if (!validOutVarPacket(sizeof(sP_FE2CL_PC_GROUP_JOIN), leadPlr->groupCnt, sizeof(sPCGroupMemberInfo))
        || !validOutVarPacket(sizeof(sP_FE2CL_PC_GROUP_JOIN)+sizeof(sNPCGroupMemberInfo), leadPlr->groupCnt, sizeof(sPCGroupMemberInfo))) {
        // Validating both cases
        std::cout << "[WARN] bad sP_FE2CL_PC_GROUP_JOIN packet size\n";
        return false;
    }

    size_t resplen = sizeof(sP_FE2CL_PC_GROUP_JOIN) + leadPlr->groupCnt * sizeof(sPCGroupMemberInfo);
    uint8_t respbuf[CN_PACKET_BUFFER_SIZE];

    memset(respbuf, 0, resplen+sizeof(sNPCGroupMemberInfo)); // some extra space just incase npcs are in group.

    sP_FE2CL_PC_GROUP_JOIN *resp = (sP_FE2CL_PC_GROUP_JOIN*)respbuf;
    sPCGroupMemberInfo *respdata = (sPCGroupMemberInfo*)(respbuf+sizeof(sP_FE2CL_PC_GROUP_JOIN));
    // I don't know if this is the right thing to do, but ill roll with it.
    sNPCGroupMemberInfo *respdata2 = (sNPCGroupMemberInfo*)(respbuf+sizeof(sP_FE2CL_PC_GROUP_JOIN)
                                     + leadPlr->groupCnt*sizeof(sPCGroupMemberInfo));

    resp->iID_NewMember = plr->iID;
    resp->iMemberPCCnt = leadPlr->groupCnt;

    craftGroupMemberData(leadPlr, respdata);

    for (int i = 0; i < leadPlr->groupCnt; i++) {
        resp->iMemberNPCCnt = 0;
        CNSocket* sock = PlayerManager::getSockFromID(leadPlr->groupIDs[i]);

        if (sock == nullptr) {
            std::cout << "[WARN] Group member is null\n";
            continue;
        }

        Player* varPlr = PlayerManager::getPlayer(sock);

        if (varPlr->groupNPC != 0 && NPCManager::NPCs.find(varPlr->groupNPC) != NPCManager::NPCs.end()) {
            resp->iMemberNPCCnt = 1;
            BaseNPC* npc = NPCManager::NPCs[varPlr->groupNPC];

            respdata2->iNPC_ID = npc->appearanceData.iNPC_ID;
            respdata2->iNPC_Type = npc->appearanceData.iNPCType;
            respdata2->iHP = npc->appearanceData.iHP = NPCManager::NPCData[npc->appearanceData.iNPCType]["m_iHP"];
            respdata2->iMapNum = (int32_t)npc->instanceID;
            respdata2->iX = npc->appearanceData.iX;
            respdata2->iY = npc->appearanceData.iY;
            respdata2->iZ = npc->appearanceData.iZ;
            sock->sendPacket((void*)&respbuf, P_FE2CL_PC_GROUP_JOIN, resplen + sizeof(sNPCGroupMemberInfo));
            continue;
        }

        sock->sendPacket((void*)&respbuf, P_FE2CL_PC_GROUP_JOIN, resplen);
    }

    return true;
}

static void joinGroup(CNSocket* sock, CNPacketData* data) {
    sP_CL2FE_REQ_PC_GROUP_JOIN* recv = (sP_CL2FE_REQ_PC_GROUP_JOIN*)data->buf;

    CNSocket* otherSock = PlayerManager::getSockFromID(recv->iID_From);
    // possible disconnect scenario
    if (otherSock == nullptr) {
        std::cout << "[WARN] Group requester has left\n";
        INITSTRUCT(sP_FE2CL_PC_GROUP_JOIN_FAIL, resp);
        sock->sendPacket((void*)&resp, P_FE2CL_PC_GROUP_JOIN_FAIL, sizeof(sP_FE2CL_PC_GROUP_JOIN_FAIL));
        return;
    }

    Player* plr = PlayerManager::getPlayer(sock);
    Player* leadPlr = getGroupLeader(otherSock);

    if (leadPlr == nullptr) {
        std::cout << "[WARN] Group leader is null\n";
        INITSTRUCT(sP_FE2CL_PC_GROUP_JOIN_FAIL, resp);
        sock->sendPacket((void*)&resp, P_FE2CL_PC_GROUP_JOIN_FAIL, sizeof(sP_FE2CL_PC_GROUP_JOIN_FAIL));
        return;
    }

    if (!addPlayerToGroup(leadPlr, plr)) {
        INITSTRUCT(sP_FE2CL_PC_GROUP_JOIN_FAIL, resp);
        sock->sendPacket((void*)&resp, P_FE2CL_PC_GROUP_JOIN_FAIL, sizeof(sP_FE2CL_PC_GROUP_JOIN_FAIL));
        return;
    }
}

bool Groups::kickPlayerFromGroup(Player* plr) {
    CNSocket* kickedSock = PlayerManager::getSockFromID(plr->iID);
    // if you are the group leader, destroy your own group and kick everybody
    if (plr->iID == plr->iIDGroup) {
        INITSTRUCT(sP_FE2CL_PC_GROUP_LEAVE_SUCC, reply);
        sendPacketToGroup(kickedSock, (void*)&reply, P_FE2CL_PC_GROUP_LEAVE_SUCC, sizeof(sP_FE2CL_PC_GROUP_LEAVE_SUCC));
        plr->groupCnt = 1;
        return true;
    }

    Player* leadPlr = PlayerManager::getPlayerFromID(plr->iIDGroup);

    // rearrange the group
    int subtract = 0;
    for (int i = 0; i < leadPlr->groupCnt; i++) {
        if (leadPlr->groupIDs[i] == plr->iID)
            subtract = 1;
        else
            leadPlr->groupIDs[i - subtract] = leadPlr->groupIDs[i];
    }

    // deal with the leaving player
    plr->iIDGroup = plr->iID;
    INITSTRUCT(sP_FE2CL_PC_GROUP_LEAVE_SUCC, reply2);
    kickedSock->sendPacket((void*)&reply2, P_FE2CL_PC_GROUP_LEAVE_SUCC, sizeof(sP_FE2CL_PC_GROUP_LEAVE_SUCC));

    // now deal with the rest
    leadPlr->groupCnt -= 1;

    if (!validOutVarPacket(sizeof(sP_FE2CL_PC_GROUP_LEAVE), leadPlr->groupCnt, sizeof(sPCGroupMemberInfo))
        || !validOutVarPacket(sizeof(sP_FE2CL_PC_GROUP_LEAVE)+sizeof(sNPCGroupMemberInfo), leadPlr->groupCnt, sizeof(sPCGroupMemberInfo))) {
        // Validating both cases
        std::cout << "[WARN] bad sP_FE2CL_PC_GROUP_LEAVE packet size\n";
        return false;
    }

    size_t resplen = sizeof(sP_FE2CL_PC_GROUP_LEAVE) + leadPlr->groupCnt * sizeof(sPCGroupMemberInfo);
    uint8_t respbuf[CN_PACKET_BUFFER_SIZE];

    memset(respbuf, 0, resplen+sizeof(sNPCGroupMemberInfo)); // some extra space just incase npcs are in group.

    sP_FE2CL_PC_GROUP_LEAVE *resp = (sP_FE2CL_PC_GROUP_LEAVE*)respbuf;
    sPCGroupMemberInfo *respdata = (sPCGroupMemberInfo*)(respbuf+sizeof(sP_FE2CL_PC_GROUP_LEAVE));
    // I don't know if this is the right thing to do, but ill roll with it.
    sNPCGroupMemberInfo *respdata2 = (sNPCGroupMemberInfo*)(respbuf+sizeof(sP_FE2CL_PC_GROUP_LEAVE)
                                     + leadPlr->groupCnt*sizeof(sPCGroupMemberInfo));

    resp->iID_LeaveMember = plr->iID;
    resp->iMemberPCCnt = leadPlr->groupCnt;

    craftGroupMemberData(leadPlr, respdata);

    for (int i = 0; i < leadPlr->groupCnt; i++) {
        resp->iMemberNPCCnt = 0;
        CNSocket* sock = PlayerManager::getSockFromID(leadPlr->groupIDs[i]);

        if (sock == nullptr) {
            std::cout << "[WARN] Group member is null\n";
            continue;
        }

        Player* varPlr = PlayerManager::getPlayer(sock);

        if (varPlr->groupNPC != 0 && NPCManager::NPCs.find(varPlr->groupNPC) != NPCManager::NPCs.end()) {
            resp->iMemberNPCCnt = 1;
            BaseNPC* npc = NPCManager::NPCs[varPlr->groupNPC];

            respdata2->iNPC_ID = npc->appearanceData.iNPC_ID;
            respdata2->iNPC_Type = npc->appearanceData.iNPCType;
            respdata2->iHP = npc->appearanceData.iHP = NPCManager::NPCData[npc->appearanceData.iNPCType]["m_iHP"];
            respdata2->iMapNum = (int32_t)npc->instanceID;
            respdata2->iX = npc->appearanceData.iX;
            respdata2->iY = npc->appearanceData.iY;
            respdata2->iZ = npc->appearanceData.iZ;
            sock->sendPacket((void*)&respbuf, P_FE2CL_PC_GROUP_LEAVE, resplen + sizeof(sNPCGroupMemberInfo));
            continue;
        }

        sock->sendPacket((void*)&respbuf, P_FE2CL_PC_GROUP_LEAVE, resplen);
    }

    return true;
}

static void leaveGroup(CNSocket* sock, CNPacketData* data) {
    Player* plr = PlayerManager::getPlayer(sock);
    kickPlayerFromGroup(plr);
}

void Groups::groupTickInfo(Player* plr) {
    if (!validOutVarPacket(sizeof(sP_FE2CL_PC_GROUP_MEMBER_INFO), plr->groupCnt, sizeof(sPCGroupMemberInfo))
        || !validOutVarPacket(sizeof(sP_FE2CL_PC_GROUP_MEMBER_INFO)+sizeof(sNPCGroupMemberInfo), plr->groupCnt, sizeof(sPCGroupMemberInfo))) {
        // Validating both cases
        std::cout << "[WARN] bad sP_FE2CL_PC_GROUP_MEMBER_INFO packet size\n";
        return;
    }

    size_t resplen = sizeof(sP_FE2CL_PC_GROUP_MEMBER_INFO) + plr->groupCnt * sizeof(sPCGroupMemberInfo);
    uint8_t respbuf[CN_PACKET_BUFFER_SIZE];

    memset(respbuf, 0, resplen+sizeof(sNPCGroupMemberInfo)); // some extra space just incase npcs are in group.

    sP_FE2CL_PC_GROUP_MEMBER_INFO *resp = (sP_FE2CL_PC_GROUP_MEMBER_INFO*)respbuf;
    sPCGroupMemberInfo *respdata = (sPCGroupMemberInfo*)(respbuf+sizeof(sP_FE2CL_PC_GROUP_MEMBER_INFO));
    // I don't know if this is the right thing to do, but ill roll with it.
    sNPCGroupMemberInfo *respdata2 = (sNPCGroupMemberInfo*)(respbuf+sizeof(sP_FE2CL_PC_GROUP_MEMBER_INFO)
                                     + plr->groupCnt*sizeof(sPCGroupMemberInfo));

    resp->iID = plr->iID;
    resp->iMemberPCCnt = plr->groupCnt;

    craftGroupMemberData(plr, respdata);

    for (int i = 0; i < plr->groupCnt; i++) {
        resp->iMemberNPCCnt = 0;
        CNSocket* sock = PlayerManager::getSockFromID(plr->groupIDs[i]);

        if (sock == nullptr) {
            std::cout << "[WARN] Group member is null\n";
            continue;
        }

        Player* varPlr = PlayerManager::getPlayer(sock);

        if (varPlr->groupNPC != 0 && NPCManager::NPCs.find(varPlr->groupNPC) != NPCManager::NPCs.end()) {
            resp->iMemberNPCCnt = 1;
            BaseNPC* npc = NPCManager::NPCs[varPlr->groupNPC];

            respdata2->iNPC_ID = npc->appearanceData.iNPC_ID;
            respdata2->iNPC_Type = npc->appearanceData.iNPCType;
            respdata2->iHP = npc->appearanceData.iHP = NPCManager::NPCData[npc->appearanceData.iNPCType]["m_iHP"];
            respdata2->iMapNum = (int32_t)npc->instanceID;
            respdata2->iX = npc->appearanceData.iX;
            respdata2->iY = npc->appearanceData.iY;
            respdata2->iZ = npc->appearanceData.iZ;
            sock->sendPacket((void*)&respbuf, P_FE2CL_PC_GROUP_MEMBER_INFO, resplen + sizeof(sNPCGroupMemberInfo));
            continue;
        }

        sock->sendPacket((void*)&respbuf, P_FE2CL_PC_GROUP_MEMBER_INFO, resplen);
    }
}

void Groups::addNPCToGroup(CNSocket* sock, BaseNPC* npc, int taskNum) {
    Player* plr = PlayerManager::getPlayer(sock);
    Player* leadPlr = PlayerManager::getPlayerFromID(plr->iIDGroup);

    if (leadPlr == nullptr) {
        std::cout << "[WARN] Group leader is null\n";
        return;
    }

    if (!validOutVarPacket(sizeof(sP_FE2CL_REP_NPC_GROUP_INVITE_SUCC)+sizeof(sNPCGroupMemberInfo), leadPlr->groupCnt, sizeof(sPCGroupMemberInfo))) {
        std::cout << "[WARN] bad sP_FE2CL_REP_NPC_GROUP_INVITE_SUCC packet size\n";
        return;
    }

    size_t resplen = sizeof(sP_FE2CL_REP_NPC_GROUP_INVITE_SUCC) + leadPlr->groupCnt * sizeof(sPCGroupMemberInfo) + sizeof(sNPCGroupMemberInfo);
    uint8_t respbuf[CN_PACKET_BUFFER_SIZE];

    memset(respbuf, 0, resplen);

    sP_FE2CL_REP_NPC_GROUP_INVITE_SUCC *resp = (sP_FE2CL_REP_NPC_GROUP_INVITE_SUCC*)respbuf;
    sPCGroupMemberInfo *respdata = (sPCGroupMemberInfo*)(respbuf+sizeof(sP_FE2CL_REP_NPC_GROUP_INVITE_SUCC));
    // I don't know if this is the right thing to do, but ill roll with it.
    sNPCGroupMemberInfo *respdata2 = (sNPCGroupMemberInfo*)(respbuf+sizeof(sP_FE2CL_REP_NPC_GROUP_INVITE_SUCC)
                                     + leadPlr->groupCnt*sizeof(sPCGroupMemberInfo));

    resp->iPC_ID = plr->iID;
    resp->iNPC_ID = npc->appearanceData.iNPC_ID;
    resp->iMemberPCCnt = leadPlr->groupCnt;
    resp->iMemberNPCCnt = 1; // believe the client is only designed to handle one

    craftGroupMemberData(leadPlr, respdata);

    respdata2->iNPC_ID = npc->appearanceData.iNPC_ID;
    respdata2->iNPC_Type = npc->appearanceData.iNPCType;
    respdata2->iHP = npc->appearanceData.iHP = NPCManager::NPCData[npc->appearanceData.iNPCType]["m_iHP"];
    respdata2->iMapNum = (int32_t)npc->instanceID;
    respdata2->iX = npc->appearanceData.iX;
    respdata2->iY = npc->appearanceData.iY;
    respdata2->iZ = npc->appearanceData.iZ;

    sock->sendPacket((void*)&respbuf, P_FE2CL_REP_NPC_GROUP_INVITE_SUCC, resplen);

    plr->groupNPC = npc->appearanceData.iNPC_ID;

    // make the npc follow its escort path if applicable
    TaskData& task = *Missions::Tasks[taskNum];
    NPCPath* path = Transport::findApplicablePath(MAPNUM(npc->appearanceData.iNPC_ID), npc->appearanceData.iNPCType, taskNum);
    if (task["m_iCSUDEPNPCFollow"] == 0 && path != nullptr) {
        Transport::constructPathNPC(npc->appearanceData.iNPC_ID, path);
        return;
    }

    // all other cases, the npc just follows the player
    // remove all other players that the npc may be following
    for (auto& pair : PlayerManager::players) {
        if (pair.second->followerNPC == npc->appearanceData.iNPC_ID)
            pair.second->followerNPC = 0;
    }

    plr->followerNPC = npc->appearanceData.iNPC_ID;
}

static void requestGroupNPC(CNSocket* sock, CNPacketData* data) {
    Player* plr = PlayerManager::getPlayer(sock);
    sP_CL2FE_REQ_NPC_GROUP_INVITE* recv = (sP_CL2FE_REQ_NPC_GROUP_INVITE*)data->buf;

    if (NPCManager::NPCs.find(recv->iNPC_ID) == NPCManager::NPCs.end())
        return; // TODO: Use fail packet here
    BaseNPC* npc = NPCManager::NPCs[recv->iNPC_ID];

    int i;
    for (i = 0; i < ACTIVE_MISSION_COUNT; i++) {
        if (plr->tasks[i] == 0)
            continue;

        TaskData& task = *Missions::Tasks[plr->tasks[i]];
        if (task["m_iCSUDEFNPCID"] == npc->appearanceData.iNPCType) // make sure the player has a mission with the npc requested
            break;
    }

    if (i == ACTIVE_MISSION_COUNT) {
        std::cout << "[WARN] Player requested the wrong escort NPC\n";
        return;
    }

    addNPCToGroup(sock, npc, plr->tasks[i]);
}

void Groups::kickNPCFromGroup(CNSocket* sock, BaseNPC* npc) {
    Player* plr = PlayerManager::getPlayer(sock);

    if (plr->groupNPC != npc->appearanceData.iNPC_ID) {
        std::cout << "[WARN] Player tried ungrouping an upgrouped npc\n";
        return;
    }

    Player* leadPlr = PlayerManager::getPlayerFromID(plr->iIDGroup);

    if (leadPlr == nullptr) {
        std::cout << "[WARN] Group leader is null\n";
        return;
    }

    if (!validOutVarPacket(sizeof(sP_FE2CL_REP_NPC_GROUP_KICK_SUCC), leadPlr->groupCnt, sizeof(sPCGroupMemberInfo))) {
        std::cout << "[WARN] bad sP_FE2CL_REP_NPC_GROUP_KICK_SUCC packet size\n";
        return;
    }

    size_t resplen = sizeof(sP_FE2CL_REP_NPC_GROUP_KICK_SUCC) + leadPlr->groupCnt * sizeof(sPCGroupMemberInfo);
    uint8_t respbuf[CN_PACKET_BUFFER_SIZE];
    memset(respbuf, 0, resplen);
    sP_FE2CL_REP_NPC_GROUP_KICK_SUCC *resp = (sP_FE2CL_REP_NPC_GROUP_KICK_SUCC*)respbuf;
    sPCGroupMemberInfo *respdata = (sPCGroupMemberInfo*)(respbuf+sizeof(sP_FE2CL_REP_NPC_GROUP_KICK_SUCC));

    resp->iPC_ID = plr->iID;
    resp->iNPC_ID = npc->appearanceData.iNPC_ID;
    resp->iMemberPCCnt = leadPlr->groupCnt;
    resp->iMemberNPCCnt = 0;

    craftGroupMemberData(leadPlr, respdata);

    sock->sendPacket((void*)&respbuf, P_FE2CL_REP_NPC_GROUP_INVITE_SUCC, resplen);

    Transport::NPCQueues.erase(npc->appearanceData.iNPC_ID); // erase existing points
    plr->followerNPC = 0;
    plr->groupNPC = 0;
}

static void leaveGroupNPC(CNSocket* sock, CNPacketData* data) {
    sP_CL2FE_REQ_NPC_GROUP_KICK* recv = (sP_CL2FE_REQ_NPC_GROUP_KICK*)data->buf;

    if (NPCManager::NPCs.find(recv->iNPC_ID) == NPCManager::NPCs.end())
        return; // TODO: Use fail packet here

    BaseNPC* npc = NPCManager::NPCs[recv->iNPC_ID];
    kickNPCFromGroup(sock, npc);
}

void Groups::init() {
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_PC_GROUP_INVITE, requestGroup);
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_PC_GROUP_INVITE_REFUSE, refuseGroup);
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_PC_GROUP_JOIN, joinGroup);
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_PC_GROUP_LEAVE, leaveGroup);
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_NPC_GROUP_INVITE, requestGroupNPC);
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_NPC_GROUP_KICK, leaveGroupNPC);
}
