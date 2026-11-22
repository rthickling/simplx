/**
 * @file testisolatedengine.cpp
 * @brief Linux integration checks for actor worker placement and lifecycle.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "isolated/options.h"
#include "trz/engine/engine.h"
#include "trz/engine/internal/node.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sched.h>
#include <unistd.h>

namespace
{
using isolated_test::Config;
using tredzone::Actor;
using tredzone::Engine;
using CpuIds = std::array<unsigned, 3>;
using Clock  = std::chrono::steady_clock;
using namespace std::chrono_literals;

void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

cpu_set_t affinity()
{
    cpu_set_t mask{};
    require(sched_getaffinity(0, sizeof(mask), &mask) == 0, "cannot read the current Linux affinity");
    return mask;
}

cpu_set_t singleCpu(unsigned cpu)
{
    cpu_set_t mask{};
    CPU_SET(cpu, &mask);
    return mask;
}

void setAffinity(const cpu_set_t &mask)
{
    require(sched_setaffinity(0, sizeof(mask), &mask) == 0, "cannot set the current Linux affinity");
}

std::set<unsigned> readCpuList(const std::string &file)
{
    const std::string  path = "/sys/devices/system/cpu/" + file;
    std::ifstream      input(path);
    std::set<unsigned> result;
    for (unsigned first; input >> first;)
    {
        unsigned last = first;
        if (input.peek() == '-')
        {
            input.get();
            require(bool(input >> last), "invalid CPU range in " + path);
        }
        require(first <= last, "invalid CPU range in " + path);
        for (unsigned cpu = first;; ++cpu)
        {
            result.insert(cpu);
            if (cpu == last)
            {
                break;
            }
        }
        if (input.peek() != ',')
        {
            require(input.peek() == '\n' || input.eof(), "invalid CPU separator in " + path);
            break;
        }
        input.get();
    }
    require(!result.empty(), "cannot read CPU IDs from " + path);
    return result;
}

void expectCoreSet(const Engine::CoreSet &cores, std::span<const unsigned> expected)
{
    require(cores.size() == expected.size(), "CoreSet size differs from the expected mask");
    for (size_t index = 0; index < expected.size(); ++index)
    {
        const auto node = static_cast<Engine::CoreSet::NodeId>(index);
        const auto cpu  = static_cast<Engine::CoreId>(expected[index]);
        require(cores.at(node) == cpu && cores.index(cpu) == node,
                "CoreSet CPU and node index mismatch for Linux CPU " + std::to_string(expected[index]));
    }
}

void checkDiscoveredSet(const std::set<unsigned> &online)
{
    const auto            allowed = affinity();
    std::vector<unsigned> expected;
    for (unsigned cpu : online)
    {
        if (cpu < CPU_SETSIZE && CPU_ISSET(cpu, &allowed))
        {
            require(cpu <= std::numeric_limits<Engine::CoreId>::max(),
                    "select a launch affinity with Linux CPU IDs representable by Simplx CoreId");
            expected.push_back(cpu);
        }
    }
    require(expected.size() <= Actor::MAX_NODE_COUNT, "select a launch affinity within Simplx's node limit");
    expectCoreSet(Engine::FullCoreSet(), expected);
}

void testDiscovery(const CpuIds &cpus, const std::set<unsigned> &online, const cpu_set_t &original)
{
    checkDiscoveredSet(online);
    auto selected = singleCpu(cpus[1]);
    setAffinity(selected);
    checkDiscoveredSet(online);
    CPU_SET(cpus[0], &selected);
    setAffinity(selected);
    checkDiscoveredSet(online);
    CPU_SET(cpus[2], &selected);
    setAffinity(selected);
    checkDiscoveredSet(online);
    setAffinity(original);
    std::cout << "PASS sparse CPU discovery for launch, singleton, pair, and three-worker masks\n";
}

std::set<pid_t> threadIds()
{
    std::set<pid_t> threads;
    for (const auto &task : std::filesystem::directory_iterator("/proc/self/task"))
    {
        threads.insert(static_cast<pid_t>(std::stoi(task.path().filename().string())));
    }
    return threads;
}

void checkCleanup(const cpu_set_t &original, const std::set<pid_t> &originalThreads)
{
    const auto current = affinity();
    require(CPU_EQUAL(&current, &original), "engine did not restore launcher affinity");

    const auto deadline = Clock::now() + 2s;
    while (threadIds() != originalThreads)
    {
        require(Clock::now() < deadline, "an unexpected thread survived engine teardown");
        std::this_thread::sleep_for(1ms);
    }
}

struct Observation
{
    pid_t         tid{};
    int           cpu{-1};
    int           policy{-1};
    int           priority{-1};
    bool          haveAffinity{};
    cpu_set_t     affinity{};
    Actor::CoreId logicalCpu{};
    size_t        node{};
};

struct Placement
{
    Observation       constructed;
    Observation       dispatched;
    std::atomic<bool> dispatchedReady{false};
};

class PlacementActor : public Actor, public Actor::Callback
{
public:
    explicit PlacementActor(Placement *placement) : placement(*placement)
    {
        this->placement.constructed = observe();
        registerCallback(*this);
    }

    void onCallback() noexcept
    {
        placement.dispatched = observe();
        placement.dispatchedReady.store(true, std::memory_order_release);
    }

private:
    Placement &placement;

    Observation observe() const noexcept
    {
        Observation result;
        result.tid    = gettid();
        result.cpu    = sched_getcpu();
        result.policy = sched_getscheduler(0);
        sched_param parameter{};
        if (sched_getparam(0, &parameter) == 0)
        {
            result.priority = parameter.sched_priority;
        }
        result.haveAffinity = sched_getaffinity(0, sizeof(result.affinity), &result.affinity) == 0;
        result.logicalCpu   = getCore();
        result.node         = getActorId().getCoreIndex();
        return result;
    }
};

Engine::CoreSet makeCoreSet(const CpuIds &cpus)
{
    Engine::CoreSet selected;
    for (unsigned cpu : cpus)
    {
        selected.set(static_cast<Engine::CoreId>(cpu));
    }
    return selected;
}

void configureFifo(Engine::StartSequence &start, const CpuIds &cpus, int priority)
{
    if (priority > 0)
    {
        tredzone::ThreadRealTimeParam parameters;
        parameters.sched_priority = priority;
        start.setRedZoneParam(parameters);
        start.setRedZoneCore(static_cast<Engine::CoreId>(cpus[0]));
        start.setRedZoneCore(static_cast<Engine::CoreId>(cpus[1]));
    }
}

void verifyObservation(const Observation &observation, unsigned cpu, size_t node, int policy, int priority)
{
    const auto expectedMask = singleCpu(cpu);
    const auto expectedCpu  = static_cast<int>(cpu);
    const auto task         = "Linux CPU " + std::to_string(cpu) + " TID " + std::to_string(observation.tid);
    require(observation.haveAffinity && CPU_EQUAL(&observation.affinity, &expectedMask),
            "incorrect singleton affinity on " + task);
    require(observation.cpu == expectedCpu && observation.logicalCpu == cpu && observation.node == node,
            "incorrect OS CPU, actor CPU or node mapping on " + task);
    require(observation.policy == policy && observation.priority == priority,
            "incorrect scheduler on " + task + " (observed policy " + std::to_string(observation.policy) +
                ", priority " + std::to_string(observation.priority) + ")");
}

void runPlacement(const CpuIds &cpus, int fifoPriority, const cpu_set_t &original,
                  const std::set<pid_t> &originalThreads)
{
    const auto               controller = gettid();
    std::array<Placement, 3> placements;
    Engine::StartSequence    start(makeCoreSet(cpus));
    configureFifo(start, cpus, fifoPriority);
    for (size_t i = 0; i < cpus.size(); ++i)
    {
        start.addActor<PlacementActor>(static_cast<Engine::CoreId>(cpus[i]), &placements[i]);
    }
    const auto controllerMask = singleCpu(cpus[2]);
    {
        Engine     engine(start);
        const auto restored = affinity();
        require(CPU_EQUAL(&restored, &original), "engine startup did not restore launcher affinity");
        setAffinity(controllerMask);
        expectCoreSet(engine.getCoreSet(), cpus);
        auto       expectedThreads = originalThreads;
        const auto deadline        = Clock::now() + 5s;
        for (size_t i = 0; i < cpus.size(); ++i)
        {
            const auto &placement = placements[i];
            require(placement.constructed.tid == controller, "starter actor constructor used an unexpected thread");
            verifyObservation(placement.constructed, cpus[i], i, SCHED_OTHER, 0);
            while (!placement.dispatchedReady.load(std::memory_order_acquire))
            {
                require(Clock::now() < deadline, "actor callback timed out on CPU " + std::to_string(cpus[i]));
                std::this_thread::sleep_for(1ms);
            }
            const auto &worker = placement.dispatched;
            require(expectedThreads.insert(worker.tid).second, "actor callbacks shared a launcher or worker TID");
            const int priority = i < 2 ? fifoPriority : 0;
            const int policy   = priority == 0 ? SCHED_OTHER : SCHED_FIFO;
            verifyObservation(worker, cpus[i], i, policy, priority);
            std::cout << "worker cpu=" << cpus[i] << " node=" << i << " tid=" << worker.tid
                      << " policy=" << (policy == SCHED_FIFO ? "SCHED_FIFO" : "SCHED_OTHER") << " priority=" << priority
                      << '\n';
        }
        require(threadIds() == expectedThreads, "unexpected worker thread while the engine was active");
    }
    const auto afterShutdown = affinity();
    require(CPU_EQUAL(&afterShutdown, &controllerMask), "engine shutdown changed controller affinity");
    setAffinity(original);
    checkCleanup(original, originalThreads);
}

struct ConstructorFailure : std::runtime_error
{
    ConstructorFailure() : std::runtime_error("intentional starter actor failure") {}
};

class FailingActor : public Actor
{
public:
    FailingActor() { throw ConstructorFailure(); }
};

void testPartialStartup(const CpuIds &cpus, int priority, const cpu_set_t &original,
                        const std::set<pid_t> &originalThreads)
{
    Placement             prepared;
    Engine::StartSequence start(makeCoreSet(cpus));
    configureFifo(start, cpus, priority);
    start.addActor<PlacementActor>(static_cast<Engine::CoreId>(cpus[0]), &prepared);
    start.addActor<FailingActor>(static_cast<Engine::CoreId>(cpus[1]));
    try
    {
        Engine engine(start);
        throw std::runtime_error("throwing actor constructor allowed startup");
    }
    catch (const ConstructorFailure &)
    {
        require(prepared.constructed.tid == gettid(), "startup did not reach the preceding starter actor");
    }
    checkCleanup(original, originalThreads);
    std::cout << "PASS partial startup with throwing actor constructor\n";
}

void testRejectedAffinity(unsigned rejectedCpu, unsigned validCpu, const cpu_set_t &original,
                          const std::set<pid_t> &originalThreads)
{
    const auto rejected = singleCpu(rejectedCpu);
    errno               = 0;
    if (sched_setaffinity(0, sizeof(rejected), &rejected) == 0)
    {
        setAffinity(original);
        throw std::runtime_error("Linux accepted affinity to negative-test CPU " + std::to_string(rejectedCpu) +
                                 "; a taskset mask alone does not prohibit explicit pinning");
    }
    require(errno == EINVAL, "unexpected errno while verifying negative CPU affinity precondition");
    Engine::CoreSet selected;
    selected.set(static_cast<Engine::CoreId>(validCpu));
    selected.set(static_cast<Engine::CoreId>(rejectedCpu));
    Engine::StartSequence start(selected);
    start.addActor<Actor>(static_cast<Engine::CoreId>(validCpu));
    try
    {
        Engine engine(start);
        throw std::runtime_error("engine accepted CPU " + std::to_string(rejectedCpu) + " rejected by Linux");
    }
    catch (const tredzone::RunTimeException &)
    {
        checkCleanup(original, originalThreads);
        std::cout << "PASS affinity failure for CPU " << rejectedCpu << '\n';
    }
}

void testDeniedFifo(const CpuIds &cpus, int priority, const cpu_set_t &original, const std::set<pid_t> &originalThreads)
{
    sched_param request{};
    request.sched_priority = priority;
    errno                  = 0;
    if (sched_setscheduler(0, SCHED_FIFO, &request) == 0)
    {
        sched_param ordinary{};
        require(sched_setscheduler(0, SCHED_OTHER, &ordinary) == 0, "cannot restore launcher scheduling policy");
        throw std::runtime_error("Linux permitted FIFO in the denial test; remove CAP_SYS_NICE and RT limits");
    }
    require(errno == EPERM, "Linux did not return EPERM for the denied FIFO precondition");

    Engine::StartSequence start(makeCoreSet(cpus));
    configureFifo(start, cpus, priority);
    start.addActor<Actor>(static_cast<Engine::CoreId>(cpus[2]));
    start.addActor<Actor>(static_cast<Engine::CoreId>(cpus[0]));
    try
    {
        Engine engine(start);
        throw std::runtime_error("engine started despite denied FIFO policy");
    }
    catch (const tredzone::AsyncNode::Thread::Exception &)
    {
        checkCleanup(original, originalThreads);
        std::cout << "PASS FIFO privilege denial\n";
    }
}

void run(const Config &config)
{
    const CpuIds cpus{config.cpuA, config.cpuB, config.housekeeping};
    const auto   original        = affinity();
    const auto   online          = readCpuList("online");
    const auto   originalThreads = threadIds();
    const auto   representable   = [](unsigned cpu) { return cpu <= std::numeric_limits<Engine::CoreId>::max(); };
    require(sched_getscheduler(0) == SCHED_OTHER, "launch the controller with ordinary SCHED_OTHER policy");
    for (auto cpu : cpus)
    {
        require(representable(cpu) && online.contains(cpu) && CPU_ISSET(cpu, &original),
                "select representable online CPUs in the launch affinity");
    }
    require(cpus[0] != cpus[1] && cpus[0] != cpus[2] && cpus[1] != cpus[2], "the three worker CPUs must be distinct");

    const int priority = config.fifoPriority;
    require(priority == 0 ||
                (priority >= sched_get_priority_min(SCHED_FIFO) && priority <= sched_get_priority_max(SCHED_FIFO)),
            "FIFO priority is out of Linux scheduling range");
    testDiscovery(cpus, online, original);
    if (config.expectFifoDenied)
    {
        require(priority > 0, "FIFO denial requires a positive FIFO priority");
        testDeniedFifo(cpus, priority, original, originalThreads);
        runPlacement(cpus, 0, original, originalThreads);
        return;
    }

    const auto present = readCpuList("present");
    auto       offline = config.offlineCpu;
    if (!offline)
    {
        const auto found =
            std::ranges::find_if(present, [&](unsigned cpu) { return representable(cpu) && !online.contains(cpu); });
        if (found != present.end())
        {
            offline = *found;
        }
    }
    if (offline)
    {
        require(representable(*offline) && present.contains(*offline) && !online.contains(*offline),
                "offline test CPU must be representable, present, and offline");
        testRejectedAffinity(*offline, cpus[2], original, originalThreads);
    }
    else
    {
        std::cout << "SKIP offline affinity rejection (no representable present offline CPU)\n";
    }
    if (config.deniedCpu)
    {
        require(representable(*config.deniedCpu) && online.contains(*config.deniedCpu),
                "denied test CPU must be representable and online");
        testRejectedAffinity(*config.deniedCpu, cpus[2], original, originalThreads);
    }

    runPlacement(cpus, priority, original, originalThreads);
    testPartialStartup(cpus, priority, original, originalThreads);
    runPlacement({cpus[1], cpus[0], cpus[2]}, priority, original, originalThreads);
    std::cout << "PASS placement, reversed roles, and restart\n";
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        run(isolated_test::parseOptions(argc, argv));
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "isolated engine test: " << error.what() << '\n';
        return 1;
    }
}
