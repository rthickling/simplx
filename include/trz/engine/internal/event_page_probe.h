#pragma once

#include <cstdint>

namespace tredzone::detail {

struct EventPageCounts {
    std::uint64_t fresh = 0;
    std::uint64_t reused = 0;
};

// Read from the worker whose Event pages are being counted.
#ifdef SIMPLX_TEST_EVENT_PAGE_COUNTS
EventPageCounts currentEventPageCounts() noexcept;
#else
inline EventPageCounts currentEventPageCounts() noexcept { return {}; }
#endif

} // namespace tredzone::detail
