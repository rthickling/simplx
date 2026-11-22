#include "isolated/options.h"
#include "simplx.h"
#include "trz/pattern/timer/timeractor.h"
#include "trz/pattern/timer/timerproxy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using tredzone::Actor;
using tredzone::Engine;
using tredzone::timer::TimerActor;
using tredzone::timer::TimerProxy;

constexpr auto     oneShotDelay     = 40ms;
constexpr auto     cancelDelay      = 90ms;
constexpr auto     repeatDelay      = 30ms;
constexpr auto     quietPeriod      = 120ms;
constexpr auto     earlyTolerance   = 2ms;
constexpr unsigned repeatCount      = 4;
constexpr auto     workloadDeadline = 45s;

struct Pulse : Actor::Event
{
    explicit Pulse(std::uint64_t sequence_) : sequence(sequence_) {}
    std::uint64_t sequence;
};

struct Result
{
    std::uint64_t   pulses = 0;
    Clock::duration oneShotElapsed{};
    Clock::duration maximumRepeatLateness{};
};

struct State
{
    Actor::ActorId             consumer;
    std::atomic<bool>          load{false};
    std::atomic<std::uint64_t> pulses{0};
    std::atomic<std::uint64_t> sent{0};
    std::atomic<const char *>  error{nullptr};
    std::atomic<bool>          done{false};
    std::atomic<const char *>  phase{"startup"};
    std::atomic<unsigned>      repeats{0};
    std::array<Result, 2>      results{};
};

class PulseSource : public Actor, public Actor::Callback
{
public:
    explicit PulseSource(State *state) : state_(*state), pipe_(*this, state->consumer) { registerCallback(*this); }

    void onCallback() noexcept
    {
        if (state_.done.load() || state_.error.load() != nullptr)
        {
            return;
        }
        const auto now = Clock::now();
        if (state_.load.load() && now >= nextPulse_ && sent_ - state_.pulses.load() < 32)
        {
            try
            {
                pipe_.push<Pulse>(sent_);
                state_.sent.store(++sent_);
                nextPulse_ = now + 50us;
            }
            catch (...)
            {
                state_.error.store("could not send timer-load event");
            }
        }
        registerCallback(*this);
    }

private:
    State             &state_;
    Actor::Event::Pipe pipe_;
    std::uint64_t      sent_ = 0;
    Clock::time_point  nextPulse_{};
};

class TimerExercise : public Actor, public Actor::Callback
{
    enum class Kind
    {
        oneShot,
        canceled,
        repeating,
        sentinel
    };

    class Probe : public TimerProxy
    {
    public:
        Probe(TimerExercise &owner, Kind kind) : TimerProxy(owner), owner_(owner), kind_(kind) {}

    private:
        void           onTimeout(const tredzone::DateTime &) noexcept override { owner_.onTimer(kind_); }
        TimerExercise &owner_;
        Kind           kind_;
    };

public:
    explicit TimerExercise(State *state) : state_(*state)
    {
        state_.consumer = getActorId();
        registerEventHandler<Pulse>(*this);
        registerCallback(*this);
    }

    void onEvent(const Pulse &event) noexcept
    {
        if (event.sequence != pulses_)
        {
            state_.error.store("timer-load events arrived out of sequence");
        }
        state_.pulses.store(++pulses_);
    }

    void onCallback() noexcept
    {
        started_ = Clock::now();
        switch (phase_)
        {
        case Phase::armOneShot:
            startPulses_ = pulses_;
            state_.load.store(run_ == 1);
            state_.phase.store(run_ == 0 ? "idle one-shot" : "loaded one-shot");
            state_.repeats.store(0);
            phase_ = Phase::oneShot;
            canceled_.set(tredzone::Time::Millisecond(cancelDelay.count()));
            if (!canceled_.isSet())
            {
                state_.error.store("cancellation probe was not armed");
                return;
            }
            oneShot_.set(tredzone::Time::Millisecond(oneShotDelay.count()));
            canceled_.unset();
            if (canceled_.isSet())
            {
                state_.error.store("cancellation probe remained armed");
            }
            break;
        case Phase::armRepeating:
            state_.phase.store(run_ == 0 ? "idle repeats" : "loaded repeats");
            repeated_ = 0;
            phase_    = Phase::repeating;
            repeating_.setRepeat(tredzone::Time::Millisecond(repeatDelay.count()));
            break;
        case Phase::armSentinel:
            state_.phase.store(run_ == 0 ? "idle cancellation" : "loaded cancellation");
            phase_ = Phase::sentinel;
            sentinel_.set(tredzone::Time::Millisecond(quietPeriod.count()));
            break;
        default:
            state_.error.store("unexpected timer test callback phase");
        }
    }

private:
    enum class Phase
    {
        armOneShot,
        oneShot,
        armRepeating,
        repeating,
        armSentinel,
        sentinel
    };

    void onTimer(Kind kind) noexcept
    {
        const auto elapsed = Clock::now() - started_;
        if (kind == Kind::oneShot && phase_ == Phase::oneShot)
        {
            state_.results[run_].oneShotElapsed = elapsed;
            if (elapsed + earlyTolerance < oneShotDelay || oneShot_.isSet())
            {
                state_.error.store("one-shot timer fired early or stayed armed");
                return;
            }
            phase_ = Phase::armRepeating;
            registerCallback(*this);
        }
        else if (kind == Kind::repeating && phase_ == Phase::repeating)
        {
            observeRepeat(elapsed);
        }
        else if (kind == Kind::sentinel && phase_ == Phase::sentinel)
        {
            if (elapsed + earlyTolerance < quietPeriod || sentinel_.isSet())
            {
                state_.error.store("cancellation observation timer fired early or stayed armed");
                return;
            }
            finishRun();
        }
        else
        {
            state_.error.store("duplicate one-shot or callback after cancellation");
        }
    }

    void observeRepeat(Clock::duration elapsed) noexcept
    {
        state_.repeats.store(++repeated_);
        const auto expected = repeatDelay * repeated_;
        if (elapsed + earlyTolerance * repeated_ < expected || !repeating_.isSet())
        {
            state_.error.store("repeating timer fired early or stopped unexpectedly");
            return;
        }
        auto &lateness = state_.results[run_].maximumRepeatLateness;
        lateness       = std::max(lateness, elapsed - expected);
        if (repeated_ == repeatCount)
        {
            repeating_.unset();
            if (repeating_.isSet())
            {
                state_.error.store("canceled repeating timer remained armed");
                return;
            }
            // Wait for a new round trip through the service past the cancellation window.
            phase_ = Phase::armSentinel;
            registerCallback(*this);
        }
    }

    void finishRun() noexcept
    {
        auto &result  = state_.results[run_];
        result.pulses = pulses_ - startPulses_;
        if ((run_ == 0 && result.pulses != 0) || (run_ == 1 && result.pulses < 100))
        {
            state_.error.store("timer test did not observe the expected event load");
            return;
        }
        if (++run_ == state_.results.size())
        {
            state_.load.store(false);
            state_.done.store(true);
        }
        else
        {
            phase_ = Phase::armOneShot;
            registerCallback(*this);
        }
    }

    State            &state_;
    Probe             oneShot_{*this, Kind::oneShot};
    Probe             canceled_{*this, Kind::canceled};
    Probe             repeating_{*this, Kind::repeating};
    Probe             sentinel_{*this, Kind::sentinel};
    Phase             phase_ = Phase::armOneShot;
    Clock::time_point started_{};
    unsigned          run_         = 0;
    unsigned          repeated_    = 0;
    std::uint64_t     pulses_      = 0;
    std::uint64_t     startPulses_ = 0;
};

void run(const isolated_test::Config &config)
{
    const auto      serviceCpu = static_cast<Actor::CoreId>(config.cpuA);
    const auto      clientCpu  = static_cast<Actor::CoreId>(config.cpuB);
    const auto      loadCpu    = static_cast<Actor::CoreId>(config.housekeeping);
    Engine::CoreSet cores;
    cores.set(serviceCpu);
    cores.set(clientCpu);
    cores.set(loadCpu);
    tredzone::threadSetAffinity(config.housekeeping);

    State                 state;
    Engine::StartSequence start(cores);
    if (config.fifoPriority != 0)
    {
        tredzone::ThreadRealTimeParam scheduling;
        scheduling.sched_priority = config.fifoPriority;
        start.setRedZoneParam(scheduling);
        start.setRedZoneCore(serviceCpu);
        start.setRedZoneCore(clientCpu);
    }
    start.addServiceActor<tredzone::service::Timer, TimerActor>(serviceCpu);
    start.addActor<TimerExercise>(clientCpu, &state);
    start.addActor<PulseSource>(loadCpu, &state);
    {
        Engine     engine(start);
        const auto deadline = Clock::now() + workloadDeadline;
        while (!state.done.load() && state.error.load() == nullptr && Clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
        if (const auto *error = state.error.load())
        {
            throw std::runtime_error(error);
        }
        if (!state.done.load())
        {
            throw std::runtime_error(std::string("timer workload timed out during ") + state.phase.load() + " after " +
                                     std::to_string(state.repeats.load()) + " repeats and " +
                                     std::to_string(state.pulses.load()) + " of " + std::to_string(state.sent.load()) +
                                     " load events");
        }
    }
    for (unsigned run = 0; run < state.results.size(); ++run)
    {
        const auto &result = state.results[run];
        std::cout << (run == 0 ? "idle" : "loaded") << " pulses=" << result.pulses << " one_shot_us="
                  << std::chrono::duration_cast<std::chrono::microseconds>(result.oneShotElapsed).count()
                  << " max_repeat_lateness_us="
                  << std::chrono::duration_cast<std::chrono::microseconds>(result.maximumRepeatLateness).count()
                  << '\n';
    }
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
        std::cerr << "timer check: " << error.what() << '\n';
        return 1;
    }
}
