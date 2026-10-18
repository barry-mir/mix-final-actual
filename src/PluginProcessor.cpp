#include "PluginProcessor.h"
#include "core/Paths.h"
#include "agent/MixPriors.h"
#if IM_HAS_EDITOR
 #include "PluginEditor.h"
#endif

namespace im
{
bool gDisableBackgroundServices = false;
thread_local bool RtScope::active = false;

namespace
{
// clamp into the parameter's range before normalising (ramped values can drift by an ulp)
void setNotifying (juce::RangedAudioParameter* p, float v)
{
    const auto& r = p->getNormalisableRange();
    p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (r.start, r.end, v)));
}
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
} // namespace

juce::AudioProcessor::BusesProperties InteractiveMixerProcessor::makeBuses()
{
    BusesProperties b;
    for (int t = 0; t < kMaxTracks; ++t)
        b = b.withInput ("Track " + juce::String (t + 1), juce::AudioChannelSet::stereo(), true);
    return b.withOutput ("Mix", juce::AudioChannelSet::stereo(), true);
}

InteractiveMixerProcessor::InteractiveMixerProcessor()
    : juce::AudioProcessor (makeBuses()),
      apvts (*this, nullptr, "IMPARAMS", createLayout())
{
    for (int i = 0; i < kNumParams; ++i)
    {
        const auto& s = spec (i);
        rawParams[(size_t) i] = apvts.getRawParameterValue (s.id);
        paramPtrs[(size_t) i] = apvts.getParameter (s.id);
        jassert (rawParams[(size_t) i] != nullptr && paramPtrs[(size_t) i] != nullptr);
        engine.params.bindRaw (i, rawParams[(size_t) i]);
        paramIndexOf[paramPtrs[(size_t) i]] = i;
        paramPtrs[(size_t) i]->addListener (this);
        animTarget[(size_t) i] = s.def;
    }
    engine.setTaps (&taps);
    {
        // The coding agent answers every turn: the engineer prompt for many tracks, no prompt-size squeeze, and every
        // prompt from the page runs one turn.
        auto st = store.settings();
        st.mode = AgentMode::Assist;
        st.promptVersion = 5;
        st.promptTokenBudget = 400000;
        st.maxTokens = 16000;
        store.setSettings (st);
    }
    pushUndoSnapshot();
    startServices();
    startTimerHz (30);
}

InteractiveMixerProcessor::~InteractiveMixerProcessor()
{
    alive.reset();
    stopTimer();
    for (auto* p : paramPtrs)
        if (p) p->removeListener (this);
    agent.stop();
    analyzer.stop();
}

void InteractiveMixerProcessor::startServices()
{
    if (gDisableBackgroundServices || servicesStarted) return;
    servicesStarted = true;
    agent.start();
}

void InteractiveMixerProcessor::settingsChanged() {}

// ------------------------------------------------------------------ buses
bool InteractiveMixerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.outputBuses.size() != 1 || layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    const auto main = layouts.getMainInputChannelSet();
    // REAPER-style: single 2*kMaxTracks-channel input, channel pair k = track k.
    if (main.size() == 2 * kMaxTracks)
    {
        for (int b = 1; b < layouts.inputBuses.size(); ++b)
            if (! layouts.inputBuses[b].isDisabled()) return false;
        return true;
    }
    if (! main.isDisabled() && main != juce::AudioChannelSet::stereo()) return false;
    for (int b = 1; b < layouts.inputBuses.size(); ++b)
    {
        const auto& s = layouts.inputBuses[b];
        if (! s.isDisabled() && s != juce::AudioChannelSet::stereo()) return false;
    }
    return true;
}

void InteractiveMixerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sampleRateNow = sampleRate;
    maxBlock = juce::jlimit (32, 2048, samplesPerBlock > 0 ? samplesPerBlock : 512);
    taps.prepare (sampleRate);
    engine.prepare (sampleRate, maxBlock);
    setLatencySamples (engine.latencySamples());
    playerBufL.assign (kMaxTracks, std::vector<float> ((size_t) maxBlock, 0.0f));
    playerBufR.assign (kMaxTracks, std::vector<float> ((size_t) maxBlock, 0.0f));
    analyzer.start (sampleRate, ! gDisableBackgroundServices);

    // Standalone convenience: IM_SESSION_DIR=<folder of stems> auto-loads on first start.
    if (! player.hasSession() && isStandaloneInstance())
        if (auto* env = std::getenv ("IM_SESSION_DIR"); env != nullptr && juce::File (env).isDirectory())
        {
            loadSessionFolder (juce::File (env));
            player.setPlaying (true);
        }
    // Standalone: resample the loaded session if the device rate changed.
    if (player.hasSession() && std::abs (player.loadedSampleRate() - sampleRate) > 0.5)
        player.loadFolder (player.loadedFolder(), sampleRate);
}

void InteractiveMixerProcessor::releaseResources() { analyzer.stop(); }

TrackMask InteractiveMixerProcessor::trackFromBuses (const juce::AudioBuffer<float>& buffer, const float** L, const float** R)
{
    TrackMask mask = 0;
    const int mainIn = getChannelCountOfBus (true, 0);
    if (mainIn >= 2 * kMaxTracks)
    {
        for (int t = 0; t < kMaxTracks; ++t)
        {
            L[t] = buffer.getReadPointer (2 * t);
            R[t] = buffer.getReadPointer (2 * t + 1);
            mask |= TrackMask (1) << t;
        }
    }
    else
    {
        const int nb = std::min (getBusCount (true), kMaxTracks);
        for (int b = 0; b < nb; ++b)
        {
            if (getChannelCountOfBus (true, b) != 2) continue;
            const int idx = getChannelIndexInProcessBlockBuffer (true, b, 0);
            if (idx + 1 >= buffer.getNumChannels()) continue;
            L[b] = buffer.getReadPointer (idx);
            R[b] = buffer.getReadPointer (idx + 1);
            mask |= TrackMask (1) << b;
        }
    }
    return mask;
}

void InteractiveMixerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    RtScope rt;
    juce::ScopedNoDenormals noDenormals;
    const auto t0 = juce::Time::getHighResolutionTicks();
    const int n = buffer.getNumSamples();
    lastProcessMs.store (nowMs(), std::memory_order_relaxed);
    if (buffer.getNumChannels() < 2 || n == 0) return;

    // Any mix parameter moved (agent ramp, user, automation): restart the analyzer's long-term level average.
    double sig = 0;
    for (size_t i = 0; i < rawParams.size(); ++i) sig += (double) rawParams[i]->load (std::memory_order_relaxed) * (double) (i % 97 + 1);
    if (sig != paramSig)
    {
        paramSig = sig;
        analyzer.noteParamChange();
    }

    const float* L[kMaxTracks] = {};
    const float* R[kMaxTracks] = {};
    TrackMask mask = 0;
    const bool usePlayer = player.hasSession();
    if (! usePlayer) mask = trackFromBuses (buffer, L, R);

    float* outL = buffer.getWritePointer (0);
    float* outR = buffer.getWritePointer (1);
    const int mb = engine.maxBlock();
    for (int off = 0; off < n; off += mb)
    {
        const int len = std::min (mb, n - off);
        const float* cl[kMaxTracks] = {};
        const float* cr[kMaxTracks] = {};
        if (usePlayer)
        {
            float* pl[kMaxTracks];
            float* pr[kMaxTracks];
            for (int t = 0; t < kMaxTracks; ++t)
            {
                pl[t] = playerBufL[(size_t) t].data();
                pr[t] = playerBufR[(size_t) t].data();
            }
            mask = player.fill (pl, pr, len);
            for (int t = 0; t < kMaxTracks; ++t)
                if ((mask >> t) & 1u) { cl[t] = pl[t]; cr[t] = pr[t]; }
        }
        else
            for (int t = 0; t < kMaxTracks; ++t)
                if (L[t]) { cl[t] = L[t] + off; cr[t] = R[t] + off; }
        engine.process (cl, cr, outL + off, outR + off, len);
    }
    for (int ch = 2; ch < getTotalNumOutputChannels(); ++ch) buffer.clear (ch, 0, n);

    present.store (mask, std::memory_order_relaxed);
    analyzer.setPresentMask (mask);
    const double sec = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    cpuLoad.store (sec / ((double) n / sampleRateNow), std::memory_order_relaxed);
}

// ------------------------------------------------------------------ agent host
float InteractiveMixerProcessor::paramValue (int index) const
{
    if (hasPendingTarget[(size_t) index].load()) return pendingTarget[(size_t) index].load();
    return rawParams[(size_t) index]->load();
}

void InteractiveMixerProcessor::applyAgentChanges (int tick, const std::vector<ParamTarget>& targets, double rampSec)
{
    // Record pending targets immediately so the agent/undo see them.
    for (auto& t : targets)
    {
        pendingTarget[(size_t) t.param].store (t.value);
        hasPendingTarget[(size_t) t.param].store (true);
    }
    std::weak_ptr<bool> weak = alive;
    juce::MessageManager::callAsync ([this, weak, tick, targets, rampSec] {
        if (weak.lock()) applyLlmBatch (tick, targets, rampSec);
    });
}

void InteractiveMixerProcessor::applyLlmBatch (int tick, const std::vector<ParamTarget>& targets, double rampSec)
{
    juce::ignoreUnused (tick);
    if (targets.empty()) return;
    ++batchCounter;
    std::vector<LlmChange> batch;
    batch.reserve (targets.size());
    const double now = nowMs();
    for (auto& t : targets)
    {
        const auto i = (size_t) t.param;
        auto* p = paramPtrs[i];
        float v = p->getNormalisableRange().snapToLegalValue (t.value);
        if (spec (t.param).kind != Kind::Float) v = clampToSpec (spec (t.param), v);
        animTarget[i] = v;
        animBatch[i] = batchCounter;
        pendingTarget[i].store (v);
        hasPendingTarget[i].store (true);
        flashMs[i] = now;
        engine.params.uiAnimFlag (t.param).store (true);
        if (std::find (animating.begin(), animating.end(), t.param) == animating.end()) animating.push_back (t.param);
        batch.push_back ({ t.param, v, (float) rampSec, batchCounter });
        if (spec (t.param).kind != Kind::Float) // toggles flip immediately in the UI
            setNotifying (p, v);
    }
    if (! engine.params.pushLlmBatch (batch.data(), batch.size()))
    {
        for (auto& c : batch)
        {
            engine.params.uiAnimFlag (c.param).store (false);
            hasPendingTarget[(size_t) c.param].store (false);
            setNotifying (paramPtrs[(size_t) c.param], c.target);
        }
        animating.clear();
    }
    pushUndoSnapshot();
}

void InteractiveMixerProcessor::setParamValue (int index, float v)
{
    const auto i = (size_t) index;
    auto* p = paramPtrs[i];
    if (engine.params.uiAnimFlag (index).load())
    {
        // Param is following an LLM ramp: retarget through the batch path with a manual-length ramp.
        ++batchCounter;
        animTarget[i] = v;
        animBatch[i] = batchCounter;
        pendingTarget[i].store (v);
        LlmChange c { index, v, ParamEngine::kManualRampSec, batchCounter };
        if (engine.params.pushLlmBatch (&c, 1)) return;
    }
    setNotifying (p, v);
}

double InteractiveMixerProcessor::llmFlashAgeMs (int index) const
{
    const double f = flashMs[(size_t) index];
    return f <= 0 ? 1e9 : nowMs() - f;
}

// ------------------------------------------------------------------ timer
void InteractiveMixerProcessor::timerCallback()
{
    updateTrackGroups(); // names can change (host track names, user edits)
    const double now = nowMs();
    const bool audioAlive = now - lastProcessMs.load() < 500.0;

    // LLM ramp UI animation: controls follow the audio thread's interpolated values.
    for (size_t k = 0; k < animating.size();)
    {
        const int i = animating[k];
        auto* p = paramPtrs[(size_t) i];
        const bool consumed = engine.params.consumedBatch (i).load() == animBatch[(size_t) i];
        if (consumed && engine.params.isLlmActive (i))
        {
            if (spec (i).kind == Kind::Float)
                setNotifying (p, engine.params.publishedValue (i));
            ++k;
        }
        else if (consumed || ! audioAlive)
        {
            setNotifying (p, animTarget[(size_t) i]);
            engine.params.uiAnimFlag (i).store (false);
            hasPendingTarget[(size_t) i].store (false);
            animating.erase (animating.begin() + (long) k);
        }
        else
            ++k;
    }

    // Stage drag debounce (1 s) -> Assist tick with an auto user message.
    if (! stageMoves.empty() && now >= stageDeadlineMs)
    {
        std::map<int, StageMove> merged;
        for (auto& mv : stageMoves)
        {
            auto it = merged.find (mv.track);
            if (it == merged.end()) merged[mv.track] = mv;
            else it->second.to = mv.to;
        }
        stageMoves.clear();
        std::string msg;
        auto fmt = [] (StagePos p) {
            char b[48];
            std::snprintf (b, sizeof (b), "(%.2f,%.2f)", p.x, p.y);
            return std::string (b);
        };
        for (auto& [t, mv] : merged)
            msg += (msg.empty() ? "" : "; ") + ("stage moved: " + store.trackName (t) + " from " + fmt (mv.from) + " to " + fmt (mv.to));
        ChatEntry e;
        e.role = "user";
        e.text = msg;
        e.time = juce::Time::currentTimeMillis() / 1000.0;
        store.addChat (e);
        agent.trigger ({ "stage", msg });
    }

    player.collectGarbage();
    if (now >= nextHostPollMs)
    {
        nextHostPollMs = now + 2000.0;
        pollHostTrackNames();
    }
}

void InteractiveMixerProcessor::pollHostTrackNames()
{
    if (! reaperTried && hostContext.load() != nullptr)
    {
        reaperTried = true;
        reaper.attach (hostContext.load());
    }
    if (! reaper.available()) return;
    const auto names = reaper.inputNames();
    for (int t = 0; t < kMaxTracks; ++t)
        if (! names[(size_t) t].empty() && store.nameSource (t) != NameSource::User && store.trackName (t) != names[(size_t) t])
            store.setTrackName (t, names[(size_t) t], NameSource::Host);
    const auto diag = reaper.diagnostics();
    if (diag != lastHostDiag)
    {
        lastHostDiag = diag;
        appDataDir().getChildFile ("host.log").appendText (juce::Time::getCurrentTime().toISO8601 (true) + "  " + diag + "\n");
    }
}

void InteractiveMixerProcessor::updateTrackProperties (const TrackProperties& props)
{
    // Generic hosts only tell us the name of the track we are inserted on: use it for input 1.
    if (! props.name.has_value()) return;
    const auto name = props.name->toStdString();
    std::weak_ptr<bool> weak = alive;
    juce::MessageManager::callAsync ([this, weak, name] {
        if (! weak.lock() || reaper.available() || name.empty()) return;
        if (store.nameSource (0) != NameSource::User) store.setTrackName (0, name, NameSource::Host);
    });
}

void InteractiveMixerProcessor::stageMoved (int track, StagePos from, StagePos to)
{
    store.setStagePos (track, to);
    stageMoves.push_back ({ track, from, to });
    stageDeadlineMs = nowMs() + 1000.0;
}

// ------------------------------------------------------------------ undo
std::vector<float> InteractiveMixerProcessor::captureRaw() const
{
    std::vector<float> v ((size_t) kNumParams);
    for (int i = 0; i < kNumParams; ++i) v[(size_t) i] = paramValue (i);
    return v;
}

void InteractiveMixerProcessor::restoreRaw (const std::vector<float>& v)
{
    restoring = true;
    for (int i = 0; i < kNumParams; ++i)
        if (std::fabs (v[(size_t) i] - paramValue (i)) > 1e-6f) setParamValue (i, v[(size_t) i]);
    restoring = false;
}

void InteractiveMixerProcessor::pushUndoSnapshot()
{
    auto snap = captureRaw();
    if (undoPos >= 0 && undoPos < (int) undoStack.size() && undoStack[(size_t) undoPos] == snap) return;
    while ((int) undoStack.size() > undoPos + 1) undoStack.pop_back();
    undoStack.push_back (std::move (snap));
    while (undoStack.size() > 51) undoStack.pop_front(); // current state + 50 undo levels
    undoPos = (int) undoStack.size() - 1;
}

void InteractiveMixerProcessor::resetAllParams()
{
    pushUndoSnapshot();
    std::vector<float> defaults ((size_t) kNumParams);
    for (int i = 0; i < kNumParams; ++i) defaults[(size_t) i] = spec (i).def;
    restoreRaw (defaults);
    pushUndoSnapshot();
}

void InteractiveMixerProcessor::undo()
{
    if (! canUndo()) return;
    --undoPos;
    restoreRaw (undoStack[(size_t) undoPos]);
}

void InteractiveMixerProcessor::redo()
{
    if (! canRedo()) return;
    ++undoPos;
    restoreRaw (undoStack[(size_t) undoPos]);
}

void InteractiveMixerProcessor::undoTick (int tick)
{
    for (auto& e : store.chat())
    {
        if (e.role != "agent" || e.tick != tick || e.undone) continue;
        for (auto& c : e.diff)
        {
            if (c.status == "rejected") continue;
            const int i = findParam (c.param);
            if (i >= 0) setParamValue (i, c.from);
        }
        store.markTickUndone (tick);
        pushUndoSnapshot();
        break;
    }
}

void InteractiveMixerProcessor::parameterGestureChanged (int, bool starting)
{
    if (starting) return;
    std::weak_ptr<bool> weak = alive;
    juce::MessageManager::callAsync ([this, weak] { if (weak.lock() && ! restoring) pushUndoSnapshot(); });
}

// ------------------------------------------------------------------ state
void InteractiveMixerProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    auto* extra = xml->createNewChildElement ("IMSTATE");
    extra->addTextElement (store.toJson().dump());
    if (player.hasSession()) xml->setAttribute ("sessionFolder", player.loadedFolder().getFullPathName());
    copyXmlToBinary (*xml, dest);
}

void InteractiveMixerProcessor::setStateInformation (const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, size));
    if (! xml) return;
    if (auto* extra = xml->getChildByName ("IMSTATE"))
    {
        auto j = json::parse (extra->getAllSubText().toStdString(), nullptr, false);
        if (! j.is_discarded()) store.fromJson (j);
        xml->removeChildElement (extra, true);
    }
    const auto folder = xml->getStringAttribute ("sessionFolder");
    if (xml->hasTagName (apvts.state.getType())) apvts.replaceState (juce::ValueTree::fromXml (*xml));
    if (folder.isNotEmpty() && isStandaloneInstance() && juce::File (folder).isDirectory())
        player.loadFolder (juce::File (folder), sampleRateNow);
    settingsChanged();
    pushUndoSnapshot();
}

juce::String InteractiveMixerProcessor::loadSessionFolder (const juce::File& dir)
{
    auto err = player.loadFolder (dir, sampleRateNow);
    if (err.isEmpty())
    {
        applyLoadedTrackNames();
        store.resetStage (player.numLoadedTracks());
        updateTrackGroups();
    }
    return err;
}

void InteractiveMixerProcessor::updateTrackGroups()
{
    const auto names = store.trackNames();
    for (int t = 0; t < kMaxTracks; ++t)
        engine.trackGroup[(size_t) t].store (names[(size_t) t].empty() ? -1 : priors::roleForName (names[(size_t) t]));
}

void InteractiveMixerProcessor::applyLoadedTrackNames()
{
    const auto names = player.loadedNames();
    for (int t = 0; t < (int) names.size() && t < kMaxTracks; ++t)
        if (store.nameSource (t) != NameSource::User) store.setTrackName (t, names[(size_t) t], NameSource::File);
}

// ------------------------------------------------------------------ editor
bool InteractiveMixerProcessor::hasEditor() const
{
#if IM_HAS_EDITOR
    return true;
#else
    return false;
#endif
}

juce::AudioProcessorEditor* InteractiveMixerProcessor::createEditor()
{
#if IM_HAS_EDITOR
    return new InteractiveMixerEditor (*this);
#else
    return nullptr;
#endif
}
} // namespace im

#if IM_HAS_EDITOR
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new im::InteractiveMixerProcessor(); }
#endif
