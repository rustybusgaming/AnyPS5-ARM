#include "SceTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

struct NpWebApi2MemoryPoolStats {
    std::size_t pool_size;
    std::size_t max_inuse_size;
    std::size_t current_inuse_size;
    std::int32_t reserved;
};

extern "C" {
int APS5_VABI sceNpWebApi2Initialize(int, std::size_t);
int APS5_VABI sceNpWebApi2Terminate(int);
int APS5_VABI sceNpWebApi2GetMemoryPoolStats(int, NpWebApi2MemoryPoolStats*);
int APS5_VABI sceNpWebApi2PushEventCreateHandle(int);
int APS5_VABI sceNpWebApi2PushEventAbortHandle(int, int);
int APS5_VABI sceNpWebApi2PushEventDeleteHandle(int, int);
int APS5_VABI sceNpWebApi2PushEventCreateFilter(int, int, const char*, std::uint32_t, const void*, std::size_t);
}

static constexpr int ErrorLibContextNotFound = static_cast<int>(0x80553404);
static constexpr int ErrorAborted = static_cast<int>(0x80553409);
static constexpr int ErrorHandleNotFound = static_cast<int>(0x8055340D);

static void Require(bool value) { if (!value) std::abort(); }

template <typename TCall>
static bool Throws(TCall call) {
    try {
        call();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

int main() {
    const int context = sceNpWebApi2Initialize(1, 0x20000);
    const int other = sceNpWebApi2Initialize(1, 0x8000);
    Require(context > 0 && other > 0 && context != other);
    NpWebApi2MemoryPoolStats stats{1, 1, 1, 1};
    Require(sceNpWebApi2GetMemoryPoolStats(context, &stats) == 0);
    Require(stats.pool_size == 0x20000 && stats.max_inuse_size == 0 && stats.current_inuse_size == 0 && stats.reserved == 0);
    Require(sceNpWebApi2GetMemoryPoolStats(other, &stats) == 0 && stats.pool_size == 0x8000);
    Require(Throws([&] { sceNpWebApi2GetMemoryPoolStats(context, nullptr); }));
    const int handle = sceNpWebApi2PushEventCreateHandle(context);
    const int kept = sceNpWebApi2PushEventCreateHandle(context);
    Require(handle > 0 && kept > 0 && handle != kept);
    const unsigned char filter[32] = {};
    Require(sceNpWebApi2PushEventCreateFilter(context, handle, nullptr, 0, filter, 1) > 0);
    Require(sceNpWebApi2PushEventAbortHandle(0x7fffffff, handle) == ErrorLibContextNotFound);
    Require(sceNpWebApi2PushEventAbortHandle(context, 0x7fffffff) == ErrorHandleNotFound);
    Require(sceNpWebApi2PushEventAbortHandle(other, handle) == ErrorHandleNotFound);
    Require(sceNpWebApi2PushEventAbortHandle(context, handle) == 0);
    Require(sceNpWebApi2PushEventAbortHandle(context, handle) == 0);
    Require(sceNpWebApi2PushEventCreateFilter(context, handle, nullptr, 0, filter, 1) == ErrorAborted);
    Require(sceNpWebApi2PushEventCreateFilter(context, kept, nullptr, 0, filter, 1) > 0);
    Require(sceNpWebApi2PushEventDeleteHandle(context, handle) == 0);
    Require(sceNpWebApi2PushEventAbortHandle(context, handle) == ErrorHandleNotFound);
    Require(sceNpWebApi2Terminate(context) == 0);
    Require(Throws([&] { sceNpWebApi2GetMemoryPoolStats(context, &stats); }));
    Require(sceNpWebApi2PushEventAbortHandle(context, kept) == ErrorLibContextNotFound);
    Require(sceNpWebApi2Terminate(other) == 0);
    return 0;
}
