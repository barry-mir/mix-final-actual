#include "Export.h"
#include "PluginProcessor.h"
#include "Render.h"
#include "core/Paths.h"
#include <nlohmann/json.hpp>

namespace mfa
{
using im::InteractiveMixerProcessor;

namespace
{
bool writeAudio (const juce::File& f, const juce::AudioBuffer<float>& b, double sr, const std::string& format)
{
    f.deleteFile();
    std::unique_ptr<juce::AudioFormat> fmt;
    if (format == "flac") fmt = std::make_unique<juce::FlacAudioFormat>();
    else fmt = std::make_unique<juce::WavAudioFormat>();
    auto os = f.createOutputStream();
    if (! os) return false;
    std::unique_ptr<juce::AudioFormatWriter> w (fmt->createWriterFor (os.get(), sr, 2, 24, {}, format == "flac" ? 5 : 0));
    if (! w) return false;
    os.release();
    return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

juce::String safeName (const std::string& s)
{
    return juce::File::createLegalFileName (juce::String (s)).substring (0, 60);
}

juce::File settingsFile() { return im::appDataDir().getChildFile ("settings.json"); }
} // namespace

std::string exportTo (InteractiveMixerProcessor& p, double sr, const ExportOptions& opts, double& lufs,
                      const std::function<void (double)>& progress)
{
    const auto names = p.store.trackNames();
    const int nTracks = p.player.numLoadedTracks();
    const auto& file = opts.file;
    const auto dir = file.getParentDirectory();
    auto report = [&] (double f) { if (progress) progress (f); return true; };
    const bool master = std::isfinite (opts.loudnessLufs);
    const int mixPasses = master && ! opts.knownGainDb ? 4 : 1;
    const int passes = mixPasses + (opts.stems ? nTracks : 0);
    juce::AudioBuffer<float> mix;
    bool ok = true;
    if (master)
    {
        auto r = masterTo (p, sr, opts.loudnessLufs, opts.knownGainDb.value_or (0.0), opts.knownGainDb.has_value(),
                           [&] (double f) { return report (f * mixPasses / passes); });
        mix = std::move (r.mix);
    }
    else
        ok = renderSong (p, sr, mix, [&] (double f) { return report (f / passes); });
    lufs = loudnessOf (mix, sr);
    ok = ok && writeAudio (file, mix, sr, opts.format);
    mix.setSize (0, 0);
    p.engine.deliveryGainDb = 0.0f; // stems as mixed
    p.engine.forceLimiter = false;
    if (ok && opts.stems)
    {
        auto stemDir = dir.getChildFile (file.getFileNameWithoutExtension() + " stems");
        stemDir.createDirectory();
        p.setParamValue (im::masterParam (im::BcOn), 0.0f);
        p.setParamValue (im::masterParam (im::LimOn), 0.0f);
        for (int t = 0; t < nTracks && ok; ++t)
        {
            for (int u = 0; u < nTracks; ++u) p.setParamValue (im::trackParam (u, im::Solo), u == t ? 1.0f : 0.0f);
            auto nm = juce::String (t + 1).paddedLeft ('0', 2) + " " + safeName (names[(size_t) t]) + juce::String (opts.format == "flac" ? ".flac" : ".wav");
            juce::AudioBuffer<float> stem;
            ok = renderSong (p, sr, stem, [&] (double f) { return report ((mixPasses + t + f) / passes); })
                 && writeAudio (stemDir.getChildFile (nm), stem, sr, opts.format);
        }
    }
    report (1.0);
    return ok ? std::string() : "could not write to " + dir.getFullPathName().toStdString();
}

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

std::string Exporter::file() const
{
    std::lock_guard<std::mutex> lk (m);
    return outFile;
}

std::string Exporter::error() const
{
    std::lock_guard<std::mutex> lk (m);
    return err;
}

juce::File Exporter::defaultFile (const juce::File& tracksDir, const std::string& projectName)
{
    const auto base = ! juce::String (projectName).trim().isEmpty() ? juce::String (projectName).trim()
                      : tracksDir.getFileName().isNotEmpty() ? tracksDir.getFileName() : juce::String ("mix");
    const auto name = juce::File::createLegalFileName (base);
    juce::File dir;
    auto j = nlohmann::json::parse (settingsFile().loadFileAsString().toStdString(), nullptr, false);
    if (j.is_object() && j.contains ("export_dir") && j["export_dir"].is_string())
        dir = juce::File (juce::String (j["export_dir"].get<std::string>()));
    if (dir == juce::File() || ! dir.isDirectory())
        dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("mix_final_ACTUAL").getChildFile (name);
    return dir.getChildFile (name + ".wav");
}

void Exporter::rememberFolder (const juce::File& dir)
{
    auto f = settingsFile();
    auto j = nlohmann::json::parse (f.loadFileAsString().toStdString(), nullptr, false);
    if (! j.is_object()) j = nlohmann::json::object();
    j["export_dir"] = dir.getFullPathName().toStdString();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (j.dump (1));
}

bool Exporter::start (InteractiveMixerProcessor& live, ExportOptions opts)
{
    if (busy.exchange (true)) return false;
    join();
    prog = 0.0;
    {
        std::lock_guard<std::mutex> lk (m);
        err.clear();
        outDir.clear();
        outFile.clear();
    }
    auto fail = [this] (std::string e) {
        std::lock_guard<std::mutex> lk (m);
        err = std::move (e);
        busy = false;
        return true;
    };
    double sr = 48000.0;
    std::string e;
    auto proc = makeOfflineEngine (live, sr, e);
    if (! proc) return fail (e);

    const auto ext = opts.format == "flac" ? juce::String (".flac") : juce::String (".wav");
    auto file = opts.file.withFileExtension (ext);
    const auto dir = file.getParentDirectory();
    if (! dir.createDirectory())
    {
        releaseOnMessageThread (proc);
        return fail ("cannot create " + dir.getFullPathName().toStdString());
    }
    rememberFolder (dir);

    opts.file = file;
    worker = std::thread ([this, proc, dir, sr, opts] {
        double lufs = 0.0;
        const auto res = exportTo (*proc, sr, opts, lufs, [this] (double f) { prog = f; });
        lufsOut = lufs;
        const bool ok = res.empty();
        {
            std::lock_guard<std::mutex> lk (m);
            if (ok)
            {
                outDir = dir.getFullPathName().toStdString();
                outFile = opts.file.getFullPathName().toStdString();
            }
            else
                err = res;
        }
        prog = 1.0;
        releaseOnMessageThread (proc);
        busy = false;
    });
    return true;
}
} // namespace mfa
