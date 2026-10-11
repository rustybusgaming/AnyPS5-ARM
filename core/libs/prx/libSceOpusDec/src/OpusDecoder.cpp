#include "prx/libSceOpusDec/include/OpusDecoder.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace OpusDecoding {

struct Decoder {
    std::uint32_t context;
    int channels;
    AVCodecContext* codec = nullptr;
    ~Decoder() { avcodec_free_context(&codec); }
};

Library::Library(std::string name, bool celtOnly) : name(std::move(name)), celtOnly(celtOnly) {}

Library::~Library() = default;

void Library::fail(const std::string& message) const {
    throw std::runtime_error(name + ": " + message);
}

int Library::packetSamples(const std::uint8_t* packet, int size) const {
    const unsigned config = packet[0] >> 3;
    if (celtOnly && config < 16) fail("SILK and hybrid packets are not CELT-only");
    const int frameSamples = config < 12 ? ((config & 3) == 3 ? 2880 : 480 << (config & 3))
                            : config < 16 ? 480 << (config & 1) : 120 << (config & 3);
    int frames = 1;
    switch (packet[0] & 3) {
    case 1:
    case 2: frames = 2; break;
    case 3:
        if (size < 2) fail("truncated packet frame count");
        frames = packet[1] & 63;
        break;
    }
    if (frames == 0 || frames * frameSamples > 5760) fail("invalid packet duration");
    return frames * frameSamples;
}

int Library::Initialize(std::uint32_t* context) {
    if (!context) fail("null context output");
    std::lock_guard lock(mutex);
    for (const auto& [id, owner] : contexts)
        if (owner == context) fail("context already initialized");
    if (nextContext == 0) fail("context identifiers exhausted");
    const auto id = nextContext++;
    contexts.emplace(id, context);
    *context = id;
    return 0;
}

int Library::Terminate(std::uint32_t* context) {
    if (!context) fail("null context");
    std::lock_guard lock(mutex);
    if (!contexts.contains(*context)) fail("unknown context");
    for (const auto& [state, decoder] : decoders)
        if (decoder->context == *context) fail("termination with live decoders is not implemented");
    contexts.erase(*context);
    return 0;
}

int Library::GetSize(int channels) const {
    if (channels != 1 && channels != 2) fail("only mono and stereo are supported");
    return 640;
}

int Library::CreateEx(std::uint32_t* context, void* state, int sampleRate, int channels) {
    if (!context || !state) fail("null context or state");
    GetSize(channels);
    if (sampleRate != 48000) fail("sample rates other than 48000 Hz are not implemented");
    std::lock_guard lock(mutex);
    if (!contexts.contains(*context)) fail("unknown context");
    if (decoders.contains(state)) fail("state already owns a decoder");
    auto decoder = std::make_unique<Decoder>();
    decoder->context = *context;
    decoder->channels = channels;
    const auto* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) fail("FFmpeg Opus decoder unavailable");
    decoder->codec = avcodec_alloc_context3(codec);
    if (!decoder->codec) throw std::bad_alloc();
    av_channel_layout_default(&decoder->codec->ch_layout, channels);
    decoder->codec->sample_rate = 48000;
    if (avcodec_open2(decoder->codec, codec, nullptr) < 0) fail("cannot open FFmpeg decoder");
    decoders.emplace(state, std::move(decoder));
    return 0;
}

int Library::Decode(void* state, const std::uint8_t* data, int bytes, std::int16_t* pcm, int capacity) {
    if (!data || bytes <= 0 || bytes > std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE || !pcm || capacity < 0)
        fail("invalid packet or PCM buffer; packet loss concealment is not implemented");
    std::lock_guard lock(mutex);
    const auto found = decoders.find(state);
    if (found == decoders.end()) fail("unknown decoder state");
    auto& decoder = *found->second;
    const int samples = packetSamples(data, bytes);
    const int outputBytes = samples * decoder.channels * 2;
    if (capacity / 2 * 2 < outputBytes) fail("PCM buffer is too small");
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> frame(av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
    if (!packet || !frame || av_new_packet(packet.get(), bytes) < 0) throw std::bad_alloc();
    std::memcpy(packet->data, data, bytes);
    if (avcodec_send_packet(decoder.codec, packet.get()) < 0 || avcodec_receive_frame(decoder.codec, frame.get()) < 0)
        fail("FFmpeg rejected the packet");
    const auto format = static_cast<AVSampleFormat>(frame->format);
    if (frame->nb_samples != samples || frame->ch_layout.nb_channels != decoder.channels || frame->sample_rate != 48000 ||
        (format != AV_SAMPLE_FMT_FLTP && format != AV_SAMPLE_FMT_FLT))
        fail("unexpected FFmpeg PCM format");
    for (int sample = 0; sample < samples; ++sample) {
        for (int channel = 0; channel < decoder.channels; ++channel) {
            const auto* source = reinterpret_cast<const float*>(frame->extended_data[format == AV_SAMPLE_FMT_FLTP ? channel : 0]);
            const float value = source[format == AV_SAMPLE_FMT_FLTP ? sample : sample * decoder.channels + channel];
            const auto converted = static_cast<std::int16_t>(std::lrint(std::clamp(value * 32768.0f, -32768.0f, 32767.0f)));
            std::memcpy(reinterpret_cast<std::uint8_t*>(pcm) + (sample * decoder.channels + channel) * 2, &converted, 2);
        }
    }
    return outputBytes;
}

int Library::Destroy(void* state) {
    std::lock_guard lock(mutex);
    if (decoders.erase(state) == 0) fail("unknown decoder state");
    return 0;
}

} // namespace OpusDecoding
