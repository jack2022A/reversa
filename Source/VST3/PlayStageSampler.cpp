#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>
#include <cmath>

namespace {
constexpr int numLayers = 4;
constexpr int maxZones = 256;
const auto navy = juce::Colour(0xff0d2541);
const auto panel = juce::Colour(0xff183b63);
const auto pale = juce::Colour(0xffdeefff);
const auto cyan = juce::Colour(0xff6cc6ff);

class RegionSound final : public juce::SamplerSound {
public:
    RegionSound(const juce::String& name, juce::AudioFormatReader& reader,
                const juce::BigInteger& notes, int root, int velMin, int velMax, double seconds)
        : juce::SamplerSound(name, reader, notes, root, 0.005, 0.4, seconds),
          low(velMin), high(velMax) {}
    bool matchesVelocity(int velocity) const { return velocity >= low && velocity <= high; }
private:
    int low, high;
};

// The default JUCE sampler checks key zones only. This subclass additionally
// selects samples by MIDI velocity, preserving the Ableton-to-.psmap mappings.
class VelocitySynth final : public juce::Synthesiser {
public:
    void noteOn(int channel, int note, float velocity) override {
        const juce::ScopedLock guard(lock);
        const int vel = juce::jlimit(1, 127, juce::roundToInt(velocity * 127.0f));
        for (int i = 0; i < getNumSounds(); ++i) {
            auto sound = getSound(i);
            auto* region = static_cast<RegionSound*>(sound.get());
            if (region == nullptr || !region->appliesToNote(note)
                || !region->appliesToChannel(channel) || !region->matchesVelocity(vel))
                continue;
            if (auto* voice = findFreeVoice(region, channel, note, isNoteStealingEnabled()))
                startVoice(voice, region, channel, note, velocity);
        }
    }
};

struct Bank {
    VelocitySynth synth;
    juce::String name;
    explicit Bank(juce::String n) : name(std::move(n)) {
        for (int i = 0; i < 48; ++i)
            synth.addVoice(new juce::SamplerVoice());
        synth.setNoteStealingEnabled(true);
    }
};

struct LayerParams {
    std::atomic<float> volume { 0.8f };
    std::atomic<int> noteLow { 0 }, noteHigh { 127 }, midiChannel { 0 }, transpose { 0 };
    std::atomic<bool> muted { false }, solo { false };
};
}

class PlayStageSampler final : public juce::AudioProcessor, private juce::AsyncUpdater {
public:
    PlayStageSampler()
        : juce::AudioProcessor(BusesProperties()
              .withOutput("Stereo Out", juce::AudioChannelSet::stereo(), true)) {
        formats.registerBasicFormats();
    }
    ~PlayStageSampler() override { cancelPendingUpdate(); }

    const juce::String getName() const override { return "PlayStage Sampler"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    bool hasEditor() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout& b) const override {
        return b.getMainInputChannelSet().isDisabled()
            && b.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Stage"; }
    void changeProgramName(int, const juce::String&) override {}

    void prepareToPlay(double sr, int block) override {
        rate.store(sr);
        scratch.setSize(2, juce::jmax(2048, block), false, false, true);
        for (auto& midi : midiBuffers) midi.ensureSize(8192);
        for (auto& p : banks) {
            auto bank = std::atomic_load(&p);
            if (bank) bank->synth.setCurrentPlaybackSampleRate(sr);
        }
        reverb.setSampleRate(sr);
        reverb.reset();
        lastReverb = -1.0f;
    }
    void releaseResources() override { reverb.reset(); }

    void processBlock(juce::AudioBuffer<float>& output, juce::MidiBuffer& midi) override {
        juce::ScopedNoDenormals noDenormals;
        const auto n = output.getNumSamples();
        output.clear();
        if (n <= 0 || output.getNumChannels() < 2) return;
        if (scratch.getNumSamples() < n)
            scratch.setSize(2, n, false, false, true); // host changed block size

        bool anySolo = false;
        for (const auto& l : layers) anySolo |= l.solo.load();
        for (int layer = 0; layer < numLayers; ++layer) {
            const auto& p = layers[(size_t) layer];
            if (p.muted.load() || (anySolo && !p.solo.load())) continue;
            auto bank = std::atomic_load(&banks[(size_t) layer]);
            if (!bank) continue;

            auto& events = midiBuffers[(size_t) layer];
            events.clear();
            for (const auto metadata : midi) {
                auto m = metadata.getMessage();
                if (p.midiChannel.load() != 0 && m.getChannel() > 0
                    && m.getChannel() != p.midiChannel.load()) continue;
                if (m.isNoteOnOrOff()) {
                    const int note = m.getNoteNumber();
                    if (note < p.noteLow.load() || note > p.noteHigh.load()) continue;
                    m.setNoteNumber(juce::jlimit(0, 127, note + p.transpose.load()));
                }
                events.addEvent(m, metadata.samplePosition);
            }
            scratch.clear();
            bank->synth.renderNextBlock(scratch, events, 0, n);
            const auto gain = p.volume.load();
            output.addFrom(0, 0, scratch, 0, 0, n, gain);
            output.addFrom(1, 0, scratch, 1, 0, n, gain);
        }
        const auto wet = reverbAmount.load();
        if (wet != lastReverb) {
            juce::Reverb::Parameters rp;
            rp.roomSize = 0.5f; rp.damping = 0.45f; rp.width = 1.0f;
            rp.wetLevel = wet; rp.dryLevel = 1.0f;
            reverb.setParameters(rp);
            lastReverb = wet;
        }
        reverb.processStereo(output.getWritePointer(0), output.getWritePointer(1), n);
        output.applyGain(masterVolume.load());
        peak.store(juce::jmax(output.getMagnitude(0, 0, n),
                              output.getMagnitude(1, 0, n)));
    }

    bool loadLayer(int index, const juce::File& source, juce::String& error) {
        if (!juce::isPositiveAndBelow(index, numLayers)) {
            error = "Camada invalida."; return false;
        }
        if (!source.existsAsFile()) {
            error = "Arquivo nao encontrado."; return false;
        }
        const bool map = source.hasFileExtension(".psmap");
        if (!map && !source.hasFileExtension(".wav;.aif;.aiff")) {
            error = "Selecione .psmap, WAV ou AIFF."; return false;
        }
        auto bank = std::make_shared<Bank>(source.getFileNameWithoutExtension());
        if (map) {
            if (!loadMap(*bank, source, error)) return false;
        } else {
            if (!addRegion(*bank, source, 60, 0, 127, 1, 127, error))
                return false;
        }
        bank->synth.setCurrentPlaybackSampleRate(rate.load());
        std::atomic_store(&banks[(size_t) index], bank);
        {
            const juce::ScopedLock lock(stateMutex);
            layerFiles[(size_t) index] = source;
            layerNames[(size_t) index] = bank->name;
        }
        return true;
    }

    juce::String getLayerName(int index) const {
        if (!juce::isPositiveAndBelow(index, numLayers)) return {};
        const juce::ScopedLock lock(stateMutex);
        return layerNames[(size_t) index].isEmpty() ? "Sem instrumento" : layerNames[(size_t) index];
    }
    void setVolume(int i, float v) { if (juce::isPositiveAndBelow(i,numLayers)) layers[(size_t)i].volume.store(juce::jlimit(0.f,1.5f,v)); }
    float getVolume(int i) const { return layers[(size_t)i].volume.load(); }
    void setMute(int i,bool v) { if (juce::isPositiveAndBelow(i,numLayers)) layers[(size_t)i].muted.store(v); }
    bool getMute(int i) const { return layers[(size_t)i].muted.load(); }
    void setSolo(int i,bool v) { if (juce::isPositiveAndBelow(i,numLayers)) layers[(size_t)i].solo.store(v); }
    bool getSolo(int i) const { return layers[(size_t)i].solo.load(); }
    void setTranspose(int i,int v) { if(juce::isPositiveAndBelow(i,numLayers)) layers[(size_t)i].transpose.store(juce::jlimit(-24,24,v)); }
    int getTranspose(int i) const { return layers[(size_t)i].transpose.load(); }
    void setMaster(float v) { masterVolume.store(juce::jlimit(0.f,1.5f,v)); }
    float getMaster() const { return masterVolume.load(); }
    void setReverb(float v) { reverbAmount.store(juce::jlimit(0.f,0.6f,v)); }
    float getReverb() const { return reverbAmount.load(); }
    float getPeak() const { return peak.load(); }

    void getStateInformation(juce::MemoryBlock& dest) override {
        juce::XmlElement root("PlayStageSampler");
        root.setAttribute("master", double(getMaster()));
        root.setAttribute("reverb", double(getReverb()));
        const juce::ScopedLock lock(stateMutex);
        for (int i=0;i<numLayers;++i) {
            auto* layer = root.createNewChildElement("Layer");
            layer->setAttribute("index",i);
            layer->setAttribute("path",layerFiles[(size_t)i].getFullPathName());
            layer->setAttribute("volume",double(getVolume(i)));
            layer->setAttribute("mute",getMute(i));
            layer->setAttribute("solo",getSolo(i));
            layer->setAttribute("transpose",getTranspose(i));
            layer->setAttribute("low",layers[(size_t)i].noteLow.load());
            layer->setAttribute("high",layers[(size_t)i].noteHigh.load());
            layer->setAttribute("channel",layers[(size_t)i].midiChannel.load());
        }
        juce::copyXmlToBinary(root,dest);
    }
    void setStateInformation(const void* data,int bytes) override {
        const auto xml=juce::getXmlFromBinary(data,bytes);
        if (!xml || !xml->hasTagName("PlayStageSampler")) return;
        setMaster(float(xml->getDoubleAttribute("master",0.85)));
        setReverb(float(xml->getDoubleAttribute("reverb",0.1)));
        {
            const juce::ScopedLock lock(stateMutex);
            for (auto* child : xml->getChildIterator()) {
                if (!child->hasTagName("Layer")) continue;
                const int i=child->getIntAttribute("index",-1);
                if (!juce::isPositiveAndBelow(i,numLayers)) continue;
                auto& p=layers[(size_t)i];
                layerFiles[(size_t)i]=juce::File(child->getStringAttribute("path"));
                p.volume.store(float(child->getDoubleAttribute("volume",0.8)));
                p.muted.store(child->getBoolAttribute("mute",false));
                p.solo.store(child->getBoolAttribute("solo",false));
                p.transpose.store(child->getIntAttribute("transpose",0));
                p.noteLow.store(child->getIntAttribute("low",0));
                p.noteHigh.store(child->getIntAttribute("high",127));
                p.midiChannel.store(child->getIntAttribute("channel",0));
            }
        }
        // Never decode file samples in a DAW's state-restore/audio callback.
        triggerAsyncUpdate();
    }

    juce::AudioProcessorEditor* createEditor() override;

private:
    bool addRegion(Bank& bank,const juce::File& f,int root,int lo,int hi,
                   int velLo,int velHi,juce::String& error) {
        if (root<0||root>127||lo<0||hi>127||lo>hi
            ||velLo<1||velHi>127||velLo>velHi) {
            error="Zona MIDI invalida: "+f.getFileName(); return false;
        }
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(f));
        if (!reader || reader->lengthInSamples<=0 || reader->sampleRate<=0) {
            error="Nao foi possivel ler o audio: "+f.getFileName(); return false;
        }
        juce::BigInteger keys;
        keys.setRange(lo,hi-lo+1,true);
        const auto seconds=juce::jmin(180.0,
            double(reader->lengthInSamples)/reader->sampleRate+0.5);
        bank.synth.addSound(new RegionSound(f.getFileNameWithoutExtension(),*reader,
                                            keys,root,velLo,velHi,seconds));
        return true;
    }

    bool loadMap(Bank& bank,const juce::File& mapFile,juce::String& error) {
        if (mapFile.getSize()>2*1024*1024) { error="Mapa maior que 2 MB.";return false; }
        auto json=juce::JSON::parse(mapFile.loadFileAsString());
        auto* root=json.getDynamicObject();
        if (!root || root->getProperty("schema").toString()!="playstage.multisample/1") {
            error="Formato .psmap nao suportado.";return false;
        }
        const juce::var sampleValue=root->getProperty("samples");
        const auto* items=sampleValue.getArray();
        if (!items || items->isEmpty() || items->size()>maxZones) {
            error="Mapa vazio ou mais de 256 regioes.";return false;
        }
        const auto base=mapFile.getParentDirectory();
        juce::int64 size=0;
        for (const auto& item:*items) {
            auto* obj=item.getDynamicObject();
            if (!obj) { error="Regiao do mapa invalida.";return false; }
            const auto relative=obj->getProperty("file").toString().replaceCharacter('\\','/');
            const auto file=base.getChildFile(relative);
            if (relative.isEmpty()||relative.startsWithChar('/')||relative.contains(":")
                ||relative.contains("../")||relative==".."||!file.isAChildOf(base)
                ||!file.existsAsFile()) {
                error="Sample ausente ou caminho inseguro: "+relative;return false;
            }
            if (!file.hasFileExtension(".wav;.aif;.aiff")) {
                error="Formato de sample nao suportado: "+relative;return false;
            }
            size+=file.getSize();
            if (size>750LL*1024*1024) { error="Banco acima de 750 MB.";return false; }
        }
        const auto n=root->getProperty("name").toString().trim();
        if (n.isNotEmpty()) bank.name=n;
        for (const auto& item:*items) {
            auto* obj=item.getDynamicObject();
            const auto file=base.getChildFile(obj->getProperty("file").toString());
            if (!addRegion(bank,file,int(obj->getProperty("root")),
                           int(obj->getProperty("lo")),int(obj->getProperty("hi")),
                           int(obj->getProperty("velLo")),int(obj->getProperty("velHi")),error))
                return false;
        }
        return true;
    }
    void handleAsyncUpdate() override {
        std::array<juce::File,numLayers> files;
        { const juce::ScopedLock lock(stateMutex); files=layerFiles; }
        for (int i=0;i<numLayers;++i) {
            if (files[(size_t)i].existsAsFile()) {
                juce::String error;
                loadLayer(i,files[(size_t)i],error);
            }
        }
    }

    juce::AudioFormatManager formats;
    std::array<std::shared_ptr<Bank>,numLayers> banks;
    std::array<LayerParams,numLayers> layers;
    mutable juce::CriticalSection stateMutex;
    std::array<juce::File,numLayers> layerFiles;
    std::array<juce::String,numLayers> layerNames;
    std::array<juce::MidiBuffer,numLayers> midiBuffers;
    juce::AudioBuffer<float> scratch;
    juce::Reverb reverb;
    float lastReverb=-1.0f;
    std::atomic<double> rate {44100.0};
    std::atomic<float> masterVolume {0.85f}, reverbAmount {0.10f}, peak {0.0f};
};

class StageEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit StageEditor(PlayStageSampler& processor)
      : juce::AudioProcessorEditor(&processor), engine(processor) {
        setSize(970,625);
        header.setText("PLAYSTAGE  |  SAMPLER VST3",juce::dontSendNotification);
        header.setColour(juce::Label::textColourId,juce::Colours::white);
        header.setFont(juce::Font(23.0f,juce::Font::bold));
        addAndMakeVisible(header);
        subtitle.setText("4 CAMADAS  /  MAPAS ABLETON .PSMAP  /  MIDI / MAC + WINDOWS",
                         juce::dontSendNotification);
        subtitle.setColour(juce::Label::textColourId,pale);
        addAndMakeVisible(subtitle);
        for (int i=0;i<numLayers;++i) {
            labels[(size_t)i].setColour(juce::Label::textColourId,juce::Colours::white);
            addAndMakeVisible(labels[(size_t)i]);
            loadButtons[(size_t)i].setButtonText("CARREGAR");
            loadButtons[(size_t)i].onClick=[this,i]{ choose(i); };
            addAndMakeVisible(loadButtons[(size_t)i]);
            muteButtons[(size_t)i].setButtonText("M");
            muteButtons[(size_t)i].setClickingTogglesState(true);
            muteButtons[(size_t)i].setToggleState(engine.getMute(i),juce::dontSendNotification);
            muteButtons[(size_t)i].onClick=[this,i]{ engine.setMute(i,muteButtons[(size_t)i].getToggleState()); };
            addAndMakeVisible(muteButtons[(size_t)i]);
            soloButtons[(size_t)i].setButtonText("S");
            soloButtons[(size_t)i].setClickingTogglesState(true);
            soloButtons[(size_t)i].setToggleState(engine.getSolo(i),juce::dontSendNotification);
            soloButtons[(size_t)i].onClick=[this,i]{ engine.setSolo(i,soloButtons[(size_t)i].getToggleState()); };
            addAndMakeVisible(soloButtons[(size_t)i]);
            initSlider(gains[(size_t)i],0.0,1.5,engine.getVolume(i));
            gains[(size_t)i].onValueChange=[this,i] {engine.setVolume(i,float(gains[(size_t)i].getValue()));};
            initSlider(transposes[(size_t)i],-24,24,engine.getTranspose(i));
            transposes[(size_t)i].setRange(-24,24,1);
            transposes[(size_t)i].onValueChange=[this,i] {engine.setTranspose(i,int(transposes[(size_t)i].getValue()));};
            addAndMakeVisible(gains[(size_t)i]);
            addAndMakeVisible(transposes[(size_t)i]);
        }
        initSlider(master,0.0,1.5,engine.getMaster());
        master.onValueChange=[this]{engine.setMaster(float(master.getValue()));};
        addAndMakeVisible(master);
        initSlider(reverb,0.0,0.6,engine.getReverb());
        reverb.onValueChange=[this]{engine.setReverb(float(reverb.getValue()));};
        addAndMakeVisible(reverb);
        masterLabel.setText("MASTER",juce::dontSendNotification);
        reverbLabel.setText("REVERB",juce::dontSendNotification);
        meter.setText("SAIDA: 0.00",juce::dontSendNotification);
        for (auto* l:{&masterLabel,&reverbLabel,&meter}) {
            l->setColour(juce::Label::textColourId,juce::Colours::white);
            addAndMakeVisible(*l);
        }
        message.setText("Selecione WAV, AIFF ou mapa .psmap. As amostras ficam no seu disco.",
                        juce::dontSendNotification);
        message.setColour(juce::Label::textColourId,pale);
        addAndMakeVisible(message);
        startTimerHz(4);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(navy);
        g.setColour(cyan);
        g.fillRect(0,0,getWidth(),5);
        for (int i=0;i<numLayers;++i) {
            const int y=111+i*99;
            g.setColour(i%2?juce::Colour(0xff21486e):panel);
            g.fillRoundedRectangle(16.0f,float(y),float(getWidth()-32),90.0f,10.0f);
            g.setColour(cyan.withAlpha(0.6f));
            g.fillRoundedRectangle(16.0f,float(y),5.0f,90.0f,2.0f);
        }
        g.setColour(panel);
        g.fillRoundedRectangle(16.0f,520.0f,float(getWidth()-32),88.0f,10.0f);
        g.setColour(pale);
        g.setFont(12.0f);
        for (int i=0;i<numLayers;++i) {
            const int y=111+i*99;
            g.drawText("VOL",490,y+16,65,20,juce::Justification::centredLeft);
            g.drawText("TRANSPOSE",700,y+16,110,20,juce::Justification::centredLeft);
        }
    }
    void resized() override {
        header.setBounds(20,18,660,34);
        subtitle.setBounds(21,56,810,24);
        for (int i=0;i<numLayers;++i) {
            const int y=111+i*99;
            labels[(size_t)i].setBounds(31,y+16,290,50);
            loadButtons[(size_t)i].setBounds(327,y+26,105,36);
            muteButtons[(size_t)i].setBounds(441,y+26,38,36);
            gains[(size_t)i].setBounds(495,y+37,182,35);
            transposes[(size_t)i].setBounds(702,y+37,132,35);
            soloButtons[(size_t)i].setBounds(866,y+26,50,36);
        }
        masterLabel.setBounds(31,536,85,24);
        master.setBounds(115,538,225,32);
        reverbLabel.setBounds(350,536,75,24);
        reverb.setBounds(426,538,186,32);
        meter.setBounds(638,534,260,28);
        message.setBounds(28,574,875,22);
    }
private:
    void initSlider(juce::Slider& s,double min,double max,double value) {
        s.setRange(min,max,0.01);
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight,false,58,23);
        s.setValue(value,juce::dontSendNotification);
        s.setColour(juce::Slider::thumbColourId,cyan);
        s.setColour(juce::Slider::trackColourId,juce::Colour(0xff438ac5));
        s.setColour(juce::Slider::textBoxTextColourId,juce::Colours::white);
    }
    void choose(int i) {
        chooser=std::make_unique<juce::FileChooser>("Carregar mapa ou sample",
                    juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                    "*.psmap;*.wav;*.aif;*.aiff");
        chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectFiles,
                             [this,i](const juce::FileChooser& fc) {
            const auto f=fc.getResult();
            if (!f.existsAsFile()) return;
            juce::String error;
            if (engine.loadLayer(i,f,error))
                message.setText("Camada "+juce::String(i+1)+" carregada: "+f.getFileName(),
                                juce::dontSendNotification);
            else
                message.setText("Erro: "+error,juce::dontSendNotification);
            refresh();
        });
    }
    void timerCallback() override { refresh(); }
    void refresh() {
        for (int i=0;i<numLayers;++i)
            labels[(size_t)i].setText("0"+juce::String(i+1)+"  |  "+
               engine.getLayerName(i),juce::dontSendNotification);
        meter.setText("SAIDA: "+juce::String(engine.getPeak(),3),
                      juce::dontSendNotification);
    }
    PlayStageSampler& engine;
    juce::Label header,subtitle,message,masterLabel,reverbLabel,meter;
    std::array<juce::Label,numLayers> labels;
    std::array<juce::TextButton,numLayers> loadButtons,muteButtons,soloButtons;
    std::array<juce::Slider,numLayers> gains,transposes;
    juce::Slider master,reverb;
    std::unique_ptr<juce::FileChooser> chooser;
};
juce::AudioProcessorEditor* PlayStageSampler::createEditor() {
    return new StageEditor(*this);
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new PlayStageSampler();
}
