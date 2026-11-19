#pragma once

#include "core/Core.hpp"

#include <map>
#include <list>
#include <sstream>
#include <string>

#define REGISTER_SHARD_PACKET(pactype, handlr) CNShardServer::ShardPackets[pactype] = handlr;
#define REGISTER_SHARD_TIMER(handlr, delta) CNShardServer::Timers.push_back(TimerEvent(handlr, delta));
#define MS_PER_PLAYER_TICK 500
#define MS_PER_COMBAT_TICK 200

class CNShardServer : public CNServer {
private:
    static void handlePacket(CNSocket* sock, CNPacketData* data);

    static void keepAliveTimer(CNServer*, time_t);
    static void periodicSaveTimer(CNServer* serv, time_t currTime);
    static void periodicItemExpireTimer(CNServer* serv, time_t currTime);

    // stepped mode (servers/Stepped.hpp, servers/Stepped.cpp)
    SOCKET controlListener = -1;
    SOCKET controlSock = -1;
    std::string controlBuf;
    bool handleControlSocket(int i);
    std::string handleControlCommand(const std::string& line);
    bool drainClientPackets(uint64_t target);
    void runTimers(time_t currTime);
    std::string stepTicks(int ticks, uint64_t target);
    std::string resetEpisode(std::istringstream& args);
    std::string dumpState();

public:
    static std::map<uint32_t, PacketHandler> ShardPackets;
    static std::list<TimerEvent> Timers;

    CNShardServer(uint16_t p);

    static void _killConnection(CNSocket *cns);

    bool checkExtraSockets(int i);
    void newConnection(CNSocket* cns);
    void killConnection(CNSocket* cns);
    void kill();
    void onStep();
};
