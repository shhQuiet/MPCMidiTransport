#include "mpc_plugin.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#if defined(__linux__)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
struct Button
{
    const char* name;
    bool defaultToggle = false;
};

constexpr Button kButtons[] = {
    {""}, {"STOP"}, {"PLAY"}, {"PLAY-START"}, {"REC"}, {"OVERDUB"}, {"TAP-TEMPO"}, {"UNDO"},
    {"NOTE-REPEAT"}, {"FULL-LEVEL", true}, {"16-LEVEL", true},
    {"BANK-A"}, {"BANK-B"}, {"BANK-C"}, {"BANK-D"},
    {"MAIN"}, {"BROWSE"}, {"MENU"}, {"TRACK-MUTE"}, {"NEXT-SEQ"}, {"STEP-SEQ"}, {"SAMPLE-EDIT"},
    {"PROG-EDIT"}, {"SHIFT"}, {"COPY"}, {"ERASE"}, {"QLINK"}, {"ENTER"}, {"MINUS"}, {"PLUS"},
    {"UP"}, {"DOWN"}, {"LEFT"}, {"RIGHT"}
};
constexpr int kButtonSteps = std::size(kButtons);
constexpr int kRowsPerPage = 8;
constexpr int kPageCount = 5;
constexpr int kMaxRows = kRowsPerPage * kPageCount;
constexpr int kChannels = 16;
constexpr int kControllers = 128;
constexpr int kMessageRange = kChannels * kControllers;
constexpr unsigned kQueueSize = 16;

// The host nudges parameter zero when opening the plugin.
enum Parameter { Reserved, RowCount, Page, AddRow, RemoveRow, OpenPicker, PickButton, FirstRow };
enum RowField { Target, Learn, Binding, RowFields };
constexpr int rowParam(int row, RowField field) { return FirstRow + row * RowFields + field; }
constexpr int kPickerVisible = rowParam(kMaxRows, Target);
constexpr int modeParam(int row) { return kPickerVisible + 1 + row; }
constexpr int kParamCount = modeParam(kMaxRows);

enum class InputState : uint8_t { Unseen, Released, Pressed };

#if defined(__linux__)
bool pressButton(int action)
{
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
        kButtons[action].name));
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
        addSetting(mpc::boolParam("reserved", "Reserved", false));
        addSetting(mpc::intParam("row-count", "Row Count", 0, kMaxRows, 0));
        addSetting(mpc::intParam("page", "Page", 0, kPageCount - 1, 0));
        addSetting(mpc::boolParam("add-row", "Add Row", false));
        addSetting(mpc::intParam("remove-row", "Remove Row", 0, kMaxRows, 0));
        addSetting(mpc::intParam("open-picker", "Open Picker", 0, kMaxRows, 0));
        addSetting(mpc::intParam("pick-button", "Pick Button", 0, kButtonSteps - 1, 0));
        for (int row = 0; row < kMaxRows; ++row)
        {
            const auto number = std::to_string(row + 1);
            addSetting(mpc::intParam(("target-" + number).c_str(), ("Target " + number).c_str(),
                                     0, kButtonSteps - 1, 0));
            addSetting(mpc::boolParam(("learn-" + number).c_str(), ("Learn " + number).c_str(), false));
            addSetting(mpc::intParam(("binding-" + number).c_str(), ("Binding " + number).c_str(),
                                     0, 2 * kMessageRange, 0));
        }
        addSetting(mpc::boolParam("picker-visible", "Picker Visible", false));
        for (int row = 0; row < kMaxRows; ++row)
        {
            const auto number = std::to_string(row + 1);
            addSetting(mpc::boolParam(("toggle-" + number).c_str(), ("Toggle " + number).c_str(), false));
        }
        for (auto& dirty : changed_)
            dirty.store(false);
        worker_ = std::thread([this] { dispatchCommands(); });
    }

    ~MidiTransport() override
    {
        stopping_.store(true);
        worker_.join();
    }

    void reset() override
    {
        inputStates_.fill(InputState::Unseen);
        cancelLearn();
        for (int row = 0; row < kMaxRows; ++row)
            setDisplayed(rowParam(row, Learn), 0);
        setDisplayed(OpenPicker, 0);
        setDisplayed(kPickerVisible, 0);
        setDisplayed(PickButton, 0);
    }

    void parameterChanged(int index, float value) override
    {
        const int step = static_cast<int>(value);
        if (index == AddRow && step)
        {
            const std::lock_guard<std::mutex> lock(rowsMutex_);
            const int count = getInt(RowCount);
            if (count < kMaxRows)
            {
                setDisplayed(RowCount, count + 1);
                setDisplayed(Page, count / kRowsPerPage);
            }
            setDisplayed(AddRow, 0);
        }
        else if (index == RemoveRow && step)
        {
            removeRow(step - 1);
            setDisplayed(RemoveRow, 0);
        }
        else if (index == OpenPicker)
            setDisplayed(kPickerVisible, step != 0);
        else if (index == PickButton && step)
        {
            const std::lock_guard<std::mutex> lock(rowsMutex_);
            const int row = getInt(OpenPicker) - 1;
            if (row >= 0 && row < getInt(RowCount) && getInt(rowParam(row, Target)) != step)
            {
                setDisplayed(rowParam(row, Target), step);
                setDisplayed(modeParam(row), kButtons[step].defaultToggle);
            }
            setDisplayed(OpenPicker, 0);
            setDisplayed(PickButton, 0);
        }
        else if (index >= FirstRow && index < kPickerVisible && (index - FirstRow) % RowFields == Learn)
        {
            int row = (index - FirstRow) / RowFields;
            if (step && row < getInt(RowCount))
            {
                const int previous = learnRow_.exchange(row);
                if (previous >= 0 && previous != row)
                    setDisplayed(rowParam(previous, Learn), 0);
            }
            else
                learnRow_.compare_exchange_strong(row, -1);
        }
    }

    bool formatValue(int index, float value, std::string& text) override
    {
        if (index < FirstRow || index >= kPickerVisible)
            return false;
        const int step = static_cast<int>(value);
        switch ((index - FirstRow) % RowFields)
        {
            case Target:
                text = step ? kButtons[step].name : "Choose control";
                break;
            case Learn:
                text = step ? "Listening" : "Learn";
                break;
            case Binding:
                if (step == 0)
                    text = "Unassigned";
                else
                {
                    const bool note = step > kMessageRange;
                    const int key = (step - 1) % kMessageRange;
                    text = "Ch " + std::to_string(key / kControllers + 1) +
                           (note ? " / Note " : " / CC ") + std::to_string(key % kControllers);
                }
                break;
        }
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
            if (midi.size != 3 || (!cc && !midi.isNoteOn() && !midi.isNoteOff()) ||
                midi.data[1] >= kControllers)
                continue;

            const int key = midi.channel() * kControllers + midi.data[1];
            const int input = key + (cc ? 0 : kMessageRange);
            const bool down = cc ? midi.controllerValue() >= 64 : midi.isNoteOn();
            const auto state = down ? InputState::Pressed : InputState::Released;
            const bool changed = inputStates_[input] != state;
            inputStates_[input] = state;
            const bool momentaryTrigger = down && (!cc || changed);
            const int binding = input + 1;
            const std::unique_lock<std::mutex> lock(rowsMutex_, std::try_to_lock);
            if (!lock.owns_lock())
                continue;
            const int count = getInt(RowCount);
            const int learning = learnRow_.load();
            if (learning >= 0 && learning < count)
            {
                if (getBool(modeParam(learning)) ? changed : momentaryTrigger)
                {
                    for (int row = 0; row < count; ++row)
                        if (row != learning && getInt(rowParam(row, Binding)) == binding)
                            setDisplayed(rowParam(row, Binding), 0);
                    setDisplayed(rowParam(learning, Binding), binding);
                    setDisplayed(rowParam(learning, Learn), 0);
                }
                continue;
            }

            for (int row = 0; row < count; ++row)
            {
                const int target = getInt(rowParam(row, Target));
                if (target && getInt(rowParam(row, Binding)) == binding)
                {
                    if (getBool(modeParam(row)) ? changed : momentaryTrigger)
                        enqueue(target);
                    break;
                }
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
        info.version = 1001;
        info.isInstrument = true;
        info.numInputs = 0;
        info.numOutputs = 2;
        return info;
    }

    void addSetting(mpc::Param param)
    {
        param.automatable = false;
        addParam(param);
    }

    void cancelLearn()
    {
        const int row = learnRow_.exchange(-1);
        if (row >= 0)
            setDisplayed(rowParam(row, Learn), 0);
    }

    void removeRow(int row)
    {
        const std::lock_guard<std::mutex> lock(rowsMutex_);
        const int count = getInt(RowCount);
        if (row >= count)
            return;

        cancelLearn();
        for (int next = row + 1; next < count; ++next)
        {
            setDisplayed(rowParam(next - 1, Target), getInt(rowParam(next, Target)));
            setDisplayed(rowParam(next - 1, Binding), getInt(rowParam(next, Binding)));
            setDisplayed(modeParam(next - 1), getBool(modeParam(next)));
        }
        setDisplayed(rowParam(count - 1, Target), 0);
        setDisplayed(rowParam(count - 1, Binding), 0);
        setDisplayed(modeParam(count - 1), 0);
        setDisplayed(RowCount, count - 1);
        setDisplayed(Page, std::min(getInt(Page), std::max(0, count - 2) / kRowsPerPage));
    }

    void setDisplayed(int index, int value)
    {
        if (getInt(index) != value)
        {
            set(index, static_cast<float>(value));
            changed_[index].store(true, std::memory_order_release);
        }
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
            for (int index = RowCount; index < kParamCount; ++index)
                if (changed_[index].load(std::memory_order_relaxed) &&
                    changed_[index].exchange(false, std::memory_order_acquire))
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

    std::atomic<int> learnRow_{-1};
    std::mutex rowsMutex_;
    std::array<std::atomic<bool>, kParamCount> changed_;
    std::array<InputState, 2 * kMessageRange> inputStates_{};
    std::array<int, kQueueSize> queue_{};
    std::atomic<unsigned> read_{0};
    std::atomic<unsigned> write_{0};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};

MPC_PLUGIN_ENTRY(MidiTransport)
