#include "mpc_plugin.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>

#if defined(__linux__) || defined(__APPLE__)
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

// These indices are part of the saved project format through 1.0.2.
constexpr Button kLegacyButtons[] = {
    {""}, {"STOP"}, {"PLAY"}, {"PLAY-START"}, {"REC"}, {"OVERDUB"}, {"TAP-TEMPO"}, {"UNDO"},
    {"NOTE-REPEAT"}, {"FULL-LEVEL", true}, {"16-LEVEL", true},
    {"BANK-A"}, {"BANK-B"}, {"BANK-C"}, {"BANK-D"},
    {"MAIN"}, {"BROWSE"}, {"MENU"}, {"TRACK-MUTE"}, {"NEXT-SEQ"}, {"STEP-SEQ"}, {"SAMPLE-EDIT"},
    {"PROG-EDIT"}, {"SHIFT"}, {"COPY"}, {"ERASE"}, {"QLINK"}, {"ENTER"}, {"MINUS"}, {"PLUS"},
    {"UP"}, {"DOWN"}, {"LEFT"}, {"RIGHT"}
};
constexpr int kFirstNamedTarget = std::size(kLegacyButtons);
constexpr int kLastNamedTarget = kFirstNamedTarget + 1;
constexpr int kChoicesPerPage = 24;
constexpr int kMaxRemoteButtons = 160;
using ButtonName = std::array<char, 48>;
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
enum PickerParameter {
    PickerStatus = modeParam(kMaxRows), PickerPosition, PickerBack, PickerMore,
    PickerNavigation, FirstChoiceName, FirstChoiceVisible = FirstChoiceName + kChoicesPerPage,
    ParamCount = FirstChoiceVisible + kChoicesPerPage
};
enum class DiscoveryStatus { Loading, Unavailable, Empty, Ready };

enum class InputState : uint8_t { Unseen, Released, Pressed };

bool validButtonName(std::string_view name)
{
    return !name.empty() && name.size() < ButtonName{}.size() &&
           name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == std::string_view::npos;
}

#if defined(__linux__) || defined(__APPLE__)
bool remoteRequest(const std::string& path, std::string* body = nullptr)
{
#if defined(__APPLE__)
    const int connection = socket(AF_INET, SOCK_STREAM, 0);
#else
    const int connection = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
    if (connection < 0)
        return false;

    timeval timeout{0, 250000};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(6720);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0
           && setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
    int sendFlags = 0;
#if defined(__APPLE__)
    const int noSigPipe = 1;
    ok = ok && setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe)) == 0;
#else
    sendFlags = MSG_NOSIGNAL;
#endif
    ok = ok && connect(connection, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    const auto request = "GET " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    for (size_t sent = 0; ok && sent < request.size();)
    {
        const auto count = send(connection, request.data() + sent, request.size() - sent, sendFlags);
        if (count <= 0)
            ok = false;
        else
            sent += static_cast<size_t>(count);
    }
    std::string response;
    char buffer[1024];
    while (ok && (body || response.size() < 12))
    {
        const auto count = recv(connection, buffer, sizeof(buffer), 0);
        if (count == 0)
            break;
        ok = count > 0 && response.size() + static_cast<size_t>(count) <= 16 * 1024;
        if (ok)
            response.append(buffer, static_cast<size_t>(count));
    }
    close(connection);
    if (!ok || response.size() < 12 || response.compare(0, 7, "HTTP/1.") != 0 || response[9] != '2')
        return false;
    if (body)
    {
        const auto separator = response.find("\r\n\r\n");
        if (separator == std::string::npos)
            return false;
        *body = response.substr(separator + 4);
    }
    return true;
}
#endif

bool discoverButtons(std::vector<std::string>& buttons)
{
#if defined(__linux__) || defined(__APPLE__)
    std::string response;
    if (!remoteRequest("/buttons", &response))
        return false;

    // Remote returns a JSON array of normalized names, which contain no escapes.
    std::string_view rest(response);
    const auto consume = [&rest](char expected) {
        rest.remove_prefix(std::min(rest.find_first_not_of(" \t\r\n"), rest.size()));
        if (rest.empty() || rest.front() != expected)
            return false;
        rest.remove_prefix(1);
        return true;
    };
    if (!consume('['))
        return false;
    if (!consume(']'))
    {
        do
        {
            if (!consume('"'))
                return false;
            const auto end = rest.find('"');
            if (end == std::string_view::npos || !validButtonName(rest.substr(0, end)) ||
                buttons.size() == kMaxRemoteButtons)
                return false;
            buttons.emplace_back(rest.substr(0, end));
            rest.remove_prefix(end + 1);
        } while (consume(','));
        if (!consume(']'))
            return false;
    }
    return rest.find_first_not_of(" \t\r\n") == std::string_view::npos;
#else
    return false;
#endif
}
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
        addSetting(mpc::intParam("pick-button", "Pick Button", 0, kChoicesPerPage, 0));
        for (int row = 0; row < kMaxRows; ++row)
        {
            const auto number = std::to_string(row + 1);
            addSetting(mpc::intParam(("target-" + number).c_str(), ("Target " + number).c_str(),
                                     0, kLastNamedTarget, 0));
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
        addSetting(mpc::intParam("picker-status", "Picker Status", 0, 3, 0));
        addSetting(mpc::boolParam("picker-position", "Picker Position", false));
        addSetting(mpc::boolParam("picker-back", "Picker Back", false));
        addSetting(mpc::boolParam("picker-more", "Picker More", false));
        addSetting(mpc::intParam("picker-navigation", "Picker Navigation", 0, 2, 0));
        for (int slot = 0; slot < kChoicesPerPage; ++slot)
        {
            const auto number = std::to_string(slot + 1);
            addSetting(mpc::boolParam(("choice-name-" + number).c_str(), ("Choice Name " + number).c_str(), false));
        }
        for (int slot = 0; slot < kChoicesPerPage; ++slot)
        {
            const auto number = std::to_string(slot + 1);
            addSetting(mpc::boolParam(("choice-visible-" + number).c_str(), ("Choice Visible " + number).c_str(), false));
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
        {
            if (step)
            {
                const std::lock_guard<std::mutex> lock(pickerMutex_);
                availableButtons_.clear();
                pickerPage_ = 0;
                setDisplayed(PickerStatus, static_cast<int>(DiscoveryStatus::Loading));
                updateChoices();
                pickerRequest_.fetch_add(1);
            }
            setDisplayed(kPickerVisible, step != 0);
        }
        else if (index == PickerNavigation && step)
        {
            const std::lock_guard<std::mutex> lock(pickerMutex_);
            if (step == 1 && pickerPage_ > 0)
                --pickerPage_;
            if (step == 2 && (pickerPage_ + 1) * kChoicesPerPage < availableButtons_.size())
                ++pickerPage_;
            updateChoices();
            setDisplayed(PickerNavigation, 0);
        }
        else if (index == PickButton && step)
        {
            const int row = getInt(OpenPicker) - 1;
            std::string name;
            {
                const std::lock_guard<std::mutex> lock(pickerMutex_);
                const auto choice = pickerPage_ * kChoicesPerPage + static_cast<size_t>(step - 1);
                if (choice < availableButtons_.size())
                    name = availableButtons_[choice];
            }
            if (!name.empty())
            {
                const std::lock_guard<std::mutex> lock(rowsMutex_);
                if (row >= 0 && row < getInt(RowCount) && name != targetName(row, getInt(rowParam(row, Target))))
                    selectTarget(row, name);
            }
            if (!name.empty())
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
        if (index == PickerPosition)
        {
            const std::lock_guard<std::mutex> lock(pickerMutex_);
            if (availableButtons_.empty())
                text.clear();
            else
            {
                const auto offset = pickerPage_ * kChoicesPerPage;
                const auto end = std::min(offset + kChoicesPerPage, availableButtons_.size());
                text = "Buttons " + std::to_string(offset + 1) + "–" + std::to_string(end)
                    + " of " + std::to_string(availableButtons_.size());
            }
            return true;
        }
        if (index >= FirstChoiceName && index < FirstChoiceVisible)
        {
            const std::lock_guard<std::mutex> lock(pickerMutex_);
            const auto choice = pickerPage_ * kChoicesPerPage + static_cast<size_t>(index - FirstChoiceName);
            text = choice < availableButtons_.size() ? availableButtons_[choice] : "";
            return true;
        }
        if (index < FirstRow || index >= kPickerVisible)
            return false;
        const int step = static_cast<int>(value);
        switch ((index - FirstRow) % RowFields)
        {
            case Target:
            {
                const std::lock_guard<std::mutex> lock(rowsMutex_);
                const auto name = targetName((index - FirstRow) / RowFields, step);
                text = name.empty() ? "Choose control" : name;
                break;
            }
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

    void saveExtraState(std::vector<uint8_t>& out) override
    {
        const std::lock_guard<std::mutex> lock(rowsMutex_);
        out.push_back(1);
        for (const auto& name : namedTargets_)
            out.insert(out.end(), name.begin(), name.end());
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        const std::lock_guard<std::mutex> lock(rowsMutex_);
        namedTargets_ = {};
        if (size != 1 + kMaxRows * ButtonName{}.size() || data[0] != 1)
            return;
        for (auto& name : namedTargets_)
        {
            const auto* begin = reinterpret_cast<const char*>(++data);
            const auto* end = std::find(begin, begin + name.size(), '\0');
            if (validButtonName(std::string_view(begin, static_cast<size_t>(end - begin))))
                std::copy(begin, end, name.begin());
            data += name.size() - 1;
        }
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
                        enqueue(targetName(row, target));
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
        info.version = 1005;
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

    std::string_view targetName(int row, int target) const
    {
        if (target >= kFirstNamedTarget)
            return namedTargets_[row].data();
        return kLegacyButtons[target].name;
    }

    void selectTarget(int row, const std::string& name)
    {
        int target = 1;
        while (target < kFirstNamedTarget && name != kLegacyButtons[target].name)
            ++target;
        const bool legacy = target < kFirstNamedTarget;
        auto& savedName = namedTargets_[row];
        savedName = {};
        if (!legacy)
        {
            // Alternate values so the host refreshes the label when a name changes.
            if (getInt(rowParam(row, Target)) == kFirstNamedTarget)
                target = kLastNamedTarget;
            std::copy(name.begin(), name.end(), savedName.begin());
        }
        setDisplayed(rowParam(row, Target), target);
        setDisplayed(modeParam(row), legacy && kLegacyButtons[target].defaultToggle);
    }

    void updateChoices()
    {
        const auto offset = pickerPage_ * kChoicesPerPage;
        const auto count = std::min(availableButtons_.size() - offset, static_cast<size_t>(kChoicesPerPage));
        for (int slot = 0; slot < kChoicesPerPage; ++slot)
        {
            setDisplayed(FirstChoiceName + slot, !getBool(FirstChoiceName + slot));
            setDisplayed(FirstChoiceVisible + slot, static_cast<size_t>(slot) < count);
        }
        setDisplayed(PickerPosition, !getBool(PickerPosition));
        setDisplayed(PickerBack, pickerPage_ != 0);
        setDisplayed(PickerMore, offset + count < availableButtons_.size());
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
            namedTargets_[next - 1] = namedTargets_[next];
            setDisplayed(rowParam(next - 1, Target), getInt(rowParam(next, Target)));
            changed_[rowParam(next - 1, Target)].store(true, std::memory_order_release);
            setDisplayed(rowParam(next - 1, Binding), getInt(rowParam(next, Binding)));
            setDisplayed(modeParam(next - 1), getBool(modeParam(next)));
        }
        namedTargets_[count - 1] = {};
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

    void enqueue(std::string_view name)
    {
        if (name.empty())
            return;
        const unsigned write = write_.load(std::memory_order_relaxed);
        const unsigned next = (write + 1) % kQueueSize;
        if (next != read_.load(std::memory_order_acquire))
        {
            auto& button = queue_[write];
            std::copy(name.begin(), name.end(), button.begin());
            button[name.size()] = '\0';
            write_.store(next, std::memory_order_release);
        }
    }

    void dispatchCommands()
    {
        unsigned observedRequest = 0;
        while (!stopping_.load())
        {
            for (int index = RowCount; index < ParamCount; ++index)
                if (changed_[index].load(std::memory_order_relaxed) &&
                    changed_[index].exchange(false, std::memory_order_acquire))
                    notifyHostParameterChanged(index);

            const unsigned read = read_.load(std::memory_order_relaxed);
            if (read != write_.load(std::memory_order_acquire))
            {
#if defined(__linux__) || defined(__APPLE__)
                if (!remoteRequest(std::string("/button?name=") + queue_[read].data()))
                    mpc::log("MIDI Transport: Open MPC Remote command failed");
#endif
                read_.store((read + 1) % kQueueSize, std::memory_order_release);
            }
            else if (const auto requested = pickerRequest_.load(); requested != observedRequest)
            {
                observedRequest = requested;
                std::vector<std::string> buttons;
                const bool found = getInt(OpenPicker) != 0 && discoverButtons(buttons);
                const std::lock_guard<std::mutex> lock(pickerMutex_);
                if (requested == pickerRequest_.load() && getInt(OpenPicker) != 0)
                {
                    if (found)
                        availableButtons_ = std::move(buttons);
                    const auto status = !found ? DiscoveryStatus::Unavailable
                        : availableButtons_.empty() ? DiscoveryStatus::Empty : DiscoveryStatus::Ready;
                    updateChoices();
                    setDisplayed(PickerStatus, static_cast<int>(status));
                }
            }
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::atomic<int> learnRow_{-1};
    std::mutex rowsMutex_;
    std::array<std::atomic<bool>, ParamCount> changed_;
    std::array<InputState, 2 * kMessageRange> inputStates_{};
    std::array<ButtonName, kMaxRows> namedTargets_{};
    std::mutex pickerMutex_;
    std::atomic<unsigned> pickerRequest_{0};
    std::vector<std::string> availableButtons_;
    size_t pickerPage_ = 0;
    std::array<ButtonName, kQueueSize> queue_{};
    std::atomic<unsigned> read_{0};
    std::atomic<unsigned> write_{0};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};

MPC_PLUGIN_ENTRY(MidiTransport)
