#include "servers/Stepped.hpp"

#include "servers/CNShardServer.hpp"

#include "core/Core.hpp"
#include "Chunking.hpp"
#include "Combat.hpp"
#include "MobAI.hpp"
#include "Nanos.hpp"
#include "NPCManager.hpp"
#include "PlayerManager.hpp"
#include "Rand.hpp"
#include "settings.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <vector>

std::atomic<int64_t> Stepped::virtualTime{Stepped::START_TIME};
std::atomic<uint64_t> Stepped::packetsProcessed{0};

SOCKET Stepped::initListener() {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (SOCKETERROR(listener)) {
        printSocketError("socket");
        std::cerr << "[FATAL] Stepped: failed to create control socket" << std::endl;
        exit(1);
    }

#ifdef _WIN32
    const char opt = 1;
#else
    int opt = 1;
#endif
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(settings::STEPPEDPORT);
    if (!inet_pton(AF_INET, settings::STEPPEDLISTENIP.c_str(), &address.sin_addr)
        || SOCKETERROR(bind(listener, (struct sockaddr*)&address, sizeof(address)))
        || SOCKETERROR(listen(listener, 1))) {
        printSocketError("bind/listen");
        std::cerr << "[FATAL] Stepped: cannot listen on control port " << settings::STEPPEDPORT << std::endl;
        exit(1);
    }

    std::cout << "[INFO] Stepped mode: virtual clock at " << now() << " ms, control channel on "
              << settings::STEPPEDLISTENIP << ":" << settings::STEPPEDPORT << std::endl;
    return listener;
}

static void closeSocket(SOCKET s) {
#ifdef _WIN32
    shutdown(s, SD_BOTH);
    closesocket(s);
#else
    shutdown(s, SHUT_RDWR);
    close(s);
#endif
}

// Packets the server has sent to the one player's socket so far (0 without a player).
static uint64_t playerPacketsSent() {
    if (PlayerManager::players.size() != 1)
        return 0;
    return PlayerManager::players.begin()->first->packetsSent;
}

static bool sendAll(SOCKET s, const std::string& msg) {
    size_t n = 0;
    while (n < msg.size()) {
        int r = send(s, msg.data() + n, (int)(msg.size() - n), 0);
        if (SOCKETERROR(r) || r == 0)
            return false;
        n += r;
    }
    return true;
}

// Returns true if fds[i] is the control listener or the control client (and handles it).
bool CNShardServer::handleControlSocket(int i) {
    SOCKET fd = fds[i].fd;

    if (fd == controlListener) {
        SOCKET s = accept(controlListener, nullptr, nullptr);
        if (SOCKETINVALID(s))
            return true;
        if (controlSock != -1) { // one controller at a time: the newest wins
            removePollFD(controlSock);
            closeSocket(controlSock);
        }
        setSocketOption(s, IPPROTO_TCP, TCP_NODELAY, 1);
        controlSock = s;
        controlBuf.clear();
        fds.push_back({s, POLLIN});
        std::cout << "[INFO] Stepped: control channel connected" << std::endl;
        return true;
    }

    if (fd != controlSock || controlSock == -1)
        return false;

    char buf[4096];
    int n = recv(controlSock, buf, sizeof(buf), 0);
    if (n <= 0) {
        std::cout << "[INFO] Stepped: control channel closed" << std::endl;
        removePollFD(controlSock);
        closeSocket(controlSock);
        controlSock = -1;
        return true;
    }

    controlBuf.append(buf, n);
    size_t pos;
    while ((pos = controlBuf.find('\n')) != std::string::npos) {
        std::string line = controlBuf.substr(0, pos);
        controlBuf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        std::string reply = handleControlCommand(line) + "\n";
        if (!sendAll(controlSock, reply))
            break;
    }
    return true;
}

std::string CNShardServer::handleControlCommand(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;

    if (cmd == "PING")
        return "OK";
    if (cmd == "TIME")
        return "OK " + std::to_string(Stepped::now());
    if (cmd == "COUNT")
        return "OK " + std::to_string(Stepped::packetsProcessed.load()) + " " + std::to_string(playerPacketsSent());
    if (cmd == "SEED") {
        uint64_t seed;
        if (!(in >> seed))
            return "ERR usage: SEED <u64>";
        Rand::init(seed);
        return "OK";
    }
    if (cmd == "STEP") {
        int ticks;
        uint64_t target;
        if (!(in >> ticks >> target) || ticks < 0)
            return "ERR usage: STEP <ticks> <packet count>";
        return stepTicks(ticks, target);
    }
    if (cmd == "RESET")
        return resetEpisode(in);
    if (cmd == "STATE")
        return dumpState();
    return "ERR unknown command: " + cmd;
}

// Read and handle client packets until `target` shard packets have been processed in total.
bool CNShardServer::drainClientPackets(uint64_t target) {
    using namespace std::chrono;
    auto deadline = steady_clock::now() + seconds(10);

    while (Stepped::packetsProcessed.load() < target) {
        if (steady_clock::now() > deadline)
            return false;

        std::vector<PollFD> cfds;
        for (auto& pair : connections)
            cfds.push_back({pair.first, POLLIN});
        if (cfds.empty())
            return false;

        int n = poll(cfds.data(), cfds.size(), 100);
        if (SOCKETERROR(n))
            return false;

        for (auto& p : cfds) {
            if (p.revents == 0)
                continue;
            std::lock_guard<std::mutex> lock(activeCrit);
            auto it = connections.find(p.fd);
            if (it == connections.end())
                continue;
            CNSocket* cSock = it->second;
            if (p.revents & ~POLLIN)
                cSock->kill();
            if (cSock->isAlive())
                cSock->step();
        }
    }
    return true;
}

// Like onStep(), but fires on scheduledEvent <= now so periods are exact on a 100 ms clock.
void CNShardServer::runTimers(time_t currTime) {
    for (TimerEvent& event : Timers) {
        if (event.scheduledEvent == 0) {
            event.scheduledEvent = currTime + event.delta;
            continue;
        }
        if (event.scheduledEvent <= currTime) {
            event.handlr(this, currTime);
            event.scheduledEvent = currTime + event.delta;
        }
    }
}

std::string CNShardServer::stepTicks(int ticks, uint64_t target) {
    if (!drainClientPackets(target))
        return "ERR timed out waiting for client packets (have " + std::to_string(Stepped::packetsProcessed.load())
            + ", want " + std::to_string(target) + ")";

    // marks[0]: packets sent once the client's packets were handled; marks[k]: after Tick k
    std::string marks = " " + std::to_string(playerPacketsSent());
    for (int k = 0; k < ticks; k++) {
        Stepped::virtualTime += Stepped::TICK_MS;
        std::lock_guard<std::mutex> lock(activeCrit);
        runTimers(Stepped::now());
        marks += " " + std::to_string(playerPacketsSent());
    }
    return "OK " + std::to_string(Stepped::now()) + " " + std::to_string(Stepped::packetsProcessed.load())
        + " " + std::to_string(playerPacketsSent()) + marks;
}

std::string CNShardServer::resetEpisode(std::istringstream& in) {
    uint64_t seed;
    int slot, mobType, n;
    if (!(in >> seed >> slot >> mobType >> n))
        return "ERR usage: RESET <seed> <slot> <mobType> <n> <dx1> <dy1> ...";
    if (n < 1 || n > 5)
        return "ERR n must be 1..5 (one group holds a leader and 4 members)";
    if (slot < -1 || slot > 2)
        return "ERR slot must be -1..2";
    std::vector<std::pair<int, int>> offsets(n);
    for (auto& o : offsets)
        if (!(in >> o.first >> o.second))
            return "ERR missing mob offsets";
    if (mobType < 0 || mobType >= (int)NPCManager::NPCData.size())
        return "ERR unknown mob type";
    if (NPCManager::NPCData[mobType]["m_iTeam"] != 2)
        return "ERR type is not a mob";
    if (PlayerManager::players.size() != 1)
        return "ERR need exactly one connected player, have " + std::to_string(PlayerManager::players.size());

    std::lock_guard<std::mutex> lock(activeCrit);
    CNSocket* sock = PlayerManager::players.begin()->first;
    Player* plr = PlayerManager::players.begin()->second;
    time_t now = Stepped::now();

    // Fresh private instance. Alternating between two map numbers means the warp always
    // leaves the previous instance, which is then destroyed with any leftover mobs.
    static int flip = 0;
    flip ^= 1;
    uint64_t instance = ((uint64_t)plr->iID << 32) | (uint64_t)(settings::STEPPEDARENAMAP + flip);
    Chunking::createInstance(instance);
    int ax = settings::STEPPEDARENAX, ay = settings::STEPPEDARENAY, az = settings::STEPPEDARENAZ;
    PlayerManager::sendPlayerTo(sock, ax, ay, az, instance);

    // Player: full HP, full nano stamina, no buffs, no consumables, the starting nano out.
    plr->HP = PC_MAXHEALTH(plr->level);
    plr->batteryW = 0;
    plr->batteryN = 0;
    plr->healCooldown = 0;
    plr->suspicionRating = 0;
    plr->lastShot = 0;
    plr->clearBuffs(true);
    for (int i = 0; i < 3; i++)
        if (plr->equippedNanos[i] != 0)
            plr->Nanos[plr->equippedNanos[i]].iStamina = 150;
    Nanos::summonNano(sock, slot);

    INITSTRUCT(sP_FE2CL_REP_PC_TICK, tick);
    tick.iHP = plr->HP;
    tick.iBatteryN = plr->batteryN;
    for (int i = 0; i < 3; i++)
        tick.aNano[i] = plr->Nanos[plr->equippedNanos[i]];
    sock->sendPacket((void*)&tick, P_FE2CL_REP_PC_TICK, sizeof(sP_FE2CL_REP_PC_TICK));

    // Mobs: summoned (removed on death, never respawn), one aggro group, leader first.
    // Follower offsets are relative to the leader, as the Twin's chase target uses them.
    Mob* leader = nullptr;
    for (int j = 0; j < n; j++) {
        int x = ax + offsets[j].first, y = ay + offsets[j].second;
        BaseNPC* npc = NPCManager::summonNPC(x, y, az, instance, mobType, false);
        Mob* mob = (Mob*)npc;
        if (j == 0) {
            leader = mob;
            mob->groupLeader = mob->id;
        } else {
            leader->groupMember[j - 1] = mob->id;
            mob->groupLeader = leader->id;
            mob->offsetX = x - (ax + offsets[0].first);
            mob->offsetY = y - (ay + offsets[0].second);
        }
        NPCManager::updateNPCPosition(npc->id, x, y, az, instance, 0);
    }

    // Seed and re-phase: timers count from the reset, the player tick before the mob tick.
    Rand::init(seed);
    for (TimerEvent& event : Timers)
        event.scheduledEvent = now + event.delta;
    Combat::resetTickPhase(now);

    return "OK " + std::to_string(now) + " " + std::to_string(playerPacketsSent());
}

static int aiStateCode(AIState s) {
    switch (s) {
    case AIState::ROAMING: return 0;
    case AIState::COMBAT: return 1;
    case AIState::RETREAT: return 2;
    case AIState::DEAD: return 3;
    default: return -1;
    }
}

std::string CNShardServer::dumpState() {
    std::ostringstream out;
    out << "OK t=" << Stepped::now();
    if (PlayerManager::players.size() != 1)
        return out.str() + " players=" + std::to_string(PlayerManager::players.size());

    Player* plr = PlayerManager::players.begin()->second;
    int activeSlot = -1;
    for (int i = 0; i < 3; i++)
        if (plr->activeNano != 0 && plr->equippedNanos[i] == plr->activeNano)
            activeSlot = i;
    out << " hp=" << plr->HP << " x=" << plr->x << " y=" << plr->y << " active=" << activeSlot << " stamina=";
    for (int i = 0; i < 3; i++)
        out << (i ? "," : "") << plr->Nanos[plr->equippedNanos[i]].iStamina;

    // mobs in the player's instance, sorted by id for a stable order
    std::vector<Mob*> mobs;
    for (auto& pair : NPCManager::NPCs)
        if (pair.second->kind == EntityKind::MOB && pair.second->instanceID == plr->instanceID)
            mobs.push_back((Mob*)pair.second);
    std::sort(mobs.begin(), mobs.end(), [](Mob* a, Mob* b) { return a->id > b->id; });

    out << " mobs=";
    for (size_t i = 0; i < mobs.size(); i++) {
        Mob* m = mobs[i];
        out << (i ? ";" : "") << m->id << ":" << aiStateCode(m->state) << ":" << m->x << ":" << m->y << ":" << m->hp
            << ":" << m->nextAttack << ":" << m->nextMovement << ":" << m->skillStyle << ":" << m->hitX << ":" << m->hitY;
    }
    return out.str();
}
