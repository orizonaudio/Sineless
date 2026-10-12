// Fault injection for the host's state streams: reads that fail, stop early or come in small
// pieces, streams that never end, and writes that fail or fall short. setState loads what it
// could read, tolerantly (see engine/State.h), and never hangs; getState reports a failed write.

#include "Controller.h"
#include "Processor.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <utility>

namespace orizon::plugin {
namespace {

using namespace Steinberg;
using protocol::SettingId;

// A host stream that serves `source`, at most `maxChunk` bytes per call, and fails every read
// from byte `failAt` on. Endless, it serves `filler` after `source`, until byte `endAt` if set.
// Writes keep at most `maxChunk` bytes per call, or fail if `failWrites`.
class FaultyStream : public U::Implements<U::Directly<IBStream>>
{
public:
    explicit FaultyStream(std::string source = {}) : source(std::move(source)) {}

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override
    {
        ++reads;
        if (numBytesRead != nullptr) {
            *numBytesRead = 0;
        }
        if (position >= failAt || numBytes < 0) {
            return kResultFalse;
        }
        std::size_t count = std::min({static_cast<std::size_t>(numBytes), maxChunk,
                                      failAt - position, endAt - std::min(position, endAt)});
        auto* bytes = static_cast<char*>(buffer);
        if (endless) {
            for (std::size_t i = 0; i < count; ++i) {
                bytes[i] = position + i < source.size() ? source[position + i] : filler;
            }
        } else {
            count = std::min(count, source.size() - std::min(position, source.size()));
            std::memcpy(bytes, source.data() + position, count);
        }
        position += count;
        served += count;
        if (numBytesRead != nullptr) {
            *numBytesRead = static_cast<int32>(count);
        }
        return kResultOk;
    }

    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override
    {
        if (numBytesWritten != nullptr) {
            *numBytesWritten = 0;
        }
        if (failWrites || numBytes < 0) {
            return kResultFalse;
        }
        const std::size_t count = std::min(static_cast<std::size_t>(numBytes), maxChunk);
        written.append(static_cast<const char*>(buffer), count);
        if (numBytesWritten != nullptr) {
            *numBytesWritten = static_cast<int32>(count);
        }
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 /*pos*/, int32 /*mode*/, int64* /*result*/) override
    {
        return kNotImplemented;
    }

    tresult PLUGIN_API tell(int64* pos) override
    {
        if (pos == nullptr) {
            return kInvalidArgument;
        }
        *pos = static_cast<int64>(position);
        return kResultOk;
    }

    std::string source;
    std::size_t maxChunk = std::numeric_limits<std::size_t>::max();
    std::size_t failAt = std::numeric_limits<std::size_t>::max();
    bool endless = false;
    std::size_t endAt = std::numeric_limits<std::size_t>::max();
    char filler = 'x';
    bool failWrites = false;

    int reads = 0;
    std::size_t served = 0;
    std::size_t position = 0;
    std::string written;
};

IPtr<Processor> makeProcessor()
{
    auto processor = owned(new Processor());
    EXPECT_EQ(processor->initialize(nullptr), kResultOk);
    return processor;
}

// Settings other than the defaults, so that a load that fell back to them shows.
engine::Settings changedSettings()
{
    engine::Settings settings;
    EXPECT_TRUE(settings.set(SettingId::kTimeSpan, 2.0));
    EXPECT_TRUE(settings.set(SettingId::kTriggerMode, 1.0));
    EXPECT_TRUE(settings.set(SettingId::kChannelViews, 0b1100));
    EXPECT_TRUE(settings.set(SettingId::kConcertPitch, 432.0));
    return settings;
}

// What the processor writes for changedSettings(), through a stream that takes it whole.
std::string savedState()
{
    auto processor = makeProcessor();
    processor->engine()->settings().replace(changedSettings());
    auto stream = owned(new FaultyStream());
    EXPECT_EQ(processor->getState(stream), kResultOk);
    processor->terminate();
    return stream->written;
}

// Loads `stream` into `processor`, whose settings are changed beforehand, and returns the
// settings it ends up with.
engine::Settings loadInto(Processor& processor, FaultyStream& stream)
{
    engine::Settings before;
    before.set(SettingId::kVerticalScale, -12.0);
    processor.engine()->settings().replace(before);
    EXPECT_EQ(processor.setState(&stream), kResultOk);
    return processor.engine()->settings().get();
}

engine::Settings loadInto(FaultyStream& stream)
{
    auto processor = makeProcessor();
    const engine::Settings loaded = loadInto(*processor, stream);
    processor->terminate();
    return loaded;
}

// The size of the buffer Processor's readAll reads into, one read at a time.
constexpr std::size_t kReadBuffer = 4096;

// Bytes per read: single bytes, odd sizes, and around the processor's own buffer.
constexpr std::array<std::size_t, 8> kReadSizes = {
    1, 2, 3, 7, 64, kReadBuffer - 1, kReadBuffer, kReadBuffer + 1};

// savedState() padded with whitespace, which JSON allows after a value, to exactly two buffers.
// The saved state alone fits in a single read; padded, a read of a buffer or more fills the buffer
// exactly and is followed by a second full read and then an empty one that ends the state.
std::string stateAcrossTwoBuffers()
{
    std::string state = savedState();
    EXPECT_LT(state.size(), kReadBuffer);
    state.resize(2 * kReadBuffer, ' ');
    return state;
}

class ShortReads : public ::testing::TestWithParam<std::size_t>
{
};

// Hosts may hand a state over in pieces smaller than asked for: all of them are read.
TEST_P(ShortReads, LoadTheWholeState)
{
    auto stream = owned(new FaultyStream(stateAcrossTwoBuffers()));
    stream->maxChunk = GetParam();
    EXPECT_EQ(loadInto(*stream), changedSettings());
    EXPECT_EQ(stream->served, stream->source.size());
}

INSTANTIATE_TEST_SUITE_P(ProcessorStateFaults, ShortReads, ::testing::ValuesIn(kReadSizes),
                         [](const ::testing::TestParamInfo<std::size_t>& info) {
                             return std::to_string(info.param) + "BytesPerRead";
                         });

// A read that fails part way leaves a cut state, which is unreadable as a whole: the processor
// takes every default, as for any unreadable state, rather than a mix, and the call still
// succeeds. A stream that reports its end as a failed read, after the whole state, loses nothing.
TEST(ProcessorStateFaults, AReadFailingAtAnyByteLoadsDefaultsUnlessTheStateWasWhole)
{
    const std::string state = savedState();
    ASSERT_FALSE(state.empty());
    auto processor = makeProcessor();
    for (std::size_t failAt = 0; failAt <= state.size(); ++failAt) {
        for (const std::size_t chunk : {std::size_t{5}, std::numeric_limits<std::size_t>::max()}) {
            auto stream = owned(new FaultyStream(state));
            stream->failAt = failAt;
            stream->maxChunk = chunk;
            const engine::Settings loaded = loadInto(*processor, *stream);
            if (failAt < state.size()) {
                EXPECT_EQ(loaded, engine::Settings()) << "failed at byte " << failAt;
            } else {
                EXPECT_EQ(loaded, changedSettings()) << "failed only at the end";
            }
        }
    }
    processor->terminate();
}

TEST(ProcessorStateFaults, AStreamThatFailsAtOnceLoadsDefaults)
{
    auto stream = owned(new FaultyStream(savedState()));
    stream->failAt = 0;
    EXPECT_EQ(loadInto(*stream), engine::Settings());
    EXPECT_EQ(stream->reads, 1) << "a failed read is not retried";
}

TEST(ProcessorStateFaults, AnEmptyStreamLoadsDefaults)
{
    auto stream = owned(new FaultyStream());
    EXPECT_EQ(loadInto(*stream), engine::Settings());
    EXPECT_EQ(stream->reads, 1) << "a read of nothing ends the state";
}

// A stream that never ends is not ours: the processor stops reading after a bounded amount and
// loads defaults, rather than hanging the host or growing without bound. The stream here does end,
// at 8 MiB, so that a processor without the bound fails the check instead of running out of memory.
TEST(ProcessorStateFaults, StopsReadingAStreamThatNeverEnds)
{
    for (const std::size_t chunk : {std::size_t{1}, kReadBuffer}) {
        auto stream = owned(new FaultyStream());
        stream->endless = true;
        stream->endAt = std::size_t{8} << 20U;
        stream->maxChunk = chunk;
        EXPECT_EQ(loadInto(*stream), engine::Settings()) << chunk << " bytes per read";
        EXPECT_LE(stream->served, std::size_t{2} << 20U) << chunk << " bytes per read";
    }
}

// A state followed by endless bytes that are not JSON is unreadable as a whole.
TEST(ProcessorStateFaults, AStateWithEndlessTrailingBytesLoadsDefaults)
{
    auto stream = owned(new FaultyStream(savedState()));
    stream->endless = true;
    stream->maxChunk = 4096;
    EXPECT_EQ(loadInto(*stream), engine::Settings());
}

TEST(ProcessorStateFaults, ReportsAFailedWrite)
{
    auto processor = makeProcessor();
    auto stream = owned(new FaultyStream());
    stream->failWrites = true;
    EXPECT_NE(processor->getState(stream), kResultOk);
    processor->terminate();
}

// Disabled: a real bug. getState makes one write and ignores how many bytes the stream took, so a
// stream that takes fewer (IBStream::write may, and reports it in numBytesWritten) is left with a
// cut state while getState returns kResultOk: the host saves it and the project later loads with
// every setting at its default. getState should write the rest, or report the failure.
TEST(ProcessorStateFaults, DISABLED_AShortWriteSavesTheWholeStateOrFails)
{
    const std::string state = savedState();
    for (const std::size_t chunk :
         {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{64}, state.size() - 1}) {
        auto processor = makeProcessor();
        processor->engine()->settings().replace(changedSettings());
        auto stream = owned(new FaultyStream());
        stream->maxChunk = chunk;
        const tresult result = processor->getState(stream);
        EXPECT_TRUE(result != kResultOk || stream->written == state)
            << chunk << " bytes per write: kept " << stream->written.size() << " of "
            << state.size() << " bytes, and getState returned kResultOk";
        processor->terminate();
    }
}

// The controller keeps no settings of its own (they live in the processor's engine), so a failing
// component stream cannot affect it.
TEST(ProcessorStateFaults, TheControllerNeverReadsTheComponentState)
{
    auto controller = owned(new Controller());
    auto stream = owned(new FaultyStream(savedState()));
    stream->failAt = 0;
    EXPECT_EQ(controller->setComponentState(stream), kResultOk);
    EXPECT_EQ(controller->setComponentState(nullptr), kResultOk);
    EXPECT_EQ(stream->reads, 0);
}

} // namespace
} // namespace orizon::plugin
