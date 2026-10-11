#include "prx/libc/include/general/VabiMacros.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <thread>
struct GuestSignalSet {
    std::uint32_t bits[4];
};
struct GuestTimespec {
    std::int64_t seconds;
    std::int64_t nanoseconds;
};
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
static_assert(sizeof(GuestSiginfo) == 0x50);
using Handler = void (APS5_VABI *)(int);
extern "C" {
int APS5_VABI sigtimedwait_nid_postfix(const GuestSignalSet*, GuestSiginfo*, const GuestTimespec*);
int APS5_VABI sigprocmask_nid_postfix(int, const void*, void*);
Handler APS5_VABI signal_nid_postfix(int, Handler);
int APS5_VABI raise_nid_postfix(int);
int APS5_VABI kill_nid_postfix(int, int);
int APS5_VABI getpid_nid_postfix(void);
void* APS5_VABI scePthreadSelf(void);
int APS5_VABI pthread_kill_nid_postfix(void*, int);
int* APS5_VABI __error_nid_postfix();
}
namespace {
constexpr int SiUser = 0x10001;
constexpr int SiLwp = 0x10007;
volatile std::sig_atomic_t received = 0;
volatile std::sig_atomic_t deliveries = 0;
void APS5_VABI Callback(int value) {
    received = value;
    deliveries = deliveries + 1;
}
void Require(bool value) { if (!value) std::abort(); }
GuestSignalSet Set(std::uint32_t bits) { return {{bits, 0, 0, 0}}; }
constexpr std::uint32_t Int = 1u << 1;
constexpr std::uint32_t Term = 1u << 14;
const GuestTimespec poll{0, 0};
bool TimesOut(const GuestSignalSet& set, const GuestTimespec* timeout) {
    *__error_nid_postfix() = 0;
    return sigtimedwait_nid_postfix(&set, nullptr, timeout) == -1 && *__error_nid_postfix() == 35;
}
}
int main() {
    const GuestSignalSet pipe = Set(1u << 12);
    const GuestSignalSet term = Set(Term);
    const GuestSignalSet both = Set(Int | Term);
    GuestSiginfo info{};
    Require(TimesOut(pipe, &poll));
    Require(sigtimedwait_nid_postfix(nullptr, &info, &poll) == -1 && *__error_nid_postfix() == 14);
    const GuestTimespec invalid{0, 1000000000};
    *__error_nid_postfix() = 0;
    Require(sigtimedwait_nid_postfix(&pipe, &info, &invalid) == -1 && *__error_nid_postfix() == 22);
    const GuestTimespec negative{-1, 0};
    Require(TimesOut(pipe, &negative));
    const GuestTimespec brief{0, 50000000};
    const auto start = std::chrono::steady_clock::now();
    Require(TimesOut(pipe, &brief));
    Require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(50));

    Require(signal_nid_postfix(2, Callback) != reinterpret_cast<Handler>(static_cast<std::uintptr_t>(-1)));
    Require(signal_nid_postfix(15, Callback) != reinterpret_cast<Handler>(static_cast<std::uintptr_t>(-1)));
    Require(sigprocmask_nid_postfix(1, &both, nullptr) == 0);
    Require(raise_nid_postfix(15) == 0 && raise_nid_postfix(15) == 0 && deliveries == 0);
    info.signo = -1;
    info.reason[4] = 1;
    Require(sigtimedwait_nid_postfix(&term, &info, &poll) == 15);
    Require(info.signo == 15 && info.error == 0 && info.code == SiLwp && info.pid == getpid_nid_postfix() && info.uid == 0);
    Require(info.status == 0 && info.address == 0 && info.value == 0 && info.reason[4] == 0);
    Require(TimesOut(term, &poll) && deliveries == 0);

    Require(kill_nid_postfix(getpid_nid_postfix(), 15) == 0 && raise_nid_postfix(2) == 0 && deliveries == 0);
    Require(sigtimedwait_nid_postfix(&term, &info, &invalid) == 15 && info.code == SiUser);
    Require(sigtimedwait_nid_postfix(&both, &info, nullptr) == 2 && info.signo == 2);

    Require(raise_nid_postfix(2) == 0 && raise_nid_postfix(15) == 0);
    Require(sigtimedwait_nid_postfix(&both, nullptr, &poll) == 2);
    Require(sigtimedwait_nid_postfix(&both, nullptr, &poll) == 15);

    Require(raise_nid_postfix(15) == 0 && deliveries == 0);
    Require(sigprocmask_nid_postfix(2, &term, nullptr) == 0);
    Require(deliveries == 1 && received == 15);
    Require(TimesOut(term, &poll));
    Require(sigprocmask_nid_postfix(1, &term, nullptr) == 0);

    const auto ignore = reinterpret_cast<Handler>(std::uintptr_t{1});
    Require(raise_nid_postfix(15) == 0);
    Require(signal_nid_postfix(15, ignore) == Callback);
    Require(TimesOut(term, &poll));
    Require(raise_nid_postfix(15) == 0);
    Require(signal_nid_postfix(15, Callback) == ignore);
    Require(TimesOut(term, &poll));

    Require(raise_nid_postfix(30) == -1 && *__error_nid_postfix() == 22);

    int waited = 0;
    Require(raise_nid_postfix(15) == 0);
    std::thread other([&] { waited = TimesOut(term, &poll) ? 0 : -1; });
    other.join();
    Require(waited == 0);
    Require(sigtimedwait_nid_postfix(&term, nullptr, &poll) == 15);
    std::thread waiter([&] {
        const GuestTimespec ten{10, 0};
        waited = sigtimedwait_nid_postfix(&term, &info, &ten);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Require(kill_nid_postfix(getpid_nid_postfix(), 15) == 0);
    waiter.join();
    Require(waited == 15 && info.code == SiUser);
    std::atomic<void*> target{nullptr};
    std::thread forever([&] {
        target.store(scePthreadSelf());
        waited = sigtimedwait_nid_postfix(&both, &info, nullptr);
    });
    while (target.load() == nullptr) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Require(pthread_kill_nid_postfix(target.load(), 2) == 0);
    forever.join();
    Require(waited == 2 && info.code == SiLwp);

    deliveries = 0;
    Require(raise_nid_postfix(2) == 0 && raise_nid_postfix(15) == 0);
    Require(sigprocmask_nid_postfix(2, &both, nullptr) == 0);
    Require(deliveries == 2);
    Require(TimesOut(both, &poll));
}
