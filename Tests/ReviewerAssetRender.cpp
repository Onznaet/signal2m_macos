// Standalone SVG-to-PNG inspection with the pinned JUCE parser; no plug-in/editor.
#include <juce_gui_basics/juce_gui_basics.h>
#include <iostream>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI initialise;
    if (argc != 3)
        return 2;

    const juce::File source (juce::File::getCurrentWorkingDirectory().getChildFile (
        juce::String::fromUTF8 (argv[1])));
    juce::MemoryBlock bytes;
    if (! source.loadFileAsData (bytes))
        return 3;
    auto drawable = juce::Drawable::createFromImageData (bytes.getData(), bytes.getSize());
    if (drawable == nullptr)
        return 4;

    const auto bounds = drawable->getDrawableBounds();
    if (bounds.isEmpty())
        return 5;
    const auto* composite = dynamic_cast<juce::DrawableComposite*> (drawable.get());
    const auto canvas = composite != nullptr ? composite->getContentArea() : bounds;
    const int width = 800;
    const int height = juce::jmax (1, juce::roundToInt (width * canvas.getHeight() / canvas.getWidth()));
    juce::Image rendered (juce::Image::ARGB, width, height, true);
    {
        juce::Graphics graphics (rendered);
        graphics.fillAll (juce::Colour (0xff20251f));
        drawable->draw (graphics, 1.0f, juce::RectanglePlacement (juce::RectanglePlacement::centred)
            .getTransformToFit (canvas, rendered.getBounds().toFloat()));
    }
    const auto output = juce::File::getCurrentWorkingDirectory().getChildFile (
        juce::String::fromUTF8 (argv[2]));
    output.deleteFile();
    auto stream = output.createOutputStream();
    if (stream == nullptr || ! juce::PNGImageFormat().writeImageToStream (rendered, *stream))
        return 6;
    std::cout << "PASS JUCE standalone SVG parse/render " << argv[1]
              << " bounds=" << bounds.toString() << " canvas=" << canvas.toString()
              << " png=" << argv[2] << '\n';
    return 0;
}
