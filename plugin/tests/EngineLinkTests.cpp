#include "Controller.h"
#include "EngineRegistry.h"
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
        host = owned(new HostApplication());
        processor = owned(new Processor());
        controller = owned(new Controller());
        ASSERT_EQ(processor->initialize(host), kResultOk);
        ASSERT_EQ(controller->initialize(host), kResultOk);
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

    IPtr<HostApplication> host;
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

TEST(EngineRegistry, ForgetsRemovedTokens)
{
    auto engine = std::make_shared<engine::Engine>();
    const int64 token = EngineRegistry::add(engine);
    EXPECT_EQ(EngineRegistry::find(token), engine);
    EngineRegistry::remove(token);
    EXPECT_EQ(EngineRegistry::find(token), nullptr);
}

} // namespace
} // namespace orizon::plugin
