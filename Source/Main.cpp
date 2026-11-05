#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace
{
constexpr int kSlots = 4;
const juce::Colour kBackground = juce::Colour(0xff1c2027);
const juce::Colour kPanel = juce::Colour(0xff303741);
const juce::Colour kAccent = juce::Colour(0xff4798db);

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
    const juce::String getName() const override { return "Volume PlayStage"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override
    {
        audio.applyGain(gain.load());
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

        for (int i = 0; i < kSlots; ++i)
        {
            auto volume = std::make_unique<VolumeProcessor>();
            volumes[static_cast<size_t>(i)] = volume.get();
            volumeNodes[static_cast<size_t>(i)] = graph->addNode(std::move(volume));
            connectVolumeToOutput(i);
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

    void setGain(int index, float volume, bool muted)
    {
        if (juce::isPositiveAndBelow(index, kSlots))
            volumes[static_cast<size_t>(index)]->setLinearGain(muted ? 0.0f : volume);
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
        if (auto old = graph->getNodeForId(pluginIds[static_cast<size_t>(index)]))
            graph->removeNode(old->nodeID);

        auto newNode = graph->addNode(std::move(instance));
        if (!newNode)
        {
            error = "Nao foi possivel adicionar o plugin ao motor de audio.";
            return false;
        }
        pluginIds[static_cast<size_t>(index)] = newNode->nodeID;

        if (newNode->getProcessor()->acceptsMidi()
            && !graph->addConnection({ { midiInput->nodeID, juce::AudioProcessorGraph::midiChannelIndex },
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
            return false;
        }

        return true;
    }

private:
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
        graph->addConnection({ { id, 0 }, { audioOutput->nodeID, 0 } });
        graph->addConnection({ { id, 1 }, { audioOutput->nodeID, 1 } });
    }

    juce::AudioDeviceManager deviceManager;
    juce::AudioProcessorPlayer player;
    juce::AudioPluginFormatManager pluginFormats;
    std::unique_ptr<juce::AudioProcessorGraph> graph;
    juce::AudioProcessorGraph::Node::Ptr audioOutput;
    juce::AudioProcessorGraph::Node::Ptr midiInput;
    std::array<juce::AudioProcessorGraph::Node::Ptr, kSlots> volumeNodes;
    std::array<juce::AudioProcessorGraph::NodeID, kSlots> pluginIds {};
    std::array<VolumeProcessor*, kSlots> volumes {};
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
    ChannelStrip(int channelNumber, juce::String preset,
                 std::function<void(int)> load,
                 std::function<void(int)> edit,
                 std::function<void(int, float, bool)> gainChanged)
        : channel(channelNumber), onLoad(std::move(load)), onEdit(std::move(edit)),
          onGainChanged(std::move(gainChanged))
    {
        channelLabel.setText("CH" + juce::String(channelNumber + 1), juce::dontSendNotification);
        channelLabel.setFont(juce::Font(19.0f, juce::Font::bold));
        channelLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(channelLabel);

        instrument.setText(std::move(preset), juce::dontSendNotification);
        instrument.setJustificationType(juce::Justification::centred);
        instrument.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(instrument);

        loadButton.setButtonText("Abrir VST3");
        loadButton.onClick = [this] { onLoad(channel); };
        addAndMakeVisible(loadButton);

        editButton.setButtonText("Editor");
        editButton.onClick = [this] { onEdit(channel); };
        editButton.setEnabled(false);
        addAndMakeVisible(editButton);

        volume.setSliderStyle(juce::Slider::LinearVertical);
        volume.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 22);
        volume.setRange(0.0, 1.5, 0.01);
        volume.setValue(0.8, juce::dontSendNotification);
        volume.onValueChange = [this] { notifyGain(); };
        addAndMakeVisible(volume);

        mute.setButtonText("Mudo");
        mute.onClick = [this] { notifyGain(); };
        addAndMakeVisible(mute);
    }

    void setPluginName(const juce::String& name)
    {
        instrument.setText(name, juce::dontSendNotification);
        editButton.setEnabled(true);
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(kPanel);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 7.0f);
        g.setColour(kAccent.withAlpha(0.7f));
        g.fillRect(0, 0, getWidth(), 3);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10);
        channelLabel.setBounds(area.removeFromTop(33));
        instrument.setBounds(area.removeFromTop(42));
        area.removeFromTop(10);
        loadButton.setBounds(area.removeFromTop(32));
        area.removeFromTop(8);
        editButton.setBounds(area.removeFromTop(30));
        area.removeFromTop(10);
        mute.setBounds(area.removeFromBottom(32));
        volume.setBounds(area);
    }

private:
    void notifyGain() { onGainChanged(channel, static_cast<float>(volume.getValue()), mute.getToggleState()); }

    int channel = 0;
    std::function<void(int)> onLoad;
    std::function<void(int)> onEdit;
    std::function<void(int, float, bool)> onGainChanged;
    juce::Label channelLabel, instrument;
    juce::TextButton loadButton, editButton;
    juce::Slider volume;
    juce::ToggleButton mute;
};

class MainComponent final : public juce::Component,
                            public juce::ListBoxModel,
                            private juce::MidiKeyboardStateListener
{
public:
    MainComponent() : keyboard(keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard),
                      libraryList("Biblioteca MainStage", this)
    {
        title.setText("PlayStage VST3 • Catalina", juce::dontSendNotification);
        title.setFont(juce::Font(25.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(title);

        project.setText("Projeto XP 10\n> Untitled Patch\n\nBiblioteca do Mac",
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

        status.setText(engine.getLastError().isEmpty()
                           ? "Pronto. Abra um instrumento .vst3 em qualquer canal."
                           : engine.getLastError(), juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(status);

        const juce::StringArray names { "Worship Strings", "Big Bright Pad", "Instrumento 3", "Instrumento 4" };
        for (int i = 0; i < kSlots; ++i)
        {
            auto strip = std::make_unique<ChannelStrip>(
                i, names[i],
                [this](int slot) { pickVst3(slot); },
                [this](int slot) { openEditor(slot); },
                [this](int slot, float gain, bool muted) { engine.setGain(slot, gain, muted); });
            addAndMakeVisible(*strip);
            strips[static_cast<size_t>(i)] = std::move(strip);
        }

        keyboard.setAvailableRange(36, 96);
        keyboardState.addListener(this);
        addAndMakeVisible(keyboard);
        setSize(1120, 680);
        scanDefaultLibrary();
    }

    ~MainComponent() override
    {
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
        g.setColour(juce::Colour(0xff242b34));
        g.fillRoundedRectangle(juce::Rectangle<float>(12, 90, 200, static_cast<float>(getHeight() - 205)), 7.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);
        auto top = area.removeFromTop(68);
        title.setBounds(top.removeFromLeft(430));
        settings.setBounds(top.removeFromRight(140).reduced(4, 10));
        refresh.setBounds(top.removeFromRight(150).reduced(4, 10));
        area.removeFromTop(10);
        auto bottom = area.removeFromBottom(110);
        keyboard.setBounds(bottom.removeFromTop(78).reduced(10, 0));
        status.setBounds(bottom.reduced(10, 0));
        auto sidebar = area.removeFromLeft(200).reduced(10);
        project.setBounds(sidebar.removeFromTop(95));
        scanLibraryButton.setBounds(sidebar.removeFromTop(30));
        sidebar.removeFromTop(5);
        customLibraryButton.setBounds(sidebar.removeFromTop(30));
        sidebar.removeFromTop(6);
        libraryList.setBounds(sidebar);
        area.removeFromLeft(10);
        for (int i = 0; i < kSlots; ++i)
        {
            const auto remaining = kSlots - i;
            const auto width = area.getWidth() / remaining;
            strips[static_cast<size_t>(i)]->setBounds(area.removeFromLeft(width).reduced(4, 0));
        }
    }

private:
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
                strips[static_cast<size_t>(index)]->setPluginName(plugin->getName());
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
            engine.getDeviceManager(), 0, 0, 0, 2, false, true, true, false));
        options.content->setSize(520, 390);
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    }

    HostEngine engine;
    juce::Label title, project, status;
    juce::TextButton settings, refresh, scanLibraryButton, customLibraryButton;
    juce::Array<juce::File> libraryFiles;
    juce::ListBox libraryList;
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard;
    std::array<std::unique_ptr<ChannelStrip>, kSlots> strips;
    std::array<std::unique_ptr<PluginEditorWindow>, kSlots> editors;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::FileChooser> libraryChooser;
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
        centreWithSize(1120, 680);
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class PlayStageApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "PlayStage VST3 Catalina"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
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
