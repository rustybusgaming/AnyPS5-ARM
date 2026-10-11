#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Pthread/Posix/Common.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/Pthread/include/Cancel.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
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
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <cerrno>
#include <pthread.h>
#endif

extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" int APS5_VABI getpid_nid_postfix(void);
extern "C" int APS5_VABI getuid_nid_postfix(void);
extern "C" Pthread APS5_VABI scePthreadSelf(void);
#ifdef _WIN32
bool DeliverToGuestThread(Pthread thread, void (APS5_VABI* handler)(int, void*), int signum);
#endif

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

struct PendingSet {
    std::uint32_t signals = 0;
    int codes[33]{};
};
PendingSet processPending;
std::map<std::int64_t, PendingSet> threadPending;
TimedWait::Condition pendingRaised;

std::uint32_t Bit(int guest) { return 1u << (guest - 1); }

void MarkPending(PendingSet& pending, int guest, int code) {
    if ((pending.signals & Bit(guest)) != 0) return;
    pending.signals |= Bit(guest);
    pending.codes[guest] = code;
    pendingRaised.NotifyAll();
}

void Forget(int guest) {
    if (guest > 32 || dispositions[guest].handler != 1) return;
    processPending.signals &= ~Bit(guest);
    for (auto entry = threadPending.begin(); entry != threadPending.end();) {
        entry->second.signals &= ~Bit(guest);
        entry = entry->second.signals == 0 ? threadPending.erase(entry) : std::next(entry);
    }
}

int Deliver(int guest) {
    {
        std::lock_guard lock(registration);
        if (dispositions[guest].handler > 1 && !Delivered(dispositions[guest])) throw std::runtime_error("raise: SA_SIGINFO handlers are not delivered");
    }
    return std::raise(NativeSignal(guest)) == 0 ? 0 : GuestEinval;
}

#ifdef _WIN32
void APS5_VABI RunHandler_nid_no_patch(int guest, void*) {
    if ((blockedMask.load() & Bit(guest)) != 0) return;
    const auto callback = handlers[guest].load();
    if (reinterpret_cast<std::uintptr_t>(callback) > 1) callback(guest);
}
#endif

int Signal(Pthread thread, int guest) {
    const int native = NativeSignal(guest);
    std::uintptr_t handler = 0;
    {
        std::lock_guard lock(registration);
        handler = dispositions[guest].handler;
        if (handler > 1 && !Delivered(dispositions[guest])) throw std::runtime_error("pthread_kill: SA_SIGINFO handlers are not delivered");
    }
    if (handler == 1) return 0;
    if (handler == 0) return std::raise(native) == 0 ? 0 : GuestEinval;
#ifdef _WIN32
    return DeliverToGuestThread(thread, RunHandler_nid_no_patch, guest) ? 0 : PosixThread::GUEST_ESRCH;
#else
    std::lock_guard lock(thread->_join_mtx);
    if (thread->_finished.load(std::memory_order_acquire)) return PosixThread::GUEST_ESRCH;
    const int result = pthread_kill(thread->hostThread, native);
    if (result == ESRCH) return PosixThread::GUEST_ESRCH;
    if (result != 0) throw std::system_error(result, std::generic_category(), "pthread_kill: cannot signal the target thread");
    return 0;
#endif
}

int Post(Pthread target, int guest, int code) {
    if (!NativeSignal(guest)) return GuestEinval;
    {
        std::lock_guard lock(registration);
        if ((blockedMask.load() & Bit(guest)) != 0) {
            if (dispositions[guest].handler != 1) MarkPending(target != nullptr ? threadPending[target->tid] : processPending, guest, code);
            return 0;
        }
    }
    if (target == nullptr || target == scePthreadSelf()) return Deliver(guest);
    return Signal(target, guest);
}

void DeliverPending(std::uint32_t signals, std::vector<std::pair<std::int64_t, std::uint32_t>>& others) {
    while (signals != 0) {
        const int guest = std::countr_zero(signals) + 1;
        signals &= ~Bit(guest);
        Deliver(guest);
    }
    for (auto [id, pending] : others) {
        const Pthread thread = AcquireGuestThreadById(id);
        if (thread == nullptr) continue;
        struct Reference {
            Pthread thread;
            ~Reference() { ReleaseGuestThread(thread); }
        } reference{thread};
        while (pending != 0 && !thread->_finished.load(std::memory_order_acquire)) {
            const int guest = std::countr_zero(pending) + 1;
            pending &= ~Bit(guest);
            Signal(thread, guest);
        }
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
    const int error = Post(scePthreadSelf(), guest, SiLwp);
    if (error == 0) return 0;
    *__error_nid_postfix() = error;
    return -1;
}
int APS5_VABI kill_nid_postfix(int pid, int guest) {
    if (guest < 0 || guest > MaxSignal) { *__error_nid_postfix() = 22; return -1; }
    const int self = getpid_nid_postfix();
    if (pid != self && pid != 0 && pid != -self) { *__error_nid_postfix() = 3; return -1; }
    if (guest == 0) return 0;
    const int error = Post(nullptr, guest, SiUser);
    if (error == 0) return 0;
    *__error_nid_postfix() = error;
    return -1;
}
int APS5_VABI pthread_kill_nid_postfix(Pthread thread, int guest) {
    if (guest < 0 || guest > MaxSignal) return PosixThread::GUEST_EINVAL;
    if (thread == nullptr) return PosixThread::GUEST_ESRCH;
    if (thread == scePthreadSelf()) return guest == 0 ? 0 : Post(thread, guest, SiLwp);
    if (!AcquireGuestThread(thread)) return PosixThread::GUEST_ESRCH;
    struct Reference {
        Pthread thread;
        ~Reference() { ReleaseGuestThread(thread); }
    } reference{thread};
    if (thread->_finished.load(std::memory_order_acquire)) return PosixThread::GUEST_ESRCH;
    return guest == 0 ? 0 : Post(thread, guest, SiLwp);
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
    const std::int64_t self = scePthreadSelf()->tid;
    std::uint32_t unblocked = 0;
    std::vector<std::pair<std::int64_t, std::uint32_t>> others;
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
        const std::uint32_t open = ~blockedMask.load();
        unblocked = processPending.signals & open;
        processPending.signals &= ~unblocked;
        for (auto entry = threadPending.begin(); entry != threadPending.end();) {
            const std::uint32_t signals = entry->second.signals & open;
            entry->second.signals &= ~signals;
            if (entry->first == self) unblocked |= signals;
            else if (signals != 0) others.emplace_back(entry->first, signals);
            entry = entry->second.signals == 0 ? threadPending.erase(entry) : std::next(entry);
        }
    }
    DeliverPending(unblocked, others);
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
    const std::int64_t self = scePthreadSelf()->tid;
    std::unique_lock lock(registration);
    for (;;) {
        const auto own = threadPending.find(self);
        const std::uint32_t mine = own != threadPending.end() ? own->second.signals : 0;
        if (const std::uint32_t ready = (mine | processPending.signals) & waited) {
            const int guest = std::countr_zero(ready) + 1;
            PendingSet& from = (mine & Bit(guest)) != 0 ? own->second : processPending;
            from.signals &= ~Bit(guest);
            const int code = from.codes[guest];
            if (own != threadPending.end() && own->second.signals == 0) threadPending.erase(own);
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
