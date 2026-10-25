#include "Controller.h"
#include "PluginIds.h"
#include "Processor.h"
#include "editor/Editor.h"
#include "engine/InstanceRegistry.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"

#include <algorithm>
#include <gtest/gtest.h>

namespace orizon::plugin {
namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

class EngineLink : public ::testing::Test
{
protected:
    void SetUp() override
    {
        processor = owned(new Processor());
        controller = owned(new Controller());
        ASSERT_EQ(processor->initialize(&host), kResultOk);
        ASSERT_EQ(controller->initialize(&host), kResultOk);
    }

    void TearDown() override
    {
        controller->terminate();
        processor->terminate();
    }

    void connect()
    {
        ASSERT_EQ(controller->connect(processor), kResultOk);
        ASSERT_EQ(processor->connect(controller), kResultOk);
    }

    void sendToken(int64 token)
    {
        IPtr<IMessage> message = owned(new HostMessage());
        message->setMessageID(kEngineTokenMessage);
        message->getAttributes()->setInt(kEngineTokenAttribute, token);
        controller->notify(message);
    }

    // Not reference counted: the SDK's HostApplication never deletes itself on release, as hosts
    // keep one for the whole process, so it lives as long as the test.
    HostApplication host;
    IPtr<Processor> processor;
    IPtr<Controller> controller;
};

TEST_F(EngineLink, ControllerReceivesTheProcessorsEngineOnConnect)
{
    connect();
    ASSERT_NE(processor->engine(), nullptr);
    EXPECT_EQ(controller->engine(), processor->engine());
}

TEST_F(EngineLink, EngineIsUnavailableWithoutConnection)
{
    EXPECT_EQ(controller->engine(), nullptr);
    IPtr<IPlugView> view = owned(controller->createView(ViewType::kEditor));
    ASSERT_NE(view, nullptr);
    EXPECT_FALSE(static_cast<editor::Editor*>(view.get())->engineAvailable());
}

TEST_F(EngineLink, EditorSeesTheEngineAfterConnect)
{
    connect();
    IPtr<IPlugView> view = owned(controller->createView(ViewType::kEditor));
    ASSERT_NE(view, nullptr);
    EXPECT_TRUE(static_cast<editor::Editor*>(view.get())->engineAvailable());
}

TEST_F(EngineLink, UnknownTokenLeavesTheEngineUnavailable)
{
    // A token from another process's registry names nothing here.
    sendToken(0x1234);
    EXPECT_EQ(controller->engine(), nullptr);
}

TEST_F(EngineLink, ControllerKeepsTheEngineAliveAfterProcessorTerminates)
{
    connect();
    const std::shared_ptr<engine::Engine> engine = controller->engine();
    processor->terminate();
    EXPECT_EQ(processor->engine(), nullptr);
    EXPECT_EQ(controller->engine(), engine);
    EXPECT_NE(controller->engine(), nullptr);
}

// Every processor is listed in its group for the multi-track view until it is terminated.
TEST_F(EngineLink, ProcessorJoinsItsGroupUntilTerminated)
{
    const auto listed = [&] {
        const auto members = engine::InstanceRegistry::shared().members(protocol::Group::kA);
        return std::ranges::any_of(members, [&](const engine::GroupMember& member) {
            return member.engine == processor->engine();
        });
    };
    ASSERT_NE(processor->engine(), nullptr);
    EXPECT_TRUE(listed());
    const std::shared_ptr<engine::Engine> engine = processor->engine();
    processor->terminate();
    EXPECT_FALSE(std::ranges::any_of(
        engine::InstanceRegistry::shared().members(protocol::Group::kA),
        [&](const engine::GroupMember& member) { return member.engine == engine; }));
}

IPtr<IAttributeList> channelInfo(const char16_t* name, int64 colour, int64 index)
{
    IPtr<IAttributeList> list = HostAttributeList::make();
    list->setString(ChannelContext::kChannelNameKey, name);
    list->setInt(ChannelContext::kChannelColorKey, colour);
    list->setInt(ChannelContext::kChannelIndexKey, index);
    list->setInt(ChannelContext::kChannelIndexNamespaceOrderKey, 2);
    return list;
}

TEST_F(EngineLink, PassesTheHostsTrackDetailsToTheEngine)
{
    // Details may arrive before the engine; they are applied once it does.
    ASSERT_EQ(controller->setChannelContextInfos(channelInfo(u"Kick", 0xffe03030, 3)), kResultTrue);
    connect();
    engine::TrackInfo track = processor->engine()->trackInfo();
    EXPECT_EQ(track.name, "Kick");
    EXPECT_EQ(track.colour, 0xe03030u) << "alpha dropped";
    EXPECT_EQ(track.index, 200003) << "namespace order, then position";
    controller->setChannelContextInfos(channelInfo(u"Kick 2 ✓", 0xff3060e0, 4));
    track = processor->engine()->trackInfo();
    EXPECT_EQ(track.name, "Kick 2 ✓") << "renamed, in UTF-8";
    EXPECT_EQ(track.colour, 0x3060e0u);
}

TEST(TrackInfo, KeepsOnlyWhatTheHostReports)
{
    IPtr<IAttributeList> list = HostAttributeList::make();
    list->setString(ChannelContext::kChannelNameKey, u"Bass");
    const engine::TrackInfo track = mergeTrackInfo({}, *list);
    EXPECT_EQ(track.name, "Bass");
    EXPECT_FALSE(track.colour.has_value());
    EXPECT_FALSE(track.index.has_value());
}

// Live reports a new track in three calls: an empty name, the name, then the colour with an
// empty name again. The name must survive the third.
TEST_F(EngineLink, CombinesTrackDetailsReportedSeparately)
{
    connect();
    IPtr<IAttributeList> first = HostAttributeList::make();
    first->setString(ChannelContext::kChannelNameKey, u"");
    IPtr<IAttributeList> second = HostAttributeList::make();
    second->setString(ChannelContext::kChannelNameKey, u"1-Kick");
    IPtr<IAttributeList> third = HostAttributeList::make();
    third->setString(ChannelContext::kChannelNameKey, u"");
    third->setInt(ChannelContext::kChannelColorKey, 0xffe03030);
    for (const auto& list : {first, second, third}) {
        ASSERT_EQ(controller->setChannelContextInfos(list), kResultTrue);
    }
    const engine::TrackInfo track = processor->engine()->trackInfo();
    EXPECT_EQ(track.name, "1-Kick");
    EXPECT_EQ(track.colour, 0xe03030u);
}

TEST(TrackInfo, KeepsTheOrderPartTheHostDidNotRepeat)
{
    IPtr<IAttributeList> both = HostAttributeList::make();
    both->setInt(ChannelContext::kChannelIndexKey, 3);
    both->setInt(ChannelContext::kChannelIndexNamespaceOrderKey, 2);
    IPtr<IAttributeList> position = HostAttributeList::make();
    position->setInt(ChannelContext::kChannelIndexKey, 5);
    const engine::TrackInfo moved = mergeTrackInfo(mergeTrackInfo({}, *both), *position);
    EXPECT_EQ(moved.index, 200005) << "the namespace order kept";
}

// Hosts save Sineless's class IDs in every project that uses it: a build without the brand takes
// neither, so a fork's plugin is never loaded in Sineless's place.
TEST(PluginIds, AreSinelessOnlyInABrandedBuild)
{
    const FUID processor(0xB704D89D, 0x96B24108, 0x878CA953, 0x316956A4);
    const FUID controller(0x46B0AF97, 0x85904BB3, 0x99DC138A, 0x997DE554);
    EXPECT_EQ(FUID::fromTUID(kProcessorUid) == processor, SINELESS_BRANDED == 1);
    EXPECT_EQ(FUID::fromTUID(kControllerUid) == controller, SINELESS_BRANDED == 1);
    EXPECT_NE(FUID::fromTUID(kProcessorUid), FUID::fromTUID(kControllerUid));
}

} // namespace
} // namespace orizon::plugin
