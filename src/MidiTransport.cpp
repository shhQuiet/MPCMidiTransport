#include "mpc_plugin.h"

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

#if defined(__linux__)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
constexpr int kBindingCount = 3;
constexpr int kChannels = 16;
constexpr int kControllers = 128;
constexpr int kMessageRange = kChannels * kControllers;
constexpr int kUnassigned = 0;
constexpr int kFirstLearn = 1;
constexpr int kFirstBinding = kFirstLearn + kBindingCount;
constexpr int kBindingEnd = kFirstBinding + kBindingCount;
constexpr unsigned kQueueSize = 16;

constexpr const char* kLearnIds[] = {"learn-play", "learn-stop", "learn-play-start"};
constexpr const char* kLearnNames[] = {"Learn Play", "Learn Stop", "Learn Play Start"};
constexpr const char* kBindingIds[] = {"play-cc", "stop-cc", "play-start-cc"};
constexpr const char* kBindingNames[] = {"Play CC", "Stop CC", "Play Start CC"};

#if defined(__linux__)
bool pressButton(int action)
{
    constexpr const char* names[] = {"PLAY", "STOP", "PLAY-START"};
    const int connection = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connection < 0)
        return false;

    timeval timeout{0, 250000};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(6720);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0
           && setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0
           && connect(connection, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    char request[128];
    const auto length = static_cast<size_t>(std::snprintf(
        request, sizeof(request), "GET /button?name=%s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n",
        names[action]));
    for (size_t sent = 0; ok && sent < length;)
    {
        const auto count = send(connection, request + sent, length - sent, MSG_NOSIGNAL);
        if (count <= 0)
            ok = false;
        else
            sent += static_cast<size_t>(count);
    }
    char status[12]{};
    size_t received = 0;
    while (ok && received < sizeof(status))
    {
        const auto count = recv(connection, status + received, sizeof(status) - received, 0);
        if (count <= 0)
            ok = false;
        else
            received += static_cast<size_t>(count);
    }
    close(connection);
    return ok && std::memcmp(status, "HTTP/1.", 7) == 0 && status[9] == '2';
}
#endif
}

class MidiTransport final : public mpc::Plugin
{
public:
    MidiTransport() : mpc::Plugin(makeInfo())
    {
        // MPC nudges parameter 0 when opening a plugin.
        auto reserved = mpc::boolParam("reserved", "Reserved", false);
        reserved.automatable = false;
        addParam(reserved);
        for (int action = 0; action < kBindingCount; ++action)
        {
            auto learn = mpc::boolParam(kLearnIds[action], kLearnNames[action], false);
            learn.automatable = false;
            addParam(learn);
        }
        for (int action = 0; action < kBindingCount; ++action)
        {
            auto binding = mpc::intParam(kBindingIds[action], kBindingNames[action], 0,
                                         2 * kMessageRange, kUnassigned);
            binding.automatable = false;
            addParam(binding);
        }
        worker_ = std::thread([this] { dispatchCommands(); });
    }

    ~MidiTransport() override
    {
        stopping_.store(true);
        worker_.join();
    }

    void reset() override
    {
        pressed_.fill(false);
        learnAction_.store(-1);
        for (int action = 0; action < kBindingCount; ++action)
            clearLearn(action);
    }

    void parameterChanged(int index, float value) override
    {
        if (index < kFirstLearn || index >= kFirstBinding)
            return;
        int action = index - kFirstLearn;
        if (value >= 0.5f)
        {
            learnAction_.store(action);
            for (int other = 0; other < kBindingCount; ++other)
                if (other != action)
                    clearLearn(other);
        }
        else
            learnAction_.compare_exchange_strong(action, -1);
    }

    bool formatValue(int index, float value, std::string& text) override
    {
        if (index >= kFirstLearn && index < kFirstBinding)
            text = value >= 0.5f ? "Listening" : "Learn";
        else if (index >= kFirstBinding && index < kBindingEnd)
        {
            const int binding = static_cast<int>(value);
            if (binding == kUnassigned)
                text = "Unassigned";
            else
            {
                const bool note = binding > kMessageRange;
                const int key = (binding - 1) % kMessageRange;
                text = "Ch " + std::to_string(key / kControllers + 1) +
                       (note ? " / Note " : " / CC ") + std::to_string(key % kControllers);
            }
        }
        else
            return false;
        return true;
    }

    void process(mpc::ProcessContext& context) override
    {
        for (int channel = 0; channel < context.numOutputs; ++channel)
            if (context.outputs[channel])
                std::fill_n(context.outputs[channel], context.numFrames, 0.0f);

        for (int index = 0; index < context.numMidiEvents; ++index)
        {
            const auto& midi = context.midi[index];
            const bool cc = midi.isController();
            if (midi.size != 3 || (!cc && !midi.isNoteOn()) || midi.data[1] >= kControllers)
                continue;

            const int key = midi.channel() * kControllers + midi.data[1];
            bool trigger = true;
            if (cc)
            {
                const bool down = midi.controllerValue() >= 64;
                trigger = down && !pressed_[key];
                pressed_[key] = down;
            }

            // The lower half preserves the CC values stored by 0.1.0.
            const int binding = 1 + key + (cc ? 0 : kMessageRange);
            const int learning = learnAction_.exchange(-1);
            if (learning >= 0)
            {
                for (int action = 0; action < kBindingCount; ++action)
                {
                    if (action != learning && getInt(kFirstBinding + action) == binding)
                        setDisplayed(kFirstBinding + action, kUnassigned);
                    clearLearn(action);
                }
                setDisplayed(kFirstBinding + learning, binding);
                continue;
            }

            if (!trigger)
                continue;
            for (int action = 0; action < kBindingCount; ++action)
                if (getInt(kFirstBinding + action) == binding)
                {
                    enqueue(action);
                    break;
                }
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "MIDI Transport";
        info.vendor = "Elsewave";
        info.uniqueId = MPC_FOURCC('E', 'L', 'm', 't');
        info.version = 101;
        info.isInstrument = true;
        info.numInputs = 0;
        info.numOutputs = 2;
        return info;
    }

    void clearLearn(int action)
    {
        const int index = kFirstLearn + action;
        if (getBool(index))
            setDisplayed(index, 0);
    }

    void setDisplayed(int index, int value)
    {
        set(index, static_cast<float>(value));
        changed_.fetch_or(1u << index, std::memory_order_release);
    }

    void enqueue(int action)
    {
        const unsigned write = write_.load(std::memory_order_relaxed);
        const unsigned next = (write + 1) % kQueueSize;
        if (next != read_.load(std::memory_order_acquire))
        {
            queue_[write] = action;
            write_.store(next, std::memory_order_release);
        }
    }

    void dispatchCommands()
    {
        while (!stopping_.load())
        {
            const auto changes = changed_.exchange(0, std::memory_order_acquire);
            for (int index = kFirstLearn; index < numParams(); ++index)
                if (changes & (1u << index))
                    notifyHostParameterChanged(index);

            const unsigned read = read_.load(std::memory_order_relaxed);
            if (read != write_.load(std::memory_order_acquire))
            {
#if defined(__linux__)
                if (!pressButton(queue_[read]))
                    mpc::log("MIDI Transport: Open MPC Remote command failed");
#endif
                read_.store((read + 1) % kQueueSize, std::memory_order_release);
            }
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::atomic<int> learnAction_{-1};
    std::atomic<unsigned> changed_{0};
    std::array<bool, kMessageRange> pressed_{};
    std::array<int, kQueueSize> queue_{};
    std::atomic<unsigned> read_{0};
    std::atomic<unsigned> write_{0};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};

MPC_PLUGIN_ENTRY(MidiTransport)
