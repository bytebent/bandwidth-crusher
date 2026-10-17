#include "PluginEditor.h"
#include "BinaryData.h"

BandwidthCrusherEditor::BandwidthCrusherEditor (BandwidthCrusherProcessor& p)
    : AudioProcessorEditor (p),
      proc (p),
      browser (juce::WebBrowserComponent::Options {}
                   .withNativeIntegrationEnabled()
                   .withInitialisationData ("version", juce::String (JucePlugin_VersionString))
                   .withResourceProvider ([] (const juce::String& url) { return getResource (url); })
                   .withEventListener ("setParam", [this] (const juce::var& v) { onSetParam (v); })
                   .withEventListener ("gesture", [this] (const juce::var& v) { onGesture (v); })
                   .withEventListener ("uiReady", [this] (const juce::var&) { needFullSync = true; }))
{
    addAndMakeVisible (browser);
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    setSize (width, height);
    proc.editorOpen = true;
    startTimerHz (30);
}

BandwidthCrusherEditor::~BandwidthCrusherEditor()
{
    stopTimer();
    proc.editorOpen = false;
    for (auto& id : openGestures)
        if (auto* param = proc.apvts.getParameter (id))
            param->endChangeGesture();
}

void BandwidthCrusherEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xffdcdce0));   // platinum, shown for a moment while the page loads
}

void BandwidthCrusherEditor::resized()
{
    browser.setBounds (getLocalBounds());
}

std::optional<juce::WebBrowserComponent::Resource> BandwidthCrusherEditor::getResource (const juce::String& url)
{
    const auto path = url.fromFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf ("?", false, false);
    if (path.isEmpty() || path == "index.html")
    {
        const auto* bytes = reinterpret_cast<const std::byte*> (BinaryData::index_html);
        return juce::WebBrowserComponent::Resource {
            std::vector<std::byte> (bytes, bytes + BinaryData::index_htmlSize), "text/html; charset=utf-8" };
    }
    return std::nullopt;
}

void BandwidthCrusherEditor::onSetParam (const juce::var& v)
{
    const auto id = v.getProperty ("id", {}).toString();
    auto* param = proc.apvts.getParameter (id);
    if (param == nullptr) return;
    const float value = juce::jlimit (0.0f, 1.0f, (float) (double) v.getProperty ("value", 0.0));

    const bool inGesture = openGestures.count (id) > 0;
    if (! inGesture) param->beginChangeGesture();
    param->setValueNotifyingHost (value);
    if (! inGesture) param->endChangeGesture();
    lastSent[id] = param->getValue();   // don't echo our own change back
}

void BandwidthCrusherEditor::onGesture (const juce::var& v)
{
    const auto id = v.getProperty ("id", {}).toString();
    auto* param = proc.apvts.getParameter (id);
    if (param == nullptr) return;
    const bool begin = (bool) v.getProperty ("begin", false);
    if (begin && openGestures.insert (id).second) param->beginChangeGesture();
    if (! begin && openGestures.erase (id) > 0) param->endChangeGesture();
}

void BandwidthCrusherEditor::timerCallback()
{
    // 1) Parameter values the UI doesn't know yet (automation, presets, undo, first load).
    for (auto* p : proc.getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (rp == nullptr) continue;
        const auto id = rp->getParameterID();
        const float value = rp->getValue();
        auto it = lastSent.find (id);
        if (needFullSync || it == lastSent.end() || std::abs (it->second - value) > 1.0e-6f)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("id", id);
            obj->setProperty ("value", value);
            browser.emitEventIfBrowserIsVisible ("param", juce::var (obj));
            lastSent[id] = value;
        }
    }
    needFullSync = false;

    // 2) Loop view frame, if the audio thread has produced a new one.
    auto& lv = proc.loopView;
    const auto counter = lv.counter.load (std::memory_order_acquire);
    if (counter != lastViewCounter)
    {
        lastViewCounter = counter;
        juce::Array<juce::var> values, ages;
        values.ensureStorageAllocated (bc::LoopView::N);
        ages.ensureStorageAllocated (bc::LoopView::N);
        for (int i = 0; i < bc::LoopView::N; ++i)
        {
            values.add (lv.value[(size_t) i].load (std::memory_order_relaxed));
            ages.add (lv.age[(size_t) i].load (std::memory_order_relaxed));
        }
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("values", values);
        obj->setProperty ("age", ages);
        obj->setProperty ("freshPct", lv.freshPct.load());
        obj->setProperty ("loopMs", lv.loopMs.load());
        obj->setProperty ("playhead", lv.playhead.load());
        obj->setProperty ("sampleRate", proc.currentSampleRate.load());
        browser.emitEventIfBrowserIsVisible ("loopview", juce::var (obj));
    }
}
