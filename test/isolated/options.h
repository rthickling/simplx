#pragma once

#include <charconv>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace isolated_test {

struct Config {
    unsigned cpuA = 0;
    unsigned cpuB = 0;
    unsigned housekeeping = 0;
    int fifoPriority = 0;
    bool expectFifoDenied = false;
    std::optional<unsigned> offlineCpu;
    std::optional<unsigned> deniedCpu;
    unsigned messages = 4096;
    double seconds = 3.0;
};

template<class T>
T parseNumber(std::string_view value, std::string_view option)
{
    T result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::invalid_argument(std::string(option) + " requires a number");
    }
    return result;
}

inline unsigned parseCpu(std::string_view value, std::string_view option)
{
    const unsigned cpu = parseNumber<unsigned>(value, option);
    if (cpu > 255) {
        throw std::invalid_argument(std::string(option) + " requires a Simplx CPU ID from 0 to 255");
    }
    return cpu;
}

inline Config parseOptions(int argc, char **argv)
{
    Config config;
    bool haveCpus = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--expect-fifo-denied") {
            config.expectFifoDenied = true;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + std::string(option));
        }
        const std::string_view value(argv[++index]);
        if (option == "--cpus") {
            const auto first = value.find(',');
            const auto second = first == value.npos ? value.npos : value.find(',', first + 1);
            if (second == value.npos) {
                throw std::invalid_argument("--cpus requires CPU_A,CPU_B,CPU_HK");
            }
            config.cpuA = parseCpu(value.substr(0, first), option);
            config.cpuB = parseCpu(value.substr(first + 1, second - first - 1), option);
            config.housekeeping = parseCpu(value.substr(second + 1), option);
            haveCpus = true;
        } else if (option == "--fifo-priority") {
            config.fifoPriority = parseNumber<int>(value, option);
            if (config.fifoPriority <= 0) {
                throw std::invalid_argument("--fifo-priority must be positive");
            }
        } else if (option == "--offline-cpu") {
            config.offlineCpu = parseCpu(value, option);
        } else if (option == "--denied-cpu") {
            config.deniedCpu = parseCpu(value, option);
        } else if (option == "--messages") {
            config.messages = parseNumber<unsigned>(value, option);
            if (config.messages == 0) {
                throw std::invalid_argument("--messages must be positive");
            }
        } else if (option == "--seconds") {
            config.seconds = parseNumber<double>(value, option);
            if (!std::isfinite(config.seconds) || config.seconds <= 0) {
                throw std::invalid_argument("--seconds must be finite and positive");
            }
        } else {
            throw std::invalid_argument("unknown option " + std::string(option));
        }
    }
    if (!haveCpus || config.cpuA == config.cpuB || config.cpuA == config.housekeeping ||
        config.cpuB == config.housekeeping) {
        throw std::invalid_argument("--cpus requires three distinct CPU IDs in CPU_A,CPU_B,CPU_HK order");
    }
    if (config.expectFifoDenied && config.fifoPriority == 0) {
        throw std::invalid_argument("--expect-fifo-denied requires --fifo-priority");
    }
    return config;
}

} // namespace isolated_test
