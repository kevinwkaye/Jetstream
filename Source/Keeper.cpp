#include "Keeper.h"

using namespace juce;

// Several plugins (and several copies of one) can keep at once, from different processes.
static const char* const kLockName = "FlipFellaKeepersLog";

Keeper::Keeper() : Thread ("Keeper writer") { startThread (Priority::low); }

Keeper::~Keeper()
{
    signalThreadShouldExit();
    notify();
    stopThread (4000);
}

File Keeper::folder()
{
    return File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Flip Fella Keepers");
}

void Keeper::prepare (double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int want = (int) (kSeconds * rate);
    if (want != size)
    {
        size = want;
        for (auto* ring : { &in, &out })
        {
            ring->l.assign ((size_t) size, 0.0f);
            ring->r.assign ((size_t) size, 0.0f);
        }
        inPos.store (0);
        outPos.store (0);
    }
}

void Keeper::push (Ring& ring, std::atomic<int>& pos, const float* l, const float* r, int n)
{
    if (size <= 0) return;
    int p = pos.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        ring.l[(size_t) p] = l[i];
        ring.r[(size_t) p] = r[i];
        if (++p >= size) p = 0;
    }
    pos.store (p, std::memory_order_release);
}

void Keeper::snapshot (const Ring& ring, int end, AudioBuffer<float>& dest) const
{
    // oldest sample first, ending at the newest
    dest.setSize (2, size);
    for (int i = 0; i < size; ++i)
    {
        const int s = (end + i) % size;
        dest.setSample (0, i, ring.l[(size_t) s]);
        dest.setSample (1, i, ring.r[(size_t) s]);
    }
}

void Keeper::keep (DynamicObject::Ptr entry)
{
    auto job = std::make_unique<Job>();
    job->kind = Job::Keep;
    job->entry = entry;
    if (size > 0)
    {
        snapshot (in, inPos.load (std::memory_order_acquire), job->inAudio);
        snapshot (out, outPos.load (std::memory_order_acquire), job->outAudio);
    }
    {
        const std::lock_guard<std::mutex> g (lock);
        jobs.push_back (std::move (job));
    }
    pendingJobs.fetch_add (1);
    session.fetch_add (1);
    notify();
}

void Keeper::addNote (int number, const String& note)
{
    if (number <= 0 || note.trim().isEmpty()) return;
    auto job = std::make_unique<Job>();
    job->kind = Job::Note;
    job->number = number;
    job->note = note.trim().substring (0, 200);
    {
        const std::lock_guard<std::mutex> g (lock);
        jobs.push_back (std::move (job));
    }
    pendingJobs.fetch_add (1);
    notify();
}

void Keeper::run()
{
    while (! threadShouldExit())
    {
        std::unique_ptr<Job> job;
        {
            const std::lock_guard<std::mutex> g (lock);
            if (! jobs.empty()) { job = std::move (jobs.front()); jobs.pop_front(); }
        }
        if (job == nullptr) { wait (500); continue; }
        if (job->kind == Job::Keep) writeKeep (*job);
        else writeNote (*job);
        pendingJobs.fetch_sub (1);
    }
}

var Keeper::readLog() const
{
    const auto f = logFile();
    if (! f.existsAsFile()) return var (Array<var>());
    auto v = JSON::parse (f);
    if (v.isObject() && v.getProperty ("keepers", var()).isArray()) return v;
    // an unreadable log is set aside rather than overwritten
    if (f.getSize() > 0) f.copyFileTo (f.getSiblingFile ("keepers-unreadable-" + Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".json"));
    return var (Array<var>());
}

bool Keeper::writeLog (const var& log)
{
    const auto f = logFile();
    TemporaryFile tmp (f);
    {
        FileOutputStream os (tmp.getFile());
        if (! os.openedOk()) return false;
        JSON::writeToStream (os, log, JSON::FormatOptions{}.withSpacing (JSON::Spacing::multiLine));
    }
    return tmp.overwriteTargetFileWithTemporary();
}

static bool writeWav (const File& f, const AudioBuffer<float>& audio, double rate)
{
    f.deleteFile();
    auto stream = std::make_unique<FileOutputStream> (f);
    if (! stream->openedOk()) return false;
    WavAudioFormat wav;
    std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (stream.get(), rate, 2, 16, {}, 0));
    if (w == nullptr) return false;
    stream.release();
    return w->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

void Keeper::writeKeep (Job& job)
{
    const auto dir = folder();
    const auto audioDir = dir.getChildFile ("audio");
    if (! audioDir.createDirectory())
    {
        setError ("Couldn't create " + audioDir.getFullPathName());
        return;
    }

    InterProcessLock ipl (kLockName);
    const InterProcessLock::ScopedLockType held (ipl);

    auto log = readLog();
    auto* root = log.getDynamicObject();
    var list = log.isArray() ? log : (root != nullptr ? root->getProperty ("keepers") : var (Array<var>()));
    if (! list.isArray()) list = var (Array<var>());
    int number = 0;
    for (const auto& e : *list.getArray()) number = jmax (number, (int) e.getProperty ("number", 0));
    ++number;

    const String stamp = Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    const String plugin = job.entry->getProperty ("plugin").toString().removeCharacters (" ");
    const String base = String (number).paddedLeft ('0', 4) + "-" + plugin + "-" + stamp;
    DynamicObject::Ptr audio = new DynamicObject();
    if (job.inAudio.getNumSamples() > 0)
    {
        const auto fin = audioDir.getChildFile (base + "-in.wav"), fout = audioDir.getChildFile (base + "-out.wav");
        if (writeWav (fin, job.inAudio, rate) && writeWav (fout, job.outAudio, rate))
        {
            audio->setProperty ("input", "audio/" + fin.getFileName());
            audio->setProperty ("output", "audio/" + fout.getFileName());
            audio->setProperty ("seconds", kSeconds);
            audio->setProperty ("sampleRate", rate);
        }
        else setError ("Couldn't write the audio for Keeper " + String (number));
    }

    job.entry->setProperty ("number", number);
    job.entry->setProperty ("audio", var (audio.get()));
    if (! job.entry->hasProperty ("note")) job.entry->setProperty ("note", "");
    list.getArray()->add (var (job.entry.get()));

    DynamicObject::Ptr out = new DynamicObject();
    out->setProperty ("format", "Flip Fella Keepers 1");
    out->setProperty ("keepers", list);
    if (writeLog (var (out.get()))) last.store (number);
    else setError ("Couldn't write " + logFile().getFullPathName());
}

void Keeper::writeNote (const Job& job)
{
    InterProcessLock ipl (kLockName);
    const InterProcessLock::ScopedLockType held (ipl);
    auto log = readLog();
    auto* root = log.getDynamicObject();
    if (root == nullptr) return;
    auto list = root->getProperty ("keepers");
    if (! list.isArray()) return;
    for (auto& e : *list.getArray())
        if ((int) e.getProperty ("number", 0) == job.number)
            if (auto* o = e.getDynamicObject()) o->setProperty ("note", job.note);
    if (! writeLog (log)) setError ("Couldn't save the note");
}

File Keeper::exportZip()
{
    const auto dir = folder();
    if (! logFile().existsAsFile()) return {};
    InterProcessLock ipl (kLockName);
    const InterProcessLock::ScopedLockType held (ipl);
    ZipFile::Builder zip;
    zip.addFile (logFile(), 9, "keepers.json");
    for (const auto& f : dir.getChildFile ("audio").findChildFiles (File::findFiles, false, "*.wav"))
        zip.addFile (f, 6, "audio/" + f.getFileName());
    const auto target = File::getSpecialLocation (File::userDocumentsDirectory)
                            .getChildFile ("Flip Fella Keepers export " + Time::getCurrentTime().formatted ("%Y-%m-%d %H%M") + ".zip");
    target.deleteFile();
    FileOutputStream os (target);
    if (! os.openedOk() || ! zip.writeToStream (os, nullptr)) { setError ("Couldn't write the export"); return {}; }
    return target;
}

void Keeper::setError (const String& e)
{
    const std::lock_guard<std::mutex> g (errorLock);
    error = e;
}

String Keeper::lastError() const
{
    const std::lock_guard<std::mutex> g (errorLock);
    return error;
}
