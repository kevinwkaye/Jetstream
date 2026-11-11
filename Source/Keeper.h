#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <deque>
#include <mutex>

// KEEPER: logs the moments Kevin loves, so his signature suite can be designed from
// real evidence. Shared code: every Flip Fella plugin writes the same format into one
// folder, Documents/Flip Fella Keepers, as one JSON log (keepers.json) plus WAV files.
//
// The audio thread only copies samples into two ring buffers. Everything else, from
// snapshotting the last 8 seconds to writing files, happens off the audio thread.
class Keeper : private juce::Thread
{
public:
    static constexpr double kSeconds = 8.0;

    Keeper();
    ~Keeper() override;

    static juce::File folder();
    static juce::File logFile() { return folder().getChildFile ("keepers.json"); }

    void prepare (double sampleRate);

    // ---- audio thread: cheap copies only ----
    void pushInput (const float* l, const float* r, int n) { push (in, inPos, l, r, n); }
    void pushOutput (const float* l, const float* r, int n) { push (out, outPos, l, r, n); }

    // ---- message thread ----
    // entry holds everything about the moment except the audio and numbering, which Keeper adds
    void keep (juce::DynamicObject::Ptr entry);
    void addNote (int number, const juce::String& note);
    juce::File exportZip();           // returns the zip it wrote, or an empty File
    int lastNumber() const { return last.load(); }
    int keptThisSession() const { return session.load(); }
    bool busy() const { return pendingJobs.load() > 0; }
    juce::String lastError() const;

private:
    struct Ring
    {
        std::vector<float> l, r;
    };
    struct Job
    {
        enum Kind { Keep, Note } kind = Keep;
        juce::DynamicObject::Ptr entry;
        juce::AudioBuffer<float> inAudio, outAudio;
        int number = 0;
        juce::String note;
    };

    void push (Ring& ring, std::atomic<int>& pos, const float* l, const float* r, int n);
    void snapshot (const Ring& ring, int end, juce::AudioBuffer<float>& dest) const;
    void run() override;
    void writeKeep (Job&);
    void writeNote (const Job&);
    juce::var readLog() const;
    bool writeLog (const juce::var&);
    void setError (const juce::String&);

    Ring in, out;
    std::atomic<int> inPos { 0 }, outPos { 0 };
    int size = 0;
    double rate = 48000.0;

    std::mutex lock;
    std::deque<std::unique_ptr<Job>> jobs;
    std::atomic<int> pendingJobs { 0 }, last { 0 }, session { 0 };
    juce::String error;
    mutable std::mutex errorLock;
};
