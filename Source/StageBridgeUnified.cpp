#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>
#include <cmath>

namespace {
constexpr int numLayers = 5;
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
    std::atomic<bool> muted { false }, solo { false }, midiEnabled { true }, flushNotes { false };
    std::atomic<float> tone { 1.0f };
};
}

class StageBridgeUnified final : public juce::AudioProcessor, private juce::AsyncUpdater {
public:
    StageBridgeUnified()
        : juce::AudioProcessor(BusesProperties()
              .withOutput("Stereo Out", juce::AudioChannelSet::stereo(), true)) {
        formats.registerBasicFormats();
    }
    ~StageBridgeUnified() override { cancelPendingUpdate(); }

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
        scratch.setSize(2, juce::jmax(65536, block), false, false, true);
        for (auto& midi : midiBuffers) midi.ensureSize(8192);
        for (auto& states : toneStates) states = { 0.0f, 0.0f };
        for (auto& p : banks) {
            auto bank = std::atomic_load(&p);
            if (bank) bank->synth.setCurrentPlaybackSampleRate(sr);
        }
        reverb.setSampleRate(sr);
        reverb.reset();
        lastReverb = -1.0f;
    }
    void releaseResources() override { reverb.reset(); padEnvelope = 0.0f; }

    void processBlock(juce::AudioBuffer<float>& output, juce::MidiBuffer& midi) override {
        juce::ScopedNoDenormals noDenormals;
        const auto n = output.getNumSamples();
        output.clear();
        if (n <= 0 || output.getNumChannels() < 2) return;
        if (scratch.getNumSamples() < n)
            return; // No audio-thread allocation; pathological host buffer rejected.

        bool anySolo = false;
        for (const auto& l : layers) anySolo |= l.solo.load();
        for (int layer = 0; layer < numLayers; ++layer) {
            auto& p = layers[(size_t) layer];
            auto bank = std::atomic_load(&banks[(size_t) layer]);
            if (!bank) continue;
            if (p.flushNotes.exchange(false))
                bank->synth.allNotesOff(0, false);

            auto& events = midiBuffers[(size_t) layer];
            events.clear();
            if (p.midiEnabled.load()) for (const auto metadata : midi) {
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
            const float tone = juce::jlimit(0.0f, 1.0f, p.tone.load());
            const float cutoffHz = 250.0f * std::pow(72.0f, tone);
            const float alpha = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * cutoffHz / float(rate.load()));
            for (int ch = 0; ch < 2; ++ch) {
                auto& state = toneStates[(size_t) layer][(size_t) ch];
                auto* data = scratch.getWritePointer(ch);
                for (int sample = 0; sample < n; ++sample) {
                    state += alpha * (data[sample] - state);
                    data[sample] = state;
                }
            }
            const auto gain = (p.muted.load() || (anySolo && !p.solo.load())) ? 0.0f : p.volume.load();
            output.addFrom(0, 0, scratch, 0, 0, n, gain);
            output.addFrom(1, 0, scratch, 1, 0, n, gain);
        }
        if (padStop.exchange(false)) padEnvelope = 0.0f;
        const bool padActive = padEnabled.load();
        const float padLevel = padVolume.load();
        const float padColour = padTone.load();
        const double sr = juce::jmax(1.0, rate.load());
        const double freq = 440.0 * std::pow(2.0, (padRoot.load() - 69.0) / 12.0);
        const double phaseStep = juce::MathConstants<double>::twoPi * freq / sr;
        for (int i = 0; i < n; ++i) {
            padEnvelope = juce::jlimit(0.0f, 1.0f, padEnvelope + (padActive ? 1.0f : -1.0f) / float(sr * 0.65));
            if (padEnvelope <= 0.0f) continue;
            const float fundamental = float(std::sin(padPhase));
            const float fifth = float(std::sin(padFifthPhase));
            const float v = (fundamental * 0.17f + fifth * (0.035f + padColour * 0.075f)
                           + float(std::sin(padPhase * 2.0)) * padColour * 0.025f)
                          * padEnvelope * padLevel;
            output.addSample(0, i, v);
            output.addSample(1, i, v);
            padPhase = std::fmod(padPhase + phaseStep, juce::MathConstants<double>::twoPi);
            padFifthPhase = std::fmod(padFifthPhase + phaseStep * 1.498307, juce::MathConstants<double>::twoPi);
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
    void setMidiEnabled(int i,bool v) {
        if (!juce::isPositiveAndBelow(i,numLayers)) return;
        layers[(size_t)i].midiEnabled.store(v);
        if (!v) layers[(size_t)i].flushNotes.store(true);
    }
    bool getMidiEnabled(int i) const { return layers[(size_t)i].midiEnabled.load(); }
    void setTone(int i,float v) { if(juce::isPositiveAndBelow(i,numLayers)) layers[(size_t)i].tone.store(juce::jlimit(0.0f,1.0f,v)); }
    float getTone(int i) const { return layers[(size_t)i].tone.load(); }
    void panic() { for (auto& l: layers) l.flushNotes.store(true); padEnabled.store(false); padStop.store(true); }
    void setPadEnabled(bool value) { padEnabled.store(value); }
    bool getPadEnabled() const { return padEnabled.load(); }
    void setPadVolume(float v) { padVolume.store(juce::jlimit(0.0f, 1.0f, v)); }
    float getPadVolume() const { return padVolume.load(); }
    void setPadTone(float v) { padTone.store(juce::jlimit(0.0f, 1.0f, v)); }
    float getPadTone() const { return padTone.load(); }
    void setPadRoot(int v) { padRoot.store(juce::jlimit(24, 72, v)); }
    int getPadRoot() const { return padRoot.load(); }
    void setMaster(float v) { masterVolume.store(juce::jlimit(0.f,1.5f,v)); }
    float getMaster() const { return masterVolume.load(); }
    void setReverb(float v) { reverbAmount.store(juce::jlimit(0.f,0.6f,v)); }
    float getReverb() const { return reverbAmount.load(); }
    float getPeak() const { return peak.load(); }

    void getStateInformation(juce::MemoryBlock& dest) override {
        juce::XmlElement root("StageBridgeUnified");
        root.setAttribute("master", double(getMaster()));
        root.setAttribute("reverb", double(getReverb()));
        root.setAttribute("padOn", getPadEnabled());
        root.setAttribute("padVolume", double(getPadVolume()));
        root.setAttribute("padTone", double(getPadTone()));
        root.setAttribute("padRoot", getPadRoot());
        const juce::ScopedLock lock(stateMutex);
        for (int i=0;i<numLayers;++i) {
            auto* layer = root.createNewChildElement("Layer");
            layer->setAttribute("index",i);
            layer->setAttribute("path",layerFiles[(size_t)i].getFullPathName());
            layer->setAttribute("volume",double(getVolume(i)));
            layer->setAttribute("mute",getMute(i));
            layer->setAttribute("solo",getSolo(i));
            layer->setAttribute("transpose",getTranspose(i));
            layer->setAttribute("midiEnabled",getMidiEnabled(i));
            layer->setAttribute("tone",double(getTone(i)));
            layer->setAttribute("low",layers[(size_t)i].noteLow.load());
            layer->setAttribute("high",layers[(size_t)i].noteHigh.load());
            layer->setAttribute("channel",layers[(size_t)i].midiChannel.load());
        }
        copyXmlToBinary(root,dest);
    }
    void setStateInformation(const void* data,int bytes) override {
        const auto xml=getXmlFromBinary(data,bytes);
        if (!xml || !xml->hasTagName("StageBridgeUnified")) return;
        setMaster(float(xml->getDoubleAttribute("master",0.85)));
        setReverb(float(xml->getDoubleAttribute("reverb",0.1)));
        setPadEnabled(xml->getBoolAttribute("padOn",false));
        setPadVolume(float(xml->getDoubleAttribute("padVolume",0.38)));
        setPadTone(float(xml->getDoubleAttribute("padTone",0.5)));
        setPadRoot(xml->getIntAttribute("padRoot",45));
        {
            const juce::ScopedLock lock(stateMutex);
            for (auto* child : xml->getChildIterator()) {
                if (!child->hasTagName("Layer")) continue;
                const int i=child->getIntAttribute("index",-1);
                if (!juce::isPositiveAndBelow(i,numLayers)) continue;
                auto& p=layers[(size_t)i];
                const auto storedPath = child->getStringAttribute("path");
                layerFiles[(size_t)i] = storedPath.isNotEmpty() ? juce::File(storedPath) : juce::File();
                p.volume.store(float(child->getDoubleAttribute("volume",0.8)));
                p.muted.store(child->getBoolAttribute("mute",false));
                p.solo.store(child->getBoolAttribute("solo",false));
                p.transpose.store(child->getIntAttribute("transpose",0));
                p.midiEnabled.store(child->getBoolAttribute("midiEnabled",true));
                p.tone.store(float(child->getDoubleAttribute("tone",1.0)));
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
    std::array<std::array<float,2>,numLayers> toneStates {};
    juce::AudioBuffer<float> scratch;
    juce::Reverb reverb;
    float lastReverb=-1.0f;
    std::atomic<double> rate {44100.0};
    std::atomic<float> masterVolume {0.85f}, reverbAmount {0.10f}, peak {0.0f};
    std::atomic<bool> padEnabled {false}, padStop {false};
    std::atomic<float> padVolume {0.38f}, padTone {0.50f};
    std::atomic<int> padRoot {45};
    float padEnvelope = 0.0f;
    double padPhase = 0.0, padFifthPhase = 0.0;
};

class StageEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit StageEditor(StageBridgeUnified& processor)
      : juce::AudioProcessorEditor(&processor), engine(processor) {
        setSize(1050,756);
        header.setText("STAGEBRIDGE + PLAYSTAGE | LIVE",juce::dontSendNotification);
        header.setColour(juce::Label::textColourId,juce::Colours::white);
        header.setFont(juce::Font(23.0f,juce::Font::bold));
        addAndMakeVisible(header);
        subtitle.setText("5 CAMADAS / VST3 + STANDALONE / ABLETON .PSMAP / PAD CONTINUO",
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
            midiButtons[(size_t)i].setButtonText("MIDI");
            midiButtons[(size_t)i].setClickingTogglesState(true);
            midiButtons[(size_t)i].setToggleState(engine.getMidiEnabled(i),juce::dontSendNotification);
            midiButtons[(size_t)i].onClick=[this,i]{ engine.setMidiEnabled(i,midiButtons[(size_t)i].getToggleState()); };
            addAndMakeVisible(midiButtons[(size_t)i]);
            initSlider(tones[(size_t)i],0.0,1.0,engine.getTone(i));
            tones[(size_t)i].onValueChange=[this,i]{ engine.setTone(i,float(tones[(size_t)i].getValue())); };
            addAndMakeVisible(tones[(size_t)i]);
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
        padButton.setButtonText("PAD CONTINUO");
        padButton.setClickingTogglesState(true);
        padButton.setToggleState(engine.getPadEnabled(),juce::dontSendNotification);
        padButton.onClick=[this] { engine.setPadEnabled(padButton.getToggleState()); };
        addAndMakeVisible(padButton);
        initSlider(padLevel,0.0,1.0,engine.getPadVolume());
        padLevel.onValueChange=[this] { engine.setPadVolume(float(padLevel.getValue())); };
        addAndMakeVisible(padLevel);
        initSlider(padBrightness,0.0,1.0,engine.getPadTone());
        padBrightness.onValueChange=[this] { engine.setPadTone(float(padBrightness.getValue())); };
        addAndMakeVisible(padBrightness);
        initSlider(padPitch,24,72,engine.getPadRoot());
        padPitch.setRange(24,72,1);
        padPitch.onValueChange=[this] { engine.setPadRoot(int(padPitch.getValue())); };
        addAndMakeVisible(padPitch);
        panicButton.setButtonText("PANICO / ALL NOTES OFF");
        panicButton.onClick=[this]{ engine.panic(); };
        addAndMakeVisible(panicButton);
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
            const int y=105+i*95;
            g.setColour(i%2?juce::Colour(0xff21486e):panel);
            g.fillRoundedRectangle(16.0f,float(y),float(getWidth()-32),85.0f,10.0f);
            g.setColour(cyan.withAlpha(0.6f));
            g.fillRoundedRectangle(16.0f,float(y),5.0f,85.0f,2.0f);
        }
        g.setColour(panel);
        g.fillRoundedRectangle(16.0f,591.0f,float(getWidth()-32),149.0f,10.0f);
        g.setColour(pale);
        g.setFont(12.0f);
        for (int i=0;i<numLayers;++i) {
            const int y=105+i*95;
            g.drawText("TONE",562,y+10,65,20,juce::Justification::centredLeft);
            g.drawText("VOL",712,y+10,65,20,juce::Justification::centredLeft);
            g.drawText("TRANSPOSE",864,y+10,110,20,juce::Justification::centredLeft);
        }
    }
    void resized() override {
        header.setBounds(20,18,660,34);
        subtitle.setBounds(21,56,810,24);
        for (int i=0;i<numLayers;++i) {
            const int y=105+i*95;
            labels[(size_t)i].setBounds(29,y+18,245,42);
            loadButtons[(size_t)i].setBounds(283,y+25,92,34);
            midiButtons[(size_t)i].setBounds(382,y+25,68,34);
            muteButtons[(size_t)i].setBounds(459,y+25,39,34);
            soloButtons[(size_t)i].setBounds(505,y+25,39,34);
            tones[(size_t)i].setBounds(559,y+29,132,41);
            gains[(size_t)i].setBounds(710,y+29,133,41);
            transposes[(size_t)i].setBounds(863,y+29,152,41);
        }
        masterLabel.setBounds(28,606,85,24);
        master.setBounds(112,607,190,32);
        reverbLabel.setBounds(312,606,75,24);
        reverb.setBounds(386,607,166,32);
        panicButton.setBounds(566,603,200,36);
        meter.setBounds(782,606,230,28);
        padButton.setBounds(32,652,153,34);
        padLevel.setBounds(198,654,205,34);
        padBrightness.setBounds(417,654,204,34);
        padPitch.setBounds(637,654,180,34);
        message.setBounds(28,713,961,22);
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
    StageBridgeUnified& engine;
    juce::Label header,subtitle,message,masterLabel,reverbLabel,meter;
    std::array<juce::Label,numLayers> labels;
    std::array<juce::TextButton,numLayers> loadButtons,muteButtons,soloButtons,midiButtons;
    juce::TextButton panicButton;
    std::array<juce::Slider,numLayers> gains,transposes,tones;
    juce::TextButton padButton;
    juce::Slider master,reverb,padLevel,padBrightness,padPitch;
    std::unique_ptr<juce::FileChooser> chooser;
};
juce::AudioProcessorEditor* StageBridgeUnified::createEditor() {
    return new StageEditor(*this);
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new StageBridgeUnified();
}
