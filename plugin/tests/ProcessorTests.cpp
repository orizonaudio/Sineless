#include "AllocationCounter.h"
#include "Controller.h"
#include "Processor.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <tuple>
#include <vector>

namespace orizon::plugin {
namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

constexpr int32 kBlockSize = 512;

IPtr<Processor> makeProcessor(SpeakerArrangement arrangement, int32 sampleSize)
{
    auto processor = owned(new Processor());
    EXPECT_EQ(processor->initialize(nullptr), kResultOk);
    EXPECT_EQ(processor->setBusArrangements(&arrangement, 1, &arrangement, 1), kResultTrue);
    ProcessSetup setup{.processMode = kRealtime,
                       .symbolicSampleSize = sampleSize,
                       .maxSamplesPerBlock = kBlockSize,
                       .sampleRate = 48000.0};
    EXPECT_EQ(processor->setupProcessing(setup), kResultOk);
    EXPECT_EQ(processor->setActive(true), kResultOk);
    // The SDK default returns kNotImplemented, which the VST3 spec allows.
    processor->setProcessing(true);
    return processor;
}

// Random bit patterns, including NaNs, infinities and denormals, so that any
// arithmetic on the signal path would show up as a mismatch.
template <typename Sample> std::vector<Sample> randomBits(size_t count, unsigned seed)
{
    std::vector<Sample> samples(count);
    std::mt19937_64 random(seed);
    for (Sample& sample : samples) {
        const uint64_t bits = random();
        std::memcpy(&sample, &bits, sizeof(Sample));
    }
    return samples;
}

template <typename Sample> void expectBitExactPassThrough(int32 numChannels, bool inPlace)
{
    const SpeakerArrangement arrangement =
        numChannels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
    const int32 sampleSize = sizeof(Sample) == sizeof(Sample64) ? kSample64 : kSample32;
    auto processor = makeProcessor(arrangement, sampleSize);

    std::vector<std::vector<Sample>> inputChannels;
    std::vector<std::vector<Sample>> outputChannels;
    std::vector<Sample*> inputPointers;
    std::vector<Sample*> outputPointers;
    for (int32 channel = 0; channel < numChannels; ++channel) {
        inputChannels.push_back(
            randomBits<Sample>(kBlockSize, 17u + static_cast<unsigned>(channel)));
        outputChannels.emplace_back(kBlockSize, Sample{});
    }
    const auto expected = inputChannels;
    for (int32 channel = 0; channel < numChannels; ++channel) {
        inputPointers.push_back(inputChannels[static_cast<size_t>(channel)].data());
        outputPointers.push_back(inPlace ? inputPointers.back()
                                         : outputChannels[static_cast<size_t>(channel)].data());
    }

    AudioBusBuffers input{};
    AudioBusBuffers output{};
    input.numChannels = numChannels;
    output.numChannels = numChannels;
    if constexpr (sizeof(Sample) == sizeof(Sample64)) {
        input.channelBuffers64 = inputPointers.data();
        output.channelBuffers64 = outputPointers.data();
    } else {
        input.channelBuffers32 = inputPointers.data();
        output.channelBuffers32 = outputPointers.data();
    }

    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = sampleSize;
    data.numSamples = kBlockSize;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;

    tresult result = kResultFalse;
    std::size_t allocations = 0;
    std::size_t deallocations = 0;
    {
        testing::AllocationCounter counter;
        result = processor->process(data);
        allocations = counter.allocations();
        deallocations = counter.deallocations();
    }
    ASSERT_EQ(result, kResultOk);
    EXPECT_EQ(allocations, 0u) << "process() must not allocate (QG-1)";
    EXPECT_EQ(deallocations, 0u) << "process() must not deallocate (QG-1)";

    using Bits = std::conditional_t<sizeof(Sample) == sizeof(uint64_t), uint64_t, uint32_t>;
    const auto channelCount = static_cast<size_t>(numChannels);
    const auto sampleCount = static_cast<size_t>(kBlockSize);
    for (size_t channel = 0; channel < channelCount; ++channel) {
        for (size_t index = 0; index < sampleCount; ++index) {
            ASSERT_EQ(std::bit_cast<Bits>(outputPointers[channel][index]),
                      std::bit_cast<Bits>(expected[channel][index]))
                << "channel " << channel << ", sample " << index;
        }
    }
}

class PassThrough : public ::testing::TestWithParam<std::tuple<bool, int32, bool>>
{
};

TEST_P(PassThrough, IsBitExact)
{
    const auto [is64Bit, numChannels, inPlace] = GetParam();
    if (is64Bit) {
        expectBitExactPassThrough<Sample64>(numChannels, inPlace);
    } else {
        expectBitExactPassThrough<Sample32>(numChannels, inPlace);
    }
}

INSTANTIATE_TEST_SUITE_P(AllLayouts, PassThrough,
                         ::testing::Combine(::testing::Bool(), ::testing::Values(1, 2),
                                            ::testing::Bool()));

TEST(Processor, AcceptsOnlyMatchingMonoOrStereoBuses)
{
    auto processor = owned(new Processor());
    ASSERT_EQ(processor->initialize(nullptr), kResultOk);

    SpeakerArrangement mono = SpeakerArr::kMono;
    SpeakerArrangement stereo = SpeakerArr::kStereo;
    SpeakerArrangement surround = SpeakerArr::k51;
    std::array<SpeakerArrangement, 2> twoStereo = {SpeakerArr::kStereo, SpeakerArr::kStereo};

    EXPECT_EQ(processor->setBusArrangements(&mono, 1, &mono, 1), kResultTrue);
    EXPECT_EQ(processor->setBusArrangements(&stereo, 1, &stereo, 1), kResultTrue);
    EXPECT_EQ(processor->setBusArrangements(&mono, 1, &stereo, 1), kResultFalse);
    EXPECT_EQ(processor->setBusArrangements(&surround, 1, &surround, 1), kResultFalse);
    EXPECT_EQ(processor->setBusArrangements(twoStereo.data(), 2, &stereo, 1), kResultFalse);
}

TEST(Processor, PassesTheInputChannelCountToTheEngine)
{
    auto processor = owned(new Processor());
    ASSERT_EQ(processor->initialize(nullptr), kResultOk);
    SpeakerArrangement mono = SpeakerArr::kMono;
    ASSERT_EQ(processor->setBusArrangements(&mono, 1, &mono, 1), kResultTrue);
    EXPECT_EQ(processor->engine()->inputChannels(), 1);
    SpeakerArrangement stereo = SpeakerArr::kStereo;
    ASSERT_EQ(processor->setBusArrangements(&stereo, 1, &stereo, 1), kResultTrue);
    EXPECT_EQ(processor->engine()->inputChannels(), 2);
}

TEST(Processor, Supports32And64BitProcessing)
{
    auto processor = owned(new Processor());
    EXPECT_EQ(processor->canProcessSampleSize(kSample32), kResultTrue);
    EXPECT_EQ(processor->canProcessSampleSize(kSample64), kResultTrue);
}

TEST(Processor, ReportsZeroLatency)
{
    auto processor = makeProcessor(SpeakerArr::kStereo, kSample32);
    EXPECT_EQ(processor->getLatencySamples(), 0u);
}

TEST(Processor, PropagatesSilenceFlags)
{
    auto processor = makeProcessor(SpeakerArr::kStereo, kSample32);
    std::vector<Sample32> left(kBlockSize, 0.0f);
    std::vector<Sample32> right(kBlockSize, 0.0f);
    std::array<Sample32*, 2> channels = {left.data(), right.data()};

    AudioBusBuffers input{};
    input.numChannels = 2;
    input.silenceFlags = 0b11;
    input.channelBuffers32 = channels.data();
    AudioBusBuffers output{};
    output.numChannels = 2;
    output.channelBuffers32 = channels.data();

    ProcessData data{};
    data.symbolicSampleSize = kSample32;
    data.numSamples = kBlockSize;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;
    ASSERT_EQ(processor->process(data), kResultOk);
    EXPECT_EQ(output.silenceFlags, 0b11u);
}

TEST(Processor, AcceptsFlushCallWithoutBuffers)
{
    auto processor = makeProcessor(SpeakerArr::kStereo, kSample32);
    ProcessData data{};
    data.symbolicSampleSize = kSample32;
    EXPECT_EQ(processor->process(data), kResultOk);
}

TEST(Processor, CapturesInputIntoTheRing)
{
    auto processor = makeProcessor(SpeakerArr::kStereo, kSample32);
    std::vector<Sample32> left(kBlockSize);
    std::vector<Sample32> right(kBlockSize);
    for (size_t i = 0; i < left.size(); ++i) {
        left[i] = static_cast<float>(i);
        right[i] = -static_cast<float>(i);
    }
    std::array<Sample32*, 2> channels = {left.data(), right.data()};
    AudioBusBuffers input{};
    input.numChannels = 2;
    input.channelBuffers32 = channels.data();
    AudioBusBuffers output = input;

    ProcessData data{};
    data.symbolicSampleSize = kSample32;
    data.numSamples = kBlockSize;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;
    ASSERT_EQ(processor->process(data), kResultOk);

    ASSERT_NE(processor->engine(), nullptr);
    const engine::CaptureRing* ring = &processor->engine()->captureRing();
    EXPECT_EQ(ring->writeIndex(), static_cast<uint64>(kBlockSize));
    std::vector<float> captured(kBlockSize);
    ASSERT_TRUE(ring->read(0, 0, captured));
    EXPECT_EQ(captured, left);
    ASSERT_TRUE(ring->read(1, 0, captured));
    EXPECT_EQ(captured, right);
}

TEST(Processor, PassesTheSampleRateToTheEngine)
{
    auto processor = owned(new Processor());
    ASSERT_EQ(processor->initialize(nullptr), kResultOk);
    ProcessSetup setup{.processMode = kRealtime,
                       .symbolicSampleSize = kSample32,
                       .maxSamplesPerBlock = kBlockSize,
                       .sampleRate = 96000.0};
    ASSERT_EQ(processor->setupProcessing(setup), kResultOk);
    EXPECT_EQ(processor->engine()->sampleRate(), 96000.0);
}

TEST(Processor, CapturesSilentChannelsAsZeros)
{
    auto processor = makeProcessor(SpeakerArr::kStereo, kSample32);
    std::vector<Sample32> garbage(kBlockSize, 0.75f);
    std::array<Sample32*, 2> channels = {garbage.data(), garbage.data()};
    AudioBusBuffers input{};
    input.numChannels = 2;
    input.silenceFlags = 0b10; // Right channel flagged silent.
    input.channelBuffers32 = channels.data();
    AudioBusBuffers output = input;

    ProcessData data{};
    data.symbolicSampleSize = kSample32;
    data.numSamples = kBlockSize;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;
    ASSERT_EQ(processor->process(data), kResultOk);

    std::vector<float> captured(kBlockSize);
    ASSERT_TRUE(processor->engine()->captureRing().read(0, 0, captured));
    EXPECT_EQ(captured, garbage);
    ASSERT_TRUE(processor->engine()->captureRing().read(1, 0, captured));
    EXPECT_EQ(captured, std::vector<float>(kBlockSize, 0.0f));
}

TEST(Processor, AdvancesTheTimelineFromTheProcessContext)
{
    auto processor = makeProcessor(SpeakerArr::kStereo, kSample32);
    std::vector<Sample32> silence(kBlockSize, 0.0f);
    std::array<Sample32*, 2> channels = {silence.data(), silence.data()};
    AudioBusBuffers input{};
    input.numChannels = 2;
    input.channelBuffers32 = channels.data();
    AudioBusBuffers output = input;
    ProcessContext context{};
    context.state = static_cast<uint32>(ProcessContext::kPlaying) |
                    static_cast<uint32>(ProcessContext::kProjectTimeMusicValid) |
                    static_cast<uint32>(ProcessContext::kTempoValid) |
                    static_cast<uint32>(ProcessContext::kTimeSigValid);
    context.projectTimeMusic = 32.0;
    context.tempo = 140.0;
    context.timeSigNumerator = 7;
    context.timeSigDenominator = 8;

    ProcessData data{};
    data.symbolicSampleSize = kSample32;
    data.numSamples = kBlockSize;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;
    data.processContext = &context;
    ASSERT_EQ(processor->process(data), kResultOk);

    const engine::MusicalStamp stamp = processor->engine()->timeline().latest();
    EXPECT_TRUE(stamp.playing);
    EXPECT_EQ(stamp.quarterNotes, 32.0);
    EXPECT_EQ(stamp.tempo, 140.0);
    EXPECT_EQ(stamp.numerator, 7);
    EXPECT_EQ(stamp.denominator, 8);
}

TEST(Controller, DeclaresNoParameters)
{
    auto controller = owned(new Controller());
    ASSERT_EQ(controller->initialize(nullptr), kResultOk);
    EXPECT_EQ(controller->getParameterCount(), 0);
    EXPECT_EQ(controller->terminate(), kResultOk);
}

} // namespace
} // namespace orizon::plugin
