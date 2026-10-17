#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

// The whole interface is ui/index.html, shown in a web view. This class only
// passes parameter values and loop-view frames between it and the processor.
class BandwidthCrusherEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BandwidthCrusherEditor (BandwidthCrusherProcessor&);
    ~BandwidthCrusherEditor() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    static constexpr int width = 780, height = 494;

private:
    void timerCallback() override;
    void onSetParam (const juce::var&);
    void onGesture (const juce::var&);
    static std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    BandwidthCrusherProcessor& proc;
    juce::WebBrowserComponent browser;

    std::map<juce::String, float> lastSent;
    std::set<juce::String> openGestures;
    bool needFullSync = true;
    uint32_t lastViewCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandwidthCrusherEditor)
};
