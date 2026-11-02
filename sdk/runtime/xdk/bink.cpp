
// BinkGetSummary divides bytes read * 1000 by the read time in ms + 1.
// Host reads take under a millisecond, so the time can remain 0 and the
// quotient of a large movie can overflow the 32-bit division.
// Raise the time just enough to avoid that overflow.

#include <mutex>
#include <unordered_set>

#include "xdk/hle.h"

using namespace kernel;

X86_EXTERN_FUNC(GUEST_ORIG(BinkGetSummary));
X86_FUNC(GUEST(BinkGetSummary)) {
    uint32_t bink = R32(ctx.esp + 4);

    if (bink) {
        // 0x134 = bytes read
        // 0x13C = read time in milliseconds
        uint64_t scaled =
            uint64_t(R32(bink + 0x134)) * 1000;

        uint32_t time = R32(bink + 0x13C);

        if ((scaled >> 32) > time) {
            W32(bink + 0x13C, uint32_t(scaled >> 32));
        }
    }

    GUEST_ORIG(BinkGetSummary)(ctx, base);
}

// Bink frame pacing.
//
// BinkWait is optional because this game's generated symbol configuration
// does not currently contain a verified BinkWait address.
//
// Only enable the frame-pacing workaround when SYM_BinkWait is defined.

#ifdef SYM_BinkWait

namespace {

constexpr uint32_t kBinkWait = SYM(BinkWait);

std::mutex g_held_lock;
std::unordered_set<uint32_t> g_held;

}  // namespace

X86_EXTERN_FUNC(GUEST_ORIG(BinkDoFrame));
X86_FUNC(GUEST(BinkDoFrame)) {
    uint32_t bink = R32(ctx.esp + 4);

    if (bink && CallGuest(ctx, kBinkWait, {bink})) {
        std::lock_guard<std::mutex> lock(g_held_lock);

        g_held.insert(bink);

        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }

    GUEST_ORIG(BinkDoFrame)(ctx, base);
}

X86_EXTERN_FUNC(GUEST_ORIG(BinkNextFrame));
X86_FUNC(GUEST(BinkNextFrame)) {
    uint32_t bink = R32(ctx.esp + 4);

    {
        std::lock_guard<std::mutex> lock(g_held_lock);

        if (g_held.erase(bink)) {
            ctx.esp += 8;
            return;
        }
    }

    GUEST_ORIG(BinkNextFrame)(ctx, base);
}

#else

// No verified BinkWait address is available.
// Preserve the original guest behavior without frame pacing.

X86_EXTERN_FUNC(GUEST_ORIG(BinkDoFrame));
X86_FUNC(GUEST(BinkDoFrame)) {
    GUEST_ORIG(BinkDoFrame)(ctx, base);
}

X86_EXTERN_FUNC(GUEST_ORIG(BinkNextFrame));
X86_FUNC(GUEST(BinkNextFrame)) {
    GUEST_ORIG(BinkNextFrame)(ctx, base);
}

#endif
