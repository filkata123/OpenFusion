#pragma once

/*
 * Stepped mode for the ff-agent experiments (ff-agent ADR 0005).
 *
 * With [stepped] enabled=true, getTime() returns a virtual clock that only the Control
 * Channel advances. The Control Channel is a line-based text protocol on a separate TCP
 * port (one client at a time). The agent's game connection carries only packets a Real
 * Client would send. Commands, each answered with one line starting "OK" or "ERR":
 *
 *   PING
 *   TIME                    -> OK <virtual ms>
 *   COUNT                   -> OK <shard packets processed since start>
 *   SEED <u64>              reseed the global RNG
 *   STEP <ticks> <count>    first process client packets until <count> shard packets have
 *                           been handled, then for each tick advance the clock 100 ms and
 *                           run the due timers. Packets go out as they are produced.
 *                           -> OK <virtual ms> <count>
 *   RESET <seed> <slot> <mobType> <n> <dx1> <dy1> ... <dxn> <dyn>
 *                           atomic Episode reset for the one connected player: warp to the
 *                           Arena centre in a fresh private instance (the previous one, with
 *                           any leftover mobs, is destroyed), restore HP and nano stamina,
 *                           zero weapon battery and nano potions, clear buffs, summon nano
 *                           <slot>, spawn n summoned mobs as one group at the given offsets
 *                           from the centre (the first is the leader), reseed the RNG and
 *                           re-phase the timers so they count from the reset.
 *                           -> OK <virtual ms>
 *   STATE                   privileged dump of the player and the mobs in its instance,
 *                           for tests and Twin Gap diagnostics only. Never for an acting
 *                           policy (ff-agent ADR 0001).
 *
 * Timers fire when scheduledEvent <= now (the wall-clock server uses <, which with
 * 100 ms steps would stretch a 200 ms timer to 300 ms).
 */

#include "core/Core.hpp"

#include <atomic>
#include <cstdint>

namespace Stepped {
    extern std::atomic<int64_t> virtualTime;
    extern std::atomic<uint64_t> packetsProcessed;

    constexpr int64_t START_TIME = 1000000; // keeps 0 meaning "unset" in mob deadlines
    constexpr int TICK_MS = 100;

    inline time_t now() { return (time_t)virtualTime.load(); }

    SOCKET initListener();
}
