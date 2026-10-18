#pragma once
#include "agent/Agent.h"
#include "analysis/Analyzer.h"
#include "core/StateStore.h"
#include "dsp/MixEngine.h"
#include "host/ReaperBridge.h"
#include "standalone/MultitrackPlayer.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <deque>

namespace im
{
// Test hook: when set before construction, no background threads (analysis,
// agent) are started; tests drive them synchronously.
extern bool gDisableBackgroundServices;

// RAII marker for "this thread is the audio thread" (allocation guard in tests).
struct RtScope
{
    static thread_local bool active;
    RtScope() { active = true; }
    ~RtScope() { active = false; }
};

class InteractiveMixerProcessor : public juce::AudioProcessor,
                                  public AgentHost,
                                  private juce::Timer,
                                  private juce::AudioProcessorParameter::Listener
{
public:
    InteractiveMixerProcessor();
    ~InteractiveMixerProcessor() override;

    // ---- AudioProcessor ---------------------------------------------------
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    const juce::String getName() const override { return "Interactive Mixer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return &vst3Ext; }
    void updateTrackProperties (const TrackProperties& props) override;
    void setStateInformation (const void*, int) override;

    // ---- AgentHost ----------------------------------------------------------
    TrackMask presentMask() const override { return present.load(); }
    float paramValue (int index) const override;
    float groupBusGrDb (int g) const override { return g >= 0 && g < kNumGroupBuses ? engine.masterMeters.groupGr[(size_t) g].load() : 0.0f; }
    float groupBusGainDb (int g) const override { return g >= 0 && g < kNumGroupBuses ? engine.masterMeters.groupGainDb[(size_t) g].load() : 0.0f; }
    // Route each track to the instrument-group bus its name implies (message thread; cheap, also run from the timer).
    void updateTrackGroups();
    std::shared_ptr<const Snapshot> snapshot() const override { return analyzer.latest(); }
    void applyAgentChanges (int tick, const std::vector<ParamTarget>& targets, double rampSec) override;

    // ---- message thread API -------------------------------------------------
    void applyLlmBatch (int tick, const std::vector<ParamTarget>& targets, double rampSec);
    void setParamValue (int index, float displayValue); // manual-style set (20 ms), cancels LLM ramp
    float rawValue (int index) const { return rawParams[(size_t) index]->load(); }
    juce::RangedAudioParameter* param (int index) const { return paramPtrs[(size_t) index]; }

    void undo();
    void redo();
    bool canUndo() const { return undoPos > 0; }
    bool canRedo() const { return undoPos + 1 < (int) undoStack.size(); }
    void undoTick (int tick);
    void pushUndoSnapshot();
    // Every mixer parameter back to its default (one undo step; LLM locks and track names are kept).
    void resetAllParams();

    void stageMoved (int track, StagePos from, StagePos to); // drag end; debounced 1 s -> Assist tick
    void settingsChanged();  // propagate StateStore settings to server/agent

    double llmFlashAgeMs (int index) const;
    bool isStandaloneInstance() const { return wrapperType == wrapperType_Standalone; }
    juce::String loadSessionFolder (const juce::File& dir);
    void applyLoadedTrackNames();

    // ---- members (public for UI/tests) -------------------------------------
    juce::AudioProcessorValueTreeState apvts;
    StateStore store;
    AnalysisTaps taps;
    MixEngine engine;
    Analyzer analyzer { taps };
    Bridge& bridge = Bridge::shared(); // turns go to the user's coding agent
    Agent agent { store, *this, bridge };
    MultitrackPlayer player;

    std::atomic<double> lastProcessMs { 0.0 };
    std::atomic<double> cpuLoad { 0.0 }; // last block time / block duration
    double currentSampleRate() const { return sampleRateNow; }

    ReaperBridge reaper; // host track names (REAPER VST3); message thread only

private:
    struct Vst3Ext : juce::VST3ClientExtensions
    {
        InteractiveMixerProcessor& p;
        explicit Vst3Ext (InteractiveMixerProcessor& o) : p (o) {}
        void setIHostApplication (Steinberg::FUnknown* host) override { p.hostContext = host; }
    } vst3Ext { *this };
    std::atomic<Steinberg::FUnknown*> hostContext { nullptr };
    bool reaperTried = false;
    double nextHostPollMs = 0;
    std::string lastHostDiag;
    void pollHostTrackNames();
    static BusesProperties makeBuses();
    void timerCallback() override;
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int parameterIndex, bool gestureIsStarting) override;
    std::vector<float> captureRaw() const;
    void restoreRaw (const std::vector<float>& v);
    void startServices();
    TrackMask trackFromBuses (const juce::AudioBuffer<float>& buffer, const float** L, const float** R);

    std::array<std::atomic<float>*, kNumParams> rawParams {};
    std::array<juce::RangedAudioParameter*, kNumParams> paramPtrs {};
    double paramSig = -1.0; // weighted sum of all raw parameter values, to detect changes on the audio thread
    std::unordered_map<juce::AudioProcessorParameter*, int> paramIndexOf;

    // LLM animation bookkeeping (message thread)
    std::array<float, kNumParams> animTarget {};
    std::array<uint32_t, kNumParams> animBatch {};
    std::array<std::atomic<float>, kNumParams> pendingTarget {};
    std::array<std::atomic<bool>, kNumParams> hasPendingTarget {};
    std::array<double, kNumParams> flashMs {};
    std::vector<int> animating;
    uint32_t batchCounter = 0;

    // undo
    std::deque<std::vector<float>> undoStack;
    int undoPos = -1;
    bool restoring = false;

    // stage debounce
    struct StageMove { int track; StagePos from, to; };
    std::vector<StageMove> stageMoves;
    double stageDeadlineMs = 0;

    // audio
    double sampleRateNow = 48000.0;
    int maxBlock = 512;
    std::vector<std::vector<float>> playerBufL, playerBufR;
    std::atomic<TrackMask> present { 1 };
    bool servicesStarted = false;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InteractiveMixerProcessor)
};
} // namespace im
