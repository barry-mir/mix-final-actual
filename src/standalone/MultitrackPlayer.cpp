#include "standalone/MultitrackPlayer.h"
#include "agent/MixPriors.h"
#include "analysis/Loudness.h"
#include "core/Paths.h"
#include <map>

namespace im
{
namespace
{
constexpr double kExcerptSec = 30.0;
constexpr double kTargetLufs = -23.0;
const char* kPatterns = "*.wav;*.WAV;*.aif;*.aiff;*.AIF;*.AIFF;*.flac;*.FLAC;*.mp3;*.ogg;*.m4a";

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

std::unique_ptr<juce::MemoryMappedAudioFormatReader> mapFile (const juce::File& f)
{
    std::unique_ptr<juce::MemoryMappedAudioFormatReader> r;
    const auto ext = f.getFileExtension().toLowerCase();
    if (ext == ".wav") r.reset (juce::WavAudioFormat().createMemoryMappedReader (f));
    else if (ext == ".aif" || ext == ".aiff") r.reset (juce::AiffAudioFormat().createMemoryMappedReader (f));
    if (r && ! r->mapEntireFile()) r.reset();
    return r;
}

// A float WAV of `f` at `rate` (first two channels) in the app cache, converted once in chunks; reused while the source
// file is unchanged.
juce::File cachedCopy (const juce::File& f, double rate, juce::AudioFormatManager& fm, juce::String& error)
{
    const auto key = f.getFullPathName() + "|" + juce::String (f.getSize()) + "|" + juce::String (f.getLastModificationTime().toMilliseconds())
                     + "|" + juce::String (rate);
    auto dir = appDataDir().getChildFile ("cache");
    dir.createDirectory();
    auto out = dir.getChildFile (juce::String::toHexString (key.hashCode64()) + ".wav");
    if (out.existsAsFile()) return out;
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r)
    {
        error = "cannot read " + f.getFileName();
        return {};
    }
    const int ch = (int) std::min<unsigned int> (2, r->numChannels);
    const double ratio = r->sampleRate / rate;
    const auto outLen = (int64_t) std::ceil ((double) r->lengthInSamples / ratio);
    auto tmp = out.getSiblingFile (out.getFileName() + ".part");
    tmp.deleteFile();
    {
        auto os = tmp.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> w;
        if (os) w.reset (juce::WavAudioFormat().createWriterFor (os.get(), rate, (unsigned int) ch, 32, {}, 0));
        if (! w)
        {
            error = "cannot write the cache in " + dir.getFullPathName();
            return {};
        }
        os.release();
        juce::AudioFormatReaderSource src (r.get(), false);
        juce::ResamplingAudioSource rs (&src, false, ch);
        rs.setResamplingRatio (ratio);
        const int block = 32768;
        rs.prepareToPlay (block, rate);
        src.prepareToPlay (block, r->sampleRate);
        juce::AudioBuffer<float> buf (std::max (ch, (int) r->numChannels), block);
        for (int64_t done = 0; done < outLen;)
        {
            const int n = (int) std::min<int64_t> (block, outLen - done);
            juce::AudioSourceChannelInfo info (&buf, 0, n);
            if (std::abs (ratio - 1.0) < 1e-9) src.getNextAudioBlock (info); // only a format change
            else rs.getNextAudioBlock (info);
            w->writeFromAudioSampleBuffer (buf, 0, n);
            done += n;
        }
    }
    if (! tmp.moveFileTo (out))
    {
        error = "cannot write the cache in " + dir.getFullPathName();
        return {};
    }
    return out;
}

// Reads frames [start, start + n) of a track as stereo into L/R (mono duplicated; past the end: silence). Real-time
// safe: the reader copies from mapped memory and converts in place.
void readStereo (const MultitrackPlayer::Track& t, int64_t start, int n, float* L, float* R)
{
    const int64_t len = t.reader->lengthInSamples;
    if (start >= len || n <= 0)
    {
        std::fill (L, L + n, 0.0f);
        std::fill (R, R + n, 0.0f);
        return;
    }
    const int k = (int) std::min<int64_t> (n, len - start);
    float* dest[2] = { L, R };
    t.reader->read (dest, t.channels, start, k);
    if (t.channels == 1) std::copy_n (L, k, R);
    if (k < n)
    {
        std::fill (L + k, L + n, 0.0f);
        std::fill (R + k, R + n, 0.0f);
    }
}

// The excerpt: 1 s frames; the 30 s window whose quietest instrument group (by name) is loudest on average, so every
// group plays in it (as make_sessions.best_excerpt for the evaluation sessions). Reads the tracks in 1 s chunks.
std::pair<int64_t, int64_t> bestExcerpt (const MultitrackPlayer::Session& s)
{
    const auto win = (int64_t) (kExcerptSec * s.sampleRate);
    if (s.length <= win) return { 0, s.length };
    const auto hop = (int64_t) s.sampleRate;
    const int frames = (int) ((s.length - hop) / hop);
    std::map<int, std::vector<int>> members;
    for (size_t t = 0; t < s.tracks.size(); ++t) members[priors::roleForName (s.names[t])].push_back ((int) t);
    std::map<int, std::vector<double>> logE;
    std::vector<float> L ((size_t) hop), R ((size_t) hop), m ((size_t) hop);
    for (auto& [g, ts] : members) logE[g].assign ((size_t) frames, 0.0);
    for (int f = 0; f < frames; ++f)
        for (auto& [g, ts] : members)
        {
            std::fill (m.begin(), m.end(), 0.0f);
            for (int t : ts)
            {
                readStereo (s.tracks[(size_t) t], f * hop, (int) hop, L.data(), R.data());
                for (size_t i = 0; i < m.size(); ++i) m[i] += 0.5f * (L[i] + R[i]);
            }
            double acc = 0;
            for (float v : m) acc += (double) v * v;
            logE[g][(size_t) f] = std::log10 (acc / (double) hop + 1e-10);
        }
    const int span = (int) kExcerptSec;
    double best = -1e9;
    int bestI = 0;
    for (int i = 0; i + span < frames; ++i)
    {
        double score = 1e9;
        for (auto& [g, e] : logE)
        {
            double sum = 0;
            for (int k = i; k < i + span; ++k) sum += e[(size_t) k];
            score = std::min (score, sum / span);
        }
        if (score > best)
        {
            best = score;
            bestI = i;
        }
    }
    return { bestI * hop, bestI * hop + win };
}
} // namespace

// Touches the mapped pages ahead of the playhead (and at the loop start, which playback jumps back to), so the audio
// thread reads memory that is already resident.
class MultitrackPlayer::Prefetcher : private juce::Thread
{
public:
    explicit Prefetcher (MultitrackPlayer& p) : juce::Thread ("track prefetch"), player (p) { startThread (juce::Thread::Priority::low); }
    ~Prefetcher() override { stopThread (2000); }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            if (auto* s = player.active.load())
            {
                const auto from = (int64_t) (player.posSec.load() * s->sampleRate);
                const auto ahead = (int64_t) (3.0 * s->sampleRate);
                const auto loopStart = (int64_t) (player.loopA.load() * s->sampleRate);
                for (auto& t : s->tracks)
                {
                    const auto step = std::max<int64_t> (1, 4096 / t.bytesPerFrame);
                    for (auto base : { from, loopStart })
                        for (int64_t i = std::max<int64_t> (0, base); i < base + ahead && i < t.reader->lengthInSamples; i += step)
                            t.reader->touchSample (i);
                }
            }
            wait (100);
        }
    }
    MultitrackPlayer& player;
};

MultitrackPlayer::MultitrackPlayer() { prefetch = std::make_unique<Prefetcher> (*this); }

MultitrackPlayer::~MultitrackPlayer()
{
    prefetch.reset();
    std::lock_guard<std::mutex> lk (infoMutex);
    owned.clear();
}

juce::Array<juce::File> MultitrackPlayer::trackFiles (const juce::File& dir)
{
    auto files = dir.findChildFiles (juce::File::findFiles, false, kPatterns);
    files.removeIf ([] (const juce::File& f) { return f.getFileName().startsWith ("."); });
    files.sort();
    return files;
}

double MultitrackPlayer::sessionRate (const juce::File& dir)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::map<int, int> count;
    for (auto& f : trackFiles (dir))
        if (std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f)); r != nullptr) ++count[(int) std::lround (r->sampleRate)];
    int best = 48000, n = 0;
    for (auto& [rate, c] : count)
        if (c > n)
        {
            best = rate;
            n = c;
        }
    return best;
}

std::unique_ptr<MultitrackPlayer::Session> MultitrackPlayer::buildSession (const juce::File& dir, double targetRate, juce::String& error) const
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    const auto files = trackFiles (dir);
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
        if ((int) s->tracks.size() >= maxTracks()) break;
        Track t;
        t.reader = mapFile (f);
        if (t.reader && std::abs (t.reader->sampleRate - targetRate) > 0.5) t.reader.reset(); // another rate: convert
        if (! t.reader)
        {
            juce::String err;
            const auto copy = cachedCopy (f, targetRate, fm, err);
            if (copy == juce::File())
            {
                error = err;
                return nullptr;
            }
            t.reader = mapFile (copy);
            if (! t.reader)
            {
                error = "cannot open the converted copy of " + f.getFileName();
                return nullptr;
            }
        }
        t.channels = (int) std::min<unsigned int> (2, t.reader->numChannels);
        t.bytesPerFrame = std::max<int64_t> (1, (int64_t) t.reader->numChannels * (int64_t) t.reader->bitsPerSample / 8);
        s->length = std::max<int64_t> (s->length, t.reader->lengthInSamples);
        s->tracks.push_back (std::move (t));
        s->names.push_back (trackLabel (f).toStdString());
    }
    // Names are how the agent addresses tracks, so they must be unique: "Guitar", "Guitar 2", ...
    {
        std::map<std::string, int> seen;
        for (auto& n : s->names)
            if (const int k = ++seen[n]; k > 1) n += " " + std::to_string (k);
    }
    const auto [a, b] = bestExcerpt (*s);
    s->excerptStart = a;
    s->excerptEnd = b;
    // One gain for every track: the unity sum of the excerpt at -23 LUFS.
    {
        const int n = (int) (b - a);
        juce::AudioBuffer<float> sum (2, n), one (2, n);
        sum.clear();
        for (auto& t : s->tracks)
        {
            readStereo (t, a, n, one.getWritePointer (0), one.getWritePointer (1));
            for (int ch = 0; ch < 2; ++ch) sum.addFrom (ch, 0, one, ch, 0, n);
        }
        const double lufs = integratedLoudness (sum.getReadPointer (0), sum.getReadPointer (1), n, targetRate);
        s->gainDb = std::isfinite (lufs) && lufs > -70.0 ? std::clamp (kTargetLufs - lufs, -40.0, 40.0) : 0.0;
        s->gain = (float) std::pow (10.0, s->gainDb / 20.0);
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
    auto* raw = s.get();
    {
        std::lock_guard<std::mutex> lk (infoMutex);
        folder = s->folder;
        names = s->names;
        owned.push_back (std::move (s));
    }
    numTracks = (int) raw->tracks.size();
    lenSec = (double) raw->length / raw->sampleRate;
    excA = (double) raw->excerptStart / raw->sampleRate;
    excB = (double) raw->excerptEnd / raw->sampleRate;
    gainDbNow = raw->gainDb;
    loadedRate = raw->sampleRate;
    setLoopRegion (excA.load(), excB.load()); // the excerpt is the default loop (and what the first mix hears)
    pending.store (raw);
    active.store (raw); // the prefetcher can start on it already
}

void MultitrackPlayer::collectGarbage() {}

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
        current = p;
        pos = p->excerptStart; // a new session starts at its excerpt (the default loop)
    }
    if (! current) return {};
    const double sk = seekTo.exchange (-1.0);
    if (sk >= 0) pos = std::clamp ((int64_t) (sk * current->sampleRate), (int64_t) 0, current->length);

    const int nt = (int) current->tracks.size();
    const TrackMask mask { nt };
    for (int t = 0; t < nt; ++t)
    {
        std::fill (L[t], L[t] + n, 0.0f);
        std::fill (R[t], R[t] + n, 0.0f);
    }
    if (! playing.load()) return mask;

    // Loop region (frames); end <= start means the whole file.
    int64_t la = (int64_t) (loopA.load (std::memory_order_relaxed) * current->sampleRate);
    int64_t lb = (int64_t) (loopB.load (std::memory_order_relaxed) * current->sampleRate);
    const bool lp = looping.load();
    if (! lp || lb <= la || la < 0 || lb > current->length)
    {
        la = 0;
        lb = current->length;
    }
    if (lp && pos >= lb) pos = la; // past the region (new region, or a seek after it): back to its start
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
            readStereo (current->tracks[(size_t) t], pos, k, L[t] + done, R[t] + done);
            if (current->gain != 1.0f)
                for (int i = done; i < done + k; ++i)
                {
                    L[t][i] *= current->gain;
                    R[t][i] *= current->gain;
                }
        }
        pos += k;
        done += k;
    }
    posSec.store ((double) pos / current->sampleRate, std::memory_order_relaxed);
    return mask;
}
} // namespace im
