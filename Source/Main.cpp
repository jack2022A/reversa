#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>

namespace
{
constexpr int kSlots = 4;
const juce::Colour kBackground = juce::Colour(0xff120d17);
const juce::Colour kPanel = juce::Colour(0xff251926);
const juce::Colour kAccent = juce::Colour(0xffc45e87);
const std::array<juce::Colour, kSlots> kLayerColours {
    juce::Colour(0xffcf527e), juce::Colour(0xff37b4d0),
    juce::Colour(0xff9a75cd), juce::Colour(0xffe5b34c)
};

struct LayerPreset
{
    juce::String file, name, pluginState;
    float volume = 0.8f, cutoff = 20000.0f;
    bool muted = false, solo = false;
    int transpose = 0;
};

struct ProgramPreset
{
    juce::String name;
    std::array<LayerPreset, kSlots> layers;
    float masterVolume = 0.8f, reverb = 0.15f;
};

class VolumeProcessor final : public juce::AudioProcessor
{
public:
    VolumeProcessor()
        : juce::AudioProcessor(juce::AudioProcessor::BusesProperties()
                                   .withInput("Stereo input", juce::AudioChannelSet::stereo(), true)
                                   .withOutput("Stereo output", juce::AudioChannelSet::stereo(), true))
    {
    }

    void setLinearGain(float value) { gain.store(juce::jlimit(0.0f, 1.5f, value)); }
    void setCutoff(float value) { cutoff.store(juce::jlimit(200.0f, 20000.0f, value)); }
    float getLevel() const { return peak.load(); }
    const juce::String getName() const override { return "Volume PlayStage"; }
    void prepareToPlay(double sampleRate, int) override
    {
        rate = sampleRate;
        state = { 0.0f, 0.0f };
    }
    void releaseResources() override { state = { 0.0f, 0.0f }; }
    void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override
    {
        const auto hz = static_cast<double>(cutoff.load());
        const auto alpha = static_cast<float>(std::exp(-juce::MathConstants<double>::twoPi
                                                     * hz / juce::jmax(1.0, rate)));
        const auto inputGain = gain.load();
        for (int channel = 0; channel < juce::jmin(audio.getNumChannels(), 2); ++channel)
        {
            auto* samples = audio.getWritePointer(channel);
            auto& last = state[static_cast<size_t>(channel)];
            for (int i = 0; i < audio.getNumSamples(); ++i)
            {
                last = (1.0f - alpha) * samples[i] + alpha * last;
                samples[i] = last * inputGain;
            }
        }
        peak.store(juce::jmax(audio.getMagnitude(0, audio.getNumSamples()), peak.load() * 0.9f));
    }
    bool isBusesLayoutSupported(const BusesLayout& layout) const override
    {
        return layout.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            && layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    std::atomic<float> gain { 0.8f };
    std::atomic<float> cutoff { 20000.0f };
    std::atomic<float> peak { 0.0f };
    std::array<float, 2> state { 0.0f, 0.0f };
    double rate = 44100.0;
};

class MidiLayerProcessor final : public juce::AudioProcessor
{
public:
    void setTranspose(int semitones) { transpose.store(juce::jlimit(-24, 24, semitones)); }
    const juce::String getName() const override { return "PlayStage MIDI Zone"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer& events) override
    {
        juce::MidiBuffer output;
        const int shift = transpose.load();
        for (const auto metadata : events)
        {
            auto midi = metadata.getMessage();
            if (midi.isNoteOnOrOff())
                midi.setNoteNumber(juce::jlimit(0, 127, midi.getNoteNumber() + shift));
            output.addEvent(midi, metadata.samplePosition);
        }
        events.swapWith(output);
    }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    std::atomic<int> transpose { 0 };
};

class MasterProcessor final : public juce::AudioProcessor
{
public:
    MasterProcessor()
        : juce::AudioProcessor(juce::AudioProcessor::BusesProperties()
                                   .withInput("Stereo input", juce::AudioChannelSet::stereo(), true)
                                   .withOutput("Stereo output", juce::AudioChannelSet::stereo(), true))
    {
    }
    void setVolume(float value) { volume.store(juce::jlimit(0.0f, 1.5f, value)); }
    void setReverb(float value) { reverbLevel.store(juce::jlimit(0.0f, 0.8f, value)); }
    float getLevel(int channel) const { return peaks[static_cast<size_t>(juce::jlimit(0, 1, channel))].load(); }
    const juce::String getName() const override { return "PlayStage Master"; }
    void prepareToPlay(double sampleRate, int) override
    {
        reverb.setSampleRate(sampleRate);
        reverb.reset();
        lastReverb = -1.0f;
    }
    void releaseResources() override { reverb.reset(); }
    void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override
    {
        if (audio.getNumChannels() < 2)
            return;
        const float wet = reverbLevel.load();
        if (wet != lastReverb)
        {
            juce::Reverb::Parameters p;
            p.roomSize = 0.48f;
            p.damping = 0.45f;
            p.width = 1.0f;
            p.wetLevel = wet;
            p.dryLevel = 1.0f;
            reverb.setParameters(p); // exclusively on the audio thread
            lastReverb = wet;
        }
        reverb.processStereo(audio.getWritePointer(0), audio.getWritePointer(1), audio.getNumSamples());
        audio.applyGain(volume.load());
        for (int channel = 0; channel < 2; ++channel)
        {
            const auto peak = audio.getMagnitude(channel, 0, audio.getNumSamples());
            peaks[static_cast<size_t>(channel)].store(juce::jmax(peak, peaks[static_cast<size_t>(channel)].load() * 0.91f));
        }
    }
    bool isBusesLayoutSupported(const BusesLayout& layout) const override
    {
        return layout.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            && layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    std::atomic<float> volume { 0.8f }, reverbLevel { 0.15f };
    std::array<std::atomic<float>, 2> peaks { 0.0f, 0.0f };
    juce::Reverb reverb;
    float lastReverb = -1.0f;
};

class HostEngine
{
public:
    HostEngine()
    {
        pluginFormats.addFormat(std::make_unique<juce::VST3PluginFormat>());
        graph = std::make_unique<juce::AudioProcessorGraph>();
        graph->setPlayConfigDetails(0, 2, 44100.0, 512);

        audioOutput = graph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));
        midiInput = graph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::midiInputNode));

        auto master = std::make_unique<MasterProcessor>();
        masterProcessor = master.get();
        masterNode = graph->addNode(std::move(master));
        graph->addConnection({ { masterNode->nodeID, 0 }, { audioOutput->nodeID, 0 } });
        graph->addConnection({ { masterNode->nodeID, 1 }, { audioOutput->nodeID, 1 } });

        for (int i = 0; i < kSlots; ++i)
        {
            auto midiZone = std::make_unique<MidiLayerProcessor>();
            midiProcessors[static_cast<size_t>(i)] = midiZone.get();
            midiNodes[static_cast<size_t>(i)] = graph->addNode(std::move(midiZone));
            graph->addConnection({ { midiInput->nodeID, juce::AudioProcessorGraph::midiChannelIndex },
                                   { midiNodes[static_cast<size_t>(i)]->nodeID,
                                     juce::AudioProcessorGraph::midiChannelIndex } });

            auto volume = std::make_unique<VolumeProcessor>();
            volumes[static_cast<size_t>(i)] = volume.get();
            volumeNodes[static_cast<size_t>(i)] = graph->addNode(std::move(volume));
            connectVolumeToOutput(i);
            layerVolumes[static_cast<size_t>(i)] = 0.8f;
        }

        const auto error = deviceManager.initialise(0, 2, nullptr, true);
        if (error.isNotEmpty())
            lastError = "Dispositivo de audio: " + error;

        graph->setPlayConfigDetails(0, 2, getSampleRate(), getBufferSize());
        player.setProcessor(graph.get());
        deviceManager.addAudioCallback(&player);
        refreshMidiDevices();
    }

    ~HostEngine()
    {
        for (const auto& device : juce::MidiInput::getAvailableDevices())
            deviceManager.removeMidiInputDeviceCallback(device.identifier, &player);
        deviceManager.removeAudioCallback(&player);
        player.setProcessor(nullptr);
        graph.reset();
    }

    juce::String getLastError() const { return lastError; }
    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }

    void refreshMidiDevices()
    {
        for (const auto& device : juce::MidiInput::getAvailableDevices())
        {
            deviceManager.removeMidiInputDeviceCallback(device.identifier, &player);
            deviceManager.setMidiInputDeviceEnabled(device.identifier, true);
            deviceManager.addMidiInputDeviceCallback(device.identifier, &player);
        }
    }

    void queueMidi(const juce::MidiMessage& message)
    {
        player.getMidiMessageCollector().addMessageToQueue(message);
    }

    void setLayerSettings(int index, float gain, bool muted, bool solo,
                          float cutoffHz, int transpose)
    {
        if (juce::isPositiveAndBelow(index, kSlots))
        {
            const auto pos = static_cast<size_t>(index);
            layerVolumes[pos] = gain;
            layerMuted[pos] = muted;
            layerSolo[pos] = solo;
            volumes[pos]->setCutoff(cutoffHz);
            midiProcessors[pos]->setTranspose(transpose);
            refreshGains();
        }
    }

    void setMaster(float volume, float reverbAmount)
    {
        masterProcessor->setVolume(volume);
        masterProcessor->setReverb(reverbAmount);
    }

    float getMasterLevel(int channel) const { return masterProcessor->getLevel(channel); }
    float getLayerLevel(int index) const
    {
        return juce::isPositiveAndBelow(index, kSlots)
                   ? volumes[static_cast<size_t>(index)]->getLevel() : 0.0f;
    }

    void panic()
    {
        for (int channel = 1; channel <= 16; ++channel)
        {
            queueMidi(juce::MidiMessage::controllerEvent(channel, 64, 0));
            queueMidi(juce::MidiMessage::allNotesOff(channel));
            queueMidi(juce::MidiMessage::allSoundOff(channel));
        }
    }

    void setPedal(bool pressed)
    {
        for (int channel = 1; channel <= 16; ++channel)
            queueMidi(juce::MidiMessage::controllerEvent(channel, 64, pressed ? 127 : 0));
    }

    void clearPlugin(int index)
    {
        if (!juce::isPositiveAndBelow(index, kSlots))
            return;
        const auto pos = static_cast<size_t>(index);
        if (auto existing = graph->getNodeForId(pluginIds[pos]))
            graph->removeNode(existing->nodeID);
        pluginIds[pos] = {};
    }

    juce::AudioProcessor* getPlugin(int index)
    {
        if (!juce::isPositiveAndBelow(index, kSlots))
            return nullptr;
        if (auto node = graph->getNodeForId(pluginIds[static_cast<size_t>(index)]))
            return node->getProcessor();
        return nullptr;
    }

    bool loadVst3(int index, const juce::File& vst3, juce::String& error)
    {
        if (!juce::isPositiveAndBelow(index, kSlots) || !vst3.exists()
            || !vst3.hasFileExtension(".vst3"))
        {
            error = "Selecione um plugin .vst3 valido para macOS.";
            return false;
        }

        juce::OwnedArray<juce::PluginDescription> candidates;
        auto* format = pluginFormats.getFormat(0);
        format->findAllTypesForFile(candidates, vst3.getFullPathName());
        if (candidates.isEmpty())
        {
            error = "Nao foi encontrado um plugin VST3 compativel neste arquivo.";
            return false;
        }

        auto instance = pluginFormats.createPluginInstance(*candidates[0], getSampleRate(),
                                                            getBufferSize(), error);
        if (!instance)
            return false;

        if (instance->getTotalNumOutputChannels() < 1)
        {
            error = "O plugin nao produz audio (instrumento/efeito sem saida).";
            return false;
        }

        // The caller must close any open plugin editor before this replaces its node.
        clearPlugin(index);

        auto newNode = graph->addNode(std::move(instance));
        if (!newNode)
        {
            error = "Nao foi possivel adicionar o plugin ao motor de audio.";
            return false;
        }
        pluginIds[static_cast<size_t>(index)] = newNode->nodeID;

        if (newNode->getProcessor()->acceptsMidi()
            && !graph->addConnection({ { midiNodes[static_cast<size_t>(index)]->nodeID,
                                        juce::AudioProcessorGraph::midiChannelIndex },
                                      { newNode->nodeID, juce::AudioProcessorGraph::midiChannelIndex } }))
        {
            error = "Aviso: nao foi possivel conectar MIDI ao plugin.";
        }

        const auto leftConnected = graph->addConnection({ { newNode->nodeID, 0 },
                                 { volumeNodes[static_cast<size_t>(index)]->nodeID, 0 } });
        bool rightConnected = false;
        if (newNode->getProcessor()->getTotalNumOutputChannels() > 1)
            rightConnected = graph->addConnection({ { newNode->nodeID, 1 },
                         { volumeNodes[static_cast<size_t>(index)]->nodeID, 1 } });
        else
            rightConnected = graph->addConnection({ { newNode->nodeID, 0 },
                         { volumeNodes[static_cast<size_t>(index)]->nodeID, 1 } });

        if (!leftConnected || !rightConnected)
        {
            error = "O plugin abriu, mas a conexao de audio nao foi concluida.";
            clearPlugin(index);
            return false;
        }

        return true;
    }

private:
    void refreshGains()
    {
        const bool anySolo = std::any_of(layerSolo.begin(), layerSolo.end(), [](bool value) { return value; });
        for (int i = 0; i < kSlots; ++i)
        {
            const auto pos = static_cast<size_t>(i);
            volumes[pos]->setLinearGain(layerMuted[pos] || (anySolo && !layerSolo[pos])
                                              ? 0.0f : layerVolumes[pos]);
        }
    }

    double getSampleRate() const
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            return device->getCurrentSampleRate();
        return 44100.0;
    }

    int getBufferSize() const
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            return device->getCurrentBufferSizeSamples();
        return 512;
    }

    void connectVolumeToOutput(int index)
    {
        auto id = volumeNodes[static_cast<size_t>(index)]->nodeID;
        graph->addConnection({ { id, 0 }, { masterNode->nodeID, 0 } });
        graph->addConnection({ { id, 1 }, { masterNode->nodeID, 1 } });
    }

    juce::AudioDeviceManager deviceManager;
    juce::AudioProcessorPlayer player;
    juce::AudioPluginFormatManager pluginFormats;
    std::unique_ptr<juce::AudioProcessorGraph> graph;
    juce::AudioProcessorGraph::Node::Ptr audioOutput;
    juce::AudioProcessorGraph::Node::Ptr midiInput;
    juce::AudioProcessorGraph::Node::Ptr masterNode;
    MasterProcessor* masterProcessor = nullptr;
    std::array<juce::AudioProcessorGraph::Node::Ptr, kSlots> midiNodes;
    std::array<juce::AudioProcessorGraph::Node::Ptr, kSlots> volumeNodes;
    std::array<juce::AudioProcessorGraph::NodeID, kSlots> pluginIds {};
    std::array<MidiLayerProcessor*, kSlots> midiProcessors {};
    std::array<VolumeProcessor*, kSlots> volumes {};
    std::array<float, kSlots> layerVolumes {};
    std::array<bool, kSlots> layerMuted {}, layerSolo {};
    juce::String lastError;
};

class PluginEditorWindow final : public juce::DocumentWindow
{
public:
    explicit PluginEditorWindow(juce::AudioProcessor& processor)
        : juce::DocumentWindow(processor.getName(), kPanel,
                               juce::DocumentWindow::closeButton)
    {
        auto* editor = processor.hasEditor() ? processor.createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor(processor);
        setContentOwned(editor, true);
        setResizable(true, true);
        centreWithSize(juce::jmax(500, getWidth()), juce::jmax(350, getHeight()));
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override { setVisible(false); }
};

class ChannelStrip final : public juce::Component
{
public:
    ChannelStrip(int channelNumber, std::function<void(int)> load,
                 std::function<void(int)> edit,
                 std::function<void(int)> clear,
                 std::function<void(int, const LayerPreset&)> changed)
        : channel(channelNumber), onLoad(std::move(load)), onEdit(std::move(edit)),
          onClear(std::move(clear)), onChanged(std::move(changed))
    {
        heading.setText("CAMADA " + juce::String(channel + 1), juce::dontSendNotification);
        heading.setFont(juce::Font(14.0f, juce::Font::bold));
        heading.setColour(juce::Label::textColourId, kLayerColours[static_cast<size_t>(channel)]);
        addAndMakeVisible(heading);

        instrument.setText("+ Instrumento VST3", juce::dontSendNotification);
        instrument.setJustificationType(juce::Justification::centred);
        instrument.setFont(juce::Font(13.0f, juce::Font::bold));
        instrument.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(instrument);

        loadButton.setButtonText("+ Adicionar VST3");
        loadButton.onClick = [this] { onLoad(channel); };
        addAndMakeVisible(loadButton);
        editButton.setButtonText("Editar plugin");
        editButton.onClick = [this] { onEdit(channel); };
        editButton.setEnabled(false);
        addAndMakeVisible(editButton);
        clearButton.setButtonText("X");
        clearButton.onClick = [this] { onClear(channel); };
        addAndMakeVisible(clearButton);

        mute.setButtonText("M");
        mute.setClickingTogglesState(true);
        mute.onClick = [this] { notify(); };
        addAndMakeVisible(mute);
        solo.setButtonText("S");
        solo.setClickingTogglesState(true);
        solo.onClick = [this] { notify(); };
        addAndMakeVisible(solo);

        volume.setSliderStyle(juce::Slider::LinearVertical);
        volume.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 24);
        volume.setRange(0.0, 1.5, 0.01);
        volume.setValue(0.8, juce::dontSendNotification);
        volume.setColour(juce::Slider::thumbColourId, kLayerColours[static_cast<size_t>(channel)]);
        volume.onValueChange = [this] { notify(); };
        addAndMakeVisible(volume);

        cutoff.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        cutoff.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 21);
        cutoff.setRange(200.0, 20000.0, 1.0);
        cutoff.setSkewFactorFromMidPoint(4200.0);
        cutoff.setValue(20000.0, juce::dontSendNotification);
        cutoff.setColour(juce::Slider::rotarySliderFillColourId,
                         kLayerColours[static_cast<size_t>(channel)]);
        cutoff.onValueChange = [this] { notify(); };
        addAndMakeVisible(cutoff);

        transpose.setSliderStyle(juce::Slider::LinearHorizontal);
        transpose.setTextBoxStyle(juce::Slider::TextBoxRight, false, 42, 20);
        transpose.setRange(-12, 12, 1);
        transpose.setValue(0, juce::dontSendNotification);
        transpose.onValueChange = [this] { notify(); };
        addAndMakeVisible(transpose);
    }

    LayerPreset getState() const
    {
        LayerPreset result = saved;
        result.volume = static_cast<float>(volume.getValue());
        result.cutoff = static_cast<float>(cutoff.getValue());
        result.muted = mute.getToggleState();
        result.solo = solo.getToggleState();
        result.transpose = static_cast<int>(transpose.getValue());
        return result;
    }

    void applyState(const LayerPreset& layer)
    {
        saved = layer;
        applying = true;
        volume.setValue(layer.volume, juce::dontSendNotification);
        cutoff.setValue(layer.cutoff, juce::dontSendNotification);
        mute.setToggleState(layer.muted, juce::dontSendNotification);
        solo.setToggleState(layer.solo, juce::dontSendNotification);
        transpose.setValue(layer.transpose, juce::dontSendNotification);
        instrument.setText(layer.name.isEmpty() ? "+ Instrumento VST3" : layer.name,
                           juce::dontSendNotification);
        editButton.setEnabled(layer.file.isNotEmpty());
        applying = false;
        notify();
    }

    void setPlugin(const juce::String& name, const juce::String& file)
    {
        saved.name = name;
        saved.file = file;
        saved.pluginState.clear();
        instrument.setText(name.isEmpty() ? "+ Instrumento VST3" : name,
                           juce::dontSendNotification);
        editButton.setEnabled(file.isNotEmpty());
    }

    void setMeter(float level) { meter = juce::jlimit(0.0f, 1.0f, level); repaint(); }

    void paint(juce::Graphics& g) override
    {
        auto colour = kLayerColours[static_cast<size_t>(channel)];
        g.setColour(kPanel);
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 9.0f);
        g.setColour(colour);
        g.fillRoundedRectangle(juce::Rectangle<float>(1, 0, static_cast<float>(getWidth() - 2), 4), 2.0f);

        const auto meterBounds = juce::Rectangle<int>(14, getHeight() - 205, 10, 128);
        g.setColour(juce::Colour(0xff090b10));
        g.fillRoundedRectangle(meterBounds.toFloat(), 3.0f);
        g.setColour(meter > 0.85f ? juce::Colours::orange : juce::Colours::seagreen);
        const auto pixels = juce::roundToInt(meter * static_cast<float>(meterBounds.getHeight()));
        g.fillRect(meterBounds.getX() + 2, meterBounds.getBottom() - pixels,
                   meterBounds.getWidth() - 4, pixels);

        g.setColour(juce::Colours::lightgrey.withAlpha(0.7f));
        g.setFont(11.0f);
        g.drawText("VOLUME", 28, getHeight() - 69, 80, 15, juce::Justification::centred);
        g.drawText("FILTRO", getWidth() - 98, getHeight() - 69, 80, 15,
                   juce::Justification::centred);
        g.drawText("TRANSPOSE", 12, getHeight() - 49, 92, 16,
                   juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);
        auto header = area.removeFromTop(29);
        heading.setBounds(header.removeFromLeft(area.getWidth() - 98));
        clearButton.setBounds(header.removeFromRight(26));
        solo.setBounds(header.removeFromRight(26));
        mute.setBounds(header.removeFromRight(26));
        area.removeFromTop(6);
        instrument.setBounds(area.removeFromTop(75));
        area.removeFromTop(7);
        loadButton.setBounds(area.removeFromTop(30));
        area.removeFromTop(7);
        editButton.setBounds(area.removeFromTop(28));

        auto bottom = area.removeFromBottom(50);
        transpose.setBounds(bottom.removeFromBottom(32));
        area.removeFromTop(6);
        auto knobs = area.reduced(16, 0);
        auto half = knobs.getWidth() / 2;
        volume.setBounds(knobs.removeFromLeft(half));
        cutoff.setBounds(knobs);
    }

private:
    void notify()
    {
        if (!applying)
            onChanged(channel, getState());
    }
    int channel = 0;
    bool applying = false;
    float meter = 0.0f;
    LayerPreset saved;
    std::function<void(int)> onLoad, onEdit, onClear;
    std::function<void(int, const LayerPreset&)> onChanged;
    juce::Label heading, instrument;
    juce::TextButton loadButton, editButton, clearButton;
    juce::Slider volume, cutoff, transpose;
    juce::ToggleButton mute, solo;
};

class MainComponent final : public juce::Component,
                            public juce::ListBoxModel,
                            private juce::MidiKeyboardStateListener,
                            private juce::Timer
{
public:
    MainComponent() : keyboard(keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard),
                      libraryList("Biblioteca MainStage", this)
    {
        title.setText("PLAYSTAGE LIVE", juce::dontSendNotification);
        title.setFont(juce::Font(29.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(title);

        subtitle.setText("VST3 / QUATRO CAMADAS / macOS Catalina", juce::dontSendNotification);
        subtitle.setFont(juce::Font(11.0f));
        subtitle.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(subtitle);

        programCaption.setText("MEUS PROGRAMAS / 10 POSICOES", juce::dontSendNotification);
        programCaption.setFont(juce::Font(11.0f, juce::Font::bold));
        programCaption.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(programCaption);

        for (int i = 0; i < 10; ++i)
        {
            programs[static_cast<size_t>(i)].name = "Programa " + juce::String(i + 1).paddedLeft('0', 2);
            auto button = std::make_unique<juce::TextButton>(juce::String(i + 1));
            button->onClick = [this, i] { selectProgram(i); };
            addAndMakeVisible(*button);
            programButtons[static_cast<size_t>(i)] = std::move(button);
        }

        programName.setText(programs[0].name);
        programName.setFont(juce::Font(15.0f));
        programName.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1b111c));
        programName.setColour(juce::TextEditor::textColourId, juce::Colours::white);
        programName.onTextChange = [this]
        {
            programs[static_cast<size_t>(selectedProgram)].name = programName.getText();
        };
        addAndMakeVisible(programName);

        saveButton.setButtonText("Salvar preset");
        saveButton.onClick = [this] { savePresetFile(); };
        addAndMakeVisible(saveButton);
        openButton.setButtonText("Abrir preset");
        openButton.onClick = [this] { openPresetFile(); };
        addAndMakeVisible(openButton);

        project.setText("BIBLIOTECA MAINSTAGE\n\nPatches do Mac (consulta)",
                        juce::dontSendNotification);
        project.setJustificationType(juce::Justification::topLeft);
        project.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(project);

        scanLibraryButton.setButtonText("Buscar biblioteca");
        scanLibraryButton.onClick = [this] { scanDefaultLibrary(); };
        addAndMakeVisible(scanLibraryButton);

        customLibraryButton.setButtonText("Outra pasta...");
        customLibraryButton.onClick = [this] { chooseLibraryFolder(); };
        addAndMakeVisible(customLibraryButton);

        libraryList.setRowHeight(25);
        libraryList.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff242b34));
        libraryList.setColour(juce::ListBox::outlineColourId, kAccent);
        addAndMakeVisible(libraryList);

        settings.setButtonText("Audio / MIDI");
        settings.onClick = [this] { showDeviceSettings(); };
        addAndMakeVisible(settings);

        refresh.setButtonText("Atualizar MIDI");
        refresh.onClick = [this]
        {
            engine.refreshMidiDevices();
            status.setText("Dispositivos MIDI atualizados.", juce::dontSendNotification);
        };
        addAndMakeVisible(refresh);

        masterVolume.setSliderStyle(juce::Slider::LinearVertical);
        masterVolume.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 65, 22);
        masterVolume.setRange(0.0, 1.5, 0.01);
        masterVolume.setValue(0.8, juce::dontSendNotification);
        masterVolume.onValueChange = [this] { updateMaster(); };
        addAndMakeVisible(masterVolume);

        reverbKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        reverbKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 20);
        reverbKnob.setRange(0.0, 0.8, 0.01);
        reverbKnob.setValue(0.15, juce::dontSendNotification);
        reverbKnob.setColour(juce::Slider::rotarySliderFillColourId, kAccent);
        reverbKnob.onValueChange = [this] { updateMaster(); };
        addAndMakeVisible(reverbKnob);

        panicButton.setButtonText("PARAR NOTAS");
        panicButton.onClick = [this]
        {
            engine.panic();
            status.setText("Panic MIDI enviado: todas as notas desligadas.", juce::dontSendNotification);
        };
        addAndMakeVisible(panicButton);

        pedalButton.setButtonText("PEDAL / OFF");
        pedalButton.setClickingTogglesState(true);
        pedalButton.onClick = [this]
        {
            engine.setPedal(pedalButton.getToggleState());
            pedalButton.setButtonText(pedalButton.getToggleState() ? "PEDAL / ON" : "PEDAL / OFF");
        };
        addAndMakeVisible(pedalButton);

        status.setText(engine.getLastError().isEmpty()
                           ? "Pronto. Abra um instrumento .vst3 em qualquer canal."
                           : engine.getLastError(), juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(status);

        for (int i = 0; i < kSlots; ++i)
        {
            auto strip = std::make_unique<ChannelStrip>(
                i,
                [this](int slot) { pickVst3(slot); },
                [this](int slot) { openEditor(slot); },
                [this](int slot) { clearLayer(slot); },
                [this](int slot, const LayerPreset& state)
                {
                    engine.setLayerSettings(slot, state.volume, state.muted, state.solo,
                                            state.cutoff, state.transpose);
                    programs[static_cast<size_t>(selectedProgram)].layers[static_cast<size_t>(slot)] = state;
                });
            addAndMakeVisible(*strip);
            strips[static_cast<size_t>(i)] = std::move(strip);
        }

        keyboard.setAvailableRange(36, 96);
        keyboardState.addListener(this);
        addAndMakeVisible(keyboard);
        setSize(1320, 790);
        scanDefaultLibrary();
        refreshProgramButtons();
        startTimerHz(18);
    }

    ~MainComponent() override
    {
        stopTimer();
        keyboardState.removeListener(this);
        for (auto& editor : editors)
            editor.reset();
    }

    int getNumRows() override { return libraryFiles.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool selected) override
    {
        if (!juce::isPositiveAndBelow(row, libraryFiles.size()))
            return;
        if (selected)
            g.fillAll(kAccent.darker(0.35f));
        g.setColour(juce::Colours::white);
        g.setFont(12.0f);
        g.drawFittedText(libraryFiles.getReference(row).getFileName(),
                         7, 0, width - 12, height, juce::Justification::centredLeft, 1);
    }

    void selectedRowsChanged(int lastRow) override
    {
        if (juce::isPositiveAndBelow(lastRow, libraryFiles.size()))
        {
            const auto path = libraryFiles.getReference(lastRow).getFullPathName();
            status.setText("Biblioteca (consulta): " + path, juce::dontSendNotification);
        }
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow(row, libraryFiles.size()))
            libraryFiles.getReference(row).revealToUser();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(kBackground);
        const auto header = juce::Rectangle<float>(0, 0, static_cast<float>(getWidth()), 156);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff68213c), 0.0f, 0.0f,
                                              juce::Colour(0xff20121e),
                                              static_cast<float>(getWidth()), 156.0f, false));
        g.fillRect(header);
        g.setColour(juce::Colour(0xff3d1f32));
        g.fillRect(0, 155, getWidth(), 2);
        g.setColour(juce::Colour(0xff1d1823));
        g.fillRoundedRectangle(sidebarBounds.toFloat(), 8.0f);
        g.setColour(juce::Colour(0xff281b2c));
        g.fillRoundedRectangle(masterBounds.toFloat(), 8.0f);
        g.setColour(kAccent);
        g.setFont(juce::Font(15.0f, juce::Font::bold));
        g.drawText("MASTER", masterBounds.getX(), masterBounds.getY() + 17,
                   masterBounds.getWidth(), 24, juce::Justification::centred);
        g.setColour(juce::Colours::lightgrey);
        g.setFont(11.0f);
        g.drawText("SAIDA", masterBounds.getX(), masterBounds.getY() + 56,
                   masterBounds.getWidth(), 20, juce::Justification::centred);
        g.drawText("REVERB", masterBounds.getX(), masterBounds.getBottom() - 181,
                   masterBounds.getWidth(), 20, juce::Justification::centred);

        const int meterBottom = masterBounds.getY() + 280;
        for (int i = 0; i < 2; ++i)
        {
            const int x = masterBounds.getX() + 16 + i * 17;
            g.setColour(juce::Colour(0xff060b0e));
            g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(x),
                                                           static_cast<float>(meterBottom - 185),
                                                           11.0f, 185.0f), 3.0f);
            const auto level = juce::jlimit(0.0f, 1.0f, engine.getMasterLevel(i));
            const auto bar = juce::roundToInt(level * 179.0f);
            g.setColour(level > 0.85f ? juce::Colours::orange : juce::Colours::seagreen);
            g.fillRect(x + 2, meterBottom - 3 - bar, 7, bar);
        }
    }

    void resized() override
    {
        const int w = getWidth();
        title.setBounds(25, 19, 380, 44);
        subtitle.setBounds(27, 62, 390, 24);
        auto actionsX = w - 650;
        saveButton.setBounds(actionsX, 30, 126, 36);
        openButton.setBounds(actionsX + 133, 30, 126, 36);
        settings.setBounds(actionsX + 266, 30, 119, 36);
        refresh.setBounds(actionsX + 392, 30, 117, 36);
        programCaption.setBounds(27, 90, 275, 18);
        for (int i = 0; i < 10; ++i)
            programButtons[static_cast<size_t>(i)]->setBounds(27 + i * 41, 113, 35, 32);
        programName.setBounds(458, 113, juce::jmax(220, w - 620), 32);

        auto area = getLocalBounds();
        area.removeFromTop(164);
        auto bottom = area.removeFromBottom(104);
        keyboard.setBounds(bottom.removeFromTop(71).reduced(22, 2));
        status.setBounds(bottom.reduced(23, 0));
        area.reduce(12, 5);
        sidebarBounds = area.removeFromLeft(188);
        auto sidebar = sidebarBounds.reduced(9);
        project.setBounds(sidebar.removeFromTop(72));
        scanLibraryButton.setBounds(sidebar.removeFromTop(30));
        sidebar.removeFromTop(5);
        customLibraryButton.setBounds(sidebar.removeFromTop(30));
        sidebar.removeFromTop(6);
        libraryList.setBounds(sidebar);
        area.removeFromLeft(8);

        masterBounds = area.removeFromRight(160);
        masterVolume.setBounds(masterBounds.getX() + 49, masterBounds.getY() + 83,
                               91, juce::jmax(175, masterBounds.getHeight() - 320));
        reverbKnob.setBounds(masterBounds.getX() + 38, masterBounds.getBottom() - 161,
                             88, 95);
        panicButton.setBounds(masterBounds.getX() + 13, masterBounds.getBottom() - 54,
                              masterBounds.getWidth() - 26, 31);
        pedalButton.setBounds(masterBounds.getX() + 12, masterBounds.getBottom() - 213,
                              masterBounds.getWidth() - 24, 28);
        area.removeFromRight(7);
        for (int i = 0; i < kSlots; ++i)
        {
            const auto remaining = kSlots - i;
            const auto width = area.getWidth() / remaining;
            strips[static_cast<size_t>(i)]->setBounds(area.removeFromLeft(width).reduced(4, 0));
        }
    }

private:
    void timerCallback() override
    {
        for (int i = 0; i < kSlots; ++i)
            strips[static_cast<size_t>(i)]->setMeter(engine.getLayerLevel(i));
        repaint(masterBounds);
    }

    void updateMaster()
    {
        const float volume = static_cast<float>(masterVolume.getValue());
        const float wet = static_cast<float>(reverbKnob.getValue());
        engine.setMaster(volume, wet);
        auto& selected = programs[static_cast<size_t>(selectedProgram)];
        selected.masterVolume = volume;
        selected.reverb = wet;
    }

    void captureActiveProgram()
    {
        auto& current = programs[static_cast<size_t>(selectedProgram)];
        current.name = programName.getText();
        current.masterVolume = static_cast<float>(masterVolume.getValue());
        current.reverb = static_cast<float>(reverbKnob.getValue());
        for (int i = 0; i < kSlots; ++i)
        {
            auto& layer = current.layers[static_cast<size_t>(i)];
            const auto previousState = layer.pluginState;
            layer = strips[static_cast<size_t>(i)]->getState();
            if (auto* plugin = engine.getPlugin(i))
            {
                juce::MemoryBlock state;
                plugin->getStateInformation(state);
                layer.pluginState = state.toBase64Encoding();
            }
            else
            {
                layer.pluginState = previousState;
            }
        }
    }

    void applyProgram(int index)
    {
        const auto& preset = programs[static_cast<size_t>(index)];
        programName.setText(preset.name, juce::dontSendNotification);
        masterVolume.setValue(preset.masterVolume, juce::dontSendNotification);
        reverbKnob.setValue(preset.reverb, juce::dontSendNotification);
        updateMaster();

        juce::StringArray problems;
        for (int i = 0; i < kSlots; ++i)
        {
            const auto& layer = preset.layers[static_cast<size_t>(i)];
            const auto pos = static_cast<size_t>(i);
            editors[pos].reset();

            if (layer.file.isEmpty())
            {
                engine.clearPlugin(i);
                loadedPaths[pos].clear();
            }
            else
            {
                if (loadedPaths[pos] != layer.file)
                {
                    juce::String error;
                    if (engine.loadVst3(i, juce::File(layer.file), error))
                        loadedPaths[pos] = layer.file;
                    else
                    {
                        engine.clearPlugin(i);
                        loadedPaths[pos].clear();
                        problems.add("Camada " + juce::String(i + 1) + ": " + error);
                    }
                }

                if (auto* plugin = engine.getPlugin(i))
                {
                    juce::MemoryBlock state;
                    if (layer.pluginState.isNotEmpty()
                        && state.fromBase64Encoding(layer.pluginState))
                        plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
                }
            }
            strips[pos]->applyState(layer);
        }
        refreshProgramButtons();
        status.setText(problems.isEmpty()
                           ? "Programa " + juce::String(index + 1) + " pronto para tocar."
                           : problems.joinIntoString(" | "),
                       juce::dontSendNotification);
    }

    void selectProgram(int index)
    {
        if (!juce::isPositiveAndBelow(index, 10) || index == selectedProgram)
            return;
        engine.panic();
        captureActiveProgram();
        selectedProgram = index;
        applyProgram(index);
    }

    void refreshProgramButtons()
    {
        for (int i = 0; i < 10; ++i)
        {
            const auto active = (i == selectedProgram);
            programButtons[static_cast<size_t>(i)]->setColour(
                juce::TextButton::buttonColourId,
                active ? kAccent : juce::Colour(0xff251b2a));
        }
    }

    void clearLayer(int index)
    {
        if (!juce::isPositiveAndBelow(index, kSlots))
            return;
        const auto pos = static_cast<size_t>(index);
        editors[pos].reset();
        engine.panic();
        engine.clearPlugin(index);
        loadedPaths[pos].clear();
        LayerPreset blank;
        strips[pos]->applyState(blank);
        status.setText("Instrumento removido da camada " + juce::String(index + 1),
                       juce::dontSendNotification);
    }

    static juce::var serializeProgram(const ProgramPreset& program)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("name", program.name);
        obj->setProperty("master", program.masterVolume);
        obj->setProperty("reverb", program.reverb);
        juce::Array<juce::var> layerArray;
        for (const auto& layer : program.layers)
        {
            auto* data = new juce::DynamicObject();
            data->setProperty("file", layer.file);
            data->setProperty("name", layer.name);
            data->setProperty("state", layer.pluginState);
            data->setProperty("volume", layer.volume);
            data->setProperty("cutoff", layer.cutoff);
            data->setProperty("mute", layer.muted);
            data->setProperty("solo", layer.solo);
            data->setProperty("transpose", layer.transpose);
            layerArray.add(juce::var(data));
        }
        obj->setProperty("layers", juce::var(layerArray));
        return juce::var(obj);
    }

    static ProgramPreset deserializeProgram(const juce::var& input, int index)
    {
        ProgramPreset result;
        result.name = "Programa " + juce::String(index + 1).paddedLeft('0', 2);
        if (auto* obj = input.getDynamicObject())
        {
            result.name = obj->getProperty("name").toString();
            result.masterVolume = static_cast<float>(double(obj->getProperty("master")));
            result.reverb = static_cast<float>(double(obj->getProperty("reverb")));
            const auto layerValue = obj->getProperty("layers");
            if (auto* layers = layerValue.getArray())
                for (int i = 0; i < juce::jmin(kSlots, layers->size()); ++i)
                {
                    const auto& value = layers->getReference(i);
                    if (auto* layer = value.getDynamicObject())
                    {
                        auto& dst = result.layers[static_cast<size_t>(i)];
                        dst.file = layer->getProperty("file").toString();
                        dst.name = layer->getProperty("name").toString();
                        dst.pluginState = layer->getProperty("state").toString();
                        dst.volume = static_cast<float>(double(layer->getProperty("volume")));
                        dst.cutoff = static_cast<float>(double(layer->getProperty("cutoff")));
                        dst.muted = bool(layer->getProperty("mute"));
                        dst.solo = bool(layer->getProperty("solo"));
                        dst.transpose = int(layer->getProperty("transpose"));
                    }
                }
        }
        return result;
    }

    void savePresetFile()
    {
        captureActiveProgram();
        auto* obj = new juce::DynamicObject();
        obj->setProperty("format", "PlayStage-2.0");
        obj->setProperty("selected", selectedProgram);
        juce::Array<juce::var> list;
        for (const auto& preset : programs)
            list.add(serializeProgram(preset));
        obj->setProperty("programs", juce::var(list));
        const auto json = juce::JSON::toString(juce::var(obj), false);

        presetChooser = std::make_unique<juce::FileChooser>(
            "Salvar banco de 10 programas PlayStage",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile("Meu_Banco_PlayStage.psstage"), "*.psstage");
        presetChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                       | juce::FileBrowserComponent::canSelectFiles,
                                   [this, json](const juce::FileChooser& selected)
        {
            auto file = selected.getResult();
            if (file.getFullPathName().isEmpty())
                return;
            if (!file.hasFileExtension(".psstage"))
                file = file.withFileExtension(".psstage");
            if (file.replaceWithText(json))
                status.setText("Banco salvo: " + file.getFullPathName(), juce::dontSendNotification);
            else
                status.setText("Nao foi possivel salvar o arquivo.", juce::dontSendNotification);
        });
    }

    void openPresetFile()
    {
        presetChooser = std::make_unique<juce::FileChooser>(
            "Abrir banco de programas PlayStage",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.psstage");
        presetChooser->launchAsync(juce::FileBrowserComponent::openMode
                                       | juce::FileBrowserComponent::canSelectFiles,
                                   [this](const juce::FileChooser& selected)
        {
            const auto file = selected.getResult();
            if (!file.existsAsFile())
                return;
            const auto content = juce::JSON::parse(file.loadFileAsString());
            auto* obj = content.getDynamicObject();
            if (obj == nullptr || obj->getProperty("format").toString() != "PlayStage-2.0")
            {
                status.setText("Arquivo .psstage invalido ou de outra versao.", juce::dontSendNotification);
                return;
            }
            const auto listValue = obj->getProperty("programs");
            auto* list = listValue.getArray();
            if (list == nullptr || list->size() != 10)
            {
                status.setText("Banco sem os 10 programas esperados.", juce::dontSendNotification);
                return;
            }

            engine.panic();
            for (int i = 0; i < 10; ++i)
                programs[static_cast<size_t>(i)] = deserializeProgram(list->getReference(i), i);
            selectedProgram = juce::jlimit(0, 9, int(obj->getProperty("selected")));
            applyProgram(selectedProgram);
            status.setText("Banco aberto: " + file.getFileName(), juce::dontSendNotification);
        });
    }

    void scanDefaultLibrary()
    {
        libraryFiles.clearQuick();
        const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
        // MainStage 3.x / Logic content locations, including Catalina-era installs.
        const juce::StringArray paths {
            "Library/Application Support/Logic/MainStage Patches",
            "Music/Audio Music Apps/Patches",
            "Music/Audio Music Apps/Channel Strip Settings",
            "Music/Audio Music Apps/Concerts" };

        for (auto& path : paths)
            indexLibraryFolder(home.getChildFile(path));

        const auto shared = juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory)
                                .getParentDirectory().getChildFile("Library/Application Support/Logic");
        indexLibraryFolder(shared.getChildFile("Sampler Instruments"));
        indexLibraryFolder(shared.getChildFile("Plug-In Settings"));
        indexLibraryFolder(shared.getChildFile("Channel Strip Settings"));
        indexLibraryFolder(shared.getChildFile("MainStage Patches"));

        libraryList.updateContent();
        libraryList.repaint();
        status.setText("Biblioteca MainStage: " + juce::String(libraryFiles.size())
                           + " itens encontrados (consulta, sem conversao para VST3).",
                       juce::dontSendNotification);
    }

    void indexLibraryFolder(const juce::File& root)
    {
        if (!root.isDirectory() || libraryFiles.size() >= 350)
            return;

        // Bounded traversal avoids enumerating gigabytes of samples at startup.
        juce::Array<juce::File> folders;
        folders.add(root);
        int inspectedFolders = 0;
        for (int i = 0; i < folders.size() && inspectedFolders < 90
                        && libraryFiles.size() < 350; ++i)
        {
            const auto dir = folders.getReference(i);
            ++inspectedFolders;
            juce::DirectoryIterator iterator(dir, false, "*", juce::File::findFilesAndDirectories);
            while (iterator.next())
            {
                const auto item = iterator.getFile();
                const auto ext = item.getFileExtension().toLowerCase();
                if (ext == ".patch" || ext == ".concert" || ext == ".cst"
                    || ext == ".exs" || ext == ".pst" || ext == ".aupreset")
                {
                    libraryFiles.addIfNotAlreadyThere(item);
                    if (libraryFiles.size() >= 350)
                        break;
                }
                else if (item.isDirectory() && folders.size() < 90)
                {
                    folders.add(item);
                }
            }
        }
    }

    void chooseLibraryFolder()
    {
        libraryChooser = std::make_unique<juce::FileChooser>(
            "Escolha a pasta com projetos ou presets do MainStage",
            juce::File::getSpecialLocation(juce::File::userHomeDirectory));
        libraryChooser->launchAsync(juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectDirectories,
                                    [this](const juce::FileChooser& chooserResult)
        {
            const auto folder = chooserResult.getResult();
            if (!folder.isDirectory())
                return;
            indexLibraryFolder(folder);
            libraryList.updateContent();
            libraryList.repaint();
            status.setText("Biblioteca indexada: " + folder.getFullPathName(),
                           juce::dontSendNotification);
        });
    }

    void handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity) override
    {
        engine.queueMidi(juce::MidiMessage::noteOn(channel, note, velocity));
    }

    void handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float velocity) override
    {
        engine.queueMidi(juce::MidiMessage::noteOff(channel, note, velocity));
    }

    void pickVst3(int index)
    {
        chooser = std::make_unique<juce::FileChooser>(
            "Selecione um plugin VST3 para macOS",
            juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                .getChildFile("Library/Audio/Plug-Ins/VST3"),
            "*.vst3");

        const auto flags = juce::FileBrowserComponent::openMode
                         | juce::FileBrowserComponent::canSelectFiles
                         | juce::FileBrowserComponent::canSelectDirectories;
        chooser->launchAsync(flags, [this, index](const juce::FileChooser& selected)
        {
            const auto file = selected.getResult();
            if (!file.exists())
                return;

            editors[static_cast<size_t>(index)].reset();
            juce::String error;
            if (!engine.loadVst3(index, file, error))
            {
                status.setText("Falha ao abrir: " + error, juce::dontSendNotification);
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                        "Plugin VST3", error);
                return;
            }

            if (auto* plugin = engine.getPlugin(index))
            {
                loadedPaths[static_cast<size_t>(index)] = file.getFullPathName();
                strips[static_cast<size_t>(index)]->setPlugin(plugin->getName(), file.getFullPathName());
                programs[static_cast<size_t>(selectedProgram)].layers[static_cast<size_t>(index)] =
                    strips[static_cast<size_t>(index)]->getState();
                status.setText("Carregado: " + plugin->getName(), juce::dontSendNotification);
            }
        });
    }

    void openEditor(int index)
    {
        auto* plugin = engine.getPlugin(index);
        if (plugin == nullptr)
            return;
        auto& window = editors[static_cast<size_t>(index)];
        if (!window)
            window = std::make_unique<PluginEditorWindow>(*plugin);
        window->setVisible(true);
        window->toFront(true);
    }

    void showDeviceSettings()
    {
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "CoreAudio e MIDI";
        options.dialogBackgroundColour = kPanel;
        options.content.setOwned(new juce::AudioDeviceSelectorComponent(
            engine.getDeviceManager(), 0, 0, 0, 2, true, false, true, false));
        options.content->setSize(520, 390);
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    }

    HostEngine engine;
    juce::Label title, subtitle, programCaption, project, status;
    juce::TextEditor programName;
    juce::TextButton settings, refresh, scanLibraryButton, customLibraryButton;
    juce::TextButton saveButton, openButton, panicButton, pedalButton;
    juce::Slider masterVolume, reverbKnob;
    juce::Rectangle<int> sidebarBounds, masterBounds;
    std::array<ProgramPreset, 10> programs;
    std::array<std::unique_ptr<juce::TextButton>, 10> programButtons;
    std::array<juce::String, kSlots> loadedPaths;
    int selectedProgram = 0;
    juce::Array<juce::File> libraryFiles;
    juce::ListBox libraryList;
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard;
    std::array<std::unique_ptr<ChannelStrip>, kSlots> strips;
    std::array<std::unique_ptr<PluginEditorWindow>, kSlots> editors;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::FileChooser> libraryChooser;
    std::unique_ptr<juce::FileChooser> presetChooser;
};

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow() : juce::DocumentWindow("PlayStage VST3 Catalina", kBackground,
                                       juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(), true);
        setResizable(true, true);
        setResizeLimits(1180, 700, 2400, 1500);
        centreWithSize(1320, 790);
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class PlayStageApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "PlayStage VST3 Catalina"; }
    const juce::String getApplicationVersion() override { return "0.2.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override
    {
        lookAndFeel.setColour(juce::TextButton::buttonColourId, kPanel.brighter(0.1f));
        lookAndFeel.setColour(juce::Slider::thumbColourId, kAccent);
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
        window = std::make_unique<MainWindow>();
    }
    void shutdown() override
    {
        window.reset();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String&) override {}

private:
    juce::LookAndFeel_V4 lookAndFeel;
    std::unique_ptr<MainWindow> window;
};
} // namespace

START_JUCE_APPLICATION(PlayStageApplication)
