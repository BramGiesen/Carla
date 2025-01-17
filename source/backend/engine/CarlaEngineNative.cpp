/*
 * Carla Plugin Host
 * Copyright (C) 2011-2019 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the doc/GPL.txt file.
 */

#include "CarlaDefines.h"

#ifdef BUILD_BRIDGE_ALTERNATIVE_ARCH
# error This file should not be compiled if building alternative-arch bridges
#endif

#include "CarlaEngineInternal.hpp"
#include "CarlaPlugin.hpp"

#include "CarlaBackendUtils.hpp"
#include "CarlaBase64Utils.hpp"
#include "CarlaBinaryUtils.hpp"
#include "CarlaMathUtils.hpp"
#include "CarlaStateUtils.hpp"

#include "CarlaExternalUI.hpp"
#include "CarlaHost.h"
#include "CarlaNative.hpp"
#include "CarlaNativePlugin.h"

#if defined(USING_JUCE) && ! (defined(CARLA_OS_MAC) || defined(CARLA_OS_WIN))
# if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6))
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wconversion"
#  pragma GCC diagnostic ignored "-Weffc++"
#  pragma GCC diagnostic ignored "-Wsign-conversion"
#  pragma GCC diagnostic ignored "-Wundef"
#  pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
# endif
# include "AppConfig.h"
# include "juce_events/juce_events.h"
# define USE_JUCE_MESSAGE_THREAD
# if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6))
#  pragma GCC diagnostic pop
# endif
#endif

#include "water/files/File.h"
#include "water/streams/MemoryOutputStream.h"
#include "water/xml/XmlDocument.h"
#include "water/xml/XmlElement.h"

using water::File;
using water::MemoryOutputStream;
using water::String;
using water::XmlDocument;
using water::XmlElement;

CARLA_BACKEND_START_NAMESPACE

static const uint32_t kNumInParams = 100;
static const uint32_t kNumOutParams = 10;

// -----------------------------------------------------------------------

#ifdef USE_JUCE_MESSAGE_THREAD
static int numScopedInitInstances = 0;

class SharedJuceMessageThread : public juce::Thread
{
public:
    SharedJuceMessageThread()
      : juce::Thread ("SharedJuceMessageThread"),
        initialised (false) {}

    ~SharedJuceMessageThread()
    {
        CARLA_SAFE_ASSERT(numScopedInitInstances == 0);

        // in case something fails
        juce::MessageManager::getInstance()->stopDispatchLoop();
        waitForThreadToExit (5000);
    }

    void incRef()
    {
        if (numScopedInitInstances++ == 0)
        {
            startThread (7);

            while (! initialised)
                juce::Thread::sleep (1);
        }
    }

    void decRef()
    {
        if (--numScopedInitInstances == 0)
        {
            juce::MessageManager::getInstance()->stopDispatchLoop();
            waitForThreadToExit (5000);
        }
    }

protected:
    void run() override
    {
        const juce::ScopedJuceInitialiser_GUI juceInitialiser;

        juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();
        initialised = true;

        juce::MessageManager::getInstance()->runDispatchLoop();
    }

private:
    volatile bool initialised;

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SharedJuceMessageThread);
};
#endif

// -----------------------------------------------------------------------

class CarlaEngineNative;

class CarlaEngineNativeUI : public CarlaExternalUI
{
public:
    CarlaEngineNativeUI(CarlaEngineNative* const engine)
        : fEngine(engine)
    {
        carla_debug("CarlaEngineNativeUI::CarlaEngineNativeUI(%p)", engine);
    }

    ~CarlaEngineNativeUI() noexcept override
    {
        carla_debug("CarlaEngineNativeUI::~CarlaEngineNativeUI()");
    }

protected:
    bool msgReceived(const char* const msg) noexcept override;

private:
    CarlaEngineNative* const fEngine;

    void _updateParamValues(CarlaPlugin* const plugin, const uint32_t pluginId,
                            const bool sendCallback, const bool sendPluginHost) const noexcept;

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CarlaEngineNativeUI)
};

// -----------------------------------------------------------------------

class CarlaEngineNative : public CarlaEngine
{
public:
    CarlaEngineNative(const NativeHostDescriptor* const host, const bool isPatchbay, const bool withMidiOut,
                      const uint32_t inChan = 2, uint32_t outChan = 2,
                      const uint32_t cvIns = 0, const uint32_t cvOuts = 0)
        : CarlaEngine(),
          pHost(host),
#ifdef USE_JUCE_MESSAGE_THREAD
          // if not running inside Carla, we will have to run event loop ourselves
          kNeedsJuceMsgThread(host->dispatcher(pHost->handle,
                                               NATIVE_HOST_OPCODE_INTERNAL_PLUGIN, 0, 0, nullptr, 0.0f) == 0),
          fJuceMsgThread(),
#endif
          kIsPatchbay(isPatchbay),
          kHasMidiOut(withMidiOut),
          fIsActive(false),
          fIsRunning(false),
          fUiServer(this),
          fOptionsForced(false)
    {
        carla_debug("CarlaEngineNative::CarlaEngineNative()");

        carla_zeroFloats(fParameters, kNumInParams+kNumOutParams);

#ifdef USE_JUCE_MESSAGE_THREAD
        if (kNeedsJuceMsgThread)
            fJuceMsgThread->incRef();
#endif

        pData->bufferSize = pHost->get_buffer_size(pHost->handle);
        pData->sampleRate = pHost->get_sample_rate(pHost->handle);
        pData->initTime(nullptr);

#ifndef BUILD_BRIDGE
        // Forced OSC setup when running as plugin
        pData->options.oscEnabled = true;
        pData->options.oscPortTCP = -1;
        pData->options.oscPortUDP = 0;
#endif

        if (outChan == 0)
            outChan = inChan;

        // set-up engine
        if (kIsPatchbay)
        {
            pData->options.processMode         = ENGINE_PROCESS_MODE_PATCHBAY;
            pData->options.transportMode       = ENGINE_TRANSPORT_MODE_PLUGIN;
            pData->options.forceStereo         = false;
            pData->options.preferPluginBridges = false;
            pData->options.preferUiBridges     = false;
            init("Carla-Patchbay");
            pData->graph.create(inChan, outChan, cvIns, cvOuts);
        }
        else
        {
            CARLA_SAFE_ASSERT(inChan == 2);
            CARLA_SAFE_ASSERT(outChan == 2);
            pData->options.processMode         = ENGINE_PROCESS_MODE_CONTINUOUS_RACK;
            pData->options.transportMode       = ENGINE_TRANSPORT_MODE_PLUGIN;
            pData->options.forceStereo         = true;
            pData->options.preferPluginBridges = false;
            pData->options.preferUiBridges     = false;
            init("Carla-Rack");
            pData->graph.create(0, 0, 0, 0); // FIXME?
        }

        if (pData->options.resourceDir != nullptr)
            delete[] pData->options.resourceDir;
        if (pData->options.binaryDir != nullptr)
            delete[] pData->options.binaryDir;

        pData->options.resourceDir = carla_strdup(pHost->resourceDir);
        pData->options.binaryDir   = carla_strdup(carla_get_library_folder());

        setCallback(_ui_server_callback, this);
        setFileCallback(_ui_file_callback, this);
    }

    ~CarlaEngineNative() override
    {
        CARLA_SAFE_ASSERT(! fIsActive);
        carla_debug("CarlaEngineNative::~CarlaEngineNative() - START");

        pData->aboutToClose = true;
        fIsRunning = false;

        removeAllPlugins();
        //runPendingRtEvents();
        close();

        pData->graph.destroy();

#ifdef USE_JUCE_MESSAGE_THREAD
        if (kNeedsJuceMsgThread)
            fJuceMsgThread->decRef();
#endif

        carla_debug("CarlaEngineNative::~CarlaEngineNative() - END");
    }

    // -------------------------------------
    // CarlaEngine virtual calls

    bool init(const char* const clientName) override
    {
        carla_debug("CarlaEngineNative::init(\"%s\")", clientName);

        fIsRunning = true;

        if (! pData->init(clientName))
        {
            close();
            setLastError("Failed to init internal data");
            return false;
        }

        pData->bufferSize = pHost->get_buffer_size(pHost->handle);
        pData->sampleRate = pHost->get_sample_rate(pHost->handle);

        return true;
    }

    bool close() override
    {
        fIsRunning = false;
        CarlaEngine::close();
        return true;
    }

    bool isRunning() const noexcept override
    {
        return fIsRunning;
    }

    bool isOffline() const noexcept override
    {
        return pHost->is_offline(pHost->handle);
    }

    bool usesConstantBufferSize() const noexcept override
    {
        // TODO LV2 hosts can report this, till then we allow this
        return true;
    }

    EngineType getType() const noexcept override
    {
        return kEngineTypePlugin;
    }

    const char* getCurrentDriverName() const noexcept override
    {
        return "Plugin";
    }

    void callback(const bool sendHost, const bool sendOsc,
                  const EngineCallbackOpcode action, const uint pluginId,
                  const int value1, const int value2, const int value3,
                  const float valuef, const char* const valueStr) noexcept override
    {
        CarlaEngine::callback(sendHost, sendOsc, action, pluginId, value1, value2, value3, valuef, valueStr);

        if (action == ENGINE_CALLBACK_IDLE && ! pData->aboutToClose) {
            pHost->dispatcher(pHost->handle,
                              NATIVE_HOST_OPCODE_HOST_IDLE,
                              0, 0, nullptr, 0.0f);
        }
    }

    // -------------------------------------------------------------------

    void touchPluginParameter(const uint id, const uint32_t parameterId, const bool touch) noexcept override
    {
        setParameterTouchFromUI(id, parameterId, touch);
    }

    // -------------------------------------------------------------------

    void setParameterValueFromUI(const uint32_t pluginId, const uint32_t index, const float value)
    {
        if (pluginId != 0)
            return;

        fParameters[index] = value;
        pHost->ui_parameter_changed(pHost->handle, index, value);
    }

    void setParameterTouchFromUI(const uint32_t pluginId, const uint32_t index, const bool touch)
    {
        if (pluginId != 0)
            return;

        pHost->dispatcher(pHost->handle,
                          NATIVE_HOST_OPCODE_UI_TOUCH_PARAMETER,
                          static_cast<int32_t>(index),
                          touch ? 1 : 0,
                          nullptr, 0.0f);
    }

    void reloadFromUI()
    {
        carla_zeroFloats(fParameters, kNumInParams+kNumOutParams);
        pHost->dispatcher(pHost->handle, NATIVE_HOST_OPCODE_RELOAD_ALL, 0, 0, nullptr, 0.0f);
    }

protected:
    // -------------------------------------------------------------------

    void bufferSizeChanged(const uint32_t newBufferSize)
    {
        if (pData->bufferSize == newBufferSize)
            return;

        {
            const CarlaMutexLocker cml(fUiServer.getPipeLock());

            if (fUiServer.writeAndFixMessage("buffer-size"))
            {
                char tmpBuf[STR_MAX+1];
                carla_zeroChars(tmpBuf, STR_MAX+1);

                std::snprintf(tmpBuf, STR_MAX, "%i\n", newBufferSize);

                if (fUiServer.writeMessage(tmpBuf))
                    fUiServer.flushMessages();
            }
        }

        pData->bufferSize = newBufferSize;
        CarlaEngine::bufferSizeChanged(newBufferSize);
    }

    void sampleRateChanged(const double newSampleRate)
    {
        if (carla_isEqual(pData->sampleRate, newSampleRate))
            return;

        {
            const CarlaMutexLocker cml(fUiServer.getPipeLock());

            if (fUiServer.writeAndFixMessage("sample-rate"))
            {
                char tmpBuf[STR_MAX+1];
                carla_zeroChars(tmpBuf, STR_MAX+1);

                {
                    const CarlaScopedLocale csl;
                    std::snprintf(tmpBuf, STR_MAX, "%f\n", newSampleRate);
                }

                if (fUiServer.writeMessage(tmpBuf))
                    fUiServer.flushMessages();
            }
        }

        pData->sampleRate = newSampleRate;
        CarlaEngine::sampleRateChanged(newSampleRate);
    }

    // -------------------------------------------------------------------

    void uiServerSendPluginInfo(CarlaPlugin* const plugin)
    {
        char tmpBuf[STR_MAX+1];
        carla_zeroChars(tmpBuf, STR_MAX+1);

        const CarlaMutexLocker cml(fUiServer.getPipeLock());

        const uint pluginId(plugin->getId());

        std::snprintf(tmpBuf, STR_MAX, "PLUGIN_INFO_%i\n", pluginId);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        std::snprintf(tmpBuf, STR_MAX, "%i:%i:%i:" P_INT64 ":%i:%i\n",
                      plugin->getType(), plugin->getCategory(),
                      plugin->getHints(), plugin->getUniqueId(),
                      plugin->getOptionsAvailable(), plugin->getOptionsEnabled());
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        if (const char* const filename = plugin->getFilename())
        {
            std::snprintf(tmpBuf, STR_MAX, "%s", filename);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
        }
        else
        {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
        }

        if (const char* const name = plugin->getName())
        {
            std::snprintf(tmpBuf, STR_MAX, "%s", name);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
        }
        else
        {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
        }

        if (const char* const iconName = plugin->getIconName())
        {
            std::snprintf(tmpBuf, STR_MAX, "%s", iconName);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
        }
        else
        {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
        }

        if (plugin->getRealName(tmpBuf)) {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
        } else {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
        }

        if (plugin->getLabel(tmpBuf)) {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
        } else {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
        }

        if (plugin->getMaker(tmpBuf)) {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
        } else {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
        }

        if (plugin->getCopyright(tmpBuf)) {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
        } else {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
        }

        std::snprintf(tmpBuf, STR_MAX, "AUDIO_COUNT_%i:%i:%i\n", pluginId, plugin->getAudioInCount(), plugin->getAudioOutCount());
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        std::snprintf(tmpBuf, STR_MAX, "MIDI_COUNT_%i:%i:%i\n", pluginId, plugin->getMidiInCount(), plugin->getMidiOutCount());
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        fUiServer.flushMessages();
    }

    void uiServerSendPluginParameters(CarlaPlugin* const plugin)
    {
        char tmpBuf[STR_MAX+1];
        carla_zeroChars(tmpBuf, STR_MAX+1);

        const CarlaMutexLocker cml(fUiServer.getPipeLock());
        const CarlaScopedLocale csl;

        const uint pluginId(plugin->getId());

        for (int32_t i=PARAMETER_ACTIVE; i>PARAMETER_MAX; --i)
        {
            std::snprintf(tmpBuf, STR_MAX, "PARAMVAL_%i:%i\n", pluginId, i);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

            std::snprintf(tmpBuf, STR_MAX, "%f\n", static_cast<double>(plugin->getInternalParameterValue(i)));
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

            fUiServer.flushMessages();
        }

        uint32_t ins, outs, count;
        plugin->getParameterCountInfo(ins, outs);
        count = plugin->getParameterCount();

        std::snprintf(tmpBuf, STR_MAX, "PARAMETER_COUNT_%i:%i:%i:%i\n", pluginId, ins, outs, count);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        for (uint32_t i=0; i<count; ++i)
        {
            const ParameterData& paramData(plugin->getParameterData(i));
            const ParameterRanges& paramRanges(plugin->getParameterRanges(i));

            std::snprintf(tmpBuf, STR_MAX, "PARAMETER_DATA_%i:%i\n", pluginId, i);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

            std::snprintf(tmpBuf, STR_MAX, "%i:%i:%i:%i\n", paramData.type, paramData.hints,
                                                            paramData.midiChannel, paramData.midiCC);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

            if (plugin->getParameterName(i, tmpBuf)) {
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
            } else {
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
            }

            if (plugin->getParameterUnit(i, tmpBuf)) {
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
            } else {
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
            }

            if (plugin->getParameterComment(i, tmpBuf)) {
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
            } else {
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
            }

            if (plugin->getParameterGroupName(i, tmpBuf)) {
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
            } else {
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
            }

            std::snprintf(tmpBuf, STR_MAX, "PARAMETER_RANGES_%i:%i\n", pluginId, i);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

            std::snprintf(tmpBuf, STR_MAX, "%f:%f:%f:%f:%f:%f\n",
                          static_cast<double>(paramRanges.def),
                          static_cast<double>(paramRanges.min),
                          static_cast<double>(paramRanges.max),
                          static_cast<double>(paramRanges.step),
                          static_cast<double>(paramRanges.stepSmall),
                          static_cast<double>(paramRanges.stepLarge));
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

            std::snprintf(tmpBuf, STR_MAX, "PARAMVAL_%i:%i\n", pluginId, i);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

            std::snprintf(tmpBuf, STR_MAX, "%f\n", static_cast<double>(plugin->getParameterValue(i)));
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        }

        fUiServer.flushMessages();
    }

    void uiServerSendPluginPrograms(CarlaPlugin* const plugin)
    {
        char tmpBuf[STR_MAX+1];
        carla_zeroChars(tmpBuf, STR_MAX+1);

        const CarlaMutexLocker cml(fUiServer.getPipeLock());

        const uint pluginId(plugin->getId());

        uint32_t count = plugin->getProgramCount();
        std::snprintf(tmpBuf, STR_MAX, "PROGRAM_COUNT_%i:%i:%i\n", pluginId, count, plugin->getCurrentProgram());
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        for (uint32_t i=0; i<count; ++i)
        {
            std::snprintf(tmpBuf, STR_MAX, "PROGRAM_NAME_%i:%i\n", pluginId, i);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

            if (plugin->getProgramName(i, tmpBuf)) {
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
            } else {
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
            }
        }

        fUiServer.flushMessages();

        count = plugin->getMidiProgramCount();
        std::snprintf(tmpBuf, STR_MAX, "MIDI_PROGRAM_COUNT_%i:%i:%i\n", pluginId, count, plugin->getCurrentMidiProgram());
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        for (uint32_t i=0; i<count; ++i)
        {
            std::snprintf(tmpBuf, STR_MAX, "MIDI_PROGRAM_DATA_%i:%i\n", pluginId, i);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

            const MidiProgramData& mpData(plugin->getMidiProgramData(i));

            std::snprintf(tmpBuf, STR_MAX, "%i:%i\n", mpData.bank, mpData.program);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

            std::snprintf(tmpBuf, STR_MAX, "%s", mpData.name);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(tmpBuf),);
        }

        fUiServer.flushMessages();
    }

    void uiServerSendPluginProperties(CarlaPlugin* const plugin)
    {
        char tmpBuf[STR_MAX+1];
        carla_zeroChars(tmpBuf, STR_MAX+1);

        const CarlaMutexLocker cml(fUiServer.getPipeLock());

        const uint pluginId(plugin->getId());

        uint32_t count = plugin->getCustomDataCount();
        std::snprintf(tmpBuf, STR_MAX, "CUSTOM_DATA_COUNT_%i:%i\n", pluginId, count);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        for (uint32_t i=0; i<count; ++i)
        {
            const CustomData& customData(plugin->getCustomData(i));
            CARLA_SAFE_ASSERT_CONTINUE(customData.isValid());

            if (std::strcmp(customData.type, CUSTOM_DATA_TYPE_PROPERTY) != 0)
                continue;

            std::snprintf(tmpBuf, STR_MAX, "CUSTOM_DATA_%i:%i\n", pluginId, i);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(customData.type),);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(customData.key),);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(customData.value),);
        }

        fUiServer.flushMessages();
    }

    void uiServerCallback(const EngineCallbackOpcode action, const uint pluginId,
                          const int value1, const int value2, const int value3,
                          const float valuef, const char* const valueStr)
    {
        if (! fIsRunning)
            return;
        if (! fUiServer.isPipeRunning())
            return;

        CarlaPlugin* plugin;

        switch (action)
        {
        case ENGINE_CALLBACK_UPDATE:
            plugin = getPlugin(pluginId);

            if (plugin != nullptr && plugin->isEnabled())
            {
                CARLA_SAFE_ASSERT_BREAK(plugin->getId() == pluginId);
                uiServerSendPluginProperties(plugin);
            }
            break;

        case ENGINE_CALLBACK_RELOAD_INFO:
            plugin = getPlugin(pluginId);

            if (plugin != nullptr && plugin->isEnabled())
            {
                CARLA_SAFE_ASSERT_BREAK(plugin->getId() == pluginId);
                uiServerSendPluginInfo(plugin);
            }
            break;

        case ENGINE_CALLBACK_RELOAD_PARAMETERS:
            plugin = getPlugin(pluginId);

            if (plugin != nullptr && plugin->isEnabled())
            {
                CARLA_SAFE_ASSERT_BREAK(plugin->getId() == pluginId);
                uiServerSendPluginParameters(plugin);
            }
            break;

        case ENGINE_CALLBACK_RELOAD_PROGRAMS:
            plugin = getPlugin(pluginId);

            if (plugin != nullptr && plugin->isEnabled())
            {
                CARLA_SAFE_ASSERT_BREAK(plugin->getId() == pluginId);
                uiServerSendPluginPrograms(plugin);
            }
            break;

        case ENGINE_CALLBACK_RELOAD_ALL:
        case ENGINE_CALLBACK_PLUGIN_ADDED:
            plugin = getPlugin(pluginId);

            if (plugin != nullptr && plugin->isEnabled())
            {
                CARLA_SAFE_ASSERT_BREAK(plugin->getId() == pluginId);
                uiServerSendPluginInfo(plugin);
                uiServerSendPluginParameters(plugin);
                uiServerSendPluginPrograms(plugin);
                uiServerSendPluginProperties(plugin);
            }
            break;

        default:
            break;
        }

        char tmpBuf[STR_MAX+1];
        carla_zeroChars(tmpBuf, STR_MAX+1);

        const CarlaMutexLocker cml(fUiServer.getPipeLock());

        std::snprintf(tmpBuf, STR_MAX, "ENGINE_CALLBACK_%i\n", int(action));
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        std::snprintf(tmpBuf, STR_MAX, "%u\n", pluginId);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        std::snprintf(tmpBuf, STR_MAX, "%i\n", value1);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        std::snprintf(tmpBuf, STR_MAX, "%i\n", value2);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        std::snprintf(tmpBuf, STR_MAX, "%i\n", value3);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        {
            const CarlaScopedLocale csl;
            std::snprintf(tmpBuf, STR_MAX, "%f\n", static_cast<double>(valuef));
        }
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        if (valueStr != nullptr) {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(valueStr),);
        } else {
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeEmptyMessage(),);
        }

        fUiServer.flushMessages();
    }

    const char* uiFileCallback(FileCallbackOpcode action, bool isDir, const char* title, const char* filter)
    {
        switch (action)
        {
        case FILE_CALLBACK_DEBUG:
            return nullptr;

        case FILE_CALLBACK_OPEN:
            return pHost->ui_open_file(pHost->handle, isDir, title, filter);

        case FILE_CALLBACK_SAVE:
            return pHost->ui_save_file(pHost->handle, isDir, title, filter);
        }

        return nullptr;
    }

    void uiServerInfo()
    {
        CARLA_SAFE_ASSERT_RETURN(fIsRunning,);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.isPipeRunning(),);

        char tmpBuf[STR_MAX+1];
        carla_zeroChars(tmpBuf, STR_MAX+1);

        const CarlaMutexLocker cml(fUiServer.getPipeLock());

#if defined(HAVE_LIBLO) && !defined(BUILD_BRIDGE)
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage("osc-urls\n"),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(pData->osc.getServerPathTCP()),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage(pData->osc.getServerPathUDP()),);
#endif

        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage("max-plugin-number\n"),);
        std::snprintf(tmpBuf, STR_MAX, "%i\n", pData->maxPluginNumber);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage("buffer-size\n"),);
        std::snprintf(tmpBuf, STR_MAX, "%i\n", pData->bufferSize);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage("sample-rate\n"),);
        {
            const CarlaScopedLocale csl;
            std::snprintf(tmpBuf, STR_MAX, "%f\n", pData->sampleRate);
        }
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        fUiServer.flushMessages();
    }

    void uiServerOptions()
    {
        CARLA_SAFE_ASSERT_RETURN(fIsRunning,);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.isPipeRunning(),);

        char tmpBuf[STR_MAX+1];
        carla_zeroChars(tmpBuf, STR_MAX+1);

        const EngineOptions& options(pData->options);
        const CarlaMutexLocker cml(fUiServer.getPipeLock());

        const char* const optionsForcedStr(fOptionsForced ? "true\n" : "false\n");
        const std::size_t optionsForcedStrSize(fOptionsForced ? 5 : 6);

        std::snprintf(tmpBuf, STR_MAX, "ENGINE_OPTION_%i\n", ENGINE_OPTION_PROCESS_MODE);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(optionsForcedStr, optionsForcedStrSize),);
        std::snprintf(tmpBuf, STR_MAX, "%i\n", options.processMode);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        fUiServer.flushMessages();

        std::snprintf(tmpBuf, STR_MAX, "ENGINE_OPTION_%i\n", ENGINE_OPTION_TRANSPORT_MODE);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(optionsForcedStr, optionsForcedStrSize),);
        std::snprintf(tmpBuf, STR_MAX, "%i\n", options.transportMode);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        fUiServer.flushMessages();

        std::snprintf(tmpBuf, STR_MAX, "ENGINE_OPTION_%i\n", ENGINE_OPTION_FORCE_STEREO);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(optionsForcedStr, optionsForcedStrSize),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(options.forceStereo ? "true\n" : "false\n"),);
        fUiServer.flushMessages();

        std::snprintf(tmpBuf, STR_MAX, "ENGINE_OPTION_%i\n", ENGINE_OPTION_PREFER_PLUGIN_BRIDGES);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(optionsForcedStr, optionsForcedStrSize),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(options.preferPluginBridges ? "true\n" : "false\n"),);
        fUiServer.flushMessages();

        std::snprintf(tmpBuf, STR_MAX, "ENGINE_OPTION_%i\n", ENGINE_OPTION_PREFER_UI_BRIDGES);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(optionsForcedStr, optionsForcedStrSize),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(options.preferUiBridges ? "true\n" : "false\n"),);
        fUiServer.flushMessages();

        std::snprintf(tmpBuf, STR_MAX, "ENGINE_OPTION_%i\n", ENGINE_OPTION_UIS_ALWAYS_ON_TOP);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(optionsForcedStr, optionsForcedStrSize),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(options.uisAlwaysOnTop ? "true\n" : "false\n"),);
        fUiServer.flushMessages();

        std::snprintf(tmpBuf, STR_MAX, "ENGINE_OPTION_%i\n", ENGINE_OPTION_MAX_PARAMETERS);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(optionsForcedStr, optionsForcedStrSize),);
        std::snprintf(tmpBuf, STR_MAX, "%i\n", options.maxParameters);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        fUiServer.flushMessages();

        std::snprintf(tmpBuf, STR_MAX, "ENGINE_OPTION_%i\n", ENGINE_OPTION_UI_BRIDGES_TIMEOUT);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(optionsForcedStr, optionsForcedStrSize),);
        std::snprintf(tmpBuf, STR_MAX, "%i\n", options.uiBridgesTimeout);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        fUiServer.flushMessages();

        std::snprintf(tmpBuf, STR_MAX, "ENGINE_OPTION_%i\n", ENGINE_OPTION_PATH_BINARIES);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage("true\n", 5),);
        std::snprintf(tmpBuf, STR_MAX, "%s\n", options.binaryDir);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        fUiServer.flushMessages();

        std::snprintf(tmpBuf, STR_MAX, "ENGINE_OPTION_%i\n", ENGINE_OPTION_PATH_RESOURCES);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage("true\n", 5),);
        std::snprintf(tmpBuf, STR_MAX, "%s\n", options.resourceDir);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        fUiServer.flushMessages();
    }

    // -------------------------------------------------------------------
    // Plugin parameter calls

    uint32_t getParameterCount() const
    {
        return kNumInParams + kNumOutParams;
    }

    const NativeParameter* getParameterInfo(const uint32_t index) const
    {
        static NativeParameter param;

        static char strBufName[STR_MAX+1];
        static char strBufUnit[STR_MAX+1];
        carla_zeroChars(strBufName, STR_MAX+1);
        carla_zeroChars(strBufUnit, STR_MAX+1);

        if (CarlaPlugin* const plugin = _getFirstPlugin())
        {
            if (index < plugin->getParameterCount())
            {
                const ParameterData& paramData(plugin->getParameterData(index));
                const ParameterRanges& paramRanges(plugin->getParameterRanges(index));

                if (! plugin->getParameterName(index, strBufName))
                    strBufName[0] = '\0';
                if (! plugin->getParameterUnit(index, strBufUnit))
                    strBufUnit[0] = '\0';

                uint hints = 0x0;

                if (paramData.hints & PARAMETER_IS_BOOLEAN)
                    hints |= NATIVE_PARAMETER_IS_BOOLEAN;
                if (paramData.hints & PARAMETER_IS_INTEGER)
                    hints |= NATIVE_PARAMETER_IS_INTEGER;
                if (paramData.hints & PARAMETER_IS_LOGARITHMIC)
                    hints |= NATIVE_PARAMETER_IS_LOGARITHMIC;
                if (paramData.hints & PARAMETER_IS_AUTOMABLE)
                    hints |= NATIVE_PARAMETER_IS_AUTOMABLE;
                if (paramData.hints & PARAMETER_USES_SAMPLERATE)
                    hints |= NATIVE_PARAMETER_USES_SAMPLE_RATE;
                if (paramData.hints & PARAMETER_USES_SCALEPOINTS)
                    hints |= NATIVE_PARAMETER_USES_SCALEPOINTS;

                if (paramData.type == PARAMETER_INPUT || paramData.type == PARAMETER_OUTPUT)
                {
                    if (paramData.hints & PARAMETER_IS_ENABLED)
                        hints |= NATIVE_PARAMETER_IS_ENABLED;
                    if (paramData.type == PARAMETER_OUTPUT)
                        hints |= NATIVE_PARAMETER_IS_OUTPUT;
                }

                param.hints = static_cast<NativeParameterHints>(hints);
                param.name  = strBufName;
                param.unit  = strBufUnit;
                param.ranges.def = paramRanges.def;
                param.ranges.min = paramRanges.min;
                param.ranges.max = paramRanges.max;
                param.ranges.step = paramRanges.step;
                param.ranges.stepSmall = paramRanges.stepSmall;
                param.ranges.stepLarge = paramRanges.stepLarge;
                param.scalePointCount = 0; // TODO
                param.scalePoints = nullptr;

                return &param;
            }
        }

        param.hints = index < kNumInParams ? static_cast<NativeParameterHints>(0x0) : NATIVE_PARAMETER_IS_OUTPUT;
        param.name  = "Unused";
        param.unit  = "";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        param.ranges.step = 0.01f;
        param.ranges.stepSmall = 0.001f;
        param.ranges.stepLarge = 0.1f;
        param.scalePointCount = 0;
        param.scalePoints = nullptr;

        return &param;
    }

    float getParameterValue(const uint32_t index) const
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
        {
            if (index < plugin->getParameterCount())
                return plugin->getParameterValue(index);
        }

        return fParameters[index];
    }

    // -------------------------------------------------------------------
    // Plugin midi-program calls

    uint32_t getMidiProgramCount() const
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
            return plugin->getMidiProgramCount();

        return 0;
    }

    const NativeMidiProgram* getMidiProgramInfo(const uint32_t index) const
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
        {
            if (index < plugin->getMidiProgramCount())
            {
                static NativeMidiProgram midiProg;

                {
                    const MidiProgramData& midiProgData(plugin->getMidiProgramData(index));

                    midiProg.bank    = midiProgData.bank;
                    midiProg.program = midiProgData.program;
                    midiProg.name    = midiProgData.name;
                }

                return &midiProg;
            }
        }

        return nullptr;
    }

    // -------------------------------------------------------------------
    // Plugin state calls

    void setParameterValue(const uint32_t index, const float value)
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
        {
            if (index < plugin->getParameterCount())
            {
                const float rvalue = plugin->getParameterRanges(index).getUnnormalizedValue(value);
                plugin->setParameterValueRT(index, rvalue, false);
            }
        }

        fParameters[index] = value;
    }

    void setMidiProgram(const uint8_t, const uint32_t bank, const uint32_t program)
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
            plugin->setMidiProgramById(bank, program, false, false, false);
    }

    // -------------------------------------------------------------------
    // Plugin process calls

    void activate()
    {
#if 0
        for (uint i=0; i < pData->curPluginCount; ++i)
        {
            CarlaPlugin* const plugin(pData->plugins[i].plugin);

            if (plugin == nullptr || ! plugin->isEnabled())
                continue;

            plugin->setActive(true, true, false);
        }
#endif
        fIsActive = true;
    }

    void deactivate()
    {
        fIsActive = false;
#if 0
        for (uint i=0; i < pData->curPluginCount; ++i)
        {
            CarlaPlugin* const plugin(pData->plugins[i].plugin);

            if (plugin == nullptr || ! plugin->isEnabled())
                continue;

            plugin->setActive(false, true, false);
        }
#endif

        // just in case
        //runPendingRtEvents();
    }

    void process(const float** const inBuffer, float** const outBuffer, const uint32_t frames,
                 const NativeMidiEvent* const midiEvents, const uint32_t midiEventCount)
    {
        if (frames > pData->bufferSize)
        {
            carla_stderr2("Host is calling process with too high number of frames! %u vs %u",
                          frames, pData->bufferSize);

            deactivate();
            bufferSizeChanged(frames);
            activate();
        }

        const PendingRtEventsRunner prt(this, frames, true);

        // ---------------------------------------------------------------
        // Time Info

        const NativeTimeInfo* const timeInfo(pHost->get_time_info(pHost->handle));

        pData->timeInfo.playing   = timeInfo->playing;
        pData->timeInfo.frame     = timeInfo->frame;
        pData->timeInfo.usecs     = timeInfo->usecs;
        pData->timeInfo.bbt.valid = timeInfo->bbt.valid;

        if (timeInfo->bbt.valid)
        {
            pData->timeInfo.bbt.bar = timeInfo->bbt.bar;
            pData->timeInfo.bbt.beat = timeInfo->bbt.beat;
            pData->timeInfo.bbt.tick = timeInfo->bbt.tick;
            pData->timeInfo.bbt.barStartTick = timeInfo->bbt.barStartTick;

            pData->timeInfo.bbt.beatsPerBar = timeInfo->bbt.beatsPerBar;
            pData->timeInfo.bbt.beatType = timeInfo->bbt.beatType;

            pData->timeInfo.bbt.ticksPerBeat = timeInfo->bbt.ticksPerBeat;
            pData->timeInfo.bbt.beatsPerMinute = timeInfo->bbt.beatsPerMinute;
        }

        // ---------------------------------------------------------------
        // Do nothing if no plugins and rack mode

        if (pData->curPluginCount == 0 && ! kIsPatchbay)
        {
            if (outBuffer[0] != inBuffer[0])
                carla_copyFloats(outBuffer[0], inBuffer[0], frames);

            if (outBuffer[1] != inBuffer[1])
                carla_copyFloats(outBuffer[1], inBuffer[1], frames);

            for (uint32_t i=0; i < midiEventCount; ++i)
            {
                if (! pHost->write_midi_event(pHost->handle, &midiEvents[i]))
                    break;
            }
            return;
        }

        // ---------------------------------------------------------------
        // initialize events

        carla_zeroStructs(pData->events.in,  kMaxEngineEventInternalCount);
        carla_zeroStructs(pData->events.out, kMaxEngineEventInternalCount);

        // ---------------------------------------------------------------
        // events input (before processing)

        {
            uint32_t engineEventIndex = 0;

            for (uint32_t i=0; i < midiEventCount && engineEventIndex < kMaxEngineEventInternalCount; ++i)
            {
                const NativeMidiEvent& midiEvent(midiEvents[i]);
                EngineEvent&           engineEvent(pData->events.in[engineEventIndex++]);

                engineEvent.time = midiEvent.time;
                engineEvent.fillFromMidiData(midiEvent.size, midiEvent.data, 0);

                if (engineEventIndex >= kMaxEngineEventInternalCount)
                    break;
            }
        }

        if (kIsPatchbay)
        {
            // -----------------------------------------------------------
            // process

            pData->graph.process(pData, inBuffer, outBuffer, frames);
        }
        else
        {
            // -----------------------------------------------------------
            // create audio buffers

            const float* inBuf[2]  = {  inBuffer[0],  inBuffer[1] };
            /* */ float* outBuf[2] = { outBuffer[0], outBuffer[1] };

            // -----------------------------------------------------------
            // process

            pData->graph.processRack(pData, inBuf, outBuf, frames);
        }

        // ---------------------------------------------------------------
        // events output (after processing)

        carla_zeroStructs(pData->events.in, kMaxEngineEventInternalCount);

        if (kHasMidiOut)
        {
            NativeMidiEvent midiEvent;

            for (uint32_t i=0; i < kMaxEngineEventInternalCount; ++i)
            {
                const EngineEvent& engineEvent(pData->events.out[i]);

                if (engineEvent.type == kEngineEventTypeNull)
                    break;

                carla_zeroStruct(midiEvent);
                midiEvent.time = engineEvent.time;

                /**/ if (engineEvent.type == kEngineEventTypeControl)
                {
                    midiEvent.port = 0;
                    midiEvent.size = engineEvent.ctrl.convertToMidiData(engineEvent.channel, midiEvent.data);
                }
                else if (engineEvent.type == kEngineEventTypeMidi)
                {
                    if (engineEvent.midi.size > 4)
                        continue;

                    midiEvent.port = engineEvent.midi.port;
                    midiEvent.size = engineEvent.midi.size;

                    midiEvent.data[0] = static_cast<uint8_t>(engineEvent.midi.data[0] | (engineEvent.channel & MIDI_CHANNEL_BIT));

                    for (uint8_t j=1; j < midiEvent.size; ++j)
                        midiEvent.data[j] = engineEvent.midi.data[j];
                }
                else
                {
                    continue;
                }

                if (midiEvent.size > 0)
                    pHost->write_midi_event(pHost->handle, &midiEvent);
            }
        }
    }

    // -------------------------------------------------------------------
    // Plugin UI calls

    void uiShow(const bool show)
    {
        if (show)
        {
            if (fUiServer.isPipeRunning())
            {
                fUiServer.writeFocusMessage();
                return;
            }

            CarlaString path(pHost->resourceDir);

            if (kIsPatchbay)
                path += CARLA_OS_SEP_STR "carla-plugin-patchbay";
            else
                path += CARLA_OS_SEP_STR "carla-plugin";
#ifdef CARLA_OS_WIN
            path += ".exe";
#endif
            carla_stdout("Trying to start carla-plugin using \"%s\"", path.buffer());

            fUiServer.setData(path, pData->sampleRate, pHost->uiName);

            if (! fUiServer.startPipeServer(false))
            {
                pHost->dispatcher(pHost->handle, NATIVE_HOST_OPCODE_UI_UNAVAILABLE, 0, 0, nullptr, 0.0f);
                return;
            }

            uiServerInfo();
            uiServerOptions();
            uiServerCallback(ENGINE_CALLBACK_ENGINE_STARTED,
                             pData->curPluginCount,
                             pData->options.processMode,
                             pData->options.transportMode,
                             static_cast<int>(pData->bufferSize),
                             static_cast<float>(pData->sampleRate),
                             "Plugin");

            fUiServer.writeShowMessage();

            for (uint i=0; i < pData->curPluginCount; ++i)
            {
                CarlaPlugin* const plugin(pData->plugins[i].plugin);

                if (plugin != nullptr && plugin->isEnabled())
                {
                    uiServerCallback(ENGINE_CALLBACK_PLUGIN_ADDED, i, 0, 0, 0, 0.0f, plugin->getName());
                }
            }

            if (kIsPatchbay)
                patchbayRefresh(true, false, false);
        }
        else
        {
            fUiServer.stopPipeServer(2000);

            // hide all custom uis
            for (uint i=0; i < pData->curPluginCount; ++i)
            {
                CarlaPlugin* const plugin(pData->plugins[i].plugin);

                if (plugin != nullptr && plugin->isEnabled())
                {
                    if (plugin->getHints() & PLUGIN_HAS_CUSTOM_UI)
                    {
                        try {
                            plugin->showCustomUI(false);
                        } CARLA_SAFE_EXCEPTION_CONTINUE("Plugin showCustomUI (hide)");
                    }
                }
            }
        }
    }

    void uiIdle()
    {
        for (uint i=0; i < pData->curPluginCount; ++i)
        {
            CarlaPlugin* const plugin(pData->plugins[i].plugin);

            if (plugin != nullptr && plugin->isEnabled())
            {
                const uint hints(plugin->getHints());

                if ((hints & PLUGIN_HAS_CUSTOM_UI) != 0 && (hints & PLUGIN_NEEDS_UI_MAIN_THREAD) != 0)
                {
                    try {
                        plugin->uiIdle();
                    } CARLA_SAFE_EXCEPTION_CONTINUE("Plugin uiIdle");
                }
            }
        }

        idlePipe();

        switch (fUiServer.getAndResetUiState())
        {
        case CarlaExternalUI::UiNone:
        case CarlaExternalUI::UiShow:
            break;
        case CarlaExternalUI::UiCrashed:
            pHost->dispatcher(pHost->handle, NATIVE_HOST_OPCODE_UI_UNAVAILABLE, 0, 0, nullptr, 0.0f);
            break;
        case CarlaExternalUI::UiHide:
            pHost->ui_closed(pHost->handle);
            fUiServer.stopPipeServer(1000);
            break;
        }
    }

    void uiSetParameterValue(const uint32_t index, const float value)
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
        {
            if (index < plugin->getParameterCount())
                plugin->uiParameterChange(index, value);
        }
    }

    void idlePipe()
    {
        if (! fUiServer.isPipeRunning())
            return;

        fUiServer.idlePipe();

        char tmpBuf[STR_MAX+1];
        carla_zeroChars(tmpBuf, STR_MAX+1);

        const CarlaMutexLocker cml(fUiServer.getPipeLock());
        const CarlaScopedLocale csl;
        const EngineTimeInfo& timeInfo(pData->timeInfo);

        // ------------------------------------------------------------------------------------------------------------
        // send engine info

        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage("runtime-info"),);
        std::snprintf(tmpBuf, STR_MAX, "%f:0\n", static_cast<double>(getDSPLoad()));
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

        fUiServer.flushMessages();

        // ------------------------------------------------------------------------------------------------------------
        // send transport

        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeAndFixMessage("transport"),);
        CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(timeInfo.playing ? "true\n" : "false\n"),);

        if (timeInfo.bbt.valid)
        {
            std::snprintf(tmpBuf, STR_MAX, P_UINT64 ":%i:%i:%i\n", timeInfo.frame,
                                                                   timeInfo.bbt.bar,
                                                                   timeInfo.bbt.beat,
                                                                   static_cast<int>(timeInfo.bbt.tick + 0.5));
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
            std::snprintf(tmpBuf, STR_MAX, "%f\n", timeInfo.bbt.beatsPerMinute);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
        }
        else
        {
            std::snprintf(tmpBuf, STR_MAX, P_UINT64 ":0:0:0\n", timeInfo.frame);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage("0.0\n"),);
        }

        fUiServer.flushMessages();

        // ------------------------------------------------------------------------------------------------------------
        // send peaks and param outputs for all plugins

        for (uint i=0; i < pData->curPluginCount; ++i)
        {
            const EnginePluginData& plugData(pData->plugins[i]);
            const CarlaPlugin* const plugin(pData->plugins[i].plugin);

            std::snprintf(tmpBuf, STR_MAX, "PEAKS_%i\n", i);
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
            std::snprintf(tmpBuf, STR_MAX, "%f:%f:%f:%f\n",
                          static_cast<double>(plugData.peaks[0]),
                          static_cast<double>(plugData.peaks[1]),
                          static_cast<double>(plugData.peaks[2]),
                          static_cast<double>(plugData.peaks[3]));
            CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

            fUiServer.flushMessages();

            for (uint32_t j=0, count=plugin->getParameterCount(); j < count; ++j)
            {
                if (! plugin->isParameterOutput(j))
                    continue;

                std::snprintf(tmpBuf, STR_MAX, "PARAMVAL_%i:%i\n", i, j);
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);
                std::snprintf(tmpBuf, STR_MAX, "%f\n", static_cast<double>(plugin->getParameterValue(j)));
                CARLA_SAFE_ASSERT_RETURN(fUiServer.writeMessage(tmpBuf),);

                fUiServer.flushMessages();
            }
        }
    }

    // -------------------------------------------------------------------
    // Plugin state calls

    char* getState() const
    {
        MemoryOutputStream out;
        saveProjectInternal(out);
        return strdup(out.toString().toRawUTF8());
    }

    void setState(const char* const data)
    {
        // remove all plugins from UI side
        for (uint i=0, count=pData->curPluginCount; i < count; ++i)
            CarlaEngine::callback(true, true, ENGINE_CALLBACK_PLUGIN_REMOVED, count-i-1, 0, 0, 0, 0.0f, nullptr);

        // remove all plugins from backend, no lock
        fIsRunning = false;
        removeAllPlugins();
        fIsRunning = true;

        // stopped during removeAllPlugins()
        if (! pData->thread.isThreadRunning())
            pData->thread.startThread();

        fOptionsForced = true;
        const String state(data);
        XmlDocument xml(state);
        loadProjectInternal(xml);
    }

    // -------------------------------------------------------------------

public:
    #define handlePtr ((CarlaEngineNative*)handle)

    static NativePluginHandle _instantiateRack(const NativeHostDescriptor* host)
    {
        return new CarlaEngineNative(host, false, true);
    }

    static NativePluginHandle _instantiateRackNoMidiOut(const NativeHostDescriptor* host)
    {
        return new CarlaEngineNative(host, false, false);
    }

    static NativePluginHandle _instantiatePatchbay(const NativeHostDescriptor* host)
    {
        return new CarlaEngineNative(host, true, true);
    }

    static NativePluginHandle _instantiatePatchbay3s(const NativeHostDescriptor* host)
    {
        return new CarlaEngineNative(host, true, true, 3, 2);
    }

    static NativePluginHandle _instantiatePatchbay16(const NativeHostDescriptor* host)
    {
        return new CarlaEngineNative(host, true, true, 16, 16);
    }

    static NativePluginHandle _instantiatePatchbay32(const NativeHostDescriptor* host)
    {
        return new CarlaEngineNative(host, true, true, 32, 32);
    }

    static NativePluginHandle _instantiatePatchbay64(const NativeHostDescriptor* host)
    {
        return new CarlaEngineNative(host, true, true, 64, 64);
    }

    static NativePluginHandle _instantiatePatchbayCV(const NativeHostDescriptor* host)
    {
        return new CarlaEngineNative(host, true, true, 2, 2, 5, 5);
    }

    static void _cleanup(NativePluginHandle handle)
    {
        delete handlePtr;
    }

    static uint32_t _get_parameter_count(NativePluginHandle handle)
    {
        return handlePtr->getParameterCount();
    }

    static const NativeParameter* _get_parameter_info(NativePluginHandle handle, uint32_t index)
    {
        return handlePtr->getParameterInfo(index);
    }

    static float _get_parameter_value(NativePluginHandle handle, uint32_t index)
    {
        return handlePtr->getParameterValue(index);
    }

    static uint32_t _get_midi_program_count(NativePluginHandle handle)
    {
        return handlePtr->getMidiProgramCount();
    }

    static const NativeMidiProgram* _get_midi_program_info(NativePluginHandle handle, uint32_t index)
    {
        return handlePtr->getMidiProgramInfo(index);
    }

    static void _set_parameter_value(NativePluginHandle handle, uint32_t index, float value)
    {
        handlePtr->setParameterValue(index, value);
    }

    static void _set_midi_program(NativePluginHandle handle, uint8_t channel, uint32_t bank, uint32_t program)
    {
        handlePtr->setMidiProgram(channel, bank, program);
    }

    static void _ui_show(NativePluginHandle handle, bool show)
    {
        handlePtr->uiShow(show);
    }

    static void _ui_idle(NativePluginHandle handle)
    {
        handlePtr->uiIdle();
    }

    static void _ui_set_parameter_value(NativePluginHandle handle, uint32_t index, float value)
    {
        handlePtr->uiSetParameterValue(index, value);
    }

    static void _activate(NativePluginHandle handle)
    {
        handlePtr->activate();
    }

    static void _deactivate(NativePluginHandle handle)
    {
        handlePtr->deactivate();
    }

    static void _process(NativePluginHandle handle,
                         const float** inBuffer, float** outBuffer, const uint32_t frames,
                         const NativeMidiEvent* midiEvents, uint32_t midiEventCount)
    {
        handlePtr->process(inBuffer, outBuffer, frames, midiEvents, midiEventCount);
    }

    static char* _get_state(NativePluginHandle handle)
    {
        return handlePtr->getState();
    }

    static void _set_state(NativePluginHandle handle, const char* data)
    {
        handlePtr->setState(data);
    }

    static intptr_t _dispatcher(NativePluginHandle handle,
                                NativePluginDispatcherOpcode opcode, int32_t index, intptr_t value, void* ptr, float opt)
    {
        switch(opcode)
        {
        case NATIVE_PLUGIN_OPCODE_NULL:
            return 0;
        case NATIVE_PLUGIN_OPCODE_BUFFER_SIZE_CHANGED:
            CARLA_SAFE_ASSERT_RETURN(value > 0, 0);
            handlePtr->bufferSizeChanged(static_cast<uint32_t>(value));
            return 0;
        case NATIVE_PLUGIN_OPCODE_SAMPLE_RATE_CHANGED:
            CARLA_SAFE_ASSERT_RETURN(opt > 0.0f, 0);
            handlePtr->sampleRateChanged(static_cast<double>(opt));
            return 0;
        case NATIVE_PLUGIN_OPCODE_OFFLINE_CHANGED:
            handlePtr->offlineModeChanged(value != 0);
            return 0;
        case NATIVE_PLUGIN_OPCODE_UI_NAME_CHANGED:
            //handlePtr->uiNameChanged(static_cast<const char*>(ptr));
            return 0;
        case NATIVE_PLUGIN_OPCODE_GET_INTERNAL_HANDLE: {
            CarlaEngineNative* const engine = handlePtr;
            return (intptr_t)(CarlaEngine*)engine;
        }
        case NATIVE_PLUGIN_OPCODE_IDLE:
            //handlePtr->idle();
            return 0;
        }

        return 0;

        // unused
        (void)index;
        (void)ptr;
    }

    // -------------------------------------------------------------------

    static void _ui_server_callback(void* handle, EngineCallbackOpcode action, uint pluginId,
                                    int value1, int value2, int value3,
                                    float valuef, const char* valueStr)
    {
        handlePtr->uiServerCallback(action, pluginId, value1, value2, value3, valuef, valueStr);
    }

    static const char* _ui_file_callback(void* handle, FileCallbackOpcode action, bool isDir,
                                         const char* title, const char* filter)
    {
        return handlePtr->uiFileCallback(action, isDir, title, filter);
    }

    // -------------------------------------------------------------------

    #undef handlePtr

private:
    const NativeHostDescriptor* const pHost;

#ifdef USE_JUCE_MESSAGE_THREAD
    const bool kNeedsJuceMsgThread;
    const juce::SharedResourcePointer<SharedJuceMessageThread> fJuceMsgThread;
#endif

    const bool kIsPatchbay; // rack if false
    const bool kHasMidiOut;
    bool fIsActive, fIsRunning;
    CarlaEngineNativeUI fUiServer;

    float fParameters[kNumInParams+kNumOutParams];

    bool fOptionsForced;

    CarlaPlugin* _getFirstPlugin() const noexcept
    {
        if (pData->curPluginCount == 0 || pData->plugins == nullptr)
            return nullptr;

        CarlaPlugin* const plugin(pData->plugins[0].plugin);

        if (plugin == nullptr || ! plugin->isEnabled())
            return nullptr;

        return pData->plugins[0].plugin;
    }

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CarlaEngineNative)
};

// -----------------------------------------------------------------------

bool CarlaEngineNativeUI::msgReceived(const char*const msg) noexcept
{
    if (CarlaExternalUI::msgReceived(msg))
        return true;

    bool ok = true;

    if (std::strcmp(msg, "set_engine_option") == 0)
    {
        uint32_t option;
        int32_t value;
        const char* valueStr = nullptr;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(option), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsInt(value), true);
        readNextLineAsString(valueStr); // can be null

        try {
            fEngine->setOption(static_cast<EngineOption>(option), value, valueStr);
        } CARLA_SAFE_EXCEPTION("setOption");

        if (valueStr != nullptr)
            delete[] valueStr;
    }
    else if (std::strcmp(msg, "clear_engine_xruns") == 0)
    {
        fEngine->clearXruns();
    }
    else if (std::strcmp(msg, "cancel_engine_action") == 0)
    {
        fEngine->setActionCanceled(true);
    }
    else if (std::strcmp(msg, "load_file") == 0)
    {
        const char* filename;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(filename), true);

        try {
            ok = fEngine->loadFile(filename);
        } CARLA_SAFE_EXCEPTION("loadFile");

        delete[] filename;
    }
    else if (std::strcmp(msg, "load_project") == 0)
    {
        const char* filename;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(filename), true);

        try {
            ok = fEngine->loadProject(filename, true);
        } CARLA_SAFE_EXCEPTION("loadProject");

        delete[] filename;
    }
    else if (std::strcmp(msg, "save_project") == 0)
    {
        const char* filename;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(filename), true);

        try {
            ok = fEngine->saveProject(filename, true);
        } CARLA_SAFE_EXCEPTION("saveProject");

        delete[] filename;
    }
    else if (std::strcmp(msg, "clear_project_filename") == 0)
    {
        fEngine->clearCurrentProjectFilename();
    }
    else if (std::strcmp(msg, "patchbay_connect") == 0)
    {
        bool external;
        uint32_t groupA, portA, groupB, portB;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsBool(external), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(groupA), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(portA), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(groupB), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(portB), true);

        try {
            ok = fEngine->patchbayConnect(external, groupA, portA, groupB, portB);
        } CARLA_SAFE_EXCEPTION("patchbayConnect");
    }
    else if (std::strcmp(msg, "patchbay_disconnect") == 0)
    {
        bool external;
        uint32_t connectionId;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsBool(external), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(connectionId), true);

        try {
            ok = fEngine->patchbayDisconnect(external, connectionId);
        } CARLA_SAFE_EXCEPTION("patchbayDisconnect");
    }
    else if (std::strcmp(msg, "patchbay_refresh") == 0)
    {
        bool external;
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsBool(external), true);

        try {
            ok = fEngine->patchbayRefresh(true, false, external);
        } CARLA_SAFE_EXCEPTION("patchbayRefresh");
    }
    else if (std::strcmp(msg, "transport_play") == 0)
    {
        fEngine->transportPlay();
    }
    else if (std::strcmp(msg, "transport_pause") == 0)
    {
        fEngine->transportPause();
    }
    else if (std::strcmp(msg, "transport_bpm") == 0)
    {
        double bpm;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsDouble(bpm), true);

        fEngine->transportBPM(bpm);
    }
    else if (std::strcmp(msg, "transport_relocate") == 0)
    {
        uint64_t frame;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsULong(frame), true);

        fEngine->transportRelocate(frame);
    }
    else if (std::strcmp(msg, "add_plugin") == 0)
    {
        uint32_t btype, ptype;
        const char* filename = nullptr;
        const char* name;
        const char* label;
        int64_t uniqueId;
        uint options;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(btype), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(ptype), true);
        readNextLineAsString(filename); // can be null
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(name), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(label), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsLong(uniqueId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(options), true);

        if (filename != nullptr && std::strcmp(filename, "(null)") == 0)
        {
            delete[] filename;
            filename = nullptr;
        }

        if (std::strcmp(name, "(null)") == 0)
        {
            delete[] name;
            name = nullptr;
        }

        ok = fEngine->addPlugin(static_cast<BinaryType>(btype), static_cast<PluginType>(ptype),
                                filename, name, label, uniqueId, nullptr, options);

        if (filename != nullptr)
            delete[] filename;
        if (name != nullptr)
            delete[] name;
        delete[] label;

        fEngine->reloadFromUI();
    }
    else if (std::strcmp(msg, "remove_plugin") == 0)
    {
        uint32_t pluginId;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);

        ok = fEngine->removePlugin(pluginId);

        if (pluginId == 0)
            fEngine->reloadFromUI();
    }
    else if (std::strcmp(msg, "remove_all_plugins") == 0)
    {
        ok = fEngine->removeAllPlugins();
        fEngine->reloadFromUI();
    }
    else if (std::strcmp(msg, "rename_plugin") == 0)
    {
        uint32_t pluginId;
        const char* newName;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(newName), true);

        ok = fEngine->renamePlugin(pluginId, newName);

        delete[] newName;
    }
    else if (std::strcmp(msg, "clone_plugin") == 0)
    {
        uint32_t pluginId;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);

        ok = fEngine->clonePlugin(pluginId);
    }
    else if (std::strcmp(msg, "replace_plugin") == 0)
    {
        uint32_t pluginId;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);

        ok = fEngine->replacePlugin(pluginId);
    }
    else if (std::strcmp(msg, "switch_plugins") == 0)
    {
        uint32_t pluginIdA, pluginIdB;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginIdA), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginIdB), true);

        ok = fEngine->switchPlugins(pluginIdA, pluginIdB);

        if (pluginIdA == 0 || pluginIdB == 0)
            fEngine->reloadFromUI();
    }
    else if (std::strcmp(msg, "load_plugin_state") == 0)
    {
        uint32_t pluginId;
        const char* filename;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(filename), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
        {
            plugin->loadStateFromFile(filename);
            _updateParamValues(plugin, pluginId, false, true);
        }

        delete[] filename;
    }
    else if (std::strcmp(msg, "save_plugin_state") == 0)
    {
        uint32_t pluginId;
        const char* filename;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(filename), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->saveStateToFile(filename);

        delete[] filename;
    }
    else if (std::strcmp(msg, "set_option") == 0)
    {
        uint32_t pluginId, option;
        bool yesNo;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(option), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsBool(yesNo), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->setOption(option, yesNo, false);
    }
    else if (std::strcmp(msg, "set_active") == 0)
    {
        uint32_t pluginId;
        bool onOff;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsBool(onOff), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->setActive(onOff, true, false);
    }
    else if (std::strcmp(msg, "set_drywet") == 0)
    {
        uint32_t pluginId;
        float value;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsFloat(value), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->setDryWet(value, true, false);
    }
    else if (std::strcmp(msg, "set_volume") == 0)
    {
        uint32_t pluginId;
        float value;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsFloat(value), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->setVolume(value, true, false);
    }
    else if (std::strcmp(msg, "set_balance_left") == 0)
    {
        uint32_t pluginId;
        float value;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsFloat(value), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->setBalanceLeft(value, true, false);
    }
    else if (std::strcmp(msg, "set_balance_right") == 0)
    {
        uint32_t pluginId;
        float value;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsFloat(value), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->setBalanceRight(value, true, false);
    }
    else if (std::strcmp(msg, "set_panning") == 0)
    {
        uint32_t pluginId;
        float value;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsFloat(value), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->setPanning(value, true, false);
    }
    else if (std::strcmp(msg, "set_ctrl_channel") == 0)
    {
        uint32_t pluginId;
        int32_t channel;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsInt(channel), true);
        CARLA_SAFE_ASSERT_RETURN(channel >= -1 && channel < MAX_MIDI_CHANNELS, true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->setCtrlChannel(int8_t(channel), true, false);
    }
    else if (std::strcmp(msg, "set_parameter_value") == 0)
    {
        uint32_t pluginId, parameterId;
        float value;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(parameterId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsFloat(value), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
        {
            plugin->setParameterValue(parameterId, value, true, true, false);
            fEngine->setParameterValueFromUI(pluginId, parameterId, value);
        }
    }
    else if (std::strcmp(msg, "set_parameter_midi_channel") == 0)
    {
        uint32_t pluginId, parameterId, channel;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(parameterId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(channel), true);
        CARLA_SAFE_ASSERT_RETURN(channel < MAX_MIDI_CHANNELS, true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->setParameterMidiChannel(parameterId, static_cast<uint8_t>(channel), true, false);
    }
    else if (std::strcmp(msg, "set_parameter_midi_cc") == 0)
    {
        uint32_t pluginId, parameterId;
        int32_t cc;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(parameterId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsInt(cc), true);
        CARLA_SAFE_ASSERT_RETURN(cc >= -1 && cc < MAX_MIDI_CONTROL, true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->setParameterMidiCC(parameterId, static_cast<int16_t>(cc), true, false);
    }
    else if (std::strcmp(msg, "set_parameter_touch") == 0)
    {
        uint32_t pluginId, parameterId;
        bool touching;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(parameterId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsBool(touching), true);

        if (fEngine->getPlugin(pluginId) != nullptr)
            fEngine->setParameterTouchFromUI(pluginId, parameterId, touching);
    }
    else if (std::strcmp(msg, "set_program") == 0)
    {
        uint32_t pluginId;
        int32_t index;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsInt(index), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
        {
            plugin->setProgram(index, true, true, false);
            _updateParamValues(plugin, pluginId, true, true);
        }
    }
    else if (std::strcmp(msg, "set_midi_program") == 0)
    {
        uint32_t pluginId;
        int32_t index;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsInt(index), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
        {
            plugin->setMidiProgram(index, true, true, false);
            _updateParamValues(plugin, pluginId, true, true);
        }
    }
    else if (std::strcmp(msg, "set_custom_data") == 0)
    {
        uint32_t pluginId;
        const char* type;
        const char* key;
        const char* value;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(type), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(key), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(value), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->setCustomData(type, key, value, true);

        delete[] type;
        delete[] key;
        delete[] value;
    }
    else if (std::strcmp(msg, "set_chunk_data") == 0)
    {
        uint32_t pluginId;
        const char* cdata;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(cdata), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
        {
            std::vector<uint8_t> chunk(carla_getChunkFromBase64String(cdata));
#ifdef CARLA_PROPER_CPP11_SUPPORT
            plugin->setChunkData(chunk.data(), chunk.size());
#else
            plugin->setChunkData(&chunk.front(), chunk.size());
#endif
            _updateParamValues(plugin, pluginId, false, true);
        }

        delete[] cdata;
    }
    else if (std::strcmp(msg, "prepare_for_save") == 0)
    {
        uint32_t pluginId;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->prepareForSave();
    }
    else if (std::strcmp(msg, "reset_parameters") == 0)
    {
        uint32_t pluginId;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
        {
            plugin->resetParameters();
            _updateParamValues(plugin, pluginId, false, true);
        }
    }
    else if (std::strcmp(msg, "randomize_parameters") == 0)
    {
        uint32_t pluginId;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
        {
            plugin->randomizeParameters();
            _updateParamValues(plugin, pluginId, false, true);
        }
    }
    else if (std::strcmp(msg, "send_midi_note") == 0)
    {
        uint32_t pluginId, channel, note, velocity;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(channel), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(note), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(velocity), true);
        CARLA_SAFE_ASSERT_RETURN(channel < MAX_MIDI_CHANNELS, true);
        CARLA_SAFE_ASSERT_RETURN(note < MAX_MIDI_VALUE, true);
        CARLA_SAFE_ASSERT_RETURN(velocity < MAX_MIDI_VALUE, true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->sendMidiSingleNote(static_cast<uint8_t>(channel), static_cast<uint8_t>(note), static_cast<uint8_t>(velocity), true, true, false);
    }
    else if (std::strcmp(msg, "show_custom_ui") == 0)
    {
        uint32_t pluginId;
        bool yesNo;

        CARLA_SAFE_ASSERT_RETURN(readNextLineAsUInt(pluginId), true);
        CARLA_SAFE_ASSERT_RETURN(readNextLineAsBool(yesNo), true);

        if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
            plugin->showCustomUI(yesNo);
    }
    else
    {
        carla_stderr("CarlaEngineNativeUI::msgReceived : %s", msg);
        return false;
    }

    if (! ok)
    {
        const CarlaMutexLocker cml(getPipeLock());

        if (writeMessage("error\n", 6) && writeAndFixMessage(fEngine->getLastError()))
            flushMessages();
    }

    return true;
}

void CarlaEngineNativeUI::_updateParamValues(CarlaPlugin* const plugin, const uint32_t pluginId,
                                             const bool sendCallback, const bool sendPluginHost) const noexcept
{
    float value;

    for (uint32_t i=0, count=plugin->getParameterCount(); i<count; ++i) {
        value = plugin->getParameterValue(i);

        if (sendCallback) {
            fEngine->callback(true, true,
                              ENGINE_CALLBACK_PARAMETER_VALUE_CHANGED,
                              pluginId,
                              static_cast<int>(i),
                              0, 0,
                              value,
                              nullptr);
        }

        if (sendPluginHost) {
            fEngine->setParameterValueFromUI(pluginId, i, value);
        }
    }
}

// -----------------------------------------------------------------------

static const NativePluginDescriptor carlaRackDesc = {
    /* category  */ NATIVE_PLUGIN_CATEGORY_OTHER,
    /* hints     */ static_cast<NativePluginHints>(NATIVE_PLUGIN_IS_SYNTH
                                                  |NATIVE_PLUGIN_HAS_UI
                                                  |NATIVE_PLUGIN_NEEDS_UI_MAIN_THREAD
                                                  |NATIVE_PLUGIN_USES_STATE
                                                  |NATIVE_PLUGIN_USES_TIME),
    /* supports  */ static_cast<NativePluginSupports>(NATIVE_PLUGIN_SUPPORTS_EVERYTHING),
    /* audioIns  */ 2,
    /* audioOuts */ 2,
    /* midiIns   */ 1,
    /* midiOuts  */ 1,
    /* paramIns  */ kNumInParams,
    /* paramOuts */ kNumOutParams,
    /* name      */ "Carla-Rack",
    /* label     */ "carlarack",
    /* maker     */ "falkTX",
    /* copyright */ "GNU GPL v2+",
    CarlaEngineNative::_instantiateRack,
    CarlaEngineNative::_cleanup,
    CarlaEngineNative::_get_parameter_count,
    CarlaEngineNative::_get_parameter_info,
    CarlaEngineNative::_get_parameter_value,
    CarlaEngineNative::_get_midi_program_count,
    CarlaEngineNative::_get_midi_program_info,
    CarlaEngineNative::_set_parameter_value,
    CarlaEngineNative::_set_midi_program,
    /* _set_custom_data        */ nullptr,
    CarlaEngineNative::_ui_show,
    CarlaEngineNative::_ui_idle,
    CarlaEngineNative::_ui_set_parameter_value,
    /* _ui_set_midi_program    */ nullptr,
    /* _ui_set_custom_data     */ nullptr,
    CarlaEngineNative::_activate,
    CarlaEngineNative::_deactivate,
    CarlaEngineNative::_process,
    CarlaEngineNative::_get_state,
    CarlaEngineNative::_set_state,
    CarlaEngineNative::_dispatcher,
    /* _render_inline_dsplay */ nullptr,
    /* cvIns  */ 0,
    /* cvOuts */ 0
};

static const NativePluginDescriptor carlaRackNoMidiOutDesc = {
    /* category  */ NATIVE_PLUGIN_CATEGORY_OTHER,
    /* hints     */ static_cast<NativePluginHints>(NATIVE_PLUGIN_IS_SYNTH
                                                  |NATIVE_PLUGIN_HAS_UI
                                                  |NATIVE_PLUGIN_NEEDS_UI_MAIN_THREAD
                                                  |NATIVE_PLUGIN_USES_STATE
                                                  |NATIVE_PLUGIN_USES_TIME),
    /* supports  */ static_cast<NativePluginSupports>(NATIVE_PLUGIN_SUPPORTS_EVERYTHING),
    /* audioIns  */ 2,
    /* audioOuts */ 2,
    /* midiIns   */ 1,
    /* midiOuts  */ 0,
    /* paramIns  */ kNumInParams,
    /* paramOuts */ kNumOutParams,
    /* name      */ "Carla-Rack (no midi out)",
    /* label     */ "carlarack-nomidiout",
    /* maker     */ "falkTX",
    /* copyright */ "GNU GPL v2+",
    CarlaEngineNative::_instantiateRackNoMidiOut,
    CarlaEngineNative::_cleanup,
    CarlaEngineNative::_get_parameter_count,
    CarlaEngineNative::_get_parameter_info,
    CarlaEngineNative::_get_parameter_value,
    CarlaEngineNative::_get_midi_program_count,
    CarlaEngineNative::_get_midi_program_info,
    CarlaEngineNative::_set_parameter_value,
    CarlaEngineNative::_set_midi_program,
    /* _set_custom_data        */ nullptr,
    CarlaEngineNative::_ui_show,
    CarlaEngineNative::_ui_idle,
    /* _ui_set_parameter_value */ nullptr,
    /* _ui_set_midi_program    */ nullptr,
    /* _ui_set_custom_data     */ nullptr,
    CarlaEngineNative::_activate,
    CarlaEngineNative::_deactivate,
    CarlaEngineNative::_process,
    CarlaEngineNative::_get_state,
    CarlaEngineNative::_set_state,
    CarlaEngineNative::_dispatcher,
    /* _render_inline_dsplay */ nullptr,
    /* cvIns  */ 0,
    /* cvOuts */ 0
};

static const NativePluginDescriptor carlaPatchbayDesc = {
    /* category  */ NATIVE_PLUGIN_CATEGORY_OTHER,
    /* hints     */ static_cast<NativePluginHints>(NATIVE_PLUGIN_IS_SYNTH
                                                  |NATIVE_PLUGIN_HAS_UI
                                                  |NATIVE_PLUGIN_NEEDS_UI_MAIN_THREAD
                                                  |NATIVE_PLUGIN_USES_STATE
                                                  |NATIVE_PLUGIN_USES_TIME),
    /* supports  */ static_cast<NativePluginSupports>(NATIVE_PLUGIN_SUPPORTS_EVERYTHING),
    /* audioIns  */ 2,
    /* audioOuts */ 2,
    /* midiIns   */ 1,
    /* midiOuts  */ 1,
    /* paramIns  */ kNumInParams,
    /* paramOuts */ kNumOutParams,
    /* name      */ "Carla-Patchbay",
    /* label     */ "carlapatchbay",
    /* maker     */ "falkTX",
    /* copyright */ "GNU GPL v2+",
    CarlaEngineNative::_instantiatePatchbay,
    CarlaEngineNative::_cleanup,
    CarlaEngineNative::_get_parameter_count,
    CarlaEngineNative::_get_parameter_info,
    CarlaEngineNative::_get_parameter_value,
    CarlaEngineNative::_get_midi_program_count,
    CarlaEngineNative::_get_midi_program_info,
    CarlaEngineNative::_set_parameter_value,
    CarlaEngineNative::_set_midi_program,
    /* _set_custom_data        */ nullptr,
    CarlaEngineNative::_ui_show,
    CarlaEngineNative::_ui_idle,
    /* _ui_set_parameter_value */ nullptr,
    /* _ui_set_midi_program    */ nullptr,
    /* _ui_set_custom_data     */ nullptr,
    CarlaEngineNative::_activate,
    CarlaEngineNative::_deactivate,
    CarlaEngineNative::_process,
    CarlaEngineNative::_get_state,
    CarlaEngineNative::_set_state,
    CarlaEngineNative::_dispatcher,
    /* _render_inline_dsplay */ nullptr,
    /* cvIns  */ 0,
    /* cvOuts */ 0
};

static const NativePluginDescriptor carlaPatchbay3sDesc = {
    /* category  */ NATIVE_PLUGIN_CATEGORY_OTHER,
    /* hints     */ static_cast<NativePluginHints>(NATIVE_PLUGIN_IS_SYNTH
                                                  |NATIVE_PLUGIN_HAS_UI
                                                  |NATIVE_PLUGIN_NEEDS_UI_MAIN_THREAD
                                                  |NATIVE_PLUGIN_USES_STATE
                                                  |NATIVE_PLUGIN_USES_TIME),
    /* supports  */ static_cast<NativePluginSupports>(NATIVE_PLUGIN_SUPPORTS_EVERYTHING),
    /* audioIns  */ 3,
    /* audioOuts */ 2,
    /* midiIns   */ 1,
    /* midiOuts  */ 1,
    /* paramIns  */ kNumInParams,
    /* paramOuts */ kNumOutParams,
    /* name      */ "Carla-Patchbay (sidechain)",
    /* label     */ "carlapatchbay3s",
    /* maker     */ "falkTX",
    /* copyright */ "GNU GPL v2+",
    CarlaEngineNative::_instantiatePatchbay3s,
    CarlaEngineNative::_cleanup,
    CarlaEngineNative::_get_parameter_count,
    CarlaEngineNative::_get_parameter_info,
    CarlaEngineNative::_get_parameter_value,
    CarlaEngineNative::_get_midi_program_count,
    CarlaEngineNative::_get_midi_program_info,
    CarlaEngineNative::_set_parameter_value,
    CarlaEngineNative::_set_midi_program,
    /* _set_custom_data        */ nullptr,
    CarlaEngineNative::_ui_show,
    CarlaEngineNative::_ui_idle,
    /* _ui_set_parameter_value */ nullptr,
    /* _ui_set_midi_program    */ nullptr,
    /* _ui_set_custom_data     */ nullptr,
    CarlaEngineNative::_activate,
    CarlaEngineNative::_deactivate,
    CarlaEngineNative::_process,
    CarlaEngineNative::_get_state,
    CarlaEngineNative::_set_state,
    CarlaEngineNative::_dispatcher,
    /* _render_inline_dsplay */ nullptr,
    /* cvIns  */ 0,
    /* cvOuts */ 0
};

static const NativePluginDescriptor carlaPatchbay16Desc = {
    /* category  */ NATIVE_PLUGIN_CATEGORY_OTHER,
    /* hints     */ static_cast<NativePluginHints>(NATIVE_PLUGIN_IS_SYNTH
                                                  |NATIVE_PLUGIN_HAS_UI
                                                  |NATIVE_PLUGIN_NEEDS_UI_MAIN_THREAD
                                                  |NATIVE_PLUGIN_USES_STATE
                                                  |NATIVE_PLUGIN_USES_TIME),
    /* supports  */ static_cast<NativePluginSupports>(NATIVE_PLUGIN_SUPPORTS_EVERYTHING),
    /* audioIns  */ 16,
    /* audioOuts */ 16,
    /* midiIns   */ 1,
    /* midiOuts  */ 1,
    /* paramIns  */ kNumInParams,
    /* paramOuts */ kNumOutParams,
    /* name      */ "Carla-Patchbay (16chan)",
    /* label     */ "carlapatchbay16",
    /* maker     */ "falkTX",
    /* copyright */ "GNU GPL v2+",
    CarlaEngineNative::_instantiatePatchbay16,
    CarlaEngineNative::_cleanup,
    CarlaEngineNative::_get_parameter_count,
    CarlaEngineNative::_get_parameter_info,
    CarlaEngineNative::_get_parameter_value,
    CarlaEngineNative::_get_midi_program_count,
    CarlaEngineNative::_get_midi_program_info,
    CarlaEngineNative::_set_parameter_value,
    CarlaEngineNative::_set_midi_program,
    /* _set_custom_data        */ nullptr,
    CarlaEngineNative::_ui_show,
    CarlaEngineNative::_ui_idle,
    /* _ui_set_parameter_value */ nullptr,
    /* _ui_set_midi_program    */ nullptr,
    /* _ui_set_custom_data     */ nullptr,
    CarlaEngineNative::_activate,
    CarlaEngineNative::_deactivate,
    CarlaEngineNative::_process,
    CarlaEngineNative::_get_state,
    CarlaEngineNative::_set_state,
    CarlaEngineNative::_dispatcher,
    /* _render_inline_dsplay */ nullptr,
    /* cvIns  */ 0,
    /* cvOuts */ 0
};

static const NativePluginDescriptor carlaPatchbay32Desc = {
    /* category  */ NATIVE_PLUGIN_CATEGORY_OTHER,
    /* hints     */ static_cast<NativePluginHints>(NATIVE_PLUGIN_IS_SYNTH
                                                  |NATIVE_PLUGIN_HAS_UI
                                                  |NATIVE_PLUGIN_NEEDS_UI_MAIN_THREAD
                                                  |NATIVE_PLUGIN_USES_STATE
                                                  |NATIVE_PLUGIN_USES_TIME),
    /* supports  */ static_cast<NativePluginSupports>(NATIVE_PLUGIN_SUPPORTS_EVERYTHING),
    /* audioIns  */ 32,
    /* audioOuts */ 32,
    /* midiIns   */ 1,
    /* midiOuts  */ 1,
    /* paramIns  */ kNumInParams,
    /* paramOuts */ kNumOutParams,
    /* name      */ "Carla-Patchbay (32chan)",
    /* label     */ "carlapatchbay32",
    /* maker     */ "falkTX",
    /* copyright */ "GNU GPL v2+",
    CarlaEngineNative::_instantiatePatchbay32,
    CarlaEngineNative::_cleanup,
    CarlaEngineNative::_get_parameter_count,
    CarlaEngineNative::_get_parameter_info,
    CarlaEngineNative::_get_parameter_value,
    CarlaEngineNative::_get_midi_program_count,
    CarlaEngineNative::_get_midi_program_info,
    CarlaEngineNative::_set_parameter_value,
    CarlaEngineNative::_set_midi_program,
    /* _set_custom_data        */ nullptr,
    CarlaEngineNative::_ui_show,
    CarlaEngineNative::_ui_idle,
    /* _ui_set_parameter_value */ nullptr,
    /* _ui_set_midi_program    */ nullptr,
    /* _ui_set_custom_data     */ nullptr,
    CarlaEngineNative::_activate,
    CarlaEngineNative::_deactivate,
    CarlaEngineNative::_process,
    CarlaEngineNative::_get_state,
    CarlaEngineNative::_set_state,
    CarlaEngineNative::_dispatcher,
    /* _render_inline_dsplay */ nullptr,
    /* cvIns  */ 0,
    /* cvOuts */ 0
};

static const NativePluginDescriptor carlaPatchbay64Desc = {
    /* category  */ NATIVE_PLUGIN_CATEGORY_OTHER,
    /* hints     */ static_cast<NativePluginHints>(NATIVE_PLUGIN_IS_SYNTH
                                                  |NATIVE_PLUGIN_HAS_UI
                                                  |NATIVE_PLUGIN_NEEDS_UI_MAIN_THREAD
                                                  |NATIVE_PLUGIN_USES_STATE
                                                  |NATIVE_PLUGIN_USES_TIME),
    /* supports  */ static_cast<NativePluginSupports>(NATIVE_PLUGIN_SUPPORTS_EVERYTHING),
    /* audioIns  */ 64,
    /* audioOuts */ 64,
    /* midiIns   */ 1,
    /* midiOuts  */ 1,
    /* paramIns  */ kNumInParams,
    /* paramOuts */ kNumOutParams,
    /* name      */ "Carla-Patchbay (64chan)",
    /* label     */ "carlapatchbay64",
    /* maker     */ "falkTX",
    /* copyright */ "GNU GPL v2+",
    CarlaEngineNative::_instantiatePatchbay64,
    CarlaEngineNative::_cleanup,
    CarlaEngineNative::_get_parameter_count,
    CarlaEngineNative::_get_parameter_info,
    CarlaEngineNative::_get_parameter_value,
    CarlaEngineNative::_get_midi_program_count,
    CarlaEngineNative::_get_midi_program_info,
    CarlaEngineNative::_set_parameter_value,
    CarlaEngineNative::_set_midi_program,
    /* _set_custom_data        */ nullptr,
    CarlaEngineNative::_ui_show,
    CarlaEngineNative::_ui_idle,
    /* _ui_set_parameter_value */ nullptr,
    /* _ui_set_midi_program    */ nullptr,
    /* _ui_set_custom_data     */ nullptr,
    CarlaEngineNative::_activate,
    CarlaEngineNative::_deactivate,
    CarlaEngineNative::_process,
    CarlaEngineNative::_get_state,
    CarlaEngineNative::_set_state,
    CarlaEngineNative::_dispatcher,
    /* _render_inline_dsplay */ nullptr,
    /* cvIns  */ 0,
    /* cvOuts */ 0
};

static const NativePluginDescriptor carlaPatchbayCVDesc = {
    /* category  */ NATIVE_PLUGIN_CATEGORY_OTHER,
    /* hints     */ static_cast<NativePluginHints>(NATIVE_PLUGIN_IS_SYNTH
                                                  |NATIVE_PLUGIN_HAS_UI
                                                  |NATIVE_PLUGIN_NEEDS_UI_MAIN_THREAD
                                                  |NATIVE_PLUGIN_USES_CONTROL_VOLTAGE
                                                  |NATIVE_PLUGIN_USES_STATE
                                                  |NATIVE_PLUGIN_USES_TIME),
    /* supports  */ static_cast<NativePluginSupports>(NATIVE_PLUGIN_SUPPORTS_EVERYTHING),
    /* audioIns  */ 2,
    /* audioOuts */ 2,
    /* midiIns   */ 1,
    /* midiOuts  */ 1,
    /* paramIns  */ kNumInParams,
    /* paramOuts */ kNumOutParams,
    /* name      */ "Carla-Patchbay (CV)",
    /* label     */ "carlapatchbaycv",
    /* maker     */ "falkTX",
    /* copyright */ "GNU GPL v2+",
    CarlaEngineNative::_instantiatePatchbayCV,
    CarlaEngineNative::_cleanup,
    CarlaEngineNative::_get_parameter_count,
    CarlaEngineNative::_get_parameter_info,
    CarlaEngineNative::_get_parameter_value,
    CarlaEngineNative::_get_midi_program_count,
    CarlaEngineNative::_get_midi_program_info,
    CarlaEngineNative::_set_parameter_value,
    CarlaEngineNative::_set_midi_program,
    /* _set_custom_data        */ nullptr,
    CarlaEngineNative::_ui_show,
    CarlaEngineNative::_ui_idle,
    /* _ui_set_parameter_value */ nullptr,
    /* _ui_set_midi_program    */ nullptr,
    /* _ui_set_custom_data     */ nullptr,
    CarlaEngineNative::_activate,
    CarlaEngineNative::_deactivate,
    CarlaEngineNative::_process,
    CarlaEngineNative::_get_state,
    CarlaEngineNative::_set_state,
    CarlaEngineNative::_dispatcher,
    /* _render_inline_dsplay */ nullptr,
    /* cvIns  */ 5,
    /* cvOuts */ 5
};

CARLA_BACKEND_END_NAMESPACE

// -----------------------------------------------------------------------

CARLA_EXPORT
void carla_register_native_plugin_carla();

void carla_register_native_plugin_carla()
{
    CARLA_BACKEND_USE_NAMESPACE;
    carla_register_native_plugin(&carlaRackDesc);
    carla_register_native_plugin(&carlaRackNoMidiOutDesc);
    carla_register_native_plugin(&carlaPatchbayDesc);
    carla_register_native_plugin(&carlaPatchbay3sDesc);
    carla_register_native_plugin(&carlaPatchbay16Desc);
    carla_register_native_plugin(&carlaPatchbay32Desc);
    carla_register_native_plugin(&carlaPatchbay64Desc);
    carla_register_native_plugin(&carlaPatchbayCVDesc);
}

// -----------------------------------------------------------------------

const NativePluginDescriptor* carla_get_native_rack_plugin()
{
    CARLA_BACKEND_USE_NAMESPACE;
    return &carlaRackDesc;
}

const NativePluginDescriptor* carla_get_native_patchbay_plugin()
{
    CARLA_BACKEND_USE_NAMESPACE;
    return &carlaPatchbayDesc;
}

const NativePluginDescriptor* carla_get_native_patchbay16_plugin()
{
    CARLA_BACKEND_USE_NAMESPACE;
    return &carlaPatchbay16Desc;
}

const NativePluginDescriptor* carla_get_native_patchbay32_plugin()
{
    CARLA_BACKEND_USE_NAMESPACE;
    return &carlaPatchbay32Desc;
}

const NativePluginDescriptor* carla_get_native_patchbay64_plugin()
{
    CARLA_BACKEND_USE_NAMESPACE;
    return &carlaPatchbay64Desc;
}

const NativePluginDescriptor* carla_get_native_patchbay_cv_plugin()
{
    CARLA_BACKEND_USE_NAMESPACE;
    return &carlaPatchbayCVDesc;
}

// -----------------------------------------------------------------------
// Extra stuff for linking purposes

#ifdef CARLA_PLUGIN_EXPORT

CARLA_BACKEND_START_NAMESPACE

CarlaEngine* CarlaEngine::newJack() { return nullptr; }

# ifdef USING_JUCE
CarlaEngine*       CarlaEngine::newJuce(const AudioApi)           { return nullptr; }
uint               CarlaEngine::getJuceApiCount()                 { return 0;       }
const char*        CarlaEngine::getJuceApiName(const uint)        { return nullptr; }
const char* const* CarlaEngine::getJuceApiDeviceNames(const uint) { return nullptr; }
const EngineDriverDeviceInfo* CarlaEngine::getJuceDeviceInfo(const uint, const char* const) { return nullptr; }
bool               CarlaEngine::showJuceDeviceControlPanel(const uint, const char* const)   { return false; }
# else
CarlaEngine*       CarlaEngine::newRtAudio(const AudioApi)           { return nullptr; }
uint               CarlaEngine::getRtAudioApiCount()                 { return 0;       }
const char*        CarlaEngine::getRtAudioApiName(const uint)        { return nullptr; }
const char* const* CarlaEngine::getRtAudioApiDeviceNames(const uint) { return nullptr; }
const EngineDriverDeviceInfo* CarlaEngine::getRtAudioDeviceInfo(const uint, const char* const) { return nullptr; }
# endif

CARLA_BACKEND_END_NAMESPACE

#define CARLA_PLUGIN_UI_CLASS_PREFIX EngineNative
#include "CarlaHostCommon.cpp"
#include "CarlaPluginUI.cpp"
#include "CarlaDssiUtils.cpp"
#include "CarlaMacUtils.cpp"
#include "CarlaPatchbayUtils.cpp"
#include "CarlaPipeUtils.cpp"
#include "CarlaStateUtils.cpp"

#endif

// -----------------------------------------------------------------------
