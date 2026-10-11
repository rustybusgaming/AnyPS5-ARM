#include "SceTypes.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>

extern "C" {
int APS5_VABI sceContentSearchInit(const ContentSearchInitParam*);
int APS5_VABI sceContentSearchTerm();
int APS5_VABI sceContentSearchSearchContent(const void*, std::uint32_t, const void*, std::uint32_t, std::uint32_t, std::uint32_t, std::int64_t*, void*, std::int64_t*);
int APS5_VABI sceContentSearchOpenMetadata(const char*, int*);
int APS5_VABI sceContentSearchCloseMetadata(int);
int APS5_VABI sceContentSearchGetMetadataFieldInfo(int, const char*, int*, int*);
int APS5_VABI sceContentSearchGetMetadataValue(int, const char*, void*);
int APS5_VABI sceContentSearchGetContentLastUpdateId(std::int64_t*);
int APS5_VABI sceContentSearchOpenMetadataByContentId(std::int64_t, int*);
int APS5_VABI sceContentSearchGetTotalContentSize(const void*, std::uint32_t, std::int64_t*, std::int64_t*);
int APS5_VABI sceContentSearchGetNumOfContent(const void*, std::uint32_t, std::int64_t*, std::int64_t*);
int APS5_VABI sceContentDeleteInitialize(const ContentDeleteInitParam*);
int APS5_VABI sceContentDeleteTerminate();
int APS5_VABI sceContentDeleteByPath(const char*);
int APS5_VABI sceContentDeleteById(std::int64_t);
int APS5_VABI sceVideoRecordingGetStatus();
int APS5_VABI sceVideoRecordingStart();
int APS5_VABI sceVideoRecordingStop();
int APS5_VABI sceVideoRecordingClose(int);
}

static void Require(bool value) { if (!value) std::abort(); }

template <typename TError, typename TAction>
static bool Throws(TAction action) {
    try {
        action();
    } catch (const TError&) {
        return true;
    }
    return false;
}

static void CheckContentSearch() {
    constexpr int notInitialized = static_cast<int>(0x809D1001);
    constexpr int limitTooBig = static_cast<int>(0x809D100A);
    unsigned char infos[2400 * 2];
    std::int64_t count = -1;
    std::int64_t updateId = -1;
    int metadata = -1;
    int type = 0;
    int size = 0;
    std::int64_t value[2]{};
    Require(sceContentSearchSearchContent(nullptr, 0, nullptr, 0, 0, 2, &count, infos, &updateId) == notInitialized);
    Require(sceContentSearchOpenMetadata("/data/a.png", &metadata) == notInitialized);
    Require(sceContentSearchCloseMetadata(1) == notInitialized);
    Require(sceContentSearchGetContentLastUpdateId(&updateId) == notInitialized && updateId == -1);
    Require(sceContentSearchOpenMetadataByContentId(7, &metadata) == notInitialized);
    Require(sceContentSearchGetNumOfContent(nullptr, 0, &count, &updateId) == notInitialized && count == -1);
    Require(sceContentSearchGetTotalContentSize(nullptr, 0, &count, &updateId) == notInitialized && count == -1);

    ContentSearchInitParam param{0x10000};
    Require(sceContentSearchInit(&param) == 0);
    Require(Throws<std::invalid_argument>([&] { sceContentSearchSearchContent(nullptr, 0, nullptr, 0, 0, 2, nullptr, infos, &updateId); }));
    Require(Throws<std::invalid_argument>([&] { sceContentSearchSearchContent(nullptr, 1, nullptr, 0, 0, 2, &count, infos, &updateId); }));
    Require(Throws<std::invalid_argument>([&] { sceContentSearchSearchContent(nullptr, 0, nullptr, 0, 0, 2, &count, nullptr, &updateId); }));
    Require(sceContentSearchSearchContent(nullptr, 0, nullptr, 0, 0, 93, &count, infos, &updateId) == limitTooBig);
    Require(sceContentSearchSearchContent(nullptr, 0, nullptr, 0, 0, 2, &count, infos, &updateId) == 0);
    Require(count == 0 && updateId == 0);
    count = -1;
    Require(sceContentSearchSearchContent(nullptr, 0, nullptr, 0, 5, 0, &count, nullptr, nullptr) == 0 && count == 0);

    Require(Throws<std::invalid_argument>([&] { sceContentSearchOpenMetadata(nullptr, &metadata); }));
    Require(Throws<std::runtime_error>([&] { sceContentSearchOpenMetadata("/data/a.png", &metadata); }));
    Require(metadata == -1);
    Require(Throws<std::invalid_argument>([] { sceContentSearchCloseMetadata(1); }));
    Require(Throws<std::invalid_argument>([&] { sceContentSearchGetMetadataFieldInfo(1, "title", &type, &size); }));
    Require(Throws<std::invalid_argument>([&] { sceContentSearchGetMetadataValue(1, "title", value); }));

    Require(Throws<std::invalid_argument>([] { sceContentSearchGetContentLastUpdateId(nullptr); }));
    updateId = -1;
    Require(sceContentSearchGetContentLastUpdateId(&updateId) == 0 && updateId == 0);
    Require(Throws<std::invalid_argument>([] { sceContentSearchOpenMetadataByContentId(7, nullptr); }));
    Require(Throws<std::runtime_error>([&] { sceContentSearchOpenMetadataByContentId(7, &metadata); }));
    Require(metadata == -1);
    const unsigned char columns[16] = {};
    Require(Throws<std::invalid_argument>([&] { sceContentSearchGetNumOfContent(nullptr, 0, nullptr, &updateId); }));
    Require(Throws<std::invalid_argument>([&] { sceContentSearchGetNumOfContent(nullptr, 1, &count, &updateId); }));
    count = updateId = -1;
    Require(sceContentSearchGetNumOfContent(columns, 1, &count, &updateId) == 0 && count == 0 && updateId == 0);
    count = -1;
    Require(sceContentSearchGetNumOfContent(nullptr, 0, &count, nullptr) == 0 && count == 0);
    Require(Throws<std::invalid_argument>([&] { sceContentSearchGetTotalContentSize(nullptr, 0, nullptr, &updateId); }));
    Require(Throws<std::invalid_argument>([&] { sceContentSearchGetTotalContentSize(nullptr, 1, &count, &updateId); }));
    count = updateId = -1;
    Require(sceContentSearchGetTotalContentSize(columns, 1, &count, &updateId) == 0 && count == 0 && updateId == 0);
    Require(sceContentSearchTerm() == 0);
}

static void CheckContentDelete() {
    Require(Throws<std::invalid_argument>([] { sceContentDeleteByPath(nullptr); }));
    Require(Throws<std::logic_error>([] { sceContentDeleteByPath("/data/a.png"); }));
    Require(Throws<std::logic_error>([] { sceContentDeleteById(7); }));
    ContentDeleteInitParam param{};
    Require(sceContentDeleteInitialize(&param) == 0);
    Require(Throws<std::runtime_error>([] { sceContentDeleteByPath("/data/a.png"); }));
    Require(Throws<std::runtime_error>([] { sceContentDeleteById(7); }));
    Require(sceContentDeleteTerminate() == 0);
}

static void CheckVideoRecording() {
    Require(sceVideoRecordingGetStatus() == 0);
    Require(Throws<std::logic_error>([] { sceVideoRecordingStart(); }));
    Require(Throws<std::logic_error>([] { sceVideoRecordingStop(); }));
    Require(Throws<std::logic_error>([] { sceVideoRecordingClose(1); }));
}

int main() {
    CheckContentSearch();
    CheckContentDelete();
    CheckVideoRecording();
    return 0;
}
