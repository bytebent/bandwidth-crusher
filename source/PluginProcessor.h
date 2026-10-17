#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "CrusherDSP.h"

class BandwidthCrusherProcessor : public juce::AudioProcessor
{
public:
    BandwidthCrusherProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Bandwidth Crusher"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    bc::LoopView loopView;
    std::atomic<bool> editorOpen { false };
    std::atomic<double> currentSampleRate { 48000.0 };

    // Tempo-synced loop lengths, in beats (quarter notes): 1/64 … 4 bars.
    static constexpr std::array<double, 9> divisionBeats { 0.0625, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    bc::Crusher dsp;
    bool wasPlaying = false;
    int viewCountdown = 0;

    std::atomic<float>* pBudget; std::atomic<float>* pBits; std::atomic<float>* pRandom; std::atomic<float>* pThreshold;
    std::atomic<float>* pLoop; std::atomic<float>* pDivision; std::atomic<float>* pSync; std::atomic<float>* pMemory;
    std::atomic<float>* pChunk; std::atomic<float>* pSmooth; std::atomic<float>* pInGain; std::atomic<float>* pOutGain;
    std::atomic<float>* pMix; std::atomic<float>* pSplit; std::atomic<float>* pResetOnPlay; std::atomic<float>* pSeed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandwidthCrusherProcessor)
};
