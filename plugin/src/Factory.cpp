#include "Controller.h"
#include "PluginIds.h"
#include "Processor.h"
#include "engine/Version.h"
#include "public.sdk/source/main/pluginfactory_constexpr.h"

using namespace orizon;

BEGIN_FACTORY_DEF(plugin::kVendorName, plugin::kVendorUrl, plugin::kVendorEmail, 2)

// Class flags are 0, not kDistributable: the controller shares the processor's engine
// in-process (AD-10).
DEF_CLASS(plugin::kProcessorUid, Steinberg::PClassInfo::kManyInstances, kVstAudioEffectClass,
          plugin::kProductName, 0, plugin::kSubCategories, engine::kVersionString.data(),
          kVstVersionString, plugin::Processor::createInstance, nullptr)

DEF_CLASS(plugin::kControllerUid, Steinberg::PClassInfo::kManyInstances,
          kVstComponentControllerClass, plugin::kProductName, 0, "", engine::kVersionString.data(),
          kVstVersionString, plugin::Controller::createInstance, nullptr)

END_FACTORY
