// Renders the Jetstream editor to PNG files for a visual check (developer tool, not part of CI).
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

static void pump (JetstreamProcessor& p, int blocks)
{
    juce::AudioBuffer<float> b (2, 512);
    juce::MidiBuffer m;
    static long n = 0;
    for (int k = 0; k < blocks; ++k)
    {
        for (int i = 0; i < 512; ++i, ++n)
        {
            const double t = (double) (n % 24000) / 48000.0;
            const float x = (float) (0.3 * std::exp (-3.0 * t) * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * (double) n / 48000.0));
            b.setSample (0, i, x); b.setSample (1, i, x);
        }
        p.processBlock (b, m);
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File outDir (argc > 1 ? argv[1] : ".");
    JetstreamProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    ed->setVisible (true);
    auto run = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
    auto shot = [&] (juce::Component& c, const juce::String& name)
    {
        run (700);
        auto img = c.createComponentSnapshot (c.getLocalBounds());
        juce::PNGImageFormat png;
        juce::FileOutputStream os (outDir.getChildFile (name + ".png"));
        os.setPosition (0); os.truncate();
        png.writeImageToStream (img, os);
    };

    proc.loadPreset (1);                  // AMBIENT BED
    pump (proc, 200);
    shot (*ed, "main_ambient");

    proc.loadPreset (13);                 // SELF REMIX
    pump (proc, 600);
    proc.footDown (2); run (50); proc.footUp (2); run (50);
    proc.keepNow();
    shot (*ed, "main_remix");

    for (auto* c : ed->getChildren())
        if (auto* page = dynamic_cast<ui::PedalPage*> (c)) { page->setVisible (true); page->toFront (false); }
    shot (*ed, "pedal_page");
    return 0;
}
