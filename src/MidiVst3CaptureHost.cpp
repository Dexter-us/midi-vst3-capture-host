#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_processors/format_types/juce_VST3PluginFormat.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
using namespace juce;

struct CaptureConfig
{
    File pluginPath;
    File inputMidiPath;
    File outputMidiPath;
    File statusPath;
    int durationSeconds = 30;
    int ticksPerBeat = 480;
    int tempoMicroseconds = 500000;

    static CaptureConfig load(const File& configPath)
    {
        const auto parsed = JSON::parse(configPath.loadFileAsString());
        auto* object = parsed.getDynamicObject();
        if (object == nullptr)
            throw std::runtime_error("The capture config is not valid JSON.");

        auto readString = [object](const Identifier& key)
        {
            return object->getProperty(key).toString();
        };

        CaptureConfig config;
        config.pluginPath = File(readString("plugin_path"));
        config.inputMidiPath = File(readString("input_midi_path"));
        config.outputMidiPath = File(readString("output_midi_path"));
        config.statusPath = File(readString("status_path"));
        config.durationSeconds =
            static_cast<int>(object->getProperty("duration_seconds"));
        config.ticksPerBeat =
            static_cast<int>(object->getProperty("ticks_per_beat"));
        config.tempoMicroseconds =
            static_cast<int>(object->getProperty("tempo_microseconds"));

        if (config.pluginPath == File()
            || !config.pluginPath.exists())
            throw std::runtime_error("The selected VST3 bundle was not found.");
        if (config.outputMidiPath == File())
            throw std::runtime_error("The output MIDI path is missing.");
        if (config.durationSeconds < 1 || config.durationSeconds > 600)
            throw std::runtime_error(
                "Capture duration must be between 1 and 600 seconds."
            );
        if (config.ticksPerBeat < 1 || config.tempoMicroseconds < 1)
            throw std::runtime_error("The target MIDI timing is invalid.");

        return config;
    }
};

void writeStatus(const File& statusPath, bool ok, const String& message)
{
    if (statusPath == File())
        return;

    DynamicObject::Ptr object = new DynamicObject();
    object->setProperty("ok", ok);
    object->setProperty("message", message);

    statusPath.getParentDirectory().createDirectory();
    FileOutputStream stream(statusPath);
    if (!stream.openedOk())
        return;
    stream.setPosition(0);
    stream.truncate();
    stream.writeText(JSON::toString( var(object.get()), true), false, false, "\n");
    stream.flush();
}

struct TimedMidiMessage
{
    double seconds = 0.0;
    MidiMessage message;
};

struct CapturedMidiMessage
{
    int64 samplePosition = 0;
    MidiMessage message;
};

class CaptureComponent final : public AudioAppComponent,
                               private Timer
{
public:
    CaptureComponent(CaptureConfig captureConfig,
                     std::function<void(bool, const String&)> completion)
        : config(std::move(captureConfig)),
          completionCallback(std::move(completion)),
          startButton("Start capture"),
          cancelButton("Cancel")
    {
        loadInputMidi();
        loadPlugin();

        addAndMakeVisible(startButton);
        addAndMakeVisible(cancelButton);
        addAndMakeVisible(statusLabel);
        addAndMakeVisible(*pluginEditor);

        startButton.onClick = [this] { startCapture(); };
        cancelButton.onClick = [this] { cancel(); };
        statusLabel.setText(
            "Plugin editor is open. Adjust the plugin, then start capture.",
            dontSendNotification
        );
        statusLabel.setColour(Label::textColourId, Colours::white);
        statusLabel.setJustificationType(Justification::centredLeft);

        const auto editorWidth = jmax(520, pluginEditor->getWidth());
        const auto editorHeight = jmax(300, pluginEditor->getHeight());
        setSize(editorWidth, editorHeight + 78);

        setAudioChannels(0, 2);
        startTimerHz(20);
    }

    ~CaptureComponent() override
    {
        stopTimer();
        shutdownAudio();
        if (pluginEditor != nullptr)
            removeChildComponent(pluginEditor);
        plugin.reset();
    }

    void cancel()
    {
        finish(false, "Capture cancelled.");
    }

    void prepareToPlay(int samplesPerBlockExpected,
                       double newSampleRate) override
    {
        currentSampleRate.store(newSampleRate);
        audioSampleCounter.store(0);
        inputEventCursor.store(0);
        if (plugin != nullptr)
            plugin->prepareToPlay(newSampleRate, samplesPerBlockExpected);
    }

    void getNextAudioBlock(const AudioSourceChannelInfo& info) override
    {
        if (info.buffer == nullptr || plugin == nullptr)
            return;

        info.clearActiveBufferRegion();

        const auto sampleRate = currentSampleRate.load();
        const auto blockStart = audioSampleCounter.fetch_add(
            info.numSamples,
            std::memory_order_relaxed
        );
        const auto captureStart = captureStartSample.load();
        const auto isRecording = capturing.load();

        MidiBuffer midiMessages;
        if (isRecording && sampleRate > 0.0)
            addInputEvents(midiMessages, blockStart, captureStart, sampleRate,
                           info.numSamples);

        plugin->processBlock(*info.buffer, midiMessages);

        if (isRecording)
            collectOutputEvents(midiMessages, blockStart, captureStart);
    }

    void releaseResources() override
    {
        if (plugin != nullptr)
            plugin->releaseResources();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto controls = area.removeFromTop(78).reduced(10, 8);
        startButton.setBounds(controls.removeFromLeft(130));
        cancelButton.setBounds(controls.removeFromLeft(100).reduced(6, 0));
        statusLabel.setBounds(controls.reduced(8, 0));
        if (pluginEditor != nullptr)
            pluginEditor->setBounds(area);
    }

private:
    void loadInputMidi()
    {
        if (config.inputMidiPath == File())
            return;

        FileInputStream stream(config.inputMidiPath);
        if (!stream.openedOk())
            throw std::runtime_error("Could not open the source MIDI file.");

        MidiFile source;
        if (!source.readFrom(stream))
            throw std::runtime_error("The source MIDI file could not be read.");
        source.convertTimestampTicksToSeconds();

        for (int trackIndex = 0; trackIndex < source.getNumTracks(); ++trackIndex)
        {
            const auto* track = source.getTrack(trackIndex);
            if (track == nullptr)
                continue;

            for (int eventIndex = 0; eventIndex < track->getNumEvents(); ++eventIndex)
            {
                const auto* event = track->getEventPointer(eventIndex);
                if (event == nullptr || event->message.isMetaEvent())
                    continue;
                inputEvents.push_back(
                    { event->message.getTimeStamp(), event->message }
                );
            }
        }

        std::stable_sort(
            inputEvents.begin(),
            inputEvents.end(),
            [](const auto& a, const auto& b) { return a.seconds < b.seconds; }
        );
    }

    void loadPlugin()
    {
        VST3PluginFormat format;
        OwnedArray<PluginDescription> descriptions;
        format.findAllTypesForFile(
            descriptions,
            config.pluginPath.getFullPathName()
        );
        if (descriptions.isEmpty())
            throw std::runtime_error(
                "No VST3 plugin was found in the selected bundle."
            );

        String error;
        formatManager.addDefaultFormats();
        plugin = formatManager.createPluginInstance(
            *descriptions.getFirst(),
            44100.0,
            512,
            error
        );
        if (plugin == nullptr)
            throw std::runtime_error(
                ("Could not load the VST3 plugin: " + error).toStdString()
            );
        if (!plugin->producesMidi())
            throw std::runtime_error(
                "This VST3 does not declare MIDI output."
            );

        pluginEditor = plugin->createEditorIfNeeded();
        if (pluginEditor == nullptr)
            throw std::runtime_error(
                "This VST3 does not provide an editor window."
            );
    }

    void startCapture()
    {
        {
            const ScopedLock lock(capturedEventsLock);
            capturedEvents.clear();
        }
        inputEventCursor.store(0);
        captureStartSample.store(audioSampleCounter.load());
        captureStartTime = std::chrono::steady_clock::now();
        capturing.store(true);
        startButton.setEnabled(false);
        statusLabel.setText(
            "Capturing MIDI output. The plugin editor remains available.",
            dontSendNotification
        );
    }

    void addInputEvents(MidiBuffer& midiMessages,
                        int64 blockStart,
                        int64 captureStart,
                        double sampleRate,
                        int numSamples)
    {
        const auto relativeBlockStart = jmax<int64>(0, blockStart - captureStart);
        const auto relativeBlockEnd = relativeBlockStart + numSamples;
        auto cursor = inputEventCursor.load();

        while (cursor < static_cast<int>(inputEvents.size()))
        {
            const auto& event = inputEvents[static_cast<size_t>(cursor)];
            const auto eventSample = static_cast<int64>(
                std::llround(event.seconds * sampleRate)
            );
            if (eventSample >= relativeBlockEnd)
                break;

            const auto sampleOffset = static_cast<int>(
                jlimit<int64>(
                    0,
                    numSamples - 1,
                    eventSample - relativeBlockStart
                )
            );
            midiMessages.addEvent(event.message, sampleOffset);
            ++cursor;
        }

        inputEventCursor.store(cursor);
    }

    void collectOutputEvents(const MidiBuffer& midiMessages,
                             int64 blockStart,
                             int64 captureStart)
    {
        const ScopedLock lock(capturedEventsLock);
        for (const auto metadata : midiMessages)
        {
            const auto message = metadata.getMessage();
            if (message.isMetaEvent())
                continue;

            capturedEvents.push_back(
                {
                    blockStart + metadata.samplePosition - captureStart,
                    message
                }
            );
        }
    }

    void timerCallback() override
    {
        if (!capturing.load())
            return;

        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - captureStartTime
        ).count();
        if (elapsed >= config.durationSeconds)
            finish(true, {});
    }

    void finish(bool saveCapture, const String& reason)
    {
        if (finished)
            return;
        finished = true;
        capturing.store(false);
        stopTimer();
        startButton.setEnabled(false);
        cancelButton.setEnabled(false);
        shutdownAudio();

        if (!saveCapture)
        {
            writeStatus(config.statusPath, false, reason);
            if (completionCallback)
                completionCallback(false, reason);
            return;
        }

        try
        {
            const auto noteOnCount = saveMidiFile();
            const auto message =
                "Captured " + String(noteOnCount)
                + " MIDI note-on event(s).";
            statusLabel.setText(message, dontSendNotification);
            writeStatus(config.statusPath, true, message);
            if (completionCallback)
                completionCallback(true, message);
        }
        catch (const std::exception& exception)
        {
            const auto message = String(exception.what());
            statusLabel.setText(message, dontSendNotification);
            writeStatus(config.statusPath, false, message);
            if (completionCallback)
                completionCallback(false, message);
        }
    }

    int saveMidiFile()
    {
        std::vector<CapturedMidiMessage> events;
        {
            const ScopedLock lock(capturedEventsLock);
            events = capturedEvents;
        }

        std::stable_sort(
            events.begin(),
            events.end(),
            [](const auto& a, const auto& b)
            {
                return a.samplePosition < b.samplePosition;
            }
        );

        const auto sampleRate = currentSampleRate.load();
        if (sampleRate <= 0.0)
            throw std::runtime_error("The audio clock did not start.");

        MidiMessageSequence sequence;
        sequence.addEvent(
            MidiMessage::tempoMetaEvent(config.tempoMicroseconds),
            0.0
        );
        int noteOnCount = 0;
        for (auto& event : events)
        {
            const auto seconds = jmax<int64>(0, event.samplePosition) / sampleRate;
            const auto ticks = seconds * 1000000.0
                * config.ticksPerBeat / config.tempoMicroseconds;
            auto message = event.message;
            message.setTimeStamp(ticks);
            sequence.addEvent(message);
            if (message.isNoteOn())
                ++noteOnCount;
        }

        if (noteOnCount < 1)
            throw std::runtime_error(
                "The VST3 capture contained no MIDI note-on events."
            );

        MidiFile output;
        output.setTicksPerQuarterNote(config.ticksPerBeat);
        output.addTrack(sequence);

        config.outputMidiPath.getParentDirectory().createDirectory();
        FileOutputStream stream(config.outputMidiPath);
        if (!stream.openedOk())
            throw std::runtime_error("Could not create the captured MIDI file.");
        stream.setPosition(0);
        stream.truncate();
        if (!output.writeTo(stream, 0) || !stream.flush())
            throw std::runtime_error("Could not write the captured MIDI file.");
        return noteOnCount;
    }

    CaptureConfig config;
    std::function<void(bool, const String&)> completionCallback;
    AudioPluginFormatManager formatManager;
    std::unique_ptr<AudioPluginInstance> plugin;
    AudioProcessorEditor* pluginEditor = nullptr;
    std::vector<TimedMidiMessage> inputEvents;
    std::vector<CapturedMidiMessage> capturedEvents;
    CriticalSection capturedEventsLock;
    std::atomic<int64> audioSampleCounter { 0 };
    std::atomic<int64> captureStartSample { 0 };
    std::atomic<int> inputEventCursor { 0 };
    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<bool> capturing { false };
    std::chrono::steady_clock::time_point captureStartTime;
    TextButton startButton;
    TextButton cancelButton;
    Label statusLabel;
    bool finished = false;
};

class MainWindow final : public DocumentWindow
{
public:
    MainWindow(const String& title,
               CaptureConfig config,
               std::function<void(bool, const String&)> completion)
        : DocumentWindow(
              title,
              Desktop::getInstance().getDefaultLookAndFeel()
                  .findColour(ResizableWindow::backgroundColourId),
              DocumentWindow::allButtons
          )
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setContentOwned(
            new CaptureComponent(std::move(config), std::move(completion)),
            true
        );
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        if (auto* component =
                dynamic_cast<CaptureComponent*>(getContentComponent()))
            component->cancel();
        JUCEApplication::getInstance()->quit();
    }
};

class CaptureHostApplication final : public JUCEApplication
{
public:
    const String getApplicationName() override
    {
        return "MIDI VST3 Capture Host";
    }

    const String getApplicationVersion() override
    {
        return "0.1.0";
    }

    void initialise(const String& commandLine) override
    {
        const auto configPath = File(commandLine.trim().unquoted());
        try
        {
            config = CaptureConfig::load(configPath);
            mainWindow = std::make_unique<MainWindow>(
                getApplicationName(),
                config,
                [this](bool, const String&)
                {
                    Timer::callAfterDelay(300, [this] { quit(); });
                }
            );
        }
        catch (const std::exception& exception)
        {
            const auto message = String(exception.what());
            if (config.statusPath != File())
                writeStatus(config.statusPath, false, message);
            Logger::writeToLog(message);
            quit();
        }
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    CaptureConfig config;
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace

START_JUCE_APPLICATION(CaptureHostApplication)