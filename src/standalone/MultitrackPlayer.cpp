#include "standalone/MultitrackPlayer.h"

namespace im
{
MultitrackPlayer::MultitrackPlayer() { retired.resize (64); }

MultitrackPlayer::~MultitrackPlayer()
{
    collectGarbage();
    delete pending.exchange (nullptr);
    delete current;
}

std::unique_ptr<MultitrackPlayer::Session> MultitrackPlayer::buildSession (const juce::File& dir, double targetRate, juce::String& error) const
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav;*.WAV;*.aif;*.aiff;*.flac");
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
        // "01 Vocals.wav" / "02_Bass.wav" -> "Vocals" / "Bass" (numeric ordering prefix removed)
        auto nm = f.getFileNameWithoutExtension();
        const auto stripped = nm.trimCharactersAtStart ("0123456789").trimCharactersAtStart (" _-.");
        if (stripped.isNotEmpty() && stripped.length() < nm.length()) nm = stripped;
        s->names.push_back (nm.toStdString());
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

    int done = 0;
    while (done < n)
    {
        if (pos >= current->length)
        {
            if (looping.load() && current->length > 0) pos = 0;
            else
            {
                playing = false;
                break;
            }
        }
        const int k = (int) std::min<int64_t> (n - done, current->length - pos);
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
