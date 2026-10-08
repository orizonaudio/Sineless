// Audio-thread benchmark: times every process() call of real Processor instances, driven
// through the VST3 API as a host would, against the block's deadline (block size / sample rate),
// at every common block size and sample rate. Reports the median, p99.9 and maximum per call, and
// the share of the deadline that all instances together take at p99.9 and at worst.
//
//   process_bench [--instances=8] [--seconds=60] [--max-share=0.05]
//
// Exits with 1 if, anywhere, all instances together take more than --max-share of the deadline
// at p99.9 (the maximum is reported but not judged: one preempted call says little).
#include "Processor.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <numbers>
#include <span>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

struct Options
{
    int instances = 8;
    double seconds = 60.0;
    double maxShare = 0.05;
};

Options parse(std::span<const char* const> arguments)
{
    Options options;
    for (const char* const text : arguments.subspan(1)) {
        const std::string argument = text;
        const auto equals = argument.find('=');
        const std::string key = argument.substr(0, equals);
        const std::string value = equals == std::string::npos ? "" : argument.substr(equals + 1);
        if (key == "--instances") {
            options.instances = std::stoi(value);
        } else if (key == "--seconds") {
            options.seconds = std::stod(value);
        } else if (key == "--max-share") {
            options.maxShare = std::stod(value);
        }
    }
    return options;
}

IPtr<orizon::plugin::Processor> makeProcessor(double sampleRate, int32 blockSize)
{
    auto processor = owned(new orizon::plugin::Processor());
    SpeakerArrangement stereo = SpeakerArr::kStereo;
    processor->initialize(nullptr);
    processor->setBusArrangements(&stereo, 1, &stereo, 1);
    ProcessSetup setup{.processMode = kRealtime,
                       .symbolicSampleSize = kSample32,
                       .maxSamplesPerBlock = blockSize,
                       .sampleRate = sampleRate};
    processor->setupProcessing(setup);
    processor->setActive(true);
    processor->setProcessing(true);
    return processor;
}

struct Result
{
    double median = 0.0; // Per call, in microseconds.
    double p999 = 0.0;
    double max = 0.0;
    double groupP999Share = 0.0; // All instances in one block, as a share of the deadline.
    double groupMaxShare = 0.0;
};

double percentile(std::vector<double>& values, double fraction)
{
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index),
                     values.end());
    return values[index];
}

Result measure(const Options& options, double sampleRate, int32 blockSize)
{
    std::vector<IPtr<orizon::plugin::Processor>> processors;
    processors.reserve(static_cast<std::size_t>(options.instances));
    for (int i = 0; i < options.instances; ++i) {
        processors.push_back(makeProcessor(sampleRate, blockSize));
    }
    std::vector<float> left(static_cast<std::size_t>(blockSize));
    std::vector<float> right(static_cast<std::size_t>(blockSize));
    std::array<float*, 2> channels{left.data(), right.data()};
    AudioBusBuffers input{};
    input.numChannels = 2;
    input.channelBuffers32 = channels.data();
    AudioBusBuffers output = input; // In place, as many hosts do.
    ProcessContext context{};
    context.state = static_cast<uint32>(ProcessContext::kPlaying) |
                    static_cast<uint32>(ProcessContext::kProjectTimeMusicValid) |
                    static_cast<uint32>(ProcessContext::kTempoValid) |
                    static_cast<uint32>(ProcessContext::kTimeSigValid);
    context.sampleRate = sampleRate;
    context.tempo = 120.0;
    context.timeSigNumerator = 4;
    context.timeSigDenominator = 4;
    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = blockSize;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;
    data.processContext = &context;

    const auto blocks = static_cast<std::size_t>(options.seconds * sampleRate / blockSize);
    const auto warmUp = static_cast<std::size_t>(sampleRate / blockSize); // One second.
    std::vector<double> calls;
    std::vector<double> groups;
    calls.reserve(blocks * processors.size());
    groups.reserve(blocks);
    std::int64_t sample = 0;
    for (std::size_t block = 0; block < warmUp + blocks; ++block) {
        for (int32 i = 0; i < blockSize; ++i) {
            const double t = static_cast<double>(sample + i) / sampleRate;
            left[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * t));
            right[static_cast<std::size_t>(i)] = left[static_cast<std::size_t>(i)];
        }
        context.projectTimeSamples = sample;
        context.projectTimeMusic = static_cast<double>(sample) / sampleRate * 2.0;
        double group = 0.0;
        for (auto& processor : processors) {
            const auto start = std::chrono::steady_clock::now();
            processor->process(data);
            const double elapsed =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                    .count();
            group += elapsed;
            if (block >= warmUp) {
                calls.push_back(elapsed);
            }
        }
        if (block >= warmUp) {
            groups.push_back(group);
        }
        sample += blockSize;
    }
    for (auto& processor : processors) {
        processor->setProcessing(false);
        processor->setActive(false);
        processor->terminate();
    }

    const double deadline = 1e6 * blockSize / sampleRate; // Microseconds.
    Result result;
    result.max = *std::ranges::max_element(calls);
    result.median = percentile(calls, 0.5);
    result.p999 = percentile(calls, 0.999);
    result.groupMaxShare = *std::ranges::max_element(groups) / deadline;
    result.groupP999Share = percentile(groups, 0.999) / deadline;
    return result;
}

int run(const Options& options)
{
    constexpr std::array kRates{44100.0, 48000.0, 96000.0, 192000.0};
    constexpr std::array kBlocks{32, 64, 128, 256, 512, 1024, 2048};
    std::printf("%d instance(s), %.0f s of audio per case; times per call in us; share = all "
                "instances in one block / deadline\n",
                options.instances, options.seconds);
    std::printf("%8s %6s %9s %8s %8s %8s %10s %9s\n", "rate", "block", "deadline", "median",
                "p99.9", "max", "share@p99.9", "share@max");
    double worst = 0.0;
    for (const double rate : kRates) {
        for (const int32 block : kBlocks) {
            const Result result = measure(options, rate, block);
            std::printf("%8.0f %6d %9.1f %8.2f %8.2f %8.1f %9.2f%% %8.2f%%\n", rate, block,
                        1e6 * block / rate, result.median, result.p999, result.max,
                        100.0 * result.groupP999Share, 100.0 * result.groupMaxShare);
            worst = std::max(worst, result.groupP999Share);
        }
    }
    std::printf("worst share at p99.9: %.2f%% (budget %.2f%%)\n", 100.0 * worst,
                100.0 * options.maxShare);
    return worst <= options.maxShare ? 0 : 1;
}

} // namespace

int main(int argc, const char* argv[])
{
    try {
        return run(parse(std::span(argv, static_cast<std::size_t>(argc))));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "process_bench: %s\n", error.what());
        return 1;
    }
}
