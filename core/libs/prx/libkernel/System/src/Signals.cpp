#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Pthread/Posix/Common.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/Pthread/include/Cancel.hpp"
#include "prx/libkernel/Time/include/TimedWait.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" int APS5_VABI getpid_nid_postfix(void);
extern "C" int APS5_VABI getuid_nid_postfix(void);
extern "C" Pthread APS5_VABI scePthreadSelf(void);

struct GuestSignalSet {
    std::uint32_t bits[4];
};

struct GuestSigaction {
    std::uintptr_t handler;
    int flags;
    GuestSignalSet mask;
};
static_assert(sizeof(GuestSigaction) == 32 && offsetof(GuestSigaction, flags) == 8 && offsetof(GuestSigaction, mask) == 12);

struct GuestSiginfo {
    int signo;
    int error;
    int code;
    int pid;
    std::uint32_t uid;
    int status;
    std::uint64_t address;
    std::uint64_t value;
    std::uint64_t reason[5];
};
static_assert(sizeof(GuestSiginfo) == 0x50 && offsetof(GuestSiginfo, address) == 0x18 && offsetof(GuestSiginfo, value) == 0x20);

struct GuestTimespec {
    std::int64_t seconds;
    std::int64_t nanoseconds;
};

namespace {
using GuestHandler = void (APS5_VABI *)(int);
constexpr int MaxSignal = 128;
constexpr int GuestSigkill = 9;
constexpr int GuestSigstop = 17;
constexpr int SaRestart = 0x2;
constexpr int SaSiginfo = 0x40;
std::atomic<GuestHandler> handlers[MaxSignal + 1]{};
static_assert(std::atomic<GuestHandler>::is_always_lock_free);
std::atomic<std::uint32_t> blockedMask{0};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
std::mutex registration;
GuestSigaction dispositions[MaxSignal + 1]{};
int NativeSignal(int guest) {
    switch (guest) {
        case 2: return SIGINT;
        case 4: return SIGILL;
        case 6: return SIGABRT;
        case 8: return SIGFPE;
        case 11: return SIGSEGV;
        case 15: return SIGTERM;
        default: return 0;
    }
}
void Dispatch(int native) {
    int guest = 0;
    for (int candidate : {2, 4, 6, 8, 11, 15})
        if (NativeSignal(candidate) == native) { guest = candidate; break; }
    if (!guest) return;
#ifdef _WIN32
    // Preserve the guest's persistent registration across CRT delivery.
    std::signal(native, Dispatch);
#endif
    if ((blockedMask.load() & (1u << (guest - 1))) != 0) return;
    const auto callback = handlers[guest].load();
    if (reinterpret_cast<std::uintptr_t>(callback) > 1) callback(guest);
}
bool Delivered(const GuestSigaction& action) {
    return action.handler > 1 && (action.flags & SaSiginfo) == 0;
}
bool Install(int guest, const GuestSigaction& action) {
    const int native = NativeSignal(guest);
    if (!native) return true;
    const auto previous = handlers[guest].exchange(Delivered(action) ? reinterpret_cast<GuestHandler>(action.handler) : nullptr);
    auto hostHandler = action.handler == 1 ? SIG_IGN : Delivered(action) ? Dispatch : SIG_DFL;
    if (std::signal(native, hostHandler) == SIG_ERR) {
        handlers[guest].store(previous);
        return false;
    }
    return true;
}

constexpr int SiUser = 0x10001;
constexpr int SiLwp = 0x10007;
constexpr int GuestEfault = 14;
constexpr int GuestEinval = 22;
constexpr int GuestEagain = 35;
constexpr std::uint64_t WaitSliceNanos = 3600000000000ULL;
std::uint32_t pendingSignals = 0;
int pendingCodes[33]{};
TimedWait::Condition pendingRaised;

std::uint32_t Bit(int guest) { return 1u << (guest - 1); }

void Forget(int guest) {
    if (guest <= 32 && dispositions[guest].handler == 1) pendingSignals &= ~Bit(guest);
}

int Deliver(int guest) {
    {
        std::lock_guard lock(registration);
        if (dispositions[guest].handler > 1 && !Delivered(dispositions[guest])) throw std::runtime_error("raise: SA_SIGINFO handlers are not delivered");
    }
    const int result = std::raise(NativeSignal(guest));
    if (result) *__error_nid_postfix() = GuestEinval;
    return result ? -1 : 0;
}

int Send(int guest, int code) {
    if (!NativeSignal(guest)) { *__error_nid_postfix() = GuestEinval; return -1; }
    {
        std::lock_guard lock(registration);
        if ((blockedMask.load() & Bit(guest)) != 0) {
            if (dispositions[guest].handler != 1 && (pendingSignals & Bit(guest)) == 0) {
                pendingSignals |= Bit(guest);
                pendingCodes[guest] = code;
                pendingRaised.NotifyAll();
            }
            return 0;
        }
    }
    return Deliver(guest);
}

void DeliverPending(std::uint32_t signals) {
    while (signals != 0) {
        const int guest = std::countr_zero(signals) + 1;
        signals &= ~Bit(guest);
        Deliver(guest);
    }
}

std::uint64_t WaitDeadline(const GuestTimespec& timeout) {
    const std::uint64_t now = TimedWait::NowNanos();
    if (timeout.seconds < 0) return 0;
    const std::uint64_t limit = std::numeric_limits<std::uint64_t>::max() - now;
    const auto seconds = static_cast<std::uint64_t>(timeout.seconds);
    if (seconds >= limit / 1000000000ULL) return std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t delay = seconds * 1000000000ULL + static_cast<std::uint64_t>(timeout.nanoseconds);
    return delay >= limit ? std::numeric_limits<std::uint64_t>::max() : now + delay;
}
}

namespace {
constexpr int GuestSigalrm = 14;
struct AlarmState {
    std::mutex lock;
    std::condition_variable changed;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    bool threadStarted = false;
};

AlarmState& Alarms() {
    static auto* state = new AlarmState();
    return *state;
}

void ExpireAlarm() {
    std::uintptr_t handler = 0;
    {
        std::lock_guard lock(registration);
        handler = dispositions[GuestSigalrm].handler;
    }
    if (handler == 1) return;
    if (handler == 0) {
        std::fprintf(stderr, "SIGALRM: the alarm expired with the default disposition; the process terminates\n");
        std::fflush(stderr);
        std::_Exit(128 + GuestSigalrm);
    }
    throw std::runtime_error("alarm: delivering SIGALRM to a guest handler is not implemented");
}

void RunAlarms() {
    auto& alarms = Alarms();
    std::unique_lock lock(alarms.lock);
    for (;;) {
        if (!alarms.deadline) {
            alarms.changed.wait(lock);
            continue;
        }
        const auto deadline = *alarms.deadline;
        if (alarms.changed.wait_until(lock, deadline) != std::cv_status::timeout || alarms.deadline != deadline) continue;
        alarms.deadline.reset();
        lock.unlock();
        ExpireAlarm();
        lock.lock();
    }
}
}

extern "C" unsigned int APS5_VABI GuestAlarm_nid_no_patch(unsigned int seconds) {
    auto& alarms = Alarms();
    std::lock_guard lock(alarms.lock);
    const auto now = std::chrono::steady_clock::now();
    unsigned int remaining = 0;
    if (alarms.deadline && *alarms.deadline > now) {
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(*alarms.deadline - now).count();
        remaining = static_cast<unsigned int>((micros + 999999) / 1000000);
    }
    if (seconds == 0) alarms.deadline.reset();
    else alarms.deadline = now + std::chrono::seconds(seconds);
    if (!alarms.threadStarted) {
        std::thread(RunAlarms).detach();
        alarms.threadStarted = true;
    }
    alarms.changed.notify_all();
    return remaining;
}

struct GuestStack {
    void* sp;
    std::size_t size;
    int flags;
};
static_assert(sizeof(GuestStack) == 24 && offsetof(GuestStack, flags) == 16);
namespace {
constexpr int SsDisable = 4;
constexpr std::size_t MinSignalStackSize = 2048;
thread_local GuestStack alternateStack{nullptr, 0, SsDisable};
}

extern "C" {
GuestHandler APS5_VABI signal_nid_postfix(int guest, GuestHandler handler) {
    const auto invalid = reinterpret_cast<GuestHandler>(static_cast<std::uintptr_t>(-1));
    if (guest < 1 || guest > MaxSignal || ((guest == GuestSigkill || guest == GuestSigstop) && handler != nullptr)) { *__error_nid_postfix() = 22; return invalid; }
    std::lock_guard lock(registration);
    const GuestSigaction action{reinterpret_cast<std::uintptr_t>(handler), SaRestart, {}};
    if (!Install(guest, action)) { *__error_nid_postfix() = 22; return invalid; }
    const auto previous = reinterpret_cast<GuestHandler>(dispositions[guest].handler);
    dispositions[guest] = action;
    Forget(guest);
    return previous;
}
int APS5_VABI sigaction_nid_postfix(int guest, const GuestSigaction* action, GuestSigaction* previous) {
    if (guest < 1 || guest > MaxSignal) { *__error_nid_postfix() = 22; return -1; }
    if (action && (guest == GuestSigkill || guest == GuestSigstop) && action->handler != 0) { *__error_nid_postfix() = 22; return -1; }
    std::lock_guard lock(registration);
    const GuestSigaction old = dispositions[guest];
    if (action) {
        if (!Install(guest, *action)) { *__error_nid_postfix() = 22; return -1; }
        dispositions[guest] = *action;
        Forget(guest);
    }
    if (previous) *previous = old;
    return 0;
}
int APS5_VABI raise_nid_postfix(int guest) {
    return Send(guest, SiLwp);
}
int APS5_VABI kill_nid_postfix(int pid, int guest) {
    if (guest < 0 || guest > MaxSignal) { *__error_nid_postfix() = 22; return -1; }
    const int self = getpid_nid_postfix();
    if (pid != self && pid != 0 && pid != -self) { *__error_nid_postfix() = 3; return -1; }
    return guest == 0 ? 0 : Send(guest, SiUser);
}
int APS5_VABI pthread_kill_nid_postfix(Pthread thread, int guest) {
    if (guest < 0 || guest > MaxSignal) return PosixThread::GUEST_EINVAL;
    if (thread == nullptr) return PosixThread::GUEST_ESRCH;
    if (guest == 0) return 0;
    if (thread != scePthreadSelf()) NotImplemented_nid_no_patch(("pthread_kill signal " + std::to_string(guest) + " to another thread").c_str());
    return raise_nid_postfix(guest) == 0 ? 0 : *__error_nid_postfix();
}
int APS5_VABI sigaltstack_nid_postfix(const GuestStack* stack, GuestStack* previous) {
    GuestStack replacement = alternateStack;
    if (stack != nullptr) {
        if ((stack->flags & ~SsDisable) != 0) { *__error_nid_postfix() = 22; return -1; }
        if (stack->flags & SsDisable) replacement.flags = SsDisable;
        else if (stack->size < MinSignalStackSize) { *__error_nid_postfix() = 12; return -1; }
        else replacement = {stack->sp, stack->size, 0};
    }
    if (previous != nullptr) *previous = alternateStack;
    alternateStack = replacement;
    return 0;
}
int APS5_VABI _sigprocmask_nid_postfix(int how, const GuestSignalSet* set, GuestSignalSet* previousSet) {
    std::uint32_t unblocked = 0;
    {
        std::lock_guard lock(registration);
        if (previousSet != nullptr) {
            previousSet->bits[0] = blockedMask.load();
            previousSet->bits[1] = 0;
            previousSet->bits[2] = 0;
            previousSet->bits[3] = 0;
        }
        if (set != nullptr) {
            switch (how) {
                case 1: blockedMask.fetch_or(set->bits[0]); break;
                case 2: blockedMask.fetch_and(~set->bits[0]); break;
                case 3: blockedMask.store(set->bits[0]); break;
                default: throw std::invalid_argument("_sigprocmask: invalid how");
            }
        }
        unblocked = pendingSignals & ~blockedMask.load();
        pendingSignals &= ~unblocked;
    }
    DeliverPending(unblocked);
    return 0;
}

int APS5_VABI sigprocmask_nid_postfix(int how, const void* set, void* previousSet) {
    return _sigprocmask_nid_postfix(how, static_cast<const GuestSignalSet*>(set),
                                    static_cast<GuestSignalSet*>(previousSet));
}

int APS5_VABI pthread_sigmask_nid_postfix(int how, const GuestSignalSet* set, GuestSignalSet* previousSet) {
    if (set != nullptr && (how < 1 || how > 3)) return 22;
    return _sigprocmask_nid_postfix(how, set, previousSet);
}

int APS5_VABI sigtimedwait_nid_postfix(const GuestSignalSet* set, GuestSiginfo* info, const GuestTimespec* timeout) {
    ThreadCancel::Check();
    if (set == nullptr) { *__error_nid_postfix() = GuestEfault; return -1; }
    const std::uint32_t waited = set->bits[0] & ~(Bit(GuestSigkill) | Bit(GuestSigstop));
    const bool valid = timeout == nullptr || (timeout->nanoseconds >= 0 && timeout->nanoseconds < 1000000000);
    const std::uint64_t deadline = timeout != nullptr && valid ? WaitDeadline(*timeout) : 0;
    std::unique_lock lock(registration);
    for (;;) {
        if (const std::uint32_t ready = pendingSignals & waited) {
            const int guest = std::countr_zero(ready) + 1;
            pendingSignals &= ~Bit(guest);
            const int code = pendingCodes[guest];
            lock.unlock();
            if (info != nullptr) {
                *info = {};
                info->signo = guest;
                info->code = code;
                info->pid = getpid_nid_postfix();
                info->uid = static_cast<std::uint32_t>(getuid_nid_postfix());
            }
            return guest;
        }
        if (ThreadCancel::Requested()) {
            lock.unlock();
            ThreadCancel::Check();
        }
        std::optional<std::uint64_t> until;
        if (timeout != nullptr) {
            if (!valid) { *__error_nid_postfix() = GuestEinval; return -1; }
            const std::uint64_t now = TimedWait::NowNanos();
            if (now >= deadline) { *__error_nid_postfix() = GuestEagain; return -1; }
            until = now + std::min(deadline - now, WaitSliceNanos);
        }
        ThreadCancel::Wait(pendingRaised, lock, until);
    }
}
}

extern "C" {

int APS5_VABI _is_signal_return_nid_postfix(std::uint64_t programCounter) {
    (void)programCounter;
    return 0;
}

}
