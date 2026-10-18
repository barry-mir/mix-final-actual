#include "Export.h"
#include "PluginProcessor.h"

namespace mfa
{
using im::InteractiveMixerProcessor;

namespace
{
constexpr double kTailSec = 3.0; // reverb and echo ring-out after the last note

// The rate of the first readable track (the export keeps the source rate).
double sourceRate (const juce::File& dir)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.wav;*.WAV;*.aif;*.aiff;*.AIF;*.AIFF;*.flac;*.FLAC;*.mp3;*.ogg;*.m4a"))
        if (std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f)); r != nullptr) return r->sampleRate;
    return 48000.0;
}

bool writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    auto os = f.createOutputStream();
    if (! os) return false;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
    if (! w) return false;
    os.release();
    return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

juce::String safeName (const std::string& s)
{
    return juce::File::createLegalFileName (juce::String (s)).substring (0, 60);
}
} // namespace

Exporter::~Exporter() { join(); }

void Exporter::join()
{
    if (worker.joinable()) worker.join();
}

std::string Exporter::folder() const
{
    std::lock_guard<std::mutex> lk (m);
    return outDir;
}

std::string Exporter::error() const
{
    std::lock_guard<std::mutex> lk (m);
    return err;
}

bool Exporter::start (InteractiveMixerProcessor& live, bool stems)
{
    if (busy.exchange (true)) return false;
    join();
    prog = 0.0;
    {
        std::lock_guard<std::mutex> lk (m);
        err.clear();
        outDir.clear();
    }
    // Snapshot of the live settings, and a fresh offline engine with them (built here, on the message thread).
    juce::MemoryBlock state;
    live.getStateInformation (state);
    const auto tracksDir = live.player.loadedFolder();
    const auto names = live.store.trackNames();
    const int nTracks = live.player.numLoadedTracks();
    const double sr = sourceRate (tracksDir);
    const int bs = 512;

    const bool before = im::gDisableBackgroundServices;
    im::gDisableBackgroundServices = true;
    auto proc = std::make_shared<InteractiveMixerProcessor>();
    im::gDisableBackgroundServices = before;
    proc->setRateAndBufferSizeDetails (sr, bs);
    proc->prepareToPlay (sr, bs);
    if (auto e = proc->loadSessionFolder (tracksDir); e.isNotEmpty())
    {
        std::lock_guard<std::mutex> lk (m);
        err = e.toStdString();
        busy = false;
        return true;
    }
    proc->setStateInformation (state.getData(), (int) state.getSize());
    proc->player.setLooping (false);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S");
    const auto dir = tracksDir.getChildFile ("mix_final_ACTUAL exports").getChildFile (stamp);

    worker = std::thread ([this, proc, dir, names, nTracks, sr, bs, stems] {
        auto& p = *proc;
        dir.createDirectory();
        const int latency = p.engine.latencySamples();
        const auto total = (int64_t) ((p.player.lengthSec() + kTailSec) * sr) + latency;
        const int passes = 1 + (stems ? nTracks : 0);
        juce::AudioBuffer<float> io (2 * im::kMaxTracks, bs);
        juce::MidiBuffer midi;

        // One pass: everything from the top (tail included), latency removed.
        auto render = [&] (int pass) {
            juce::AudioBuffer<float> out (2, (int) (total - latency));
            p.engine.prepare (sr, bs); // clears reverb/echo/dynamics state: no tails from the previous pass
            p.player.seek (0.0);
            p.player.setPlaying (true);
            int64_t pos = 0;
            while (pos < total)
            {
                io.clear();
                p.processBlock (io, midi);
                const int n = (int) std::min<int64_t> (bs, total - pos);
                for (int i = 0; i < n; ++i)
                {
                    const int64_t o = pos + i - latency;
                    if (o < 0 || o >= out.getNumSamples()) continue;
                    out.setSample (0, (int) o, io.getSample (0, i));
                    out.setSample (1, (int) o, io.getSample (1, i));
                }
                pos += bs;
                prog = (pass + (double) pos / (double) total) / passes;
            }
            return out;
        };

        bool ok = writeWav (dir.getChildFile ("mix.wav"), render (0), sr);
        if (ok && stems)
        {
            auto stemDir = dir.getChildFile ("stems");
            stemDir.createDirectory();
            p.setParamValue (im::masterParam (im::BcOn), 0.0f);
            p.setParamValue (im::masterParam (im::LimOn), 0.0f);
            for (int t = 0; t < nTracks && ok; ++t)
            {
                for (int u = 0; u < nTracks; ++u) p.setParamValue (im::trackParam (u, im::Solo), u == t ? 1.0f : 0.0f);
                auto nm = juce::String (t + 1).paddedLeft ('0', 2) + " " + safeName (names[(size_t) t]) + ".wav";
                ok = writeWav (stemDir.getChildFile (nm), render (1 + t), sr);
            }
        }
        {
            std::lock_guard<std::mutex> lk (m);
            if (ok) outDir = dir.getFullPathName().toStdString();
            else err = "could not write to " + dir.getFullPathName().toStdString();
        }
        prog = 1.0;
        // The engine must be released on the message thread (it owns timers and parameter listeners).
        juce::MessageManager::callAsync ([proc] {});
        busy = false;
    });
    return true;
}
} // namespace mfa
