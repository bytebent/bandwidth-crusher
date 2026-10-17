#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
using APF = juce::AudioParameterFloat;
using Attr = juce::AudioParameterFloatAttributes;

// Log-scaled range: normalised n maps to min * (max/min)^n. Must match ui/index.html.
juce::NormalisableRange<float> logRange (float min, float max)
{
    return { min, max,
             [] (float a, float b, float n) { return a * std::pow (b / a, n); },
             [] (float a, float b, float v) { return std::log (v / a) / std::log (b / a); },
             [] (float, float, float v) { return v; } };
}

juce::String pct (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }
juce::String dbStr (float v, int) { return juce::String (v, 1) + " dB"; }
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout BandwidthCrusherProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    auto id = [] (const char* s) { return juce::ParameterID { s, 1 }; };

    l.add (std::make_unique<APF> (id ("budget"), "Budget", logRange (0.5f, 1000.0f), 40.0f,
        Attr().withLabel ("kB/s").withStringFromValueFunction ([] (float v, int) {
            return juce::String (v, v < 10.0f ? 1 : 0) + " kB/s"; })));
    l.add (std::make_unique<juce::AudioParameterInt> (id ("bits"), "Bit depth", 2, 16, 12,
        juce::AudioParameterIntAttributes().withLabel ("bit")));
    l.add (std::make_unique<APF> (id ("randomness"), "Greedy / Random", juce::NormalisableRange<float> (0.0f, 1.0f), 0.3f,
        Attr().withStringFromValueFunction (pct)));
    l.add (std::make_unique<APF> (id ("threshold"), "Error threshold", juce::NormalisableRange<float> (0.0f, 0.2f), 0.0f,
        Attr().withStringFromValueFunction ([] (float v, int) {
            return v <= 0.0001f ? juce::String ("off") : juce::String (juce::roundToInt (20.0f * std::log10 (v))) + " dB"; })));
    l.add (std::make_unique<APF> (id ("loop"), "Loop length", logRange (1.0f, 4000.0f), 120.0f,
        Attr().withLabel ("ms").withStringFromValueFunction ([] (float v, int) {
            return v < 1000.0f ? juce::String (v, v < 10.0f ? 1 : 0) + " ms" : juce::String (v / 1000.0f, 2) + " s"; })));
    l.add (std::make_unique<juce::AudioParameterChoice> (id ("division"), "Synced loop length",
        juce::StringArray { "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars" }, 2));
    l.add (std::make_unique<juce::AudioParameterBool> (id ("sync"), "Sync to tempo", false));
    l.add (std::make_unique<APF> (id ("memory"), "Memory", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f,
        Attr().withStringFromValueFunction (pct)));
    l.add (std::make_unique<APF> (id ("chunk"), "Chunk size", logRange (1.0f, 4096.0f), 1.0f,
        Attr().withLabel ("smp").withStringFromValueFunction ([] (float v, int) {
            return juce::String (juce::roundToInt (v)) + " smp"; })));
    l.add (std::make_unique<APF> (id ("smooth"), "Edge smoothing", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f,
        Attr().withStringFromValueFunction (pct)));
    l.add (std::make_unique<APF> (id ("inGain"), "Input gain", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f,
        Attr().withStringFromValueFunction (dbStr)));
    l.add (std::make_unique<APF> (id ("outGain"), "Output gain", juce::NormalisableRange<float> (-24.0f, 12.0f), 0.0f,
        Attr().withStringFromValueFunction (dbStr)));
    l.add (std::make_unique<APF> (id ("mix"), "Dry / Wet", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f,
        Attr().withStringFromValueFunction (pct)));
    l.add (std::make_unique<juce::AudioParameterBool> (id ("stereoSplit"), "Split stereo budget", false));
    l.add (std::make_unique<juce::AudioParameterBool> (id ("resetOnPlay"), "Reset when playback starts", true));
    l.add (std::make_unique<juce::AudioParameterInt> (id ("seed"), "Seed", 0, 9999, 1));
    return l;
}

BandwidthCrusherProcessor::BandwidthCrusherProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    auto get = [this] (const char* s) { return apvts.getRawParameterValue (s); };
    pBudget = get ("budget"); pBits = get ("bits"); pRandom = get ("randomness"); pThreshold = get ("threshold");
    pLoop = get ("loop"); pDivision = get ("division"); pSync = get ("sync"); pMemory = get ("memory");
    pChunk = get ("chunk"); pSmooth = get ("smooth"); pInGain = get ("inGain"); pOutGain = get ("outGain");
    pMix = get ("mix"); pSplit = get ("stereoSplit"); pResetOnPlay = get ("resetOnPlay"); pSeed = get ("seed");
}

bool BandwidthCrusherProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return out == layouts.getMainInputChannelSet();
}

void BandwidthCrusherProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    dsp.prepare (sampleRate, getTotalNumOutputChannels(), 8.0);
    dsp.reset ((uint32_t) pSeed->load());
    setLatencySamples (dsp.latencySamples());
    wasPlaying = false;
    viewCountdown = 0;
}

void BandwidthCrusherProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumOutputChannels());
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
    if (numCh <= 0) return;

    bc::Params p;
    p.budgetKbs = pBudget->load(); p.bits = pBits->load(); p.randomness = pRandom->load();
    p.threshold = pThreshold->load(); p.memory = pMemory->load(); p.chunk = pChunk->load();
    p.smooth = pSmooth->load(); p.inGainDb = pInGain->load(); p.outGainDb = pOutGain->load();
    p.mix = pMix->load(); p.stereoSplit = pSplit->load() > 0.5f;
    p.loopMs = pLoop->load();

    bool playing = false;
    double bpm = 120.0;
    if (auto* head = getPlayHead())
        if (auto pos = head->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto b = pos->getBpm()) bpm = juce::jlimit (20.0, 999.0, *b);
        }

    if (pSync->load() > 0.5f)
    {
        const int d = juce::jlimit (0, (int) divisionBeats.size() - 1, (int) pDivision->load());
        p.loopMs = (float) juce::jlimit (1.0, 8000.0, divisionBeats[(size_t) d] * 60000.0 / bpm);
    }

    if (playing && ! wasPlaying && pResetOnPlay->load() > 0.5f)
        dsp.reset ((uint32_t) pSeed->load());
    wasPlaying = playing;

    dsp.process (buffer.getArrayOfWritePointers(), numCh, buffer.getNumSamples(), p);

    if (editorOpen.load (std::memory_order_relaxed))
    {
        viewCountdown -= buffer.getNumSamples();
        if (viewCountdown <= 0)
        {
            dsp.writeView (loopView);
            viewCountdown = (int) (currentSampleRate.load() / 30.0);
        }
    }
}

juce::AudioProcessorEditor* BandwidthCrusherProcessor::createEditor()
{
    return new BandwidthCrusherEditor (*this);
}

void BandwidthCrusherProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void BandwidthCrusherProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BandwidthCrusherProcessor();
}
