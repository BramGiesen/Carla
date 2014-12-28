/*
 * Carla Plugin Bridge
 * Copyright (C) 2011-2014 Filipe Coelho <falktx@falktx.com>
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

#ifdef BUILD_BRIDGE
# error This file should be used under bridge mode
#endif

#include "CarlaPluginInternal.hpp"

#include "CarlaBackendUtils.hpp"
#include "CarlaBase64Utils.hpp"
#include "CarlaBridgeUtils.hpp"
#include "CarlaEngineUtils.hpp"
#include "CarlaMathUtils.hpp"
#include "CarlaShmUtils.hpp"
#include "CarlaThread.hpp"

#include "jackbridge/JackBridge.hpp"

#include <ctime>

// -------------------------------------------------------------------------------------------------------------------

using juce::ChildProcess;
using juce::File;
using juce::ScopedPointer;
using juce::String;
using juce::StringArray;

CARLA_BACKEND_START_NAMESPACE

// -------------------------------------------------------------------------------------------------------------------

struct BridgeAudioPool {
    CarlaString filename;
    std::size_t size;
    float* data;
    shm_t shm;

    BridgeAudioPool() noexcept
        : filename(),
          size(0),
          data(nullptr)
#ifdef CARLA_PROPER_CPP11_SUPPORT
        , shm(shm_t_INIT) {}
#else
    {
        shm = shm_t_INIT;
    }
#endif

    ~BridgeAudioPool() noexcept
    {
        // should be cleared by now
        CARLA_SAFE_ASSERT(data == nullptr);

        clear();
    }

    bool initialize() noexcept
    {
        char tmpFileBase[64];

        std::sprintf(tmpFileBase, PLUGIN_BRIDGE_NAMEPREFIX_AUDIO_POOL "XXXXXX");

        shm = carla_shm_create_temp(tmpFileBase);

        if (! carla_is_shm_valid(shm))
            return false;

        filename = tmpFileBase;
        return true;
    }

    void clear() noexcept
    {
        filename.clear();

        if (! carla_is_shm_valid(shm))
        {
            CARLA_SAFE_ASSERT(data == nullptr);
            return;
        }

        if (data != nullptr)
        {
            carla_shm_unmap(shm, data);
            data = nullptr;
        }

        size = 0;
        carla_shm_close(shm);
        carla_shm_init(shm);
    }

    void resize(const uint32_t bufferSize, const uint32_t audioPortCount, const uint32_t cvPortCount) noexcept
    {
        CARLA_SAFE_ASSERT_RETURN(carla_is_shm_valid(shm),);

        if (data != nullptr)
            carla_shm_unmap(shm, data);

        size = (audioPortCount+cvPortCount)*bufferSize*sizeof(float);

        if (size == 0)
            size = sizeof(float);

        data = (float*)carla_shm_map(shm, size);
    }

    CARLA_DECLARE_NON_COPY_STRUCT(BridgeAudioPool)
};

// -------------------------------------------------------------------------------------------------------------------

struct BridgeRtClientControl : public CarlaRingBufferControl<SmallStackBuffer> {
    BridgeRtClientData* data;
    CarlaString filename;
    bool needsSemDestroy;
    shm_t shm;

    BridgeRtClientControl()
        : data(nullptr),
          filename(),
          needsSemDestroy(false),
#ifdef CARLA_PROPER_CPP11_SUPPORT
          shm(shm_t_INIT) {}
#else
    {
        shm = shm_t_INIT;
    }
#endif

    ~BridgeRtClientControl() noexcept override
    {
        // should be cleared by now
        CARLA_SAFE_ASSERT(data == nullptr);

        clear();
    }

    bool initialize() noexcept
    {
        char tmpFileBase[64];

        std::sprintf(tmpFileBase, PLUGIN_BRIDGE_NAMEPREFIX_RT_CLIENT "XXXXXX");

        shm = carla_shm_create_temp(tmpFileBase);

        if (! carla_is_shm_valid(shm))
            return false;

        if (! mapData())
        {
            carla_shm_close(shm);
            carla_shm_init(shm);
            return false;
        }

        CARLA_SAFE_ASSERT(data != nullptr);

        if (! jackbridge_sem_init(&data->sem.server))
        {
            unmapData();
            carla_shm_close(shm);
            carla_shm_init(shm);
            return false;
        }

        if (! jackbridge_sem_init(&data->sem.client))
        {
            jackbridge_sem_destroy(&data->sem.server);
            unmapData();
            carla_shm_close(shm);
            carla_shm_init(shm);
            return false;
        }

        filename = tmpFileBase;
        needsSemDestroy = true;
        return true;
    }

    void clear() noexcept
    {
        filename.clear();

        if (needsSemDestroy)
        {
            jackbridge_sem_destroy(&data->sem.client);
            jackbridge_sem_destroy(&data->sem.server);
            needsSemDestroy = false;
        }

        if (data != nullptr)
            unmapData();

        if (! carla_is_shm_valid(shm))
            return;

        carla_shm_close(shm);
        carla_shm_init(shm);
    }

    bool mapData() noexcept
    {
        CARLA_SAFE_ASSERT(data == nullptr);

        if (carla_shm_map<BridgeRtClientData>(shm, data))
        {
            carla_zeroStruct(data->sem);
            carla_zeroStruct(data->timeInfo);
            carla_zeroBytes(data->midiOut, kBridgeRtClientDataMidiOutSize);
            setRingBuffer(&data->ringBuffer, true);
            return true;
        }

        return false;
    }

    void unmapData() noexcept
    {
        CARLA_SAFE_ASSERT_RETURN(data != nullptr,);

        carla_shm_unmap(shm, data);
        data = nullptr;

        setRingBuffer(nullptr, false);
    }

    bool waitForClient(const uint secs) noexcept
    {
        CARLA_SAFE_ASSERT_RETURN(data != nullptr, false);

        jackbridge_sem_post(&data->sem.server);

        return jackbridge_sem_timedwait(&data->sem.client, secs);
    }

    void writeOpcode(const PluginBridgeRtClientOpcode opcode) noexcept
    {
        writeUInt(static_cast<uint32_t>(opcode));
    }

    CARLA_DECLARE_NON_COPY_STRUCT(BridgeRtClientControl)
};

// -------------------------------------------------------------------------------------------------------------------

struct BridgeNonRtClientControl : public CarlaRingBufferControl<BigStackBuffer> {
    BridgeNonRtClientData* data;
    CarlaString filename;
    CarlaMutex mutex;
    shm_t shm;

    BridgeNonRtClientControl() noexcept
        : data(nullptr),
          filename(),
          mutex()
#ifdef CARLA_PROPER_CPP11_SUPPORT
        , shm(shm_t_INIT) {}
#else
    {
        shm = shm_t_INIT;
    }
#endif

    ~BridgeNonRtClientControl() noexcept override
    {
        // should be cleared by now
        CARLA_SAFE_ASSERT(data == nullptr);

        clear();
    }

    bool initialize() noexcept
    {
        char tmpFileBase[64];

        std::sprintf(tmpFileBase, PLUGIN_BRIDGE_NAMEPREFIX_NON_RT_CLIENT "XXXXXX");

        shm = carla_shm_create_temp(tmpFileBase);

        if (! carla_is_shm_valid(shm))
            return false;

        if (! mapData())
        {
            carla_shm_close(shm);
            carla_shm_init(shm);
            return false;
        }

        CARLA_SAFE_ASSERT(data != nullptr);

        filename = tmpFileBase;
        return true;
    }

    void clear() noexcept
    {
        filename.clear();

        if (data != nullptr)
            unmapData();

        if (! carla_is_shm_valid(shm))
            return;

        carla_shm_close(shm);
        carla_shm_init(shm);
    }

    bool mapData() noexcept
    {
        CARLA_SAFE_ASSERT(data == nullptr);

        if (carla_shm_map<BridgeNonRtClientData>(shm, data))
        {
            setRingBuffer(&data->ringBuffer, true);
            return true;
        }

        return false;
    }

    void unmapData() noexcept
    {
        CARLA_SAFE_ASSERT_RETURN(data != nullptr,);

        carla_shm_unmap(shm, data);
        data = nullptr;

        setRingBuffer(nullptr, false);
    }

    void writeOpcode(const PluginBridgeNonRtClientOpcode opcode) noexcept
    {
        writeUInt(static_cast<uint32_t>(opcode));
    }

    CARLA_DECLARE_NON_COPY_STRUCT(BridgeNonRtClientControl)
};

// -------------------------------------------------------------------------------------------------------------------

struct BridgeNonRtServerControl : public CarlaRingBufferControl<HugeStackBuffer> {
    BridgeNonRtServerData* data;
    CarlaString filename;
    shm_t shm;

    BridgeNonRtServerControl() noexcept
        : data(nullptr),
          filename()
#ifdef CARLA_PROPER_CPP11_SUPPORT
        , shm(shm_t_INIT) {}
#else
    {
        shm = shm_t_INIT;
    }
#endif

    ~BridgeNonRtServerControl() noexcept override
    {
        // should be cleared by now
        CARLA_SAFE_ASSERT(data == nullptr);

        clear();
    }

    bool initialize() noexcept
    {
        char tmpFileBase[64];

        std::sprintf(tmpFileBase, PLUGIN_BRIDGE_NAMEPREFIX_NON_RT_SERVER "XXXXXX");

        shm = carla_shm_create_temp(tmpFileBase);

        if (! carla_is_shm_valid(shm))
            return false;

        if (! mapData())
        {
            carla_shm_close(shm);
            carla_shm_init(shm);
            return false;
        }

        CARLA_SAFE_ASSERT(data != nullptr);

        filename = tmpFileBase;
        return true;
    }

    void clear() noexcept
    {
        filename.clear();

        if (data != nullptr)
            unmapData();

        if (! carla_is_shm_valid(shm))
            return;

        carla_shm_close(shm);
        carla_shm_init(shm);
    }

    bool mapData() noexcept
    {
        CARLA_SAFE_ASSERT(data == nullptr);

        if (carla_shm_map<BridgeNonRtServerData>(shm, data))
        {
            setRingBuffer(&data->ringBuffer, true);
            return true;
        }

        return false;
    }

    void unmapData() noexcept
    {
        CARLA_SAFE_ASSERT_RETURN(data != nullptr,);

        carla_shm_unmap(shm, data);
        data = nullptr;

        setRingBuffer(nullptr, false);
    }

    PluginBridgeNonRtServerOpcode readOpcode() noexcept
    {
        return static_cast<PluginBridgeNonRtServerOpcode>(readUInt());
    }

    CARLA_DECLARE_NON_COPY_STRUCT(BridgeNonRtServerControl)
};

// -------------------------------------------------------------------------------------------------------------------

struct BridgeParamInfo {
    float value;
    CarlaString name;
    CarlaString unit;

    BridgeParamInfo() noexcept
        : value(0.0f),
          name(),
          unit() {}

    CARLA_DECLARE_NON_COPY_STRUCT(BridgeParamInfo)
};

// -------------------------------------------------------------------------------------------------------------------

class CarlaPluginBridgeThread : public CarlaThread
{
public:
    CarlaPluginBridgeThread(CarlaEngine* const engine, CarlaPlugin* const plugin) noexcept
        : CarlaThread("CarlaPluginBridgeThread"),
          kEngine(engine),
          kPlugin(plugin),
          fBinary(),
          fLabel(),
          fShmIds(),
          fProcess(),
          leakDetector_CarlaPluginBridgeThread() {}

    void setData(const char* const binary, const char* const label, const char* const shmIds) noexcept
    {
        CARLA_SAFE_ASSERT_RETURN(binary != nullptr && binary[0] != '\0',);
        CARLA_SAFE_ASSERT_RETURN(label != nullptr,);
        CARLA_SAFE_ASSERT_RETURN(shmIds != nullptr && shmIds[0] != '\0',);
        CARLA_SAFE_ASSERT(! isThreadRunning());

        fBinary = binary;
        fLabel  = label;
        fShmIds = shmIds;

        if (fLabel.isEmpty())
            fLabel = "\"\"";
    }

protected:
    void run()
    {
        if (fProcess == nullptr)
        {
            fProcess = new ChildProcess();
        }
        else if (fProcess->isRunning())
        {
            carla_stderr("CarlaPluginBridgeThread::run() - already running, giving up...");
        }

        String name(kPlugin->getName());
        String filename(kPlugin->getFilename());

        if (name.isEmpty())
            name = "(none)";

        if (filename.isEmpty())
            filename = "\"\"";

        StringArray arguments;

#ifndef CARLA_OS_WIN
        // start with "wine" if needed
        if (fBinary.endsWithIgnoreCase(".exe"))
            arguments.add("wine");
#endif

        // binary
        arguments.add(fBinary);

        // plugin type
        arguments.add(getPluginTypeAsString(kPlugin->getType()));

        // filename
        arguments.add(filename);

        // label
        arguments.add(fLabel);

        // uniqueId
        arguments.add(String(static_cast<juce::int64>(kPlugin->getUniqueId())));

        bool started;

        {
            char strBuf[STR_MAX+1];
            strBuf[STR_MAX] = '\0';

            const EngineOptions& options(kEngine->getOptions());
            const ScopedEngineEnvironmentLocker _seel(kEngine);

#ifdef CARLA_OS_LINUX
            const char* const oldPreload(std::getenv("LD_PRELOAD"));

            if (oldPreload != nullptr)
                ::unsetenv("LD_PRELOAD");
#endif

            carla_setenv("ENGINE_OPTION_FORCE_STEREO",          bool2str(options.forceStereo));
            carla_setenv("ENGINE_OPTION_PREFER_PLUGIN_BRIDGES", bool2str(options.preferPluginBridges));
            carla_setenv("ENGINE_OPTION_PREFER_UI_BRIDGES",     bool2str(options.preferUiBridges));
            carla_setenv("ENGINE_OPTION_UIS_ALWAYS_ON_TOP",     bool2str(options.uisAlwaysOnTop));

            std::snprintf(strBuf, STR_MAX, "%u", options.maxParameters);
            carla_setenv("ENGINE_OPTION_MAX_PARAMETERS", strBuf);

            std::snprintf(strBuf, STR_MAX, "%u", options.uiBridgesTimeout);
            carla_setenv("ENGINE_OPTION_UI_BRIDGES_TIMEOUT",strBuf);

            if (options.pathLADSPA != nullptr)
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_LADSPA", options.pathLADSPA);
            else
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_LADSPA", "");

            if (options.pathDSSI != nullptr)
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_DSSI", options.pathDSSI);
            else
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_DSSI", "");

            if (options.pathLV2 != nullptr)
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_LV2", options.pathLV2);
            else
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_LV2", "");

            if (options.pathVST2 != nullptr)
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_VST2", options.pathVST2);
            else
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_VST2", "");

            if (options.pathVST3 != nullptr)
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_VST3", options.pathVST3);
            else
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_VST3", "");

            if (options.pathAU != nullptr)
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_AU", options.pathAU);
            else
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_AU", "");

            if (options.pathGIG != nullptr)
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_GIG", options.pathGIG);
            else
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_GIG", "");

            if (options.pathSF2 != nullptr)
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_SF2", options.pathSF2);
            else
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_SF2", "");

            if (options.pathSFZ != nullptr)
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_SFZ", options.pathSFZ);
            else
                carla_setenv("ENGINE_OPTION_PLUGIN_PATH_SFZ", "");

            if (options.binaryDir != nullptr)
                carla_setenv("ENGINE_OPTION_PATH_BINARIES", options.binaryDir);
            else
                carla_setenv("ENGINE_OPTION_PATH_BINARIES", "");

            if (options.resourceDir != nullptr)
                carla_setenv("ENGINE_OPTION_PATH_RESOURCES", options.resourceDir);
            else
                carla_setenv("ENGINE_OPTION_PATH_RESOURCES", "");

            carla_setenv("ENGINE_OPTION_PREVENT_BAD_BEHAVIOUR", bool2str(options.preventBadBehaviour));

            std::snprintf(strBuf, STR_MAX, P_UINTPTR, options.frontendWinId);
            carla_setenv("ENGINE_OPTION_FRONTEND_WIN_ID", strBuf);

            carla_setenv("ENGINE_BRIDGE_SHM_IDS", fShmIds.toRawUTF8());
            carla_setenv("WINEDEBUG", "-all");

            carla_stdout("starting plugin bridge, command is:\n%s \"%s\" \"%s\" \"%s\" " P_INT64,
                         fBinary.toRawUTF8(), getPluginTypeAsString(kPlugin->getType()), filename.toRawUTF8(), fLabel.toRawUTF8(), kPlugin->getUniqueId());

            started = fProcess->start(arguments);

#ifdef CARLA_OS_LINUX
            if (oldPreload != nullptr)
                ::setenv("LD_PRELOAD", oldPreload, 1);
#endif
        }

        if (! started)
        {
            carla_stdout("failed!");
            fProcess = nullptr;
            return;
        }

        for (; fProcess->isRunning() && ! shouldThreadExit();)
            carla_sleep(1);

        // we only get here if bridge crashed or thread asked to exit
        if (fProcess->isRunning() && shouldThreadExit())
        {
            fProcess->waitForProcessToFinish(2000);

            if (fProcess->isRunning())
            {
                carla_stdout("CarlaPluginBridgeThread::run() - bridge refused to close, force kill now");
                fProcess->kill();
            }
            else
            {
                carla_stdout("CarlaPluginBridgeThread::run() - bridge auto-closed successfully");
            }
        }
        else
        {
            // forced quit, may have crashed
            if (fProcess->getExitCode() != 0 /*|| fProcess->exitStatus() == QProcess::CrashExit*/)
            {
                carla_stderr("CarlaPluginBridgeThread::run() - bridge crashed");

                CarlaString errorString("Plugin '" + CarlaString(kPlugin->getName()) + "' has crashed!\n"
                                        "Saving now will lose its current settings.\n"
                                        "Please remove this plugin, and not rely on it from this point.");
                kEngine->callback(CarlaBackend::ENGINE_CALLBACK_ERROR, kPlugin->getId(), 0, 0, 0.0f, errorString);
            }
            else
                carla_stderr("CarlaPluginBridgeThread::run() - bridge closed cleanly");
        }

        carla_stdout("plugin bridge finished");
        fProcess = nullptr;
    }

private:
    CarlaEngine* const kEngine;
    CarlaPlugin* const kPlugin;

    String fBinary;
    String fLabel;
    String fShmIds;

    ScopedPointer<ChildProcess> fProcess;

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CarlaPluginBridgeThread)
};

// -------------------------------------------------------------------------------------------------------------------

class CarlaPluginBridge : public CarlaPlugin
{
public:
    CarlaPluginBridge(CarlaEngine* const engine, const uint id, const BinaryType btype, const PluginType ptype)
        : CarlaPlugin(engine, id),
          fBinaryType(btype),
          fPluginType(ptype),
          fInitiated(false),
          fInitError(false),
          fSaved(false),
          fTimedOut(false),
          fLastPongCounter(-1),
          fBridgeBinary(),
          fBridgeThread(engine, this),
          fShmAudioPool(),
          fShmRtClientControl(),
          fShmNonRtClientControl(),
          fShmNonRtServerControl(),
          fInfo(),
          fParams(nullptr),
          leakDetector_CarlaPluginBridge()
    {
        carla_debug("CarlaPluginBridge::CarlaPluginBridge(%p, %i, %s, %s)", engine, id, BinaryType2Str(btype), PluginType2Str(ptype));

        pData->hints |= PLUGIN_IS_BRIDGE;
    }

    ~CarlaPluginBridge() override
    {
        carla_debug("CarlaPluginBridge::~CarlaPluginBridge()");

        // close UI
        if (pData->hints & PLUGIN_HAS_CUSTOM_UI)
            pData->transientTryCounter = 0;

        pData->singleMutex.lock();
        pData->masterMutex.lock();

        if (pData->client != nullptr && pData->client->isActive())
            pData->client->deactivate();

        if (pData->active)
        {
            deactivate();
            pData->active = false;
        }

        if (fBridgeThread.isThreadRunning())
        {
            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientQuit);
            fShmNonRtClientControl.commitWrite();

            fShmRtClientControl.writeOpcode(kPluginBridgeRtClientQuit);
            fShmRtClientControl.commitWrite();

            if (! fTimedOut)
                fShmRtClientControl.waitForClient(3);
        }

        fBridgeThread.stopThread(3000);

        fShmNonRtServerControl.clear();
        fShmNonRtClientControl.clear();
        fShmRtClientControl.clear();
        fShmAudioPool.clear();

        clearBuffers();

        fInfo.chunk.clear();
    }

    // -------------------------------------------------------------------
    // Information (base)

    BinaryType getBinaryType() const noexcept
    {
        return fBinaryType;
    }

    PluginType getType() const noexcept override
    {
        return fPluginType;
    }

    PluginCategory getCategory() const noexcept override
    {
        return fInfo.category;
    }

    int64_t getUniqueId() const noexcept override
    {
        return fInfo.uniqueId;
    }

    // -------------------------------------------------------------------
    // Information (count)

    uint32_t getMidiInCount() const noexcept override
    {
        return fInfo.mIns;
    }

    uint32_t getMidiOutCount() const noexcept override
    {
        return fInfo.mOuts;
    }

    // -------------------------------------------------------------------
    // Information (current data)

    std::size_t getChunkData(void** const dataPtr) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(pData->options & PLUGIN_OPTION_USE_CHUNKS, 0);
        CARLA_SAFE_ASSERT_RETURN(dataPtr != nullptr, 0);
        CARLA_SAFE_ASSERT_RETURN(fInfo.chunk.size() > 0, 0);

        *dataPtr = fInfo.chunk.data();
        return fInfo.chunk.size();
    }

    // -------------------------------------------------------------------
    // Information (per-plugin data)

    uint getOptionsAvailable() const noexcept override
    {
        return fInfo.optionsAvailable;
    }

    float getParameterValue(const uint32_t parameterId) const noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(parameterId < pData->param.count, 0.0f);

        return fParams[parameterId].value;
    }

    void getLabel(char* const strBuf) const noexcept override
    {
        std::strncpy(strBuf, fInfo.label, STR_MAX);
    }

    void getMaker(char* const strBuf) const noexcept override
    {
        std::strncpy(strBuf, fInfo.maker, STR_MAX);
    }

    void getCopyright(char* const strBuf) const noexcept override
    {
        std::strncpy(strBuf, fInfo.copyright, STR_MAX);
    }

    void getRealName(char* const strBuf) const noexcept override
    {
        std::strncpy(strBuf, fInfo.name, STR_MAX);
    }

    void getParameterName(const uint32_t parameterId, char* const strBuf) const noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(parameterId < pData->param.count, nullStrBuf(strBuf));

        std::strncpy(strBuf, fParams[parameterId].name.buffer(), STR_MAX);
    }

    void getParameterUnit(const uint32_t parameterId, char* const strBuf) const noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(parameterId < pData->param.count, nullStrBuf(strBuf));

        std::strncpy(strBuf, fParams[parameterId].unit.buffer(), STR_MAX);
    }

    // -------------------------------------------------------------------
    // Set data (state)

    void prepareForSave() override
    {
        fSaved = false;

        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientPrepareForSave);
            fShmNonRtClientControl.commitWrite();
        }

        carla_stdout("CarlaPluginBridge::prepareForSave() - sent, now waiting...");

        for (int i=0; i < 200; ++i)
        {
            if (fSaved)
                break;
            carla_msleep(30);
            pData->engine->callback(ENGINE_CALLBACK_IDLE, 0, 0, 0, 0.0f, nullptr);
            pData->engine->idle();
        }

        if (! fSaved)
            carla_stderr("CarlaPluginBridge::prepareForSave() - Timeout while requesting save state");
        else
            carla_stdout("CarlaPluginBridge::prepareForSave() - success!");
    }

    // -------------------------------------------------------------------
    // Set data (internal stuff)

    void setOption(const uint option, const bool yesNo, const bool sendCallback) override
    {
        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetOption);
            fShmNonRtClientControl.writeUInt(option);
            fShmNonRtClientControl.writeBool(yesNo);
            fShmNonRtClientControl.commitWrite();
        }

        CarlaPlugin::setOption(option, yesNo, sendCallback);
    }

    void setCtrlChannel(const int8_t channel, const bool sendOsc, const bool sendCallback) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(sendOsc || sendCallback,); // never call this from RT

        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetCtrlChannel);
            fShmNonRtClientControl.writeShort(channel);
            fShmNonRtClientControl.commitWrite();
        }

        CarlaPlugin::setCtrlChannel(channel, sendOsc, sendCallback);
    }

    // -------------------------------------------------------------------
    // Set data (plugin-specific stuff)

    void setParameterValue(const uint32_t parameterId, const float value, const bool sendGui, const bool sendOsc, const bool sendCallback) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(sendGui || sendOsc || sendCallback,); // never call this from RT
        CARLA_SAFE_ASSERT_RETURN(parameterId < pData->param.count,);

        const float fixedValue(pData->param.getFixedValue(parameterId, value));
        fParams[parameterId].value = fixedValue;

        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetParameterValue);
            fShmNonRtClientControl.writeUInt(parameterId);
            fShmNonRtClientControl.writeFloat(value);
            fShmNonRtClientControl.commitWrite();
        }

        CarlaPlugin::setParameterValue(parameterId, fixedValue, sendGui, sendOsc, sendCallback);
    }

    void setParameterMidiChannel(const uint32_t parameterId, const uint8_t channel, const bool sendOsc, const bool sendCallback) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(sendOsc || sendCallback,); // never call this from RT
        CARLA_SAFE_ASSERT_RETURN(parameterId < pData->param.count,);
        CARLA_SAFE_ASSERT_RETURN(channel < MAX_MIDI_CHANNELS,);

        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetParameterMidiChannel);
            fShmNonRtClientControl.writeUInt(parameterId);
            fShmNonRtClientControl.writeByte(channel);
            fShmNonRtClientControl.commitWrite();
        }

        CarlaPlugin::setParameterMidiChannel(parameterId, channel, sendOsc, sendCallback);
    }

    void setParameterMidiCC(const uint32_t parameterId, const int16_t cc, const bool sendOsc, const bool sendCallback) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(sendOsc || sendCallback,); // never call this from RT
        CARLA_SAFE_ASSERT_RETURN(parameterId < pData->param.count,);
        CARLA_SAFE_ASSERT_RETURN(cc >= -1 && cc < MAX_MIDI_CONTROL,);

        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetParameterMidiCC);
            fShmNonRtClientControl.writeUInt(parameterId);
            fShmNonRtClientControl.writeShort(cc);
            fShmNonRtClientControl.commitWrite();
        }

        CarlaPlugin::setParameterMidiCC(parameterId, cc, sendOsc, sendCallback);
    }

    void setProgram(const int32_t index, const bool sendGui, const bool sendOsc, const bool sendCallback) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(sendGui || sendOsc || sendCallback,); // never call this from RT
        CARLA_SAFE_ASSERT_RETURN(index >= -1 && index < static_cast<int32_t>(pData->prog.count),);

        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetProgram);
            fShmNonRtClientControl.writeInt(index);
            fShmNonRtClientControl.commitWrite();
        }

        CarlaPlugin::setProgram(index, sendGui, sendOsc, sendCallback);
    }

    void setMidiProgram(const int32_t index, const bool sendGui, const bool sendOsc, const bool sendCallback) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(sendGui || sendOsc || sendCallback,); // never call this from RT
        CARLA_SAFE_ASSERT_RETURN(index >= -1 && index < static_cast<int32_t>(pData->midiprog.count),);

        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetMidiProgram);
            fShmNonRtClientControl.writeInt(index);
            fShmNonRtClientControl.commitWrite();
        }

        CarlaPlugin::setMidiProgram(index, sendGui, sendOsc, sendCallback);
    }

    void setCustomData(const char* const type, const char* const key, const char* const value, const bool sendGui) override
    {
        CARLA_SAFE_ASSERT_RETURN(type != nullptr && type[0] != '\0',);
        CARLA_SAFE_ASSERT_RETURN(key != nullptr && key[0] != '\0',);
        CARLA_SAFE_ASSERT_RETURN(value != nullptr,);

        using namespace juce;

        const uint32_t typeLen(static_cast<uint32_t>(std::strlen(type)));
        const uint32_t keyLen(static_cast<uint32_t>(std::strlen(key)));

        MemoryOutputStream valueMemStream;
        GZIPCompressorOutputStream compressedValueStream(&valueMemStream, 9, false);
        compressedValueStream.write(value, std::strlen(value));

        const CarlaString valueBase64(CarlaString::asBase64(valueMemStream.getData(), valueMemStream.getDataSize()));
        const uint32_t valueBase64Len(static_cast<uint32_t>(valueBase64.length()));
        CARLA_SAFE_ASSERT_RETURN(valueBase64.length() > 0,);

        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetCustomData);

            fShmNonRtClientControl.writeUInt(typeLen);
            fShmNonRtClientControl.writeCustomData(type, typeLen);

            fShmNonRtClientControl.writeUInt(keyLen);
            fShmNonRtClientControl.writeCustomData(key, keyLen);

            fShmNonRtClientControl.writeUInt(valueBase64Len);
            fShmNonRtClientControl.writeCustomData(valueBase64.buffer(), valueBase64Len);

            fShmNonRtClientControl.commitWrite();
        }

        CarlaPlugin::setCustomData(type, key, value, sendGui);
    }

    void setChunkData(const void* const data, const std::size_t dataSize) override
    {
        CARLA_SAFE_ASSERT_RETURN(pData->options & PLUGIN_OPTION_USE_CHUNKS,);
        CARLA_SAFE_ASSERT_RETURN(data != nullptr,);
        CARLA_SAFE_ASSERT_RETURN(dataSize > 0,);

        CarlaString dataBase64(CarlaString::asBase64(data, dataSize));
        CARLA_SAFE_ASSERT_RETURN(dataBase64.length() > 0,);

        String filePath(File::getSpecialLocation(File::tempDirectory).getFullPathName());

        filePath += CARLA_OS_SEP_STR ".CarlaChunk_";
        filePath += fShmAudioPool.filename.buffer() + 18;

        if (File(filePath).replaceWithText(dataBase64.buffer()))
        {
            const uint32_t ulength(static_cast<uint32_t>(filePath.length()));

            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetChunkDataFile);
            fShmNonRtClientControl.writeUInt(ulength);
            fShmNonRtClientControl.writeCustomData(filePath.toRawUTF8(), ulength);
            fShmNonRtClientControl.commitWrite();
        }
    }

    // -------------------------------------------------------------------
    // Set ui stuff

    void showCustomUI(const bool yesNo) override
    {
        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(yesNo ? kPluginBridgeNonRtClientShowUI : kPluginBridgeNonRtClientHideUI);
            fShmNonRtClientControl.commitWrite();
        }

        if (yesNo)
        {
            pData->tryTransient();
        }
        else
        {
            pData->transientTryCounter = 0;
        }
    }

    void idle() override
    {
        if (fBridgeThread.isThreadRunning())
        {
            if (fInitiated && fTimedOut && pData->active)
                setActive(false, true, true);

            {
                const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

                fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientPing);
                fShmNonRtClientControl.commitWrite();
            }

            try {
                handleNonRtData();
            } CARLA_SAFE_EXCEPTION("handleNonRtData");
        }
        else
            carla_stderr2("TESTING: Bridge has closed!");

        CarlaPlugin::idle();
    }

    // -------------------------------------------------------------------
    // Plugin state

    void reload() override
    {
        CARLA_SAFE_ASSERT_RETURN(pData->engine != nullptr,);
        carla_debug("CarlaPluginBridge::reload() - start");

        const EngineProcessMode processMode(pData->engine->getProccessMode());

        // Safely disable plugin for reload
        const ScopedDisabler sd(this);

        bool needsCtrlIn, needsCtrlOut;
        needsCtrlIn = needsCtrlOut = false;

        if (fInfo.aIns > 0)
        {
            pData->audioIn.createNew(fInfo.aIns);
        }

        if (fInfo.aOuts > 0)
        {
            pData->audioOut.createNew(fInfo.aOuts);
            needsCtrlIn = true;
        }

        if (fInfo.cvIns > 0)
        {
            pData->cvIn.createNew(fInfo.cvIns);
        }

        if (fInfo.cvOuts > 0)
        {
            pData->cvOut.createNew(fInfo.cvOuts);
        }

        if (fInfo.mIns > 0)
            needsCtrlIn = true;

        if (fInfo.mOuts > 0)
            needsCtrlOut = true;

        const uint portNameSize(pData->engine->getMaxPortNameSize());
        CarlaString portName;

        // Audio Ins
        for (uint32_t j=0; j < fInfo.aIns; ++j)
        {
            portName.clear();

            if (processMode == ENGINE_PROCESS_MODE_SINGLE_CLIENT)
            {
                portName  = pData->name;
                portName += ":";
            }

            if (fInfo.aIns > 1)
            {
                portName += "input_";
                portName += CarlaString(j+1);
            }
            else
                portName += "input";

            portName.truncate(portNameSize);

            pData->audioIn.ports[j].port   = (CarlaEngineAudioPort*)pData->client->addPort(kEnginePortTypeAudio, portName, true);
            pData->audioIn.ports[j].rindex = j;
        }

        // Audio Outs
        for (uint32_t j=0; j < fInfo.aOuts; ++j)
        {
            portName.clear();

            if (processMode == ENGINE_PROCESS_MODE_SINGLE_CLIENT)
            {
                portName  = pData->name;
                portName += ":";
            }

            if (fInfo.aOuts > 1)
            {
                portName += "output_";
                portName += CarlaString(j+1);
            }
            else
                portName += "output";

            portName.truncate(portNameSize);

            pData->audioOut.ports[j].port   = (CarlaEngineAudioPort*)pData->client->addPort(kEnginePortTypeAudio, portName, false);
            pData->audioOut.ports[j].rindex = j;
        }

        // TODO - CV

        if (needsCtrlIn)
        {
            portName.clear();

            if (processMode == ENGINE_PROCESS_MODE_SINGLE_CLIENT)
            {
                portName  = pData->name;
                portName += ":";
            }

            portName += "event-in";
            portName.truncate(portNameSize);

            pData->event.portIn = (CarlaEngineEventPort*)pData->client->addPort(kEnginePortTypeEvent, portName, true);
        }

        if (needsCtrlOut)
        {
            portName.clear();

            if (processMode == ENGINE_PROCESS_MODE_SINGLE_CLIENT)
            {
                portName  = pData->name;
                portName += ":";
            }

            portName += "event-out";
            portName.truncate(portNameSize);

            pData->event.portOut = (CarlaEngineEventPort*)pData->client->addPort(kEnginePortTypeEvent, portName, false);
        }

        // extra plugin hints
        pData->extraHints = 0x0;

        if (fInfo.mIns > 0)
            pData->extraHints |= PLUGIN_EXTRA_HINT_HAS_MIDI_IN;

        if (fInfo.mOuts > 0)
            pData->extraHints |= PLUGIN_EXTRA_HINT_HAS_MIDI_OUT;

        if (fInfo.aIns <= 2 && fInfo.aOuts <= 2 && (fInfo.aIns == fInfo.aOuts || fInfo.aIns == 0 || fInfo.aOuts == 0))
            pData->extraHints |= PLUGIN_EXTRA_HINT_CAN_RUN_RACK;

        bufferSizeChanged(pData->engine->getBufferSize());
        reloadPrograms(true);

        carla_debug("CarlaPluginBridge::reload() - end");
    }

    // -------------------------------------------------------------------
    // Plugin processing

    void activate() noexcept override
    {
        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientActivate);
            fShmNonRtClientControl.commitWrite();
        }

        bool timedOut = true;

        try {
            timedOut = waitForClient(1);
        } CARLA_SAFE_EXCEPTION("activate - waitForClient");

        if (! timedOut)
            fTimedOut = false;
    }

    void deactivate() noexcept override
    {
        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientDeactivate);
            fShmNonRtClientControl.commitWrite();
        }

        bool timedOut = true;

        try {
            timedOut = waitForClient(1);
        } CARLA_SAFE_EXCEPTION("deactivate - waitForClient");

        if (! timedOut)
            fTimedOut = false;
    }

    void process(const float** const audioIn, float** const audioOut, const float** const cvIn, float** const cvOut, const uint32_t frames) override
    {
        // --------------------------------------------------------------------------------------------------------
        // Check if active

        if (fTimedOut || ! pData->active)
        {
            // disable any output sound
            for (uint32_t i=0; i < pData->audioOut.count; ++i)
                FloatVectorOperations::clear(audioOut[i], static_cast<int>(frames));
            for (uint32_t i=0; i < pData->cvOut.count; ++i)
                FloatVectorOperations::clear(cvOut[i], static_cast<int>(frames));
            return;
        }

        // --------------------------------------------------------------------------------------------------------
        // Check if needs reset

        if (pData->needsReset)
        {
            // TODO

            pData->needsReset = false;
        }

        // --------------------------------------------------------------------------------------------------------
        // Event Input

        if (pData->event.portIn != nullptr)
        {
            // ----------------------------------------------------------------------------------------------------
            // MIDI Input (External)

            if (pData->extNotes.mutex.tryLock())
            {
                for (RtLinkedList<ExternalMidiNote>::Itenerator it = pData->extNotes.data.begin(); it.valid(); it.next())
                {
                    const ExternalMidiNote& note(it.getValue());

                    CARLA_SAFE_ASSERT_CONTINUE(note.channel >= 0 && note.channel < MAX_MIDI_CHANNELS);

                    uint8_t data1, data2, data3;
                    data1 = uint8_t((note.velo > 0 ? MIDI_STATUS_NOTE_ON : MIDI_STATUS_NOTE_OFF) | (note.channel & MIDI_CHANNEL_BIT));
                    data2 = note.note;
                    data3 = note.velo;

                    fShmRtClientControl.writeOpcode(kPluginBridgeRtClientMidiEvent);
                    fShmRtClientControl.writeUInt(0); // time
                    fShmRtClientControl.writeByte(0); // port
                    fShmRtClientControl.writeByte(3); // size
                    fShmRtClientControl.writeByte(data1);
                    fShmRtClientControl.writeByte(data2);
                    fShmRtClientControl.writeByte(data3);
                    fShmRtClientControl.commitWrite();
                }

                pData->extNotes.data.clear();
                pData->extNotes.mutex.unlock();

            } // End of MIDI Input (External)

            // ----------------------------------------------------------------------------------------------------
            // Event Input (System)

            bool allNotesOffSent = false;

            for (uint32_t i=0, numEvents=pData->event.portIn->getEventCount(); i < numEvents; ++i)
            {
                const EngineEvent& event(pData->event.portIn->getEvent(i));

                // Control change
                switch (event.type)
                {
                case kEngineEventTypeNull:
                    break;

                case kEngineEventTypeControl: {
                    const EngineControlEvent& ctrlEvent = event.ctrl;

                    switch (ctrlEvent.type)
                    {
                    case kEngineControlEventTypeNull:
                        break;

                    case kEngineControlEventTypeParameter:
                        // Control backend stuff
                        if (event.channel == pData->ctrlChannel)
                        {
                            float value;

                            if (MIDI_IS_CONTROL_BREATH_CONTROLLER(ctrlEvent.param) && (pData->hints & PLUGIN_CAN_DRYWET) != 0)
                            {
                                value = ctrlEvent.value;
                                setDryWet(value, false, false);
                                pData->postponeRtEvent(kPluginPostRtEventParameterChange, PARAMETER_DRYWET, 0, value);
                                break;
                            }

                            if (MIDI_IS_CONTROL_CHANNEL_VOLUME(ctrlEvent.param) && (pData->hints & PLUGIN_CAN_VOLUME) != 0)
                            {
                                value = ctrlEvent.value*127.0f/100.0f;
                                setVolume(value, false, false);
                                pData->postponeRtEvent(kPluginPostRtEventParameterChange, PARAMETER_VOLUME, 0, value);
                                break;
                            }

                            if (MIDI_IS_CONTROL_BALANCE(ctrlEvent.param) && (pData->hints & PLUGIN_CAN_BALANCE) != 0)
                            {
                                float left, right;
                                value = ctrlEvent.value/0.5f - 1.0f;

                                if (value < 0.0f)
                                {
                                    left  = -1.0f;
                                    right = (value*2.0f)+1.0f;
                                }
                                else if (value > 0.0f)
                                {
                                    left  = (value*2.0f)-1.0f;
                                    right = 1.0f;
                                }
                                else
                                {
                                    left  = -1.0f;
                                    right = 1.0f;
                                }

                                setBalanceLeft(left, false, false);
                                setBalanceRight(right, false, false);
                                pData->postponeRtEvent(kPluginPostRtEventParameterChange, PARAMETER_BALANCE_LEFT, 0, left);
                                pData->postponeRtEvent(kPluginPostRtEventParameterChange, PARAMETER_BALANCE_RIGHT, 0, right);
                                break;
                            }
                        }

                        fShmRtClientControl.writeOpcode(kPluginBridgeRtClientControlEventParameter);
                        fShmRtClientControl.writeUInt(event.time);
                        fShmRtClientControl.writeByte(event.channel);
                        fShmRtClientControl.writeUShort(event.ctrl.param);
                        fShmRtClientControl.writeFloat(event.ctrl.value);
                        fShmRtClientControl.commitWrite();
                        break;

                    case kEngineControlEventTypeMidiBank:
                        if (pData->options & PLUGIN_OPTION_MAP_PROGRAM_CHANGES)
                        {
                            fShmRtClientControl.writeOpcode(kPluginBridgeRtClientControlEventMidiBank);
                            fShmRtClientControl.writeUInt(event.time);
                            fShmRtClientControl.writeByte(event.channel);
                            fShmRtClientControl.writeUShort(event.ctrl.param);
                            fShmRtClientControl.commitWrite();
                        }
                        break;

                    case kEngineControlEventTypeMidiProgram:
                        if (pData->options & PLUGIN_OPTION_MAP_PROGRAM_CHANGES)
                        {
                            fShmRtClientControl.writeOpcode(kPluginBridgeRtClientControlEventMidiProgram);
                            fShmRtClientControl.writeUInt(event.time);
                            fShmRtClientControl.writeByte(event.channel);
                            fShmRtClientControl.writeUShort(event.ctrl.param);
                            fShmRtClientControl.commitWrite();
                        }
                        break;

                    case kEngineControlEventTypeAllSoundOff:
                        if (pData->options & PLUGIN_OPTION_SEND_ALL_SOUND_OFF)
                        {
                            fShmRtClientControl.writeOpcode(kPluginBridgeRtClientControlEventAllSoundOff);
                            fShmRtClientControl.writeUInt(event.time);
                            fShmRtClientControl.writeByte(event.channel);
                            fShmRtClientControl.commitWrite();
                        }
                        break;

                    case kEngineControlEventTypeAllNotesOff:
                        if (pData->options & PLUGIN_OPTION_SEND_ALL_SOUND_OFF)
                        {
                            if (event.channel == pData->ctrlChannel && ! allNotesOffSent)
                            {
                                allNotesOffSent = true;
                                sendMidiAllNotesOffToCallback();
                            }

                            fShmRtClientControl.writeOpcode(kPluginBridgeRtClientControlEventAllNotesOff);
                            fShmRtClientControl.writeUInt(event.time);
                            fShmRtClientControl.writeByte(event.channel);
                            fShmRtClientControl.commitWrite();
                        }
                        break;
                    } // switch (ctrlEvent.type)
                    break;
                } // case kEngineEventTypeControl

                case kEngineEventTypeMidi: {
                    const EngineMidiEvent& midiEvent(event.midi);

                    if (midiEvent.size == 0 || midiEvent.size >= MAX_MIDI_VALUE)
                        continue;

                    const uint8_t* const midiData(midiEvent.size > EngineMidiEvent::kDataSize ? midiEvent.dataExt : midiEvent.data);

                    uint8_t status = uint8_t(MIDI_GET_STATUS_FROM_DATA(midiData));

                    if (status == MIDI_STATUS_CHANNEL_PRESSURE && (pData->options & PLUGIN_OPTION_SEND_CHANNEL_PRESSURE) == 0)
                        continue;
                    if (status == MIDI_STATUS_CONTROL_CHANGE && (pData->options & PLUGIN_OPTION_SEND_CONTROL_CHANGES) == 0)
                        continue;
                    if (status == MIDI_STATUS_POLYPHONIC_AFTERTOUCH && (pData->options & PLUGIN_OPTION_SEND_NOTE_AFTERTOUCH) == 0)
                        continue;
                    if (status == MIDI_STATUS_PITCH_WHEEL_CONTROL && (pData->options & PLUGIN_OPTION_SEND_PITCHBEND) == 0)
                        continue;

                    // Fix bad note-off
                    if (status == MIDI_STATUS_NOTE_ON && midiData[2] == 0)
                        status = MIDI_STATUS_NOTE_OFF;

                    fShmRtClientControl.writeOpcode(kPluginBridgeRtClientMidiEvent);
                    fShmRtClientControl.writeUInt(event.time);
                    fShmRtClientControl.writeByte(midiEvent.port);
                    fShmRtClientControl.writeByte(midiEvent.size);

                    fShmRtClientControl.writeByte(uint8_t(midiData[0] | (event.channel & MIDI_CHANNEL_BIT)));

                    for (uint8_t j=1; j < midiEvent.size; ++j)
                        fShmRtClientControl.writeByte(midiData[j]);

                    fShmRtClientControl.commitWrite();

                    if (status == MIDI_STATUS_NOTE_ON)
                        pData->postponeRtEvent(kPluginPostRtEventNoteOn, event.channel, midiData[1], midiData[2]);
                    else if (status == MIDI_STATUS_NOTE_OFF)
                        pData->postponeRtEvent(kPluginPostRtEventNoteOff, event.channel, midiData[1], 0.0f);
                } break;
                }
            }

            pData->postRtEvents.trySplice();

        } // End of Event Input

        processSingle(audioIn, audioOut, cvIn, cvOut, frames);
    }

    bool processSingle(const float** const audioIn, float** const audioOut, const float** const cvIn, float** const cvOut, const uint32_t frames)
    {
        CARLA_SAFE_ASSERT_RETURN(frames > 0, false);

        if (pData->audioIn.count > 0)
        {
            CARLA_SAFE_ASSERT_RETURN(audioIn != nullptr, false);
        }
        if (pData->audioOut.count > 0)
        {
            CARLA_SAFE_ASSERT_RETURN(audioOut != nullptr, false);
        }
        if (pData->cvIn.count > 0)
        {
            CARLA_SAFE_ASSERT_RETURN(cvIn != nullptr, false);
        }
        if (pData->cvOut.count > 0)
        {
            CARLA_SAFE_ASSERT_RETURN(cvOut != nullptr, false);
        }

        // --------------------------------------------------------------------------------------------------------
        // Try lock, silence otherwise

        if (pData->engine->isOffline())
        {
            pData->singleMutex.lock();
        }
        else if (! pData->singleMutex.tryLock())
        {
            for (uint32_t i=0; i < pData->audioOut.count; ++i)
                FloatVectorOperations::clear(audioOut[i], static_cast<int>(frames));
            for (uint32_t i=0; i < pData->cvOut.count; ++i)
                FloatVectorOperations::clear(cvOut[i], static_cast<int>(frames));
            return false;
        }

        // --------------------------------------------------------------------------------------------------------
        // Reset audio buffers

        for (uint32_t i=0; i < fInfo.aIns; ++i)
            FloatVectorOperations::copy(fShmAudioPool.data + (i * frames), audioIn[i], static_cast<int>(frames));

        // --------------------------------------------------------------------------------------------------------
        // TimeInfo

        const EngineTimeInfo& timeInfo(pData->engine->getTimeInfo());
        BridgeTimeInfo& bridgeTimeInfo(fShmRtClientControl.data->timeInfo);

        bridgeTimeInfo.playing = timeInfo.playing;
        bridgeTimeInfo.frame   = timeInfo.frame;
        bridgeTimeInfo.usecs   = timeInfo.usecs;
        bridgeTimeInfo.valid   = timeInfo.valid;

        if (timeInfo.valid & EngineTimeInfo::kValidBBT)
        {
            bridgeTimeInfo.bar  = timeInfo.bbt.bar;
            bridgeTimeInfo.beat = timeInfo.bbt.beat;
            bridgeTimeInfo.tick = timeInfo.bbt.tick;

            bridgeTimeInfo.beatsPerBar = timeInfo.bbt.beatsPerBar;
            bridgeTimeInfo.beatType    = timeInfo.bbt.beatType;

            bridgeTimeInfo.ticksPerBeat   = timeInfo.bbt.ticksPerBeat;
            bridgeTimeInfo.beatsPerMinute = timeInfo.bbt.beatsPerMinute;
            bridgeTimeInfo.barStartTick   = timeInfo.bbt.barStartTick;
        }

        // --------------------------------------------------------------------------------------------------------
        // Run plugin

        {
            fShmRtClientControl.writeOpcode(kPluginBridgeRtClientProcess);
            fShmRtClientControl.commitWrite();
        }

        if (! waitForClient(2))
        {
            pData->singleMutex.unlock();
            return true;
        }

        for (uint32_t i=0; i < fInfo.aOuts; ++i)
            FloatVectorOperations::copy(audioOut[i], fShmAudioPool.data + ((i + fInfo.aIns) * frames), static_cast<int>(frames));

        // --------------------------------------------------------------------------------------------------------
        // Post-processing (dry/wet, volume and balance)

        {
            const bool doVolume  = (pData->hints & PLUGIN_CAN_VOLUME) != 0 && ! carla_compareFloats(pData->postProc.volume, 1.0f);
            const bool doDryWet  = (pData->hints & PLUGIN_CAN_DRYWET) != 0 && ! carla_compareFloats(pData->postProc.dryWet, 1.0f);
            const bool doBalance = (pData->hints & PLUGIN_CAN_BALANCE) != 0 && ! (carla_compareFloats(pData->postProc.balanceLeft, -1.0f) && carla_compareFloats(pData->postProc.balanceRight, 1.0f));

            bool isPair;
            float bufValue, oldBufLeft[doBalance ? frames : 1];

            for (uint32_t i=0; i < pData->audioOut.count; ++i)
            {
                // Dry/Wet
                if (doDryWet)
                {
                    for (uint32_t k=0; k < frames; ++k)
                    {
                        bufValue        = audioIn[(pData->audioIn.count == 1) ? 0 : i][k];
                        audioOut[i][k] = (audioOut[i][k] * pData->postProc.dryWet) + (bufValue * (1.0f - pData->postProc.dryWet));
                    }
                }

                // Balance
                if (doBalance)
                {
                    isPair = (i % 2 == 0);

                    if (isPair)
                    {
                        CARLA_ASSERT(i+1 < pData->audioOut.count);
                        FloatVectorOperations::copy(oldBufLeft, audioOut[i], static_cast<int>(frames));
                    }

                    float balRangeL = (pData->postProc.balanceLeft  + 1.0f)/2.0f;
                    float balRangeR = (pData->postProc.balanceRight + 1.0f)/2.0f;

                    for (uint32_t k=0; k < frames; ++k)
                    {
                        if (isPair)
                        {
                            // left
                            audioOut[i][k]  = oldBufLeft[k]     * (1.0f - balRangeL);
                            audioOut[i][k] += audioOut[i+1][k] * (1.0f - balRangeR);
                        }
                        else
                        {
                            // right
                            audioOut[i][k]  = audioOut[i][k] * balRangeR;
                            audioOut[i][k] += oldBufLeft[k]   * balRangeL;
                        }
                    }
                }

                // Volume (and buffer copy)
                if (doVolume)
                {
                    for (uint32_t k=0; k < frames; ++k)
                        audioOut[i][k] *= pData->postProc.volume;
                }
            }

        } // End of Post-processing

        // --------------------------------------------------------------------------------------------------------

        pData->singleMutex.unlock();
        return true;
    }

    void bufferSizeChanged(const uint32_t newBufferSize) override
    {
        resizeAudioPool(newBufferSize);

        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);
            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetBufferSize);
            fShmNonRtClientControl.writeUInt(newBufferSize);
            fShmNonRtClientControl.commitWrite();
        }

        fShmRtClientControl.waitForClient(1);
    }

    void sampleRateChanged(const double newSampleRate) override
    {
        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);
            fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetSampleRate);
            fShmNonRtClientControl.writeDouble(newSampleRate);
            fShmNonRtClientControl.commitWrite();
        }

        fShmRtClientControl.waitForClient(1);
    }

    void offlineModeChanged(const bool isOffline) override
    {
        {
            const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);
            fShmNonRtClientControl.writeOpcode(isOffline ? kPluginBridgeNonRtClientSetOffline : kPluginBridgeNonRtClientSetOnline);
            fShmNonRtClientControl.commitWrite();
        }

        fShmRtClientControl.waitForClient(1);
    }

    // -------------------------------------------------------------------
    // Plugin buffers

    void clearBuffers() noexcept override
    {
        if (fParams != nullptr)
        {
            delete[] fParams;
            fParams = nullptr;
        }

        CarlaPlugin::clearBuffers();
    }

    // -------------------------------------------------------------------
    // Post-poned UI Stuff

    void uiParameterChange(const uint32_t index, const float value) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(index < pData->param.count,);

        const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

        fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientUiParameterChange);
        fShmNonRtClientControl.writeUInt(index);
        fShmNonRtClientControl.writeFloat(value);
        fShmNonRtClientControl.commitWrite();
    }

    void uiProgramChange(const uint32_t index) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(index < pData->midiprog.count,);

        const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

        fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientUiProgramChange);
        fShmNonRtClientControl.writeUInt(index);
        fShmNonRtClientControl.commitWrite();
    }

    void uiMidiProgramChange(const uint32_t index) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(index < pData->midiprog.count,);

        const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

        fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientUiMidiProgramChange);
        fShmNonRtClientControl.writeUInt(index);
        fShmNonRtClientControl.commitWrite();
    }

    void uiNoteOn(const uint8_t channel, const uint8_t note, const uint8_t velo) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(channel < MAX_MIDI_CHANNELS,);
        CARLA_SAFE_ASSERT_RETURN(note < MAX_MIDI_NOTE,);
        CARLA_SAFE_ASSERT_RETURN(velo > 0 && velo < MAX_MIDI_VALUE,);

        const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

        fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientUiNoteOn);
        fShmNonRtClientControl.writeByte(channel);
        fShmNonRtClientControl.writeByte(note);
        fShmNonRtClientControl.writeByte(velo);
        fShmNonRtClientControl.commitWrite();
    }

    void uiNoteOff(const uint8_t channel, const uint8_t note) noexcept override
    {
        CARLA_SAFE_ASSERT_RETURN(channel < MAX_MIDI_CHANNELS,);
        CARLA_SAFE_ASSERT_RETURN(note < MAX_MIDI_NOTE,);

        const CarlaMutexLocker _cml(fShmNonRtClientControl.mutex);

        fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientUiNoteOff);
        fShmNonRtClientControl.writeByte(channel);
        fShmNonRtClientControl.writeByte(note);
        fShmNonRtClientControl.commitWrite();
    }

    // -------------------------------------------------------------------

    void handleNonRtData()
    {
        for (; fShmNonRtServerControl.isDataAvailableForReading();)
        {
            const PluginBridgeNonRtServerOpcode opcode(fShmNonRtServerControl.readOpcode());
#if 1//def DEBUG
            if (opcode != kPluginBridgeNonRtServerPong) {
                carla_stdout("CarlaPluginBridge::handleNonRtData() - got opcode: %s", PluginBridgeNonRtServerOpcode2str(opcode));
            }
#endif
            switch (opcode)
            {
            case kPluginBridgeNonRtServerNull:
                break;

            case kPluginBridgeNonRtServerPong:
                if (fLastPongCounter > 0)
                    fLastPongCounter = 0;
                break;

            case kPluginBridgeNonRtServerPluginInfo1: {
                // uint/category, uint/hints, uint/optionsAvailable, uint/optionsEnabled, long/uniqueId
                const uint32_t category = fShmNonRtServerControl.readUInt();
                const uint32_t hints    = fShmNonRtServerControl.readUInt();
                const uint32_t optionAv = fShmNonRtServerControl.readUInt();
                const uint32_t optionEn = fShmNonRtServerControl.readUInt();
                const  int64_t uniqueId = fShmNonRtServerControl.readLong();

                pData->hints   = hints | PLUGIN_IS_BRIDGE;
                pData->options = optionEn;

                fInfo.category = static_cast<PluginCategory>(category);
                fInfo.uniqueId = uniqueId;
                fInfo.optionsAvailable = optionAv;
            }   break;

            case kPluginBridgeNonRtServerPluginInfo2: {
                // uint/size, str[] (realName), uint/size, str[] (label), uint/size, str[] (maker), uint/size, str[] (copyright)

                // realName
                const uint32_t realNameSize(fShmNonRtServerControl.readUInt());
                char realName[realNameSize+1];
                carla_zeroChar(realName, realNameSize+1);
                fShmNonRtServerControl.readCustomData(realName, realNameSize);

                // label
                const uint32_t labelSize(fShmNonRtServerControl.readUInt());
                char label[labelSize+1];
                carla_zeroChar(label, labelSize+1);
                fShmNonRtServerControl.readCustomData(label, labelSize);

                // maker
                const uint32_t makerSize(fShmNonRtServerControl.readUInt());
                char maker[makerSize+1];
                carla_zeroChar(maker, makerSize+1);
                fShmNonRtServerControl.readCustomData(maker, makerSize);

                // copyright
                const uint32_t copyrightSize(fShmNonRtServerControl.readUInt());
                char copyright[copyrightSize+1];
                carla_zeroChar(copyright, copyrightSize+1);
                fShmNonRtServerControl.readCustomData(copyright, copyrightSize);

                fInfo.name  = realName;
                fInfo.label = label;
                fInfo.maker = maker;
                fInfo.copyright = copyright;

                if (pData->name == nullptr)
                    pData->name = pData->engine->getUniquePluginName(realName);
            }   break;

            case kPluginBridgeNonRtServerAudioCount: {
                // uint/ins, uint/outs
                fInfo.aIns  = fShmNonRtServerControl.readUInt();
                fInfo.aOuts = fShmNonRtServerControl.readUInt();
            }   break;

            case kPluginBridgeNonRtServerMidiCount: {
                // uint/ins, uint/outs
                fInfo.mIns  = fShmNonRtServerControl.readUInt();
                fInfo.mOuts = fShmNonRtServerControl.readUInt();
            }   break;

            case kPluginBridgeNonRtServerParameterCount: {
                // uint/ins, uint/outs
                const uint32_t ins  = fShmNonRtServerControl.readUInt();
                const uint32_t outs = fShmNonRtServerControl.readUInt();

                // delete old data
                pData->param.clear();

                if (fParams != nullptr)
                {
                    delete[] fParams;
                    fParams = nullptr;
                }

                if (uint32_t count = ins+outs)
                {
                    const uint32_t maxParams(pData->engine->getOptions().maxParameters);

                    if (count > maxParams)
                    {
                        // this is expected right now, to be handled better later
                        //carla_safe_assert_int2("count <= pData->engine->getOptions().maxParameters", __FILE__, __LINE__, count, maxParams);
                        count = maxParams;
                    }

                    pData->param.createNew(count, false);
                    fParams = new BridgeParamInfo[count];
                }
            }   break;

            case kPluginBridgeNonRtServerProgramCount: {
                // uint/count
                pData->prog.clear();

                if (const uint32_t count = fShmNonRtServerControl.readUInt())
                    pData->prog.createNew(static_cast<uint32_t>(count));

            }   break;

            case kPluginBridgeNonRtServerMidiProgramCount: {
                // uint/count
                pData->midiprog.clear();

                if (const uint32_t count = fShmNonRtServerControl.readUInt())
                    pData->midiprog.createNew(static_cast<uint32_t>(count));

            }   break;

            case kPluginBridgeNonRtServerParameterData1: {
                // uint/index, int/rindex, uint/type, uint/hints, int/cc
                const uint32_t index  = fShmNonRtServerControl.readUInt();
                const  int32_t rindex = fShmNonRtServerControl.readInt();
                const uint32_t type   = fShmNonRtServerControl.readUInt();
                const uint32_t hints  = fShmNonRtServerControl.readUInt();
                const  int16_t midiCC = fShmNonRtServerControl.readShort();

                CARLA_SAFE_ASSERT_BREAK(midiCC >= -1 && midiCC < MAX_MIDI_CONTROL);
                CARLA_SAFE_ASSERT_INT2(index < pData->param.count, index, pData->param.count);

                if (index < pData->param.count)
                {
                    pData->param.data[index].type   = static_cast<ParameterType>(type);
                    pData->param.data[index].index  = static_cast<int32_t>(index);
                    pData->param.data[index].rindex = rindex;
                    pData->param.data[index].hints  = hints;
                    pData->param.data[index].midiCC = midiCC;
                }
            }   break;

            case kPluginBridgeNonRtServerParameterData2: {
                // uint/index, uint/size, str[] (name), uint/size, str[] (unit)
                const uint32_t index = fShmNonRtServerControl.readUInt();

                // name
                const uint32_t nameSize(fShmNonRtServerControl.readUInt());
                char name[nameSize+1];
                carla_zeroChar(name, nameSize+1);
                fShmNonRtServerControl.readCustomData(name, nameSize);

                // unit
                const uint32_t unitSize(fShmNonRtServerControl.readUInt());
                char unit[unitSize+1];
                carla_zeroChar(unit, unitSize+1);
                fShmNonRtServerControl.readCustomData(unit, unitSize);

                CARLA_SAFE_ASSERT_INT2(index < pData->param.count, index, pData->param.count);

                if (index < pData->param.count)
                {
                    fParams[index].name = name;
                    fParams[index].unit = unit;
                }
            }   break;

            case kPluginBridgeNonRtServerParameterRanges1: {
                // uint/index, float/def, float/min, float/max
                const uint32_t index = fShmNonRtServerControl.readUInt();
                const float def      = fShmNonRtServerControl.readFloat();
                const float min      = fShmNonRtServerControl.readFloat();
                const float max      = fShmNonRtServerControl.readFloat();

                CARLA_SAFE_ASSERT_BREAK(min < max);
                CARLA_SAFE_ASSERT_BREAK(def >= min);
                CARLA_SAFE_ASSERT_BREAK(def <= max);
                CARLA_SAFE_ASSERT_INT2(index < pData->param.count, index, pData->param.count);

                if (index < pData->param.count)
                {
                    pData->param.ranges[index].def = def;
                    pData->param.ranges[index].min = min;
                    pData->param.ranges[index].max = max;
                }
            }   break;

            case kPluginBridgeNonRtServerParameterRanges2: {
                // uint/index float/step, float/stepSmall, float/stepLarge
                const uint32_t index  = fShmNonRtServerControl.readUInt();
                const float step      = fShmNonRtServerControl.readFloat();
                const float stepSmall = fShmNonRtServerControl.readFloat();
                const float stepLarge = fShmNonRtServerControl.readFloat();

                CARLA_SAFE_ASSERT_INT2(index < pData->param.count, index, pData->param.count);

                if (index < pData->param.count)
                {
                    pData->param.ranges[index].step      = step;
                    pData->param.ranges[index].stepSmall = stepSmall;
                    pData->param.ranges[index].stepLarge = stepLarge;
                }
            }   break;

            case kPluginBridgeNonRtServerParameterValue: {
                // uint/index float/value
                const uint32_t index = fShmNonRtServerControl.readUInt();
                const float    value = fShmNonRtServerControl.readFloat();

                CARLA_SAFE_ASSERT_INT2(index < pData->param.count, index, pData->param.count);

                if (index < pData->param.count)
                {
                    const float fixedValue(pData->param.getFixedValue(index, value));
                    fParams[index].value = fixedValue;

                    CarlaPlugin::setParameterValue(index, fixedValue, false, true, true);
                }
            }   break;

            case kPluginBridgeNonRtServerDefaultValue: {
                // uint/index float/value
                const uint32_t index = fShmNonRtServerControl.readUInt();
                const float    value = fShmNonRtServerControl.readFloat();

                CARLA_SAFE_ASSERT_INT2(index < pData->param.count, index, pData->param.count);

                if (index < pData->param.count)
                    pData->param.ranges[index].def = value;
            }   break;

            case kPluginBridgeNonRtServerCurrentProgram: {
                // int/index
                const int32_t index = fShmNonRtServerControl.readInt();

                CARLA_SAFE_ASSERT_BREAK(index >= -1);
                CARLA_SAFE_ASSERT_INT2(index < static_cast<int32_t>(pData->prog.count), index, pData->prog.count);

                CarlaPlugin::setProgram(index, false, true, true);
            }   break;

            case kPluginBridgeNonRtServerCurrentMidiProgram: {
                // int/index
                const int32_t index = fShmNonRtServerControl.readInt();

                CARLA_SAFE_ASSERT_BREAK(index >= -1);
                CARLA_SAFE_ASSERT_INT2(index < static_cast<int32_t>(pData->midiprog.count), index, pData->midiprog.count);

                CarlaPlugin::setMidiProgram(index, false, true, true);
            }   break;

            case kPluginBridgeNonRtServerProgramName: {
                // uint/index, uint/size, str[] (name)
                const uint32_t index = fShmNonRtServerControl.readUInt();

                // name
                const uint32_t nameSize(fShmNonRtServerControl.readUInt());
                char name[nameSize+1];
                carla_zeroChar(name, nameSize+1);
                fShmNonRtServerControl.readCustomData(name, nameSize);

                CARLA_SAFE_ASSERT_INT2(index < pData->prog.count, index, pData->prog.count);

                if (index < pData->prog.count)
                {
                    if (pData->prog.names[index] != nullptr)
                        delete[] pData->prog.names[index];
                    pData->prog.names[index] = carla_strdup(name);
                }
            }   break;

            case kPluginBridgeNonRtServerMidiProgramData: {
                // uint/index, uint/bank, uint/program, uint/size, str[] (name)
                const uint32_t index   = fShmNonRtServerControl.readUInt();
                const uint32_t bank    = fShmNonRtServerControl.readUInt();
                const uint32_t program = fShmNonRtServerControl.readUInt();

                // name
                const uint32_t nameSize(fShmNonRtServerControl.readUInt());
                char name[nameSize+1];
                carla_zeroChar(name, nameSize+1);
                fShmNonRtServerControl.readCustomData(name, nameSize);

                CARLA_SAFE_ASSERT_INT2(index < pData->midiprog.count, index, pData->midiprog.count);

                if (index < pData->midiprog.count)
                {
                    if (pData->midiprog.data[index].name != nullptr)
                        delete[] pData->midiprog.data[index].name;
                    pData->midiprog.data[index].bank    = bank;
                    pData->midiprog.data[index].program = program;
                    pData->midiprog.data[index].name    = carla_strdup(name);
                }
            }   break;

            case kPluginBridgeNonRtServerSetCustomData: {
                // uint/size, str[], uint/size, str[], uint/size, str[] (compressed)

                using namespace juce;

                // type
                const uint32_t typeSize(fShmNonRtServerControl.readUInt());
                char type[typeSize+1];
                carla_zeroChar(type, typeSize+1);
                fShmNonRtServerControl.readCustomData(type, typeSize);

                // key
                const uint32_t keySize(fShmNonRtServerControl.readUInt());
                char key[keySize+1];
                carla_zeroChar(key, keySize+1);
                fShmNonRtServerControl.readCustomData(key, keySize);

                // value
                const uint32_t valueBase64Size(fShmNonRtServerControl.readUInt());
                char valueBase64[valueBase64Size+1];
                carla_zeroChar(valueBase64, valueBase64Size+1);
                fShmNonRtServerControl.readCustomData(valueBase64, valueBase64Size);

                const std::vector<uint8_t> valueChunk(carla_getChunkFromBase64String(valueBase64));

                MemoryInputStream valueMemStream(valueChunk.data(), valueChunk.size(), false);
                GZIPDecompressorInputStream decompressedValueStream(valueMemStream);

                CarlaPlugin::setCustomData(type, key, decompressedValueStream.readEntireStreamAsString().toRawUTF8(), false);
            }   break;

            case kPluginBridgeNonRtServerSetChunkDataFile: {
                // uint/size, str[] (filename)

                // chunkFilePath
                const uint32_t chunkFilePathSize(fShmNonRtServerControl.readUInt());
                char chunkFilePath[chunkFilePathSize+1];
                carla_zeroChar(chunkFilePath, chunkFilePathSize+1);
                fShmNonRtServerControl.readCustomData(chunkFilePath, chunkFilePathSize);

                String realChunkFilePath(chunkFilePath);
                carla_stdout("chunk save path BEFORE => %s", realChunkFilePath.toRawUTF8());

#ifndef CARLA_OS_WIN
                // Using Wine, fix temp dir
                if (fBinaryType == BINARY_WIN32 || fBinaryType == BINARY_WIN64)
                {
                    // Get WINEPREFIX
                    String wineDir;
                    if (const char* const WINEPREFIX = getenv("WINEPREFIX"))
                        wineDir = String(WINEPREFIX);
                    else
                        wineDir = File::getSpecialLocation(File::userHomeDirectory).getFullPathName() + "/.wine";

                    const StringArray driveLetterSplit(StringArray::fromTokens(realChunkFilePath, ":/", ""));

                    realChunkFilePath  = wineDir;
                    realChunkFilePath += "/drive_";
                    realChunkFilePath += driveLetterSplit[0].toLowerCase();
                    realChunkFilePath += "/";
                    realChunkFilePath += driveLetterSplit[1];

                    realChunkFilePath  = realChunkFilePath.replace("\\", "/");
                    carla_stdout("chunk save path AFTER => %s", realChunkFilePath.toRawUTF8());
                }
#endif

                File chunkFile(realChunkFilePath);

                if (chunkFile.existsAsFile())
                {
                    fInfo.chunk = carla_getChunkFromBase64String(chunkFile.loadFileAsString().toRawUTF8());
                    chunkFile.deleteFile();
                    carla_stderr("chunk data final");
                }
            }   break;

            case kPluginBridgeNonRtServerSetLatency: {
                // uint
            }   break;

            case kPluginBridgeNonRtServerReady:
                fInitiated = true;
                break;

            case kPluginBridgeNonRtServerSaved:
                fSaved = true;
                break;

            case kPluginBridgeNonRtServerUiClosed:
                pData->engine->callback(ENGINE_CALLBACK_UI_STATE_CHANGED, pData->id, 0, 0, 0.0f, nullptr);
                break;

            case kPluginBridgeNonRtServerError: {
                // error
                const uint32_t errorSize(fShmNonRtServerControl.readUInt());
                char error[errorSize+1];
                carla_zeroChar(error, errorSize+1);
                fShmNonRtServerControl.readCustomData(error, errorSize);

                pData->engine->setLastError(error);

                fInitError = true;
                fInitiated = true;
            }   break;
            }
        }
    }

    // -------------------------------------------------------------------

    const void* getExtraStuff() const noexcept override
    {
        return fBridgeBinary.isNotEmpty() ? fBridgeBinary.buffer() : nullptr;
    }

    bool init(const char* const filename, const char* const name, const char* const label, const char* const bridgeBinary)
    {
        CARLA_SAFE_ASSERT_RETURN(pData->engine != nullptr, false);

        // ---------------------------------------------------------------
        // first checks

        if (pData->client != nullptr)
        {
            pData->engine->setLastError("Plugin client is already registered");
            return false;
        }

        // ---------------------------------------------------------------
        // set info

        if (name != nullptr && name[0] != '\0')
            pData->name = pData->engine->getUniquePluginName(name);

        pData->filename = carla_strdup(filename);

        if (bridgeBinary != nullptr)
            fBridgeBinary = bridgeBinary;

        std::srand(static_cast<uint>(std::time(nullptr)));

        // ---------------------------------------------------------------
        // init sem/shm

        if (! fShmAudioPool.initialize())
        {
            carla_stdout("Failed to initialize shared memory audio pool");
            return false;
        }

        if (! fShmRtClientControl.initialize())
        {
            carla_stdout("Failed to initialize RT client control");
            fShmAudioPool.clear();
            return false;
        }

        if (! fShmNonRtClientControl.initialize())
        {
            carla_stdout("Failed to initialize Non-RT client control");
            fShmRtClientControl.clear();
            fShmAudioPool.clear();
            return false;
        }

        if (! fShmNonRtServerControl.initialize())
        {
            carla_stdout("Failed to initialize Non-RT server control");
            fShmNonRtClientControl.clear();
            fShmRtClientControl.clear();
            fShmAudioPool.clear();
            return false;
        }

        // ---------------------------------------------------------------

        carla_stdout("Carla Server Info:");
        carla_stdout("  sizeof(BridgeRtClientData):    " P_SIZE, sizeof(BridgeRtClientData));
        carla_stdout("  sizeof(BridgeNonRtClientData): " P_SIZE, sizeof(BridgeNonRtClientData));
        carla_stdout("  sizeof(BridgeNonRtServerData): " P_SIZE, sizeof(BridgeNonRtServerData));

        // initial values
        fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientNull);
        fShmNonRtClientControl.writeUInt(static_cast<uint32_t>(sizeof(BridgeRtClientData)));
        fShmNonRtClientControl.writeUInt(static_cast<uint32_t>(sizeof(BridgeNonRtClientData)));
        fShmNonRtClientControl.writeUInt(static_cast<uint32_t>(sizeof(BridgeNonRtServerData)));

        fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetBufferSize);
        fShmNonRtClientControl.writeUInt(pData->engine->getBufferSize());

        fShmNonRtClientControl.writeOpcode(kPluginBridgeNonRtClientSetSampleRate);
        fShmNonRtClientControl.writeDouble(pData->engine->getSampleRate());

        fShmNonRtClientControl.commitWrite();

        // init bridge thread
        {
            char shmIdsStr[6*4+1];
            carla_zeroChar(shmIdsStr, 6*4+1);

            std::strncpy(shmIdsStr+6*0, &fShmAudioPool.filename[fShmAudioPool.filename.length()-6], 6);
            std::strncpy(shmIdsStr+6*1, &fShmRtClientControl.filename[fShmRtClientControl.filename.length()-6], 6);
            std::strncpy(shmIdsStr+6*2, &fShmNonRtClientControl.filename[fShmNonRtClientControl.filename.length()-6], 6);
            std::strncpy(shmIdsStr+6*3, &fShmNonRtServerControl.filename[fShmNonRtServerControl.filename.length()-6], 6);

            fBridgeThread.setData(bridgeBinary, label, shmIdsStr);
            fBridgeThread.startThread();
        }

        fInitiated = false;
        fLastPongCounter = 0;

        for (; fLastPongCounter++ < 500;)
        {
            if (fInitiated || ! fBridgeThread.isThreadRunning())
                break;
            carla_msleep(20);
            pData->engine->callback(ENGINE_CALLBACK_IDLE, 0, 0, 0, 0.0f, nullptr);
            pData->engine->idle();
            idle();
        }

        fLastPongCounter = -1;

        if (fInitError || ! fInitiated)
        {
            fBridgeThread.stopThread(6000);

            if (! fInitError)
                pData->engine->setLastError("Timeout while waiting for a response from plugin-bridge\n(or the plugin crashed on initialization?)");

            return false;
        }

        // ---------------------------------------------------------------
        // register client

        if (pData->name == nullptr)
        {
            if (name != nullptr && name[0] != '\0')
                pData->name = pData->engine->getUniquePluginName(name);
            else if (label != nullptr && label[0] != '\0')
                pData->name = pData->engine->getUniquePluginName(label);
            else
                pData->name = pData->engine->getUniquePluginName("unknown");
        }

        pData->client = pData->engine->addClient(this);

        if (pData->client == nullptr || ! pData->client->isOk())
        {
            pData->engine->setLastError("Failed to register plugin client");
            return false;
        }

        return true;
    }

private:
    const BinaryType fBinaryType;
    const PluginType fPluginType;

    bool fInitiated;
    bool fInitError;
    bool fSaved;
    bool fTimedOut;

    int32_t fLastPongCounter;

    CarlaString             fBridgeBinary;
    CarlaPluginBridgeThread fBridgeThread;

    BridgeAudioPool          fShmAudioPool;
    BridgeRtClientControl    fShmRtClientControl;
    BridgeNonRtClientControl fShmNonRtClientControl;
    BridgeNonRtServerControl fShmNonRtServerControl;

    struct Info {
        uint32_t aIns, aOuts;
        uint32_t cvIns, cvOuts;
        uint32_t mIns, mOuts;
        PluginCategory category;
        uint optionsAvailable;
        int64_t uniqueId;
        CarlaString name;
        CarlaString label;
        CarlaString maker;
        CarlaString copyright;
        std::vector<uint8_t> chunk;

        Info()
            : aIns(0),
              aOuts(0),
              cvIns(0),
              cvOuts(0),
              mIns(0),
              mOuts(0),
              category(PLUGIN_CATEGORY_NONE),
              optionsAvailable(0),
              uniqueId(0),
              name(),
              label(),
              maker(),
              copyright(),
              chunk() {}
    } fInfo;

    BridgeParamInfo* fParams;

    void resizeAudioPool(const uint32_t bufferSize)
    {
        fShmAudioPool.resize(bufferSize, fInfo.aIns+fInfo.aOuts, fInfo.cvIns+fInfo.cvOuts);

        fShmRtClientControl.writeOpcode(kPluginBridgeRtClientSetAudioPool);
        fShmRtClientControl.writeULong(static_cast<uint64_t>(fShmAudioPool.size));

        fShmRtClientControl.commitWrite();

        waitForClient();
    }

    bool waitForClient(const uint secs = 5)
    {
        CARLA_SAFE_ASSERT_RETURN(! fTimedOut, false);

        if (! fShmRtClientControl.waitForClient(secs))
        {
            carla_stderr("waitForClient() timeout here");
            fTimedOut = true;
            return false;
        }

        return true;
    }

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CarlaPluginBridge)
};

CARLA_BACKEND_END_NAMESPACE

// -------------------------------------------------------------------------------------------------------------------

CARLA_BACKEND_START_NAMESPACE

CarlaPlugin* CarlaPlugin::newBridge(const Initializer& init, BinaryType btype, PluginType ptype, const char* const bridgeBinary)
{
    carla_debug("CarlaPlugin::newBridge({%p, \"%s\", \"%s\", \"%s\"}, %s, %s, \"%s\")", init.engine, init.filename, init.name, init.label, BinaryType2Str(btype), PluginType2Str(ptype), bridgeBinary);

    if (bridgeBinary == nullptr || bridgeBinary[0] == '\0')
    {
        init.engine->setLastError("Bridge not possible, bridge-binary not found");
        return nullptr;
    }

    CarlaPluginBridge* const plugin(new CarlaPluginBridge(init.engine, init.id, btype, ptype));

    if (! plugin->init(init.filename, init.name, init.label, bridgeBinary))
    {
        delete plugin;
        return nullptr;
    }

    plugin->reload();

    bool canRun = true;

    if (init.engine->getProccessMode() == ENGINE_PROCESS_MODE_CONTINUOUS_RACK)
    {
        if (! plugin->canRunInRack())
        {
            init.engine->setLastError("Carla's rack mode can only work with Stereo Bridged plugins, sorry!");
            canRun = false;
        }
        else if (plugin->getCVInCount() > 0 || plugin->getCVInCount() > 0)
        {
            init.engine->setLastError("Carla's rack mode cannot work with plugins that have CV ports, sorry!");
            canRun = false;
        }
    }
    else if (init.engine->getProccessMode() == ENGINE_PROCESS_MODE_PATCHBAY && (plugin->getCVInCount() > 0 || plugin->getCVInCount() > 0))
    {
        init.engine->setLastError("CV ports in patchbay mode is still TODO");
        canRun = false;
    }

    if (! canRun)
    {
        delete plugin;
        return nullptr;
    }

    return plugin;
}

CARLA_BACKEND_END_NAMESPACE

// -------------------------------------------------------------------------------------------------------------------
