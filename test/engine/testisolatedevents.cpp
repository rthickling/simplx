#include "isolated/options.h"

#include <simplx.h>
#include <trz/engine/internal/event_page_probe.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sched.h>
#include <stdexcept>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace
{
using isolated_test::Config;
using tredzone::Actor;
using tredzone::Engine;
using Clock       = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
using namespace std::chrono_literals;

constexpr unsigned workerCount  = 3;
constexpr unsigned laneA        = 0;
constexpr unsigned laneB        = 1;
constexpr unsigned laneHK       = 2;
constexpr unsigned burstSize    = 1024;
constexpr unsigned window       = 128;
constexpr auto     phaseTimeout = 15s;

enum class Phase
{
    waiting,
    first,
    burst,
    secondBurst,
    reusedBurst,
    stalled,
    pipeline,
    paced,
    rejected,
    shutdown,
    restart,
    count
};
constexpr auto       phaseCount = static_cast<std::size_t>(Phase::count);
constexpr std::array phaseNames{"waiting",          "first_after_idle",      "burst",    "second_burst",
                                "reused_burst",     "stalled_receiver",      "pipeline", "paced",
                                "return_to_sender", "shutdown_with_pending", "restart"};
static_assert(phaseNames.size() == phaseCount);

struct Stream
{
    unsigned                      total = 0;
    std::atomic<unsigned>         sent{0};
    std::atomic<unsigned>         received{0};
    std::atomic<unsigned>         returned{0};
    std::vector<Nanoseconds::rep> latency;
    std::vector<Nanoseconds::rep> scheduledLatency;
};

struct Progress
{
    Clock::time_point previous{};
    Nanoseconds       maxGap{};

    void update(Clock::time_point now)
    {
        if (previous != Clock::time_point{})
            maxGap = std::max(maxGap, std::chrono::duration_cast<Nanoseconds>(now - previous));
        previous = now;
    }
};

struct Work
{
    std::array<std::array<Stream, workerCount>, workerCount> streams;
    std::array<Progress, workerCount>                        progress;
    Clock::time_point                                        start;
    Nanoseconds                                              duration{0};
    unsigned                                                 peakPending = 0;

    void setStream(unsigned source, unsigned destination, unsigned count, bool pace = false)
    {
        Stream &stream = streams[source][destination];
        stream.total   = count;
        stream.latency.resize(count);
        if (pace)
            stream.scheduledLatency.resize(count);
    }

    void setAllDirections(unsigned count, bool pace = false)
    {
        for (unsigned source = 0; source < workerCount; ++source)
            for (unsigned destination = 0; destination < workerCount; ++destination)
                if (source != destination)
                    setStream(source, destination, count, pace);
    }

    bool complete() const
    {
        for (const auto &outgoing : streams)
            for (const Stream &stream : outgoing)
                if (stream.sent.load(std::memory_order_acquire) != stream.total ||
                    stream.received.load(std::memory_order_acquire) + stream.returned.load(std::memory_order_acquire) !=
                        stream.total)
                    return false;
        return true;
    }

    void observePending()
    {
        unsigned pending = 0;
        for (const auto &outgoing : streams)
            for (const Stream &stream : outgoing)
            {
                const unsigned sent = stream.sent.load(std::memory_order_acquire);
                const unsigned resolved =
                    stream.received.load(std::memory_order_acquire) + stream.returned.load(std::memory_order_acquire);
                pending += sent > resolved ? sent - resolved : 0;
            }
        peakPending = std::max(peakPending, pending);
    }
};

struct WorkerLocation
{
    pid_t tid      = -1;
    int   cpu      = -1;
    int   policy   = -1;
    int   priority = -1;
};

struct Shared
{
    std::array<Actor::ActorId, workerCount> ids;
    std::array<WorkerLocation, workerCount> locations;
    std::array<Work, phaseCount>            phases;
#ifdef SIMPLX_TEST_EVENT_PAGE_COUNTS
    std::array<tredzone::detail::EventPageCounts, workerCount> eventPages{};
#endif
    std::atomic<Phase>        active{Phase::waiting};
    std::atomic<unsigned>     ready{0};
    std::atomic<bool>         receiverHeld{false};
    std::atomic<bool>         releaseReceiver{false};
    std::atomic<bool>         destructionRequested{false};
    std::atomic<const char *> failure{nullptr};

    Work &work(Phase phase) { return phases[static_cast<std::size_t>(phase)]; }

    void fail(const char *reason) noexcept
    {
        const char *empty = nullptr;
        failure.compare_exchange_strong(empty, reason);
    }

    void check() const
    {
        if (const char *reason = failure.load())
            throw std::runtime_error(reason);
    }
};

constexpr std::uint8_t byteAt(unsigned source, unsigned sequence, std::size_t offset)
{
    return static_cast<std::uint8_t>((source * 53u + sequence * 19u + offset * 7u) ^ (sequence >> 8));
}

template <std::size_t size> struct Message : Actor::Event
{
    Phase                          phase;
    unsigned                       source;
    unsigned                       sequence;
    Clock::time_point              sentAt;
    Clock::time_point              scheduledAt;
    std::array<std::uint8_t, size> payload;

    Message(Phase phase, unsigned source, unsigned sequence, Clock::time_point sentAt, Clock::time_point scheduledAt)
        : phase(phase), source(source), sequence(sequence), sentAt(sentAt), scheduledAt(scheduledAt)
    {
        for (std::size_t index = 0; index < payload.size(); ++index)
            payload[index] = byteAt(source, sequence, index);
    }
};

struct Rejected : Actor::Event
{
};

class Worker : public Actor, public Actor::Callback
{
public:
    struct Init
    {
        Shared  &shared;
        unsigned lane;
    };

    explicit Worker(const Init &init) : shared(init.shared), lane(init.lane)
    {
        shared.ids[lane] = getActorId();
        registerEventHandler<Message<64>>(*this);
        registerEventHandler<Message<512>>(*this);
        registerEventHandler<Rejected>(*this);
        registerUndeliveredEventHandler<Message<64>>(*this);
        registerUndeliveredEventHandler<Message<512>>(*this);
        registerUndeliveredEventHandler<Rejected>(*this);
        registerPerformanceNeutralCallback(*this);
    }

#ifdef SIMPLX_TEST_EVENT_PAGE_COUNTS
    ~Worker() override { shared.eventPages[lane] = tredzone::detail::currentEventPageCounts(); }
#endif

    void onCallback() noexcept
    {
        if (!announced)
        {
            announced = true;
            sched_param scheduling{};
            const bool  havePriority = sched_getparam(0, &scheduling) == 0;
            shared.locations[lane]   = {gettid(), sched_getcpu(), sched_getscheduler(0),
                                      havePriority ? scheduling.sched_priority : -1};
            shared.ready.fetch_add(1);
        }
        const Phase phase = shared.active.load(std::memory_order_acquire);
        if (phase != Phase::waiting && !shared.failure.load(std::memory_order_relaxed))
        {
            try
            {
                drive(phase);
            }
            catch (...)
            {
                shared.fail("a producer could not enqueue an Event");
            }
        }
        registerPerformanceNeutralCallback(*this);
    }

    template <std::size_t size> void onEvent(const Message<size> &message)
    {
        const auto now = Clock::now();
        if (message.source >= workerCount || message.getSourceActorId() != shared.ids[message.source] ||
            message.getDestinationActorId() != getActorId())
        {
            shared.fail("incorrect Event source or destination");
            return;
        }
        Work          &work     = shared.work(message.phase);
        Stream        &stream   = work.streams[message.source][lane];
        const unsigned expected = stream.received.load(std::memory_order_relaxed);
        if (message.sequence != expected || expected >= stream.total)
        {
            shared.fail("duplicate, out-of-order or unexpected Event");
            return;
        }
        for (std::size_t index = 0; index < message.payload.size(); ++index)
            if (message.payload[index] != byteAt(message.source, message.sequence, index))
            {
                shared.fail("incorrect Event payload");
                return;
            }
        stream.latency[expected] = std::chrono::duration_cast<Nanoseconds>(now - message.sentAt).count();
        if (!stream.scheduledLatency.empty())
            stream.scheduledLatency[expected] =
                std::chrono::duration_cast<Nanoseconds>(now - message.scheduledAt).count();
        stream.received.store(expected + 1, std::memory_order_release);

        if (message.phase == Phase::pipeline && lane != laneA)
        {
            // B validates and transforms the message. HK checks B's payload and sends the receipt to A.
            const unsigned next = lane == laneB ? laneHK : laneA;
            try
            {
                send(work.streams[lane][next], next, message.phase, message.sequence, message.sentAt,
                     message.scheduledAt);
            }
            catch (...)
            {
                shared.fail("pipeline forwarding failed");
            }
        }
    }

    void onEvent(const Rejected &) { throw Actor::ReturnToSenderException(); }

    template <std::size_t size> void onUndeliveredEvent(const Message<size> &)
    {
        shared.fail("an Event that should have been delivered was returned");
    }

    void onUndeliveredEvent(const Rejected &event)
    {
        Stream &stream = shared.work(Phase::rejected).streams[laneA][laneB];
        if (lane != laneA || event.getSourceActorId() != getActorId() ||
            event.getDestinationActorId() != shared.ids[laneB] || stream.returned.fetch_add(1) != 0)
            shared.fail("incorrect return-to-sender result");
    }

private:
    Shared  &shared;
    unsigned lane;
    bool     announced = false;
    Phase    held      = Phase::waiting;

    void onDestroyRequest() noexcept override
    {
        if (shared.active.load(std::memory_order_acquire) == Phase::shutdown)
        {
            shared.destructionRequested.store(true, std::memory_order_release);
            if (!shared.failure.load() && !shared.work(Phase::shutdown).complete())
            {
                requestDestroy();
                return;
            }
        }
        Actor::onDestroyRequest();
    }

    void holdReceiver(Phase phase)
    {
        if (held == phase)
            return;
        held = phase;
        shared.receiverHeld.store(true, std::memory_order_release);
        const auto deadline = Clock::now() + phaseTimeout;
        while (!shared.releaseReceiver.load(std::memory_order_acquire) && !shared.failure.load())
        {
            if (Clock::now() >= deadline)
            {
                shared.fail("timed out waiting to release the receiver");
                return;
            }
            std::this_thread::sleep_for(100us);
        }
    }

    void send(Stream &stream, unsigned destination, Phase phase, unsigned sequence, Clock::time_point sentAt,
              Clock::time_point scheduledAt)
    {
        Event::Pipe pipe(*this, shared.ids[destination]);
        if (sequence % 2 == 0)
            pipe.push<Message<512>>(phase, lane, sequence, sentAt, scheduledAt);
        else
            pipe.push<Message<64>>(phase, lane, sequence, sentAt, scheduledAt);
        stream.sent.store(sequence + 1, std::memory_order_release);
    }

    void drive(Phase phase)
    {
        Work &work = shared.work(phase);
        work.progress[lane].update(Clock::now());
        const bool holdB = phase == Phase::stalled || phase == Phase::shutdown;
        if ((holdB && lane == laneB) || (phase == Phase::pipeline && lane == laneHK))
            holdReceiver(phase);
        if (shared.failure.load() || ((holdB || phase == Phase::pipeline) && !shared.receiverHeld.load()))
            return;
        if (phase == Phase::rejected)
        {
            Stream &stream = work.streams[laneA][laneB];
            if (lane == laneA && stream.sent.load(std::memory_order_relaxed) == 0)
            {
                Event::Pipe(*this, shared.ids[laneB]).push<Rejected>();
                stream.sent.store(1, std::memory_order_release);
            }
            return;
        }
        if (phase == Phase::pipeline && lane != laneA)
            return;
        for (unsigned destination = 0; destination < workerCount; ++destination)
        {
            Stream  &stream   = work.streams[lane][destination];
            unsigned sequence = stream.sent.load(std::memory_order_relaxed);
            unsigned limit    = stream.total;
            if (sequence >= limit)
                continue;
            if (phase == Phase::pipeline)
            {
                const unsigned acknowledged = work.streams[laneHK][laneA].received.load(std::memory_order_acquire);
                limit                       = std::min(limit, acknowledged + window);
            }
            if (phase == Phase::paced)
            {
                const auto elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - work.start);
                // Cap catch-up batches while preserving the same offered schedule in every run.
                const unsigned due =
                    elapsed >= work.duration
                        ? stream.total
                        : 1 + static_cast<unsigned>(elapsed.count() * stream.total / work.duration.count());
                limit = std::min(due, sequence + 32);
            }
            for (; sequence < limit; ++sequence)
            {
                const auto now = Clock::now();
                const auto scheduled =
                    phase == Phase::paced ? work.start + work.duration * sequence / stream.total : now;
                send(stream, destination, phase, sequence, now, scheduled);
            }
        }
    }
};

std::unique_ptr<Engine> startEngine(const Config &config, Shared &shared)
{
    const std::array cpus{config.cpuA, config.cpuB, config.housekeeping};
    Engine::CoreSet  cores;
    for (unsigned cpu : cpus)
        cores.set(static_cast<Actor::CoreId>(cpu));
    Engine::StartSequence start(cores);
    if (config.fifoPriority != 0)
    {
        tredzone::ThreadRealTimeParam policy;
        policy.sched_priority = config.fifoPriority;
        start.setRedZoneParam(policy);
        start.setRedZoneCore(static_cast<Actor::CoreId>(config.cpuA));
        start.setRedZoneCore(static_cast<Actor::CoreId>(config.cpuB));
    }
    for (unsigned lane = 0; lane < workerCount; ++lane)
        start.addActor<Worker>(static_cast<Actor::CoreId>(cpus[lane]), Worker::Init{shared, lane});
    return std::make_unique<Engine>(start);
}

template <typename Predicate>
void waitFor(Shared &shared, const char *failure, Predicate done, Clock::duration timeout = phaseTimeout)
{
    const auto deadline = Clock::now() + timeout;
    while (!done())
    {
        shared.check();
        if (Clock::now() >= deadline)
            throw std::runtime_error(failure);
        std::this_thread::sleep_for(1ms);
    }
    shared.check();
}

void begin(Shared &shared, Phase phase)
{
    shared.receiverHeld.store(false);
    shared.releaseReceiver.store(false);
    shared.work(phase).start = Clock::now();
    shared.active.store(phase, std::memory_order_release);
}

void ready(Shared &shared, const Config &config, const char *runName)
{
    waitFor(shared, "workers did not start", [&] { return shared.ready.load() == workerCount; });
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        throw std::runtime_error("unable to read CLOCK_MONOTONIC for READY");
    constexpr std::array roles{"A", "B", "HK"};
    const std::array     cpus{config.cpuA, config.cpuB, config.housekeeping};
    for (unsigned lane = 0; lane < workerCount; ++lane)
    {
        const auto location = shared.locations[lane];
        const int  priority = lane == laneHK ? 0 : config.fifoPriority;
        const int  policy   = priority == 0 ? SCHED_OTHER : SCHED_FIFO;
        if (location.tid <= 0 || location.cpu != static_cast<int>(cpus[lane]) || location.policy != policy ||
            location.priority != priority)
            throw std::runtime_error("an Event worker reported unexpected CPU, scheduler policy, or priority");
        std::cout << "READY run=" << runName << " monotonic_ns=" << now.tv_sec * 1000000000LL + now.tv_nsec
                  << " worker=" << lane << " role=" << roles[lane] << " tid=" << location.tid << " cpu=" << location.cpu
                  << " policy=" << location.policy << " priority=" << location.priority << '\n';
    }
    std::cout.flush();
}

void drain(Shared &shared, Phase phase)
{
    Work &work = shared.work(phase);
    waitFor(
        shared, phaseNames[static_cast<std::size_t>(phase)],
        [&]
        {
            work.observePending();
            return work.complete();
        },
        work.duration + phaseTimeout);
}

void run(Shared &shared, Phase phase)
{
    begin(shared, phase);
    drain(shared, phase);
}

void verifyHold(Shared &shared, Phase phase, unsigned source, unsigned destination, unsigned expected)
{
    Work   &work   = shared.work(phase);
    Stream &stream = work.streams[source][destination];
    waitFor(shared, "receiver failed to hold the expected backlog",
            [&]
            {
                work.observePending();
                return shared.receiverHeld.load() && stream.sent.load(std::memory_order_acquire) == expected;
            });
    std::this_thread::sleep_for(40ms);
    work.observePending();
    if (stream.received.load(std::memory_order_acquire) != 0)
        throw std::runtime_error("the fenced receiver made unexpected progress");
}

void printLatency(const char *prefix, std::vector<Nanoseconds::rep> &samples)
{
    if (samples.empty())
        return;
    std::ranges::sort(samples);
    const auto percentile = [&](std::size_t thousandths)
    {
        const auto rank = (samples.size() * thousandths + 999) / 1000;
        return samples[rank - 1];
    };
    std::cout << ' ' << prefix << "_p50_ns=" << percentile(500) << ' ' << prefix << "_p99_ns=" << percentile(990) << ' '
              << prefix << "_p999_ns=" << percentile(999) << ' ' << prefix << "_max_ns=" << samples.back();
}

void report(Phase phase, Shared &shared)
{
    Work                         &work     = shared.work(phase);
    unsigned                      sent     = 0;
    unsigned                      received = 0;
    unsigned                      returned = 0;
    std::vector<Nanoseconds::rep> latencies;
    std::vector<Nanoseconds::rep> scheduled;
    for (const auto &outgoing : work.streams)
        for (const Stream &stream : outgoing)
        {
            sent += stream.sent.load();
            const unsigned delivered = stream.received.load();
            received += delivered;
            returned += stream.returned.load();
            latencies.insert(latencies.end(), stream.latency.begin(), stream.latency.begin() + delivered);
            if (!stream.scheduledLatency.empty())
                scheduled.insert(scheduled.end(), stream.scheduledLatency.begin(),
                                 stream.scheduledLatency.begin() + delivered);
        }
    const auto start = std::chrono::duration_cast<Nanoseconds>(work.start.time_since_epoch()).count();
    std::cout << "phase=" << phaseNames[static_cast<std::size_t>(phase)] << " monotonic_start_ns=" << start
              << " sent=" << sent << " received=" << received << " returned=" << returned
              << " peak_sampled_pending=" << work.peakPending;
    for (unsigned lane = 0; lane < workerCount; ++lane)
        std::cout << " worker" << lane << "_max_gap_ns=" << work.progress[lane].maxGap.count();
    printLatency("latency", latencies);
    printLatency("scheduled_latency", scheduled);
    if (phase == Phase::pipeline)
    {
        printLatency("pipeline_delivery", work.streams[laneB][laneHK].latency);
        printLatency("pipeline_round_trip", work.streams[laneHK][laneA].latency);
    }
    std::cout << '\n';
}

void exercise(const Config &config)
{
    if (config.messages > 250000 || config.seconds > 3600)
        throw std::invalid_argument("--messages must be at most 250000 and --seconds at most 3600");
    tredzone::threadSetAffinity(config.housekeeping);
    tredzone::threadSetRealTime(false, tredzone::ThreadRealTimeParam{});
    Shared shared;
    shared.work(Phase::first).setAllDirections(1);
    constexpr std::array bursts{Phase::burst, Phase::secondBurst, Phase::reusedBurst};
    for (const Phase burst : bursts)
        shared.work(burst).setAllDirections(burstSize);
    shared.work(Phase::stalled).setStream(laneA, laneB, burstSize);
    shared.work(Phase::stalled).setStream(laneHK, laneB, burstSize);
    shared.work(Phase::pipeline).setStream(laneA, laneB, burstSize);
    shared.work(Phase::pipeline).setStream(laneB, laneHK, burstSize);
    shared.work(Phase::pipeline).setStream(laneHK, laneA, burstSize);
    Work &paced = shared.work(Phase::paced);
    paced.setAllDirections(config.messages, true);
    paced.duration =
        std::max(1ns, std::chrono::duration_cast<Nanoseconds>(std::chrono::duration<double>(config.seconds)));
    shared.work(Phase::rejected).setStream(laneA, laneB, 1);
    shared.work(Phase::shutdown).setStream(laneA, laneB, burstSize);

    auto engine = startEngine(config, shared);
    ready(shared, config, "initial");
    std::this_thread::sleep_for(100ms);
    run(shared, Phase::first);
    // The first burst overflows the initial pages. The repeated batches force the
    // sender to rotate delivered pages back into the free chain and use them again.
    for (const Phase burst : bursts)
        run(shared, burst);
    begin(shared, Phase::stalled);
    verifyHold(shared, Phase::stalled, laneA, laneB, burstSize);
    verifyHold(shared, Phase::stalled, laneHK, laneB, burstSize);
    shared.releaseReceiver.store(true);
    drain(shared, Phase::stalled);

    begin(shared, Phase::pipeline);
    verifyHold(shared, Phase::pipeline, laneB, laneHK, window);
    if (shared.work(Phase::pipeline).streams[laneA][laneB].sent.load() != window)
        throw std::runtime_error("the pipeline exceeded its credit window");
    shared.releaseReceiver.store(true);
    drain(shared, Phase::pipeline);
    run(shared, Phase::paced);
    run(shared, Phase::rejected);

    begin(shared, Phase::shutdown);
    verifyHold(shared, Phase::shutdown, laneA, laneB, burstSize);
    const auto stoppedAt = Clock::now();
    {
        std::jthread releaseAfterDestroyRequest(
            [&]
            {
                try
                {
                    waitFor(shared, "shutdown failed to request actor destruction",
                            [&] { return shared.destructionRequested.load(std::memory_order_acquire); });
                }
                catch (...)
                {
                    shared.fail("shutdown failed to request actor destruction");
                }
                shared.releaseReceiver.store(true);
            });
        engine.reset();
    }
    shared.check();
    if (!shared.work(Phase::shutdown).complete())
        throw std::runtime_error("shutdown failed to account for pending Events");
    std::cout << "shutdown_ns=" << std::chrono::duration_cast<Nanoseconds>(Clock::now() - stoppedAt).count() << '\n';

    for (const Phase phase : {Phase::first, Phase::burst, Phase::secondBurst, Phase::reusedBurst, Phase::stalled,
                              Phase::pipeline, Phase::paced, Phase::rejected, Phase::shutdown})
        report(phase, shared);
#ifdef SIMPLX_TEST_EVENT_PAGE_COUNTS
    for (unsigned lane = 0; lane < workerCount; ++lane)
    {
        const auto counts = shared.eventPages[lane];
        std::cout << "worker=" << lane << " fresh_event_pages=" << counts.fresh
                  << " reused_event_pages=" << counts.reused << '\n';
        if (counts.fresh == 0 || counts.reused == 0)
            throw std::runtime_error("event-page positive control did not see both branches on every producer");
    }
#endif
    Shared restarted;
    restarted.work(Phase::restart).setAllDirections(1);
    {
        auto nextEngine = startEngine(config, restarted);
        ready(restarted, config, "restart");
        run(restarted, Phase::restart);
    }
    restarted.check();
    report(Phase::restart, restarted);
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        const Config config = isolated_test::parseOptions(argc, argv);
        exercise(config);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Isolated Event test failed: " << error.what() << '\n';
        return 1;
    }
}
