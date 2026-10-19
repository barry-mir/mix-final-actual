#include "standalone/MultitrackPlayer.h"
#include "agent/MixPriors.h"
#include "analysis/Loudness.h"
#include "core/Paths.h"
#include <algorithm>
#include <cmath>
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
                auto* ed = player.rtEdits.load();
                if (ed && (ed->session != s || ed->identity)) ed = nullptr;
                auto touch = [&] (const Track& t, int64_t a, int64_t b) {
                    const auto step = std::max<int64_t> (1, 4096 / t.bytesPerFrame);
                    for (int64_t i = std::max<int64_t> (0, a); i < b && i < t.reader->lengthInSamples; i += step) t.reader->touchSample (i);
                };
                for (size_t k = 0; k < s->tracks.size(); ++k)
                    for (auto base : { from, loopStart })
                    {
                        if (! ed) { touch (s->tracks[k], base, base + ahead); continue; }
                        for (auto& r : ed->tracks[k]) // the source spans the next seconds of the timeline play
                        {
                            const auto a = std::max (base, r.pos), b = std::min (base + ahead, r.pos + r.len);
                            if (a < b) touch (s->tracks[(size_t) r.srcTrack], r.src + (a - r.pos), r.src + (b - r.pos));
                        }
                    }
            }
            wait (100);
        }
    }
    MultitrackPlayer& player;
};

// Builds the waveform overview of one session, track by track, in the background.
class MultitrackPlayer::PeakBuilder : private juce::Thread
{
public:
    PeakBuilder (MultitrackPlayer& p, const Session* s) : juce::Thread ("track peaks"), player (p), session (s) { startThread (juce::Thread::Priority::low); }
    ~PeakBuilder() override { stopThread (4000); }

private:
    void run() override
    {
        const int block = player.peakBlock.load();
        std::vector<std::vector<uint8_t>> out (session->tracks.size());
        const int chunk = block * 256;
        std::vector<float> L ((size_t) chunk), R ((size_t) chunk);
        for (size_t t = 0; t < session->tracks.size(); ++t)
        {
            const auto& trk = session->tracks[t];
            const int64_t len = trk.reader->lengthInSamples;
            auto& o = out[t];
            o.reserve ((size_t) (len / block + 1));
            for (int64_t a = 0; a < len; a += chunk)
            {
                if (threadShouldExit()) return;
                const int n = (int) std::min<int64_t> (chunk, len - a);
                readStereo (trk, a, n, L.data(), R.data());
                for (int b = 0; b < n; b += block)
                {
                    float pk = 0;
                    for (int i = b; i < std::min (n, b + block); ++i) pk = std::max (pk, std::max (std::fabs (L[(size_t) i]), std::fabs (R[(size_t) i])));
                    o.push_back ((uint8_t) std::clamp ((int) std::lround (255.0 * std::sqrt (std::min (1.0f, pk))), 0, 255));
                }
            }
        }
        std::lock_guard<std::mutex> lk (player.infoMutex);
        if (player.active.load() != session) return;
        player.peakData = std::move (out);
        player.peaksDone = true;
    }
    MultitrackPlayer& player;
    const Session* session;
};

MultitrackPlayer::MultitrackPlayer() { prefetch = std::make_unique<Prefetcher> (*this); }

MultitrackPlayer::~MultitrackPlayer()
{
    peakBuilder.reset();
    prefetch.reset();
    std::lock_guard<std::mutex> lk (infoMutex);
    rtOwned.clear();
    owned.clear();
}

std::string MultitrackPlayer::normaliseRegions (Regions& in, int ownTrack, const std::vector<double>& fileLens)
{
    constexpr double kMin = 0.01, kEps = 0.001;
    if ((int) in.size() > kMaxRegions) return "at most " + std::to_string (kMaxRegions) + " clips per track";
    for (size_t k = 0; k < in.size(); ++k)
    {
        auto& r = in[k];
        const auto where = "clip " + std::to_string (k + 1) + ": ";
        for (double v : { r.pos, r.src, r.len, r.gainDb, r.fadeIn, r.fadeOut })
            if (! std::isfinite (v)) return where + "not a number";
        if (r.srcTrack < 0) r.srcTrack = ownTrack;
        if (r.srcTrack < 0 || r.srcTrack >= (int) fileLens.size()) return where + "srcTrack is not a track of this session";
        const double fileLen = fileLens[(size_t) r.srcTrack];
        if (r.pos < 0) return where + "starts before 0";
        if (r.src < 0) return where + "starts before the file";
        if (r.len < kMin) return where + "shorter than 10 ms";
        if (r.src + r.len > fileLen + kEps) return where + "runs past the end of the file";
        if (r.gainDb < -48 || r.gainDb > 24) return where + "gain must be -48..+24 dB";
        if (r.fadeIn < 0 || r.fadeOut < 0) return where + "fades cannot be negative";
        r.len = std::min (r.len, fileLen - r.src);
        r.fadeIn = std::min (r.fadeIn, r.len / 2);
        r.fadeOut = std::min (r.fadeOut, r.len / 2);
    }
    // Overlapping clips are kept (they sum; the page gives them crossfades). Sorted by position for the audio thread.
    std::stable_sort (in.begin(), in.end(), [] (const Region& x, const Region& y) { return x.pos < y.pos; });
    return {};
}

std::vector<double> MultitrackPlayer::fileLengths (const Session& s)
{
    std::vector<double> v;
    for (auto& t : s.tracks) v.push_back ((double) t.reader->lengthInSamples / s.sampleRate);
    return v;
}

double MultitrackPlayer::trackLengthSec (int t) const
{
    auto* s = active.load();
    if (! s || t < 0 || t >= (int) s->tracks.size()) return 0.0;
    return (double) s->tracks[(size_t) t].reader->lengthInSamples / s->sampleRate;
}

std::vector<float> MultitrackPlayer::mixdown (const std::vector<int>& tracks, double startSec, double lenSec) const
{
    auto* s = active.load();
    if (! s) return {};
    const auto a = std::max<int64_t> (0, (int64_t) (startSec * s->sampleRate));
    const auto n = (int) std::clamp<int64_t> ((int64_t) (lenSec * s->sampleRate), 0, std::max<int64_t> (0, s->length - a));
    std::vector<float> out ((size_t) n, 0.0f), L ((size_t) n), R ((size_t) n);
    for (int t : tracks)
    {
        if (t < 0 || t >= (int) s->tracks.size()) continue;
        readStereo (s->tracks[(size_t) t], a, n, L.data(), R.data());
        for (int i = 0; i < n; ++i) out[(size_t) i] += 0.5f * (L[(size_t) i] + R[(size_t) i]);
    }
    return out;
}

std::vector<MultitrackPlayer::Regions> MultitrackPlayer::edits() const
{
    std::vector<Regions> out;
    auto* s = active.load();
    if (! s) return out;
    std::lock_guard<std::mutex> lk (infoMutex);
    for (size_t t = 0; t < s->tracks.size(); ++t)
    {
        if (t < editSec.size() && ! editSec[t].empty()) out.push_back (editSec[t]);
        else if (t < clearedSec.size() && clearedSec[t]) out.push_back ({});
        else out.push_back ({ Region { 0, 0, (double) s->tracks[t].reader->lengthInSamples / s->sampleRate, 0, 0, 0, (int) t } });
    }
    return out;
}

bool MultitrackPlayer::editsAreIdentity() const
{
    auto* e = rtEdits.load();
    return e == nullptr || e->identity;
}

std::string MultitrackPlayer::setTrackRegions (int t, Regions r)
{
    auto* s = active.load();
    if (! s || t < 0 || t >= (int) s->tracks.size()) return "no such track";
    if (! r.empty())
        if (auto e = normaliseRegions (r, t, fileLengths (*s)); ! e.empty()) return e;
    std::lock_guard<std::mutex> lk (infoMutex);
    if (editSec.size() < s->tracks.size()) editSec.resize (s->tracks.size());
    if (clearedSec.size() < s->tracks.size()) clearedSec.resize (s->tracks.size(), 0);
    clearedSec[(size_t) t] = r.empty() ? 1 : 0; // every clip deleted: silent, not back to the whole file
    editSec[(size_t) t] = std::move (r);
    publishEdits (s);
    return {};
}

void MultitrackPlayer::setAllEdits (const std::vector<Regions>& all, const std::vector<char>& cleared)
{
    std::lock_guard<std::mutex> lk (infoMutex);
    editSec = all;
    clearedSec = cleared;
    if (auto* s = active.load()) publishEdits (s);
    else ++editsVer;
}

void MultitrackPlayer::publishEdits (const Session* s)
{
    auto e = std::make_unique<RtEdits>();
    e->session = s;
    e->end = s->length;
    const double fs = s->sampleRate;
    const auto lens = fileLengths (*s);
    for (size_t t = 0; t < s->tracks.size(); ++t)
    {
        const auto fileLen = s->tracks[t].reader->lengthInSamples;
        std::vector<RtRegion> rt;
        Regions r = t < editSec.size() ? editSec[t] : Regions {};
        if (! r.empty() && ! normaliseRegions (r, (int) t, lens).empty()) r.clear(); // not valid for this session
        if (t < editSec.size()) editSec[t] = r;
        const bool cleared = t < clearedSec.size() && clearedSec[t] && r.empty();
        if (r.empty() && ! cleared) rt.push_back ({ 0, 0, fileLen, 0, 0, 1.0f, (int) t });
        if (cleared) e->identity = false;
        for (auto& x : r)
        {
            const auto srcLen = s->tracks[(size_t) x.srcTrack].reader->lengthInSamples;
            rt.push_back ({ (int64_t) std::llround (x.pos * fs), (int64_t) std::llround (x.src * fs),
                            std::min<int64_t> ((int64_t) std::llround (x.len * fs), srcLen - (int64_t) std::llround (x.src * fs)),
                            (int64_t) std::llround (x.fadeIn * fs), (int64_t) std::llround (x.fadeOut * fs), (float) std::pow (10.0, x.gainDb / 20.0),
                            x.srcTrack });
        }
        const bool ident = rt.size() == 1 && rt[0].pos == 0 && rt[0].src == 0 && rt[0].len == fileLen && rt[0].fadeIn == 0 && rt[0].fadeOut == 0
                           && rt[0].gain == 1.0f && rt[0].srcTrack == (int) t;
        e->identity = e->identity && ident;
        for (auto& x : rt) e->end = std::max (e->end, x.pos + x.len);
        e->tracks.push_back (std::move (rt));
    }
    lenSec = (double) e->end / fs;
    rtEdits.store (e.get(), std::memory_order_release);
    rtOwned.push_back (std::move (e));
    ++editsVer;
}

std::vector<uint8_t> MultitrackPlayer::peaks (int t) const
{
    std::lock_guard<std::mutex> lk (infoMutex);
    return t >= 0 && t < (int) peakData.size() ? peakData[(size_t) t] : std::vector<uint8_t> {};
}

void MultitrackPlayer::renderRegions (const Session& s, const std::vector<RtRegion>& regs, int64_t start, int n, float* L, float* R) noexcept
{
    std::fill (L, L + n, 0.0f);
    std::fill (R, R + n, 0.0f);
    const int64_t end = start + n;
    for (auto& r : regs)
    {
        if (r.pos >= end) break; // sorted by position
        const int64_t a = std::max (start, r.pos), b = std::min (end, r.pos + r.len);
        if (a >= b) continue;
        const auto& trk = s.tracks[(size_t) r.srcTrack];
        // Overlapping clips sum, so each one is read into scratch and added (in scratch-sized pieces).
        for (int64_t c = a; c < b; c += kScratch)
        {
            const int k = (int) std::min<int64_t> (kScratch, b - c);
            const int off = (int) (c - start);
            float* sl = scratchL.data();
            float* sr = scratchR.data();
            readStereo (trk, r.src + (c - r.pos), k, sl, sr);
            const int64_t x0 = c - r.pos;
            const bool fades = (r.fadeIn > 0 && x0 < r.fadeIn) || (r.fadeOut > 0 && x0 + k > r.len - r.fadeOut);
            if (! fades && r.gain == 1.0f)
                for (int i = 0; i < k; ++i)
                {
                    L[off + i] += sl[i];
                    R[off + i] += sr[i];
                }
            else
                for (int i = 0; i < k; ++i)
                {
                    const int64_t x = x0 + i;
                    float g = r.gain;
                    if (x < r.fadeIn) g *= std::sin (1.5707963f * (float) ((double) x / (double) r.fadeIn)); // equal power
                    if (r.len - x <= r.fadeOut && r.fadeOut > 0) g *= std::sin (1.5707963f * (float) ((double) (r.len - x) / (double) r.fadeOut));
                    L[off + i] += sl[i] * g;
                    R[off + i] += sr[i] * g;
                }
        }
    }
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
    {
        std::lock_guard<std::mutex> lk (infoMutex);
        publishEdits (raw); // the edits carry over (checked against the new files)
        peaksDone = false;
        peakData.clear();
        peakBlock = 256 * std::max (1, (int) std::lround (raw->sampleRate / 48000.0));
    }
    excA = (double) raw->excerptStart / raw->sampleRate;
    excB = (double) raw->excerptEnd / raw->sampleRate;
    gainDbNow = raw->gainDb;
    loadedRate = raw->sampleRate;
    setLoopRegion (excA.load(), excB.load()); // the excerpt is the default loop (and what the first mix hears)
    pending.store (raw);
    active.store (raw); // the prefetcher can start on it already
    peakBuilder.reset();
    peakBuilder = std::make_unique<PeakBuilder> (*this, raw);
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
    auto* ed = rtEdits.load (std::memory_order_acquire);
    if (ed && ed->session != current) ed = nullptr; // edits for a session not picked up yet
    const int64_t songLen = ed ? ed->end : current->length;
    const bool plain = ! ed || ed->identity;
    const double sk = seekTo.exchange (-1.0);
    if (sk >= 0) pos = std::clamp ((int64_t) (sk * current->sampleRate), (int64_t) 0, songLen);

    const int nt = (int) current->tracks.size();
    const TrackMask mask { nt };
    for (int t = 0; t < nt; ++t)
    {
        std::fill (L[t], L[t] + n, 0.0f);
        std::fill (R[t], R[t] + n, 0.0f);
    }
    if (! playing.load())
    {
        posSec.store ((double) pos / current->sampleRate, std::memory_order_relaxed); // a seek while stopped shows at once
        return mask;
    }

    // Loop region (frames); end <= start means the whole file.
    int64_t la = (int64_t) (loopA.load (std::memory_order_relaxed) * current->sampleRate);
    int64_t lb = (int64_t) (loopB.load (std::memory_order_relaxed) * current->sampleRate);
    const bool lp = looping.load();
    if (! lp || lb <= la || la < 0 || lb > songLen)
    {
        la = 0;
        lb = songLen;
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
            if (plain) readStereo (current->tracks[(size_t) t], pos, k, L[t] + done, R[t] + done);
            else renderRegions (*current, ed->tracks[(size_t) t], pos, k, L[t] + done, R[t] + done);
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
