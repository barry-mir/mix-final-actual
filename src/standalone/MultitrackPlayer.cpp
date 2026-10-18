#include "standalone/MultitrackPlayer.h"
#include "agent/MixPriors.h"
#include "analysis/Loudness.h"
#include <map>

namespace im
{
MultitrackPlayer::MultitrackPlayer() { retired.resize (64); }

MultitrackPlayer::~MultitrackPlayer()
{
    collectGarbage();
    delete pending.exchange (nullptr);
    delete current;
}

namespace
{
constexpr double kExcerptSec = 30.0;
constexpr double kTargetLufs = -23.0;

// "01 Lead_Vocal-dry.wav" -> "Lead Vocal dry": ordering number out, separators to spaces.
juce::String trackLabel (const juce::File& f)
{
    auto nm = f.getFileNameWithoutExtension();
    const auto stripped = nm.trimCharactersAtStart ("0123456789").trimCharactersAtStart (" _-.");
    if (stripped.isNotEmpty() && stripped.length() < nm.length()) nm = stripped;
    nm = nm.replaceCharacters ("_-", "  ");
    while (nm.contains ("  ")) nm = nm.replace ("  ", " ");
    return nm.trim().substring (0, 40);
}

// The excerpt: 1 s frames; the 30 s window whose quietest instrument group (by name) is loudest on average, so every
// group plays in it (as make_sessions.best_excerpt for the evaluation sessions).
std::pair<int64_t, int64_t> bestExcerpt (const std::vector<juce::AudioBuffer<float>>& tracks, const std::vector<std::string>& names,
                                         int64_t length, double sr)
{
    const auto win = (int64_t) (kExcerptSec * sr);
    if (length <= win) return { 0, length };
    std::map<int, std::vector<double>> groupEnergy; // group -> energy per 1 s frame (mono sum)
    const auto hop = (int64_t) sr;
    const int frames = (int) ((length - hop) / hop);
    std::map<int, std::vector<int>> members;
    for (size_t t = 0; t < tracks.size(); ++t) members[priors::roleForName (names[t])].push_back ((int) t);
    for (auto& [g, ts] : members)
    {
        auto& e = groupEnergy[g];
        e.assign ((size_t) frames, 1e-10);
        for (int f = 0; f < frames; ++f)
        {
            double acc = 0;
            for (int64_t i = f * hop; i < (f + 1) * hop; i += 4) // every 4th sample is plenty for 1 s energies
            {
                double m = 0;
                for (int t : ts) m += 0.5 * ((double) tracks[(size_t) t].getSample (0, (int) i) + tracks[(size_t) t].getSample (1, (int) i));
                acc += m * m;
            }
            e[(size_t) f] += acc / (double) (hop / 4);
        }
        for (auto& v : e) v = std::log10 (v);
    }
    const int span = (int) kExcerptSec;
    double best = -1e9;
    int bestI = 0;
    for (int i = 0; i + span < frames; ++i)
    {
        double score = 1e9;
        for (auto& [g, e] : groupEnergy)
        {
            double s = 0;
            for (int k = i; k < i + span; ++k) s += e[(size_t) k];
            score = std::min (score, s / span);
        }
        if (score > best) { best = score; bestI = i; }
    }
    return { bestI * hop, bestI * hop + win };
}
} // namespace

std::unique_ptr<MultitrackPlayer::Session> MultitrackPlayer::buildSession (const juce::File& dir, double targetRate, juce::String& error) const
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav;*.WAV;*.aif;*.aiff;*.AIF;*.AIFF;*.flac;*.FLAC;*.mp3;*.ogg;*.m4a");
    files.removeIf ([] (const juce::File& f) { return f.getFileName().startsWith ("._") || f.getFileName().startsWith ("."); });
    files.sort();
    if (files.isEmpty())
    {
        error = "no audio files in " + dir.getFullPathName();
        return nullptr;
    }
    auto s = std::make_unique<Session>();
    s->sampleRate = targetRate;
    s->folder = dir;
    for (auto& f : files)
    {
        if ((int) s->tracks.size() >= kMaxTracks) break;
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (! r) continue;
        const int srcLen = (int) r->lengthInSamples;
        juce::AudioBuffer<float> src (2, srcLen);
        r->read (&src, 0, srcLen, 0, true, r->numChannels > 1);
        if (r->numChannels == 1) src.copyFrom (1, 0, src, 0, 0, srcLen); // mono -> stereo
        if (std::abs (r->sampleRate - targetRate) > 0.5)
        {
            const double ratio = r->sampleRate / targetRate;
            const int outLen = (int) std::ceil (srcLen / ratio);
            juce::AudioBuffer<float> dst (2, outLen);
            for (int ch = 0; ch < 2; ++ch)
            {
                juce::LagrangeInterpolator li;
                li.process (ratio, src.getReadPointer (ch), dst.getWritePointer (ch), outLen, srcLen, 0);
            }
            src = std::move (dst);
        }
        s->length = std::max<int64_t> (s->length, src.getNumSamples());
        s->tracks.push_back (std::move (src));
        s->names.push_back (trackLabel (f).toStdString());
    }
    if (s->tracks.empty())
    {
        error = "could not read any audio file in " + dir.getFullPathName();
        return nullptr;
    }
    for (auto& t : s->tracks) // pad to common length
        if (t.getNumSamples() < s->length)
        {
            const int old = t.getNumSamples();
            t.setSize (2, (int) s->length, true, true);
            t.clear (old, (int) s->length - old);
        }
    // Names are how the agent addresses tracks, so they must be unique: "Guitar", "Guitar 2", ...
    {
        std::map<std::string, int> seen;
        for (auto& n : s->names)
            if (const int k = ++seen[n]; k > 1) n += " " + std::to_string (k);
    }
    const auto [a, b] = bestExcerpt (s->tracks, s->names, s->length, targetRate);
    s->excerptStart = a;
    s->excerptEnd = b;
    // One gain for every track: the unity sum of the excerpt at -23 LUFS.
    {
        juce::AudioBuffer<float> sum (2, (int) (b - a));
        sum.clear();
        for (auto& t : s->tracks)
            for (int ch = 0; ch < 2; ++ch) sum.addFrom (ch, 0, t, ch, (int) a, (int) (b - a));
        const double lufs = integratedLoudness (sum.getReadPointer (0), sum.getReadPointer (1), sum.getNumSamples(), targetRate);
        s->gainDb = std::isfinite (lufs) && lufs > -70.0 ? std::clamp (kTargetLufs - lufs, -40.0, 40.0) : 0.0;
        const float g = (float) std::pow (10.0, s->gainDb / 20.0);
        for (auto& t : s->tracks) t.applyGain (g);
    }
    return s;
}

juce::String MultitrackPlayer::loadFolder (const juce::File& dir, double targetRate)
{
    juce::String err;
    auto s = buildSession (dir, targetRate, err);
    if (! s) return err;
    setSession (std::move (s));
    return {};
}

void MultitrackPlayer::setSession (std::unique_ptr<Session> s)
{
    {
        std::lock_guard<std::mutex> lk (infoMutex);
        folder = s->folder;
        names = s->names;
    }
    numTracks = (int) s->tracks.size();
    lenSec = (double) s->length / s->sampleRate;
    excA = (double) s->excerptStart / s->sampleRate;
    excB = (double) s->excerptEnd / s->sampleRate;
    gainDbNow = s->gainDb;
    setLoopRegion (excA.load(), excB.load()); // the excerpt is the default loop (and what the first mix hears)
    loadedRate = s->sampleRate;
    delete pending.exchange (s.release());
    collectGarbage();
}

void MultitrackPlayer::collectGarbage()
{
    Session* p = nullptr;
    while (retired.pop (p)) delete p;
}

juce::File MultitrackPlayer::loadedFolder() const
{
    std::lock_guard<std::mutex> lk (infoMutex);
    return folder;
}

std::vector<std::string> MultitrackPlayer::loadedNames() const
{
    std::lock_guard<std::mutex> lk (infoMutex);
    return names;
}

TrackMask MultitrackPlayer::fill (float* const* L, float* const* R, int n) noexcept
{
    if (auto* p = pending.exchange (nullptr))
    {
        if (current && ! retired.push (current)) { /* retire queue full: leak rather than free on audio thread */ }
        current = p;
        pos = 0;
    }
    if (! current) return 0;
    const double sk = seekTo.exchange (-1.0);
    if (sk >= 0) pos = std::clamp ((int64_t) (sk * current->sampleRate), (int64_t) 0, current->length);

    const int nt = (int) current->tracks.size();
    TrackMask mask = 0;
    for (int t = 0; t < nt; ++t) mask |= TrackMask (1) << t;
    for (int t = 0; t < nt; ++t)
    {
        std::fill (L[t], L[t] + n, 0.0f);
        std::fill (R[t], R[t] + n, 0.0f);
    }
    if (! playing.load()) return mask;

    // Loop region (samples); end <= start means the whole file.
    int64_t la = (int64_t) (loopA.load (std::memory_order_relaxed) * current->sampleRate);
    int64_t lb = (int64_t) (loopB.load (std::memory_order_relaxed) * current->sampleRate);
    const bool lp = looping.load();
    if (! lp || lb <= la || la < 0 || lb > current->length) { la = 0; lb = current->length; }
    if (lp && (pos < la || pos >= lb)) pos = la; // a new region, or a seek outside it
    int done = 0;
    while (done < n)
    {
        if (pos >= lb)
        {
            if (lp && lb > la) pos = la;
            else
            {
                playing = false;
                break;
            }
        }
        const int k = (int) std::min<int64_t> (n - done, lb - pos);
        for (int t = 0; t < nt; ++t)
        {
            const auto& b = current->tracks[(size_t) t];
            std::copy_n (b.getReadPointer (0, (int) pos), k, L[t] + done);
            std::copy_n (b.getReadPointer (1, (int) pos), k, R[t] + done);
        }
        pos += k;
        done += k;
    }
    posSec.store ((double) pos / current->sampleRate, std::memory_order_relaxed);
    return mask;
}
} // namespace im
