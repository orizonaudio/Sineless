#include "Processor.h"
#include "public.sdk/source/common/memorystream.h"

#include <cstring>
#include <gtest/gtest.h>
#include <string>

namespace orizon::plugin {
namespace {

using namespace Steinberg;
using protocol::SettingId;

IPtr<Processor> makeProcessor()
{
    auto processor = owned(new Processor());
    EXPECT_EQ(processor->initialize(nullptr), kResultOk);
    return processor;
}

TEST(ProcessorState, RoundTripsThroughAHostStream)
{
    auto source = makeProcessor();
    source->engine()->settings().set(SettingId::kTimeSpan, 2.0);
    source->engine()->settings().set(SettingId::kTriggerMode, 1.0);
    auto stream = owned(new MemoryStream());
    ASSERT_EQ(source->getState(stream), kResultOk);

    auto restored = makeProcessor();
    stream->seek(0, IBStream::kIBSeekSet, nullptr);
    ASSERT_EQ(restored->setState(stream), kResultOk);
    EXPECT_EQ(restored->engine()->settings().get(), source->engine()->settings().get());
    source->terminate();
    restored->terminate();
}

TEST(ProcessorState, LoadsDefaultsFromGarbageWithoutFailing)
{
    auto processor = makeProcessor();
    processor->engine()->settings().set(SettingId::kTimeSpan, 2.0);
    const std::string garbage = "\x01\x02 not a state";
    auto stream = owned(
        new MemoryStream(const_cast<char*>(garbage.data()), static_cast<TSize>(garbage.size())));
    EXPECT_EQ(processor->setState(stream), kResultOk);
    EXPECT_EQ(processor->engine()->settings().get(), engine::Settings());
    processor->terminate();
}

TEST(ProcessorState, RejectsANullStream)
{
    auto processor = makeProcessor();
    EXPECT_EQ(processor->getState(nullptr), kInvalidArgument);
    EXPECT_EQ(processor->setState(nullptr), kInvalidArgument);
    processor->terminate();
}

} // namespace
} // namespace orizon::plugin
