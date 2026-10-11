#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace OpusDecoding {

struct Decoder;

class Library {
public:
    Library(std::string name, bool celtOnly);
    ~Library();

    int Initialize(std::uint32_t* context);
    int Terminate(std::uint32_t* context);
    int GetSize(int channels) const;
    int CreateEx(std::uint32_t* context, void* state, int sampleRate, int channels);
    int Decode(void* state, const std::uint8_t* data, int bytes, std::int16_t* pcm, int capacity);
    int Destroy(void* state);

private:
    [[noreturn]] void fail(const std::string& message) const;
    int packetSamples(const std::uint8_t* packet, int size) const;

    std::string name;
    bool celtOnly;
    std::mutex mutex;
    std::unordered_map<std::uint32_t, std::uint32_t*> contexts;
    std::unordered_map<void*, std::unique_ptr<Decoder>> decoders;
    std::uint32_t nextContext = 1;
};

} // namespace OpusDecoding
