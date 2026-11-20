#include "mpc_vst2_abi.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace
{
VstIntPtr host(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void*, float)
{
    return opcode == audioMasterVersion ? 2400 : 0;
}

void prepare(AEffect* plugin)
{
    auto dispatch = plugin->dispatcher;
    dispatch(plugin, effOpen, 0, 0, nullptr, 0);
    dispatch(plugin, effSetSampleRate, 0, 0, nullptr, 44100);
    dispatch(plugin, effSetBlockSize, 0, 128, nullptr, 0);
    dispatch(plugin, effMainsChanged, 0, 1, nullptr, 0);
}

void close(AEffect* plugin)
{
    plugin->dispatcher(plugin, effMainsChanged, 0, 0, nullptr, 0);
    plugin->dispatcher(plugin, effClose, 0, 0, nullptr, 0);
}

int parameter(AEffect* plugin, const char* name)
{
    for (int index = 0; index < plugin->numParams; ++index)
    {
        char buffer[64]{};
        plugin->dispatcher(plugin, effGetParamName, index, 0, buffer, 0);
        if (std::strcmp(name, buffer) == 0)
            return index;
    }
    std::fprintf(stderr, "Missing VST parameter: %s\n", name);
    std::abort();
}

void set(AEffect* plugin, const char* name, float normalised)
{
    plugin->setParameter(plugin, parameter(plugin, name), normalised);
}

std::string value(AEffect* plugin, const char* name)
{
    char buffer[64]{};
    plugin->dispatcher(plugin, effGetParamDisplay, parameter(plugin, name), 0, buffer, 0);
    return buffer;
}

void deliver(AEffect* plugin, int status = 0, int number = 0, int value = 0)
{
    if (status)
    {
        VstMidiEvent midi{};
        midi.type = kVstMidiType;
        midi.byteSize = sizeof(midi);
        midi.midiData[0] = static_cast<char>(status);
        midi.midiData[1] = static_cast<char>(number);
        midi.midiData[2] = static_cast<char>(value);
        VstEvents events{};
        events.numEvents = 1;
        events.events[0] = reinterpret_cast<VstEvent*>(&midi);
        plugin->dispatcher(plugin, effProcessEvents, 0, 0, &events, 0);
    }
    float left[128]{1}, right[128]{1};
    float* output[] = {left, right};
    plugin->processReplacing(plugin, nullptr, output, 128);
    for (const auto* channel : output)
        for (int frame = 0; frame < 128; ++frame)
            assert(channel[frame] == 0);
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library)
    {
        std::puts(dlerror());
        return 1;
    }
    using CreatePlugin = AEffect* (*)(audioMasterCallback);
    auto create = reinterpret_cast<CreatePlugin>(dlsym(library, "VSTPluginMain"));
    assert(create);

    auto* plugin = create(host);
    assert(plugin && plugin->uniqueID == 0x454c6d74 && plugin->version > 0);
    prepare(plugin);
    set(plugin, "Add Row", 1);
    set(plugin, "Add Row", 1);
    set(plugin, "Open Picker", 1.0f / 40);
    set(plugin, "Pick Button", 1.0f / 33);
    assert(value(plugin, "Target 1") == "STOP");
    set(plugin, "Learn 1", 1);
    deliver(plugin, 0xB2, 104, 0);
    assert(value(plugin, "Learn 1") == "Listening");
    deliver(plugin, 0xB2, 104, 127);
    assert(value(plugin, "Binding 1") == "Ch 3 / CC 104");

    set(plugin, "Open Picker", 2.0f / 40);
    set(plugin, "Pick Button", 9.0f / 33);
    assert(value(plugin, "Target 2") == "FULL-LEVEL");
    assert(value(plugin, "Toggle 2") == "On");
    set(plugin, "Learn 2", 1);
    deliver(plugin, 0x92, 60, 127);
    assert(value(plugin, "Binding 2") == "Ch 3 / Note 60");

    set(plugin, "Learn 1", 1);
    set(plugin, "Open Picker", 2.0f / 40);
    void* chunk = nullptr;
    const auto size = plugin->dispatcher(plugin, effGetChunk, 0, 0, &chunk, 0);
    assert(size > 0 && chunk);
    const auto* data = static_cast<const char*>(chunk);
    std::vector<char> saved(data, data + size);
    close(plugin);

    auto* restored = create(host);
    assert(restored);
    prepare(restored);
    assert(restored->dispatcher(restored, effSetChunk, 0, size, saved.data(), 0));
    deliver(restored);
    assert(value(restored, "Open Picker") == "0");
    assert(value(restored, "Learn 1") == "Learn");
    assert(value(restored, "Target 1") == "STOP");
    assert(value(restored, "Binding 1") == "Ch 3 / CC 104");
    assert(value(restored, "Target 2") == "FULL-LEVEL");
    assert(value(restored, "Toggle 2") == "On");
    assert(value(restored, "Binding 2") == "Ch 3 / Note 60");
    set(restored, "Remove Row", 1.0f / 40);
    assert(value(restored, "Target 1") == "FULL-LEVEL");
    assert(value(restored, "Toggle 1") == "On");
    assert(value(restored, "Binding 1") == "Ch 3 / Note 60");
    assert(value(restored, "Binding 2") == "Unassigned");
    close(restored);
    dlclose(library);
    std::puts("PASS VST load, silent output, MIDI Learn, mode defaults, saved mappings, transient reset, row removal");
}
