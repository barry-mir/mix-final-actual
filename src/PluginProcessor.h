#pragma once
#include "agent/Agent.h"
#include "analysis/Analyzer.h"
#include "core/StateStore.h"
#include "dsp/MixEngine.h"
#include "standalone/MultitrackPlayer.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <deque>
#include <functional>

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
    // Live: blocks until `needSec` of audio has played since the last change from anyone (parameters, groups, clip
    // edits), starting playback if it is stopped. Offline engines (no background services) return at once.
    void waitForFreshAudio (double needSec, const std::function<bool()>& stop) override;
    double freshAudioSec() const { return (double) freshSamples.load() / sampleRateNow; }
    std::atomic<double> listenHave { 0.0 }, listenNeed { 0.0 }; // progress of the wait above (status)

    // ---- clips: JSON views of the player's edits (message thread) ----------
    nlohmann::json editsJson() const;             // {"version", "tracks": [[{pos, src, len, gain, fadeIn, fadeOut}...]...]}
    static std::string regionsFromJson (const nlohmann::json& arr, MultitrackPlayer::Regions& out); // "" or the error

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

private:
    static BusesProperties makeBuses();
    void timerCallback() override;
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int parameterIndex, bool gestureIsStarting) override;
    std::vector<float> captureRaw() const;
    void restoreRaw (const std::vector<float>& v);
    void startServices();

    std::vector<std::atomic<float>*> rawParams = std::vector<std::atomic<float>*> ((size_t) numParams());
    std::vector<juce::RangedAudioParameter*> paramPtrs = std::vector<juce::RangedAudioParameter*> ((size_t) numParams());
    double paramSig = -1.0; // weighted sum of all raw parameter values, to detect changes on the audio thread
    int seenEditsVer = -1, seenGroupSig = -1;  // audio thread: clip edits and group routing seen last block
    std::atomic<int64_t> freshSamples { 0 };   // samples played since the last change (audio thread writes)
    std::unordered_map<juce::AudioProcessorParameter*, int> paramIndexOf;

    // LLM animation bookkeeping (message thread)
    std::vector<float> animTarget = std::vector<float> ((size_t) numParams());
    std::vector<uint32_t> animBatch = std::vector<uint32_t> ((size_t) numParams());
    std::vector<std::atomic<float>> pendingTarget = std::vector<std::atomic<float>> ((size_t) numParams());
    std::vector<std::atomic<bool>> hasPendingTarget = std::vector<std::atomic<bool>> ((size_t) numParams());
    std::vector<double> flashMs = std::vector<double> ((size_t) numParams());
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
    std::vector<float*> fillL, fillR;             // per-track pointers into playerBuf (sized in prepareToPlay)
    std::vector<const float*> inL, inR;           // what the engine gets: nullptr = track not present
    std::atomic<TrackMask> present { TrackMask {} };
    bool servicesStarted = false;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InteractiveMixerProcessor)
};
} // namespace im
