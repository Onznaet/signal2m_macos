// Independent macOS host for the actual VST3 bundle. No editor or DAW.
// CFBundle sequence follows the bundled Steinberg module_mac.mm host.
#include <CoreFoundation/CoreFoundation.h>
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace Steinberg;
using namespace Steinberg::Vst;
int failures = 0, assertions = 0;
void expect(bool value, const std::string& description)
{
    ++assertions;
    std::cout << (value ? "PASS " : "FAIL ") << description << '\n';
    if (!value) ++failures;
}
void require(bool value, const std::string& description)
{
    expect(value, description);
    if (!value) throw std::runtime_error(description);
}
bool optionalControllerState(tresult value) { return value == kResultOk || value == kNotImplemented; }
std::string ascii(const TChar* input)
{
    std::string result;
    for (int i = 0; i < 128 && input[i] != 0; ++i)
        result.push_back(input[i] <= 127 ? static_cast<char>(input[i]) : '?');
    return result;
}
template <class T> T* query(FUnknown* source)
{
    T* target = nullptr;
    if (source != nullptr) source->queryInterface(T::iid, reinterpret_cast<void**>(&target));
    return target;
}
class MemoryStream final : public IBStream
{
public:
    std::vector<char> bytes;
    std::size_t position = 0;
    tresult PLUGIN_API queryInterface(const TUID id, void** result) override
    {
        if (FUnknownPrivate::iidEqual(id, FUnknown::iid) || FUnknownPrivate::iidEqual(id, IBStream::iid))
        { *result = static_cast<IBStream*>(this); addRef(); return kResultOk; }
        *result = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API read(void* destination, int32 size, int32* count) override
    {
        if (size < 0 || destination == nullptr) return kInvalidArgument;
        const auto available = position < bytes.size() ? bytes.size() - position : 0;
        const auto n = std::min<std::size_t>(static_cast<std::size_t>(size), available);
        if (n != 0) std::memcpy(destination, bytes.data() + position, n);
        position += n;
        if (count) *count = static_cast<int32>(n);
        // Match the official SDK MemoryStream: EOF is reported through bytesRead.
        return kResultOk;
    }
    tresult PLUGIN_API write(void* source, int32 size, int32* count) override
    {
        if (size < 0 || source == nullptr) return kInvalidArgument;
        const auto n = static_cast<std::size_t>(size);
        if (position + n > bytes.size()) bytes.resize(position + n);
        if (n != 0) std::memcpy(bytes.data() + position, source, n);
        position += n;
        if (count) *count = size;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 offset, int32 origin, int64* result) override
    {
        int64 base = 0;
        if (origin == kIBSeekCur) base = static_cast<int64>(position);
        else if (origin == kIBSeekEnd) base = static_cast<int64>(bytes.size());
        else if (origin != kIBSeekSet) return kInvalidArgument;
        if (base + offset < 0) return kInvalidArgument;
        position = static_cast<std::size_t>(base + offset);
        if (result) *result = static_cast<int64>(position);
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* result) override
    { if (!result) return kInvalidArgument; *result = static_cast<int64>(position); return kResultOk; }
};
class Queue final : public IParamValueQueue
{
public:
    explicit Queue(ParamID parameter) : id(parameter) {}
    ParamID id;
    std::vector<std::pair<int32, ParamValue>> points;
    tresult PLUGIN_API queryInterface(const TUID wanted, void** result) override
    {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) || FUnknownPrivate::iidEqual(wanted, IParamValueQueue::iid))
        { *result = static_cast<IParamValueQueue*>(this); return kResultOk; }
        *result = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return static_cast<int32>(points.size()); }
    tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& value) override
    {
        if (index < 0 || static_cast<std::size_t>(index) >= points.size()) return kInvalidArgument;
        offset = points[static_cast<std::size_t>(index)].first;
        value = points[static_cast<std::size_t>(index)].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, ParamValue value, int32& index) override
    {
        index = static_cast<int32>(points.size()); points.emplace_back(offset, value); return kResultOk;
    }
};
class Changes final : public IParameterChanges
{
public:
    std::vector<std::unique_ptr<Queue>> queues;
    tresult PLUGIN_API queryInterface(const TUID wanted, void** result) override
    {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) || FUnknownPrivate::iidEqual(wanted, IParameterChanges::iid))
        { *result = static_cast<IParameterChanges*>(this); return kResultOk; }
        *result = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getParameterCount() override { return static_cast<int32>(queues.size()); }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override
    {
        return index < 0 || static_cast<std::size_t>(index) >= queues.size() ? nullptr : queues[static_cast<std::size_t>(index)].get();
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override
    {
        for (std::size_t i = 0; i < queues.size(); ++i)
            if (queues[i]->id == id) { index = static_cast<int32>(i); return queues[i].get(); }
        index = static_cast<int32>(queues.size());
        queues.push_back(std::make_unique<Queue>(id));
        return queues.back().get();
    }
    void add(ParamID id, double value, int32 offset = 0)
    {
        int32 index = 0;
        auto* q = addParameterData(id, index);
        q->addPoint(offset, value, index);
    }
};
enum class MidiKind { on, off, cc };
struct MidiAction
{
    std::size_t sample;
    MidiKind kind;
    int pitch;
    float value = 1.0f;
    int channel = 0; // VST3 zero-based MIDI channel, unlike JUCE's 1..16.
};
class Events final : public IEventList
{
public:
    std::vector<Event> events;
    tresult PLUGIN_API queryInterface(const TUID wanted, void** result) override
    {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) || FUnknownPrivate::iidEqual(wanted, IEventList::iid))
        { *result = static_cast<IEventList*>(this); return kResultOk; }
        *result = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getEventCount() override { return static_cast<int32>(events.size()); }
    tresult PLUGIN_API getEvent(int32 index, Event& output) override
    {
        if (index < 0 || static_cast<std::size_t>(index) >= events.size()) return kInvalidArgument;
        output = events[static_cast<std::size_t>(index)]; return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event& event) override { events.push_back(event); return kResultOk; }
};
template <class T> class CFHandle
{
public:
    explicit CFHandle(T initial = nullptr) : value(initial) {}
    ~CFHandle() { reset(); }
    CFHandle(const CFHandle&) = delete;
    CFHandle& operator=(const CFHandle&) = delete;
    T get() const noexcept { return value; }
    void reset(T replacement = nullptr) noexcept
    {
        if (value) CFRelease(value);
        value = replacement;
    }
private:
    T value;
};
std::string cfText(CFStringRef value)
{
    if (!value) return {};
    const auto capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    if (capacity <= 0) return {};
    std::vector<char> bytes(static_cast<std::size_t>(capacity));
    return CFStringGetCString(value, bytes.data(), capacity, kCFStringEncodingUTF8) ? bytes.data() : std::string{};
}
struct Module
{
    CFHandle<CFBundleRef> bundle;
    IPluginFactory* factory = nullptr;
    bool initialised = false;
    std::array<char, 16> componentClass{};
    using Entry = bool (*)(CFBundleRef);
    using Exit = bool (*)();
    Exit exit = nullptr;
    template <class T> T symbol(CFStringRef name) const
    { return reinterpret_cast<T>(CFBundleGetFunctionPointerForName(bundle.get(), name)); }
    std::string information(CFStringRef key) const
    {
        const auto value = CFBundleGetValueForInfoDictionaryKey(bundle.get(), key);
        return value && CFGetTypeID(value) == CFStringGetTypeID() ? cfText(static_cast<CFStringRef>(value)) : std::string{};
    }
    explicit Module(const std::filesystem::path& suppliedPath)
    {
        try
        {
            const auto path = std::filesystem::absolute(suppliedPath).lexically_normal();
            require(path.filename() == "Signal2M.vst3" && std::filesystem::is_directory(path),
                    "actual Signal2M.vst3 bundle directory supplied");
            require(std::filesystem::is_regular_file(path / "Contents" / "Info.plist"), "bundle contains Info.plist");
            require(std::filesystem::is_regular_file(path / "Contents" / "MacOS" / "Signal2M"), "bundle contains native executable");
            const auto utf8Path = path.u8string();
            std::cout << "MODULE " << utf8Path << '\n';
            CFHandle<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                reinterpret_cast<const UInt8*>(utf8Path.data()), static_cast<CFIndex>(utf8Path.size()), true));
            require(url.get() != nullptr, "create UTF-8 bundle URL");
            bundle.reset(CFBundleCreate(kCFAllocatorDefault, url.get()));
            require(bundle.get() != nullptr, "CFBundleCreate for real module");
            expect(information(CFSTR("CFBundleIdentifier")) == "sound.lmz.signal2m", "stable macOS bundle identifier");
            expect(information(CFSTR("CFBundleExecutable")) == "Signal2M", "physical executable name preserved");
            expect(information(CFSTR("CFBundleName")) == "Signal2M", "physical bundle name preserved");
            expect(information(CFSTR("CFBundlePackageType")) == "BNDL", "VST3 has bundle package type");
            expect(information(CFSTR("CFBundleShortVersionString")) == "0.3.1", "bundle marketing version is 0.3.1");
            expect(information(CFSTR("CFBundleVersion")) == "0.3.1", "bundle build version is 0.3.1");
            CFHandle<CFURLRef> executable(CFBundleCopyExecutableURL(bundle.get()));
            require(executable.get() != nullptr, "CoreFoundation locates native executable");
            // The executable URL may be a filename relative to a base URL.
            // CFURLCopyFileSystemPath alone does not resolve that base.
            CFHandle<CFURLRef> absoluteExecutable(CFURLCopyAbsoluteURL(executable.get()));
            require(absoluteExecutable.get() != nullptr, "resolve native executable URL against its base");
            CFHandle<CFStringRef> executablePath(CFURLCopyFileSystemPath(absoluteExecutable.get(), kCFURLPOSIXPathStyle));
            require(executablePath.get() != nullptr, "copy resolved native executable POSIX path");
            const auto resolvedExecutable = std::filesystem::u8path(cfText(executablePath.get()));
            require(resolvedExecutable.is_absolute(), "CFBundle executable path is absolute: " + resolvedExecutable.u8string());
            require(std::filesystem::equivalent(resolvedExecutable,
                    path / "Contents" / "MacOS" / "Signal2M"), "CFBundle executable resolves inside the supplied bundle");
            CFErrorRef rawError = nullptr;
            const bool loaded = CFBundleLoadExecutableAndReturnError(bundle.get(), &rawError);
            CFHandle<CFErrorRef> error(rawError);
            CFHandle<CFStringRef> description(rawError ? CFErrorCopyDescription(rawError) : nullptr);
            require(loaded, "load actual native bundle: " + cfText(description.get()));
            require(CFBundleIsExecutableLoaded(bundle.get()), "CFBundle executable is loaded");
            const auto entry = symbol<Entry>(CFSTR("bundleEntry"));
            exit = symbol<Exit>(CFSTR("bundleExit"));
            const auto getFactory = symbol<IPluginFactory* (*)()>(CFSTR("GetPluginFactory"));
            require(entry != nullptr && exit != nullptr, "native bundleEntry/bundleExit exports");
            require(getFactory != nullptr, "GetPluginFactory export");
            initialised = entry(bundle.get());
            require(initialised, "bundleEntry accepts the real CFBundle");
            factory = getFactory();
            require(factory != nullptr, "factory returned");
            PFactoryInfo information{};
            require(factory->getFactoryInfo(&information) == kResultOk, "factory metadata");
            expect(std::strcmp(information.vendor, "LMZ") == 0, "factory vendor remains LMZ");
            std::cout << "FACTORY vendor=" << information.vendor << " classes=" << factory->countClasses() << '\n';
            int found = 0;
            for (int32 index = 0; index < factory->countClasses(); ++index)
            {
                PClassInfo info{};
                require(factory->getClassInfo(index, &info) == kResultOk, "factory class metadata");
                std::cout << "CLASS name=" << info.name << " category=" << info.category << '\n';
                if (std::strcmp(info.category, kVstAudioEffectClass) == 0)
                { std::memcpy(componentClass.data(), info.cid, 16); ++found; }
            }
            require(found == 1, "exactly one audio processing class advertised");
        }
        catch (...)
        {
            close();
            throw;
        }
    }
    void close() noexcept
    {
        if (factory) { factory->release(); factory = nullptr; }
        if (initialised && exit) expect(exit(), "bundleExit completes after all instances are released");
        initialised = false;
        exit = nullptr;
        // Objective-C bundles need not be unloadable. Release our CFBundle retain;
        // claiming executable unmapping here would conceal callback-lifetime bugs.
        bundle.reset();
    }
    ~Module() { close(); }
};
struct Instance
{
    HostApplication host;
    IComponent* component = nullptr;
    IAudioProcessor* processor = nullptr;
    IEditController* controller = nullptr;
    IConnectionPoint* processorConnection = nullptr;
    IConnectionPoint* controllerConnection = nullptr;
    bool controllerSeparate = true, active = false;
    double sampleRate = 48000.0;
    int64 framePosition = 0;
    int outputChannels = 2;
    int maximumBlock = 1024;
    std::vector<ParameterInfo> parameters;
    std::map<std::string, ParamID> ids;
    explicit Instance(Module& module)
    {
        require(module.factory->createInstance(module.componentClass.data(), IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk && component,
                "create IComponent");
        TUID controllerClass{};
        const bool hasControllerClass = component->getControllerClassId(controllerClass) == kResultOk;
        require(component->initialize(&host) == kResultOk, "initialize component with technical host");
        processor = query<IAudioProcessor>(component);
        require(processor != nullptr, "query IAudioProcessor");
        controller = query<IEditController>(component);
        controllerSeparate = controller == nullptr;
        if (!controller)
        {
            require(hasControllerClass, "separate controller class supplied");
            require(module.factory->createInstance(controllerClass, IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk && controller,
                    "create IEditController");
            require(controller->initialize(&host) == kResultOk, "initialize controller");
        }
        processorConnection = query<IConnectionPoint>(component);
        controllerConnection = query<IConnectionPoint>(controller);
        if (processorConnection && controllerConnection)
        {
            require(processorConnection->connect(controllerConnection) == kResultOk, "connect component to controller");
            require(controllerConnection->connect(processorConnection) == kResultOk, "connect controller to component");
        }
        MemoryStream initial;
        require(component->getState(&initial) == kResultOk, "initial component state");
        initial.position = 0;
        require(optionalControllerState(controller->setComponentState(&initial)), "initial controller state synchronisation");
        for (int32 i = 0; i < controller->getParameterCount(); ++i)
        {
            ParameterInfo info{};
            if (controller->getParameterInfo(i, info) != kResultOk) throw std::runtime_error("parameter metadata unavailable");
            parameters.push_back(info);
            ids[ascii(info.title)] = info.id;
        }
    }
    ~Instance()
    {
        stop();
        if (processorConnection && controllerConnection)
        {
            controllerConnection->disconnect(processorConnection);
            processorConnection->disconnect(controllerConnection);
        }
        if (controllerConnection) controllerConnection->release();
        if (processorConnection) processorConnection->release();
        if (controller && controllerSeparate) controller->terminate();
        if (controller) controller->release();
        if (component) component->terminate();
        if (processor) processor->release();
        if (component) component->release();
    }
    ParamID id(const std::string& name) const
    {
        const auto found = ids.find(name);
        if (found == ids.end()) throw std::runtime_error("Missing parameter: " + name);
        return found->second;
    }
    double plain(const std::string& name, double value) const
    {
        // JUCE's VST3 plain conversion intentionally returns normalised values;
        // physical text is the portable host-facing route to Hz/ms parameters.
        const auto number = std::to_string(value);
        String128 text{};
        for (std::size_t i = 0; i < number.size() && i < 127; ++i) text[i] = static_cast<TChar>(number[i]);
        ParamValue normalised = 0.0;
        if (controller->getParamValueByString(id(name), text, normalised) != kResultOk)
            throw std::runtime_error("Cannot parse physical parameter value: " + name);
        return normalised;
    }
    void start(double sr, int maxBlock = 1024, int channels = 2)
    {
        stop();
        SpeakerArrangement arrangement = channels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
        require(processor->setBusArrangements(nullptr, 0, &arrangement, 1) == kResultOk, "set output arrangement channels=" + std::to_string(channels));
        ProcessSetup setup{};
        setup.processMode = kRealtime; setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = maxBlock; setup.sampleRate = sr;
        require(processor->setupProcessing(setup) == kResultOk, "setupProcessing sr=" + std::to_string(static_cast<int>(sr)));
        require(component->activateBus(kAudio, kOutput, 0, true) == kResultOk, "activate output bus");
        if (component->getBusCount(kEvent, kInput) > 0)
            require(component->activateBus(kEvent, kInput, 0, true) == kResultOk, "activate MIDI event input");
        require(component->setActive(true) == kResultOk, "activate component");
        require(processor->setProcessing(true) == kResultOk, "start processing");
        sampleRate = sr; outputChannels = channels; maximumBlock = maxBlock; framePosition = 0; active = true;
    }
    void stop()
    {
        if (!active) return;
        processor->setProcessing(false); component->setActive(false); active = false;
    }
    ParamID midiController(int channel, int controllerNumber)
    {
        auto* mapping = query<IMidiMapping>(controller);
        require(mapping != nullptr, "IMidiMapping available for channel-mode messages");
        ParamID result = 0;
        const auto status = mapping->getMidiControllerAssignment(0, static_cast<int16>(channel), static_cast<CtrlNumber>(controllerNumber), result);
        mapping->release();
        require(status == kResultOk, "MIDI CC mapped channel=" + std::to_string(channel) + " controller=" + std::to_string(controllerNumber));
        return result;
    }
    std::vector<float> render(double seconds, const std::vector<int>& chunks = {257}, Changes* initialChanges = nullptr,
                              double tempo = 120.0, int channels = 0, const std::vector<MidiAction>* schedule = nullptr, bool playing = true)
    {
        if (channels == 0) channels = outputChannels;
        const auto count = static_cast<std::size_t>(std::llround(seconds * sampleRate));
        std::vector<float> output(count), right(2048);
        std::size_t at = 0, chunk = 0;
        while (at < count)
        {
            const int n = static_cast<int>(std::min<std::size_t>(std::min(maximumBlock, chunks[chunk++ % chunks.size()]), count - at));
            if (right.size() < static_cast<std::size_t>(n)) right.resize(static_cast<std::size_t>(n));
            Sample32* pointers[2] = {output.data() + at, right.data()};
            AudioBusBuffers buffers{};
            buffers.numChannels = channels; buffers.channelBuffers32 = pointers;
            ProcessContext context{};
            context.sampleRate = sampleRate; context.projectTimeSamples = framePosition + static_cast<int64>(at);
            context.tempo = tempo; context.state = ProcessContext::kTempoValid | (playing ? ProcessContext::kPlaying : 0);
            Changes changes;
            if (at == 0 && initialChanges)
                for (const auto& queue : initialChanges->queues)
                    for (const auto& point : queue->points) changes.add(queue->id, point.second, point.first);
            Events events;
            if (schedule)
                for (const auto& action : *schedule)
                {
                    if (action.sample < at || action.sample >= at + static_cast<std::size_t>(n)) continue;
                    const auto offset = static_cast<int32>(action.sample - at);
                    if (action.kind == MidiKind::cc)
                    {
                        changes.add(midiController(action.channel, action.pitch), action.value, offset);
                        continue;
                    }
                    Event event{};
                    event.busIndex = 0; event.sampleOffset = offset; event.flags = Event::kIsLive;
                    if (action.kind == MidiKind::on)
                    {
                        event.type = Event::kNoteOnEvent;
                        event.noteOn.channel = static_cast<int16>(action.channel);
                        event.noteOn.pitch = static_cast<int16>(action.pitch);
                        event.noteOn.velocity = action.value;
                        event.noteOn.length = 10; // Optional hint must not replace an explicit NoteOff.
                        event.noteOn.noteId = -1;
                    }
                    else
                    {
                        event.type = Event::kNoteOffEvent;
                        event.noteOff.channel = static_cast<int16>(action.channel);
                        event.noteOff.pitch = static_cast<int16>(action.pitch);
                        event.noteOff.velocity = action.value;
                        event.noteOff.noteId = -1;
                    }
                    events.addEvent(event);
                }
            ProcessData data{};
            data.processMode = kRealtime; data.symbolicSampleSize = kSample32;
            data.numSamples = n; data.numOutputs = 1; data.outputs = &buffers;
            data.inputParameterChanges = changes.queues.empty() ? nullptr : &changes;
            data.inputEvents = component->getBusCount(kEvent, kInput) > 0 ? &events : nullptr;
            data.processContext = &context;
            if (processor->process(data) != kResultOk) throw std::runtime_error("process() rejected valid note/event data");
            for (int i = 0; i < n; ++i)
                if (!std::isfinite(output[at + static_cast<std::size_t>(i)]) || std::abs(output[at + static_cast<std::size_t>(i)]) > 1.0f
                    || (channels > 1 && output[at + static_cast<std::size_t>(i)] != right[static_cast<std::size_t>(i)]))
                    throw std::runtime_error("nonfinite/unbounded output or mismatch in mono duplication");
            at += static_cast<std::size_t>(n);
        }
        framePosition += static_cast<int64>(count);
        return output;
    }
    std::vector<float> notes(double seconds, const std::vector<MidiAction>& schedule, const std::vector<int>& chunks = {257},
                             double tempo = 120.0, bool playing = true)
    { return render(seconds, chunks, nullptr, tempo, 0, &schedule, playing); }
    Changes change(const std::map<ParamID, double>& values)
    {
        Changes changes;
        for (const auto& entry : values)
        {
            require(controller->setParamNormalized(entry.first, entry.second) == kResultOk, "controller accepts parameter id=" + std::to_string(entry.first));
            changes.add(entry.first, entry.second);
        }
        return changes;
    }
    void apply(const std::map<ParamID, double>& values)
    { auto changes = change(values); render(0.15, {257}, &changes); }
    std::map<ParamID, double> referenceSettings() const
    {
        return {{id("VCO Freq"), plain("VCO Freq", 220.0)}, {id("LFO mod"), 0.0},
                {id("LFO freq"), plain("LFO freq", 2.0)}, {id("Master"), 0.7},
                {id("Mix"), 0.0}, {id("Delay"), plain("Delay", 187.0)}, {id("Repeats"), 0.0},
                {id("LFO rounded"), 1.0}, {id("LFO edge shape"), 0.0}, {id("Sync"), 0.0}, {id("Oscillation"), 0.0}};
    }
};
double rms(const std::vector<float>& output, std::size_t start = 0)
{
    double sum = 0.0;
    for (std::size_t i = start; i < output.size(); ++i) sum += static_cast<double>(output[i]) * output[i];
    return std::sqrt(sum / std::max<std::size_t>(1, output.size() - start));
}
double difference(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size()) return 1.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) { const double d = static_cast<double>(a[i]) - b[i]; sum += d * d; }
    return std::sqrt(sum / std::max<std::size_t>(1, a.size()));
}
double frequency(const std::vector<float>& output, double sr)
{
    std::size_t first = 0, last = 0, crossings = 0;
    for (std::size_t i = static_cast<std::size_t>(sr * 0.3); i < output.size(); ++i)
        if (output[i - 1] <= 0.0f && output[i] > 0.0f)
        { if (!crossings) first = i; last = i; ++crossings; }
    return crossings > 2 ? static_cast<double>(crossings - 1) * sr / static_cast<double>(last - first) : 0.0;
}
const std::map<std::string, ParamID> panelIds {
    {"VCO Freq", 430462261}, {"LFO mod", 91772408}, {"LFO freq", 697255490},
    {"Master", 1066216034}, {"Mix", 108124}, {"Delay", 95467907}, {"Repeats", 1094288952},
    {"LFO rounded", 1317983907}, {"LFO edge shape", 340517548}, {"Sync", 3545755}, {"Oscillation", 1291011307}
};
std::size_t frame(double sr, double seconds) { return static_cast<std::size_t>(std::llround(sr * seconds)); }
double peak(const std::vector<float>& data, std::size_t begin = 0, std::size_t end = 0)
{
    if (end == 0) end = data.size();
    double result = 0.0;
    for (std::size_t i = begin; i < std::min(end, data.size()); ++i) result = std::max(result, std::abs(static_cast<double>(data[i])));
    return result;
}
double regionFrequency(const std::vector<float>& data, double sr, double beginSeconds, double endSeconds)
{
    std::size_t first = 0, last = 0, count = 0;
    const auto begin = std::max<std::size_t>(1, frame(sr, beginSeconds));
    const auto end = std::min(data.size(), frame(sr, endSeconds));
    for (std::size_t i = begin; i < end; ++i)
        if (data[i - 1] <= 0.0f && data[i] > 0.0f) { if (!count) first = i; last = i; ++count; }
    return count > 2 ? static_cast<double>(count - 1) * sr / static_cast<double>(last - first) : 0.0;
}
double compareRegion(const std::vector<float>& a, std::size_t startA, const std::vector<float>& b, std::size_t startB, std::size_t count)
{
    if (startA + count > a.size() || startB + count > b.size()) return 1.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) { const double delta = static_cast<double>(a[startA + i]) - b[startB + i]; sum += delta * delta; }
    return std::sqrt(sum / std::max<std::size_t>(1, count));
}
std::map<ParamID, double> changedSettings(Instance& instance)
{
    auto values = instance.referenceSettings();
    const std::array<std::string, 7> names{{"VCO Freq", "LFO mod", "LFO freq", "Master", "Mix", "Delay", "Repeats"}};
    const std::array<double, 7> controls{{0.13, 0.71, 0.22, 0.63, 0.84, 0.43, 0.77}};
    for (std::size_t i = 0; i < names.size(); ++i) values[instance.id(names[i])] = controls[i];
    values[instance.id("LFO rounded")] = 0.0; values[instance.id("LFO edge shape")] = 1.0;
    values[instance.id("Sync")] = 1.0; values[instance.id("Oscillation")] = 1.0;
    return values;
}
void fresh(Instance& instance, double sr, int maximumBlock = 1024)
{
    instance.start(sr, maximumBlock);
    instance.apply(instance.referenceSettings());
    instance.stop();
    instance.start(sr, maximumBlock);
}
void metadataChecks(Module& module, Instance& instance)
{
    auto* factory2 = query<IPluginFactory2>(module.factory);
    require(factory2 != nullptr, "versioned class metadata is exposed");
    std::set<std::string> categories;
    expect(factory2->countClasses() == 3, "exactly processor, controller and compatibility classes");
    for (int32 index = 0; index < factory2->countClasses(); ++index)
    {
        PClassInfo2 info{};
        require(factory2->getClassInfo2(index, &info) == kResultOk, "read versioned class metadata");
        expect(categories.insert(info.category).second, "factory class category is unique");
        if (std::strcmp(info.category, kVstAudioEffectClass) == 0
            || std::strcmp(info.category, kVstComponentControllerClass) == 0
            || std::strcmp(info.category, "Plugin Compatibility Class") == 0)
        {
            expect(std::strcmp(info.name, "LMZ Signal 2M") == 0, "transparent host-visible name is LMZ Signal 2M");
            expect(std::strcmp(info.version, "0.3.1") == 0, "class reports actual build version 0.3.1");
            expect(std::strcmp(info.vendor, "LMZ") == 0, "manufacturer remains LMZ");
        }
        if (std::strcmp(info.category, "Plugin Compatibility Class") == 0)
            expect(FUID::fromTUID(info.cid) == FUID(0xABCDEF01, 0xC0DEF00D, 0x4C6D7A7A, 0x5367326D),
                   "semantic compatibility class identity preserved on macOS");
    }
    expect(categories == std::set<std::string>{kVstAudioEffectClass, kVstComponentControllerClass, "Plugin Compatibility Class"},
           "expected VST3 factory categories preserved");
    factory2->release();
    const std::set<std::string> continuous = {"VCO Freq", "LFO mod", "LFO freq", "Master", "Mix", "Delay", "Repeats"};
    std::set<ParamID> unique;
    std::size_t working = 0, bypass = 0, midiCc = 0;
    bool validIds = true, validDefaults = true;
    for (const auto& info : instance.parameters)
    {
        const auto name = ascii(info.title);
        validIds = validIds && unique.insert(info.id).second && info.id <= 0x7fffffffU;
        validDefaults = validDefaults && std::isfinite(info.defaultNormalizedValue) && info.defaultNormalizedValue >= 0.0 && info.defaultNormalizedValue <= 1.0;
        if (name.rfind("MIDI CC ", 0) == 0)
        {
            ++midiCc;
            validIds = validIds && (info.flags & ParameterInfo::kCanAutomate) == 0;
            continue;
        }
        if ((info.flags & ParameterInfo::kIsBypass) != 0) { ++bypass; continue; }
        ++working;
        std::cout << "PANEL_PARAM id=" << info.id << " name=" << name << " steps=" << info.stepCount << " flags=" << info.flags << '\n';
        expect(panelIds.count(name) && panelIds.at(name) == info.id, "0.1 panel parameter ID preserved: " + name);
        expect((info.flags & ParameterInfo::kCanAutomate) != 0 && (info.flags & (ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden)) == 0,
               "visible automatable panel parameter: " + name);
        expect(info.stepCount == (continuous.count(name) ? 0 : 1), "correct panel parameter stepCount: " + name);
        String128 text{};
        expect(instance.controller->getParamStringByValue(info.id, info.defaultNormalizedValue, text) == kResultOk && text[0] != 0,
               "host display string: " + name);
    }
    expect(validIds && validDefaults, "all VST3 IDs/defaults valid, unique; SDK MIDI CC parameters are not panel automation");
    expect(working == 11 && bypass <= 1, "eleven unchanged panel controls plus standard SDK entries");
    std::cout << "PARAMETER_COUNTS total=" << instance.parameters.size() << " panel=" << working << " bypass=" << bypass << " SDK_CC=" << midiCc << '\n';
    // Compare semantic FUIDs, allowing the SDK's COM/non-COM platform byte layout.
    expect(FUID::fromTUID(module.componentClass.data()) == FUID(0xABCDEF01, 0x9182FAEB, 0x4C6D7A7A, 0x5367326D),
           "0.1 semantic processor class identity preserved on macOS");
    TUID actualController{};
    expect(instance.component->getControllerClassId(actualController) == kResultOk
           && FUID::fromTUID(actualController) == FUID(0xABCDEF01, 0x1234ABCD, 0x4C6D7A7A, 0x5367326D),
           "0.1 semantic controller class identity preserved on macOS");
    expect(instance.component->getBusCount(kAudio, kInput) == 0 && instance.component->getBusCount(kAudio, kOutput) == 1,
           "instrument has one output bus and no external audio input");
    expect(instance.component->getBusCount(kEvent, kInput) == 1, "MIDI input event bus advertised");
    expect(instance.processor->canProcessSampleSize(kSample32) == kResultOk, "float processing supported");
}
void gateAndPitchChecks(Instance& instance, double sr)
{
    const auto label = std::to_string(static_cast<int>(sr));
    fresh(instance, sr);
    expect(peak(instance.render(1.0, {1, 17, 64, 1024})) == 0.0, "exact silence before MIDI at sr=" + label);
    const auto on = std::size_t(13), off = frame(sr, 0.61) + 7;
    const auto duration = instance.notes(0.8, {{on, MidiKind::on, 60, 0.7f}, {off, MidiKind::off, 60}}, {257});
    std::size_t firstSignal = duration.size(), lastSignal = 0;
    for (std::size_t i = 0; i < duration.size(); ++i) if (std::abs(duration[i]) > 1.0e-8f)
    { firstSignal = std::min(firstSignal, i); lastSignal = i; }
    const auto releaseBound = static_cast<std::size_t>(std::ceil(sr * 0.005));
    std::cout << "GATE sr=" << sr << " on=" << on << " first=" << firstSignal << " off=" << off << " last=" << lastSignal << '\n';
    expect(peak(duration, 0, on) == 0.0 && firstSignal >= on && firstSignal <= on + 2,
           "NoteOn starts at its nonzero sample offset without pre-sound sr=" + label);
    expect(std::abs(regionFrequency(duration, sr, 0.1, 0.55) - 220.0) < 1.0,
           "MIDI60 is neutral VCO pitch and note length hint does not stop held note sr=" + label);
    expect(lastSignal < off + releaseBound + 1 && peak(duration, off + releaseBound + 1) == 0.0,
           "NoteOff silences complete output within nominal 5ms sr=" + label);
    expect(instance.processor->getTailSamples() > 0 && instance.processor->getTailSamples() <= releaseBound + 1,
           "VST3 advertises finite at-most-5ms tail sr=" + label);

    for (const auto& item : std::array<std::pair<int, double>, 5>{{{48,110.0},{60,220.0},{72,440.0},{56,174.614116},{64,277.182631}}})
    {
        fresh(instance, sr);
        const auto audio = instance.notes(0.65, {{7, MidiKind::on, item.first, 1.0f}}, {17, 1024, 1, 257});
        const double observed = regionFrequency(audio, sr, 0.15, 0.6);
        std::cout << "PITCH sr=" << sr << " note=" << item.first << " expected=" << item.second << " observed=" << observed << '\n';
        expect(std::abs(observed - item.second) < 1.3, "piano-roll/root-equivalent MIDI transposition note=" + std::to_string(item.first) + " sr=" + label);
    }
    fresh(instance, sr);
    const auto quietVelocity = instance.notes(0.5, {{0, MidiKind::on, 60, 0.15f}});
    fresh(instance, sr);
    const auto loudVelocity = instance.notes(0.5, {{0, MidiKind::on, 60, 1.0f}});
    expect(difference(quietVelocity, loudVelocity) == 0.0, "nonzero velocity is a gate without an invented gain parameter sr=" + label);
    const auto zeroVelocity = instance.notes(0.2, {{19, MidiKind::on, 60, 0.0f}});
    expect(peak(zeroVelocity, 19 + releaseBound + 1) == 0.0, "velocity-zero NoteOn acts as NoteOff sr=" + label);
    instance.stop();
}
void retriggerAndOverlapChecks(Instance& instance, double sr)
{
    const auto label = std::to_string(static_cast<int>(sr));
    fresh(instance, sr);
    instance.apply({{instance.id("LFO mod"), 0.5}, {instance.id("LFO freq"), instance.plain("LFO freq", 3.7)},
                    {instance.id("Delay"), instance.plain("Delay", 80.0)}, {instance.id("Mix"), 1.0},
                    {instance.id("Repeats"), 1.0}, {instance.id("Oscillation"), 1.0}});
    instance.stop(); instance.start(sr);
    const auto baseline = instance.notes(0.5, {{0, MidiKind::on, 60}});
    instance.stop(); instance.start(sr);
    const auto retriggerAt = frame(sr, 0.713) + 11;
    const auto retriggered = instance.notes(1.22, {{0, MidiKind::on, 60}, {retriggerAt, MidiKind::on, 60}});
    const auto ignoreAttack = frame(sr, 0.006);
    const double resetError = compareRegion(baseline, ignoreAttack, retriggered, retriggerAt + ignoreAttack, frame(sr, 0.42));
    std::cout << "RETRIGGER sr=" << sr << " history_reset_rms=" << resetError << '\n';
    expect(resetError < 1.0e-7, "every NoteOn retriggers VCO/LFO/filters and removes old delay/Oscillation history sr=" + label);

    fresh(instance, sr);
    const auto overlap = instance.notes(0.9, {{0,MidiKind::on,60},{frame(sr,0.25),MidiKind::on,72},
         {frame(sr,0.4),MidiKind::off,60},{frame(sr,0.65),MidiKind::off,72}});
    expect(std::abs(regionFrequency(overlap,sr,0.43,0.61)-440.0)<1.2, "latest held note wins and inactive NoteOff cannot cut it sr="+label);
    expect(peak(overlap,frame(sr,0.656))==0.0, "last held note releases normally sr="+label);
    fresh(instance,sr);
    const auto fallback = instance.notes(0.95, {{0,MidiKind::on,60},{frame(sr,0.25),MidiKind::on,72},
         {frame(sr,0.5),MidiKind::off,72},{frame(sr,0.8),MidiKind::off,60}});
    expect(std::abs(regionFrequency(fallback,sr,0.56,0.75)-220.0)<1.2, "releasing active note falls back to latest remaining held note sr="+label);
    expect(peak(fallback,frame(sr,0.806))==0.0, "fallback note releases without stuck voice sr="+label);
    fresh(instance,sr);
    const auto channels = instance.notes(0.8, {{0,MidiKind::on,60,1.0f,0},{frame(sr,0.2),MidiKind::off,60,1.0f,1},
         {frame(sr,0.6),MidiKind::off,60,1.0f,0}});
    expect(std::abs(regionFrequency(channels,sr,0.3,0.55)-220.0)<1.2, "foreign-channel NoteOff does not cut a held pitch sr="+label);
    expect(peak(channels,frame(sr,0.606))==0.0, "matching-channel NoteOff releases held pitch sr="+label);
    fresh(instance,sr);
    const auto duplicates = instance.notes(0.8, {{0,MidiKind::on,60},{frame(sr,0.1),MidiKind::on,60},
         {frame(sr,0.3),MidiKind::off,60},{frame(sr,0.6),MidiKind::off,60}});
    expect(std::abs(regionFrequency(duplicates,sr,0.35,0.55)-220.0)<1.2 && peak(duplicates,frame(sr,0.606))==0.0,
           "overlapping repeated same-pitch NoteOns are balanced by matching NoteOffs sr="+label);

    fresh(instance,sr);
    const std::vector<MidiAction> timeline {{13,MidiKind::on,60},{frame(sr,0.3)+11,MidiKind::on,67},
        {frame(sr,0.4)+1,MidiKind::off,60},{frame(sr,0.62)-9,MidiKind::off,67}};
    const auto regular = instance.notes(0.9,timeline,{1024});
    instance.stop(); instance.start(sr);
    const auto varied = instance.notes(0.9,timeline,{1,17,257,64,511});
    expect(difference(regular,varied)<1.0e-7, "real note offsets/retrigger/release retain the same timeline across block sequences sr="+label);
    instance.stop();
}
void channelModeAndTransportChecks(Instance& instance,double sr)
{
    const auto label=std::to_string(static_cast<int>(sr));
    fresh(instance,sr);
    const auto cc123 = instance.notes(0.9, {{0,MidiKind::on,60,1,0},{frame(sr,0.25),MidiKind::cc,123,0,1},
        {frame(sr,0.6)+17,MidiKind::cc,123,0,0}},{1024});
    expect(std::abs(regionFrequency(cc123,sr,0.3,0.55)-220.0)<1.2, "CC123 on another channel preserves active note sr="+label);
    expect(peak(cc123,frame(sr,0.606)+17)==0.0, "mapped CC123 releases its channel at sample offset sr="+label);
    fresh(instance,sr);
    instance.apply({{instance.id("Mix"),1},{instance.id("Oscillation"),1},{instance.id("Repeats"),1}});
    const auto cut=frame(sr,0.7)+29;
    const auto cc120=instance.notes(0.9,{{0,MidiKind::on,60,1,0},{frame(sr,0.3),MidiKind::cc,120,0,1},{cut,MidiKind::cc,120,0,0}},{1024});
    expect(peak(cc120,frame(sr,0.35),frame(sr,0.6))>0.01, "CC120 on another channel preserves held Oscillation voice sr="+label);
    expect(peak(cc120,cut)==0.0, "mapped CC120 hard-stops the entire channel including delay at its offset sr="+label);
    fresh(instance,sr);
    expect(rms(instance.notes(0.4,{{0,MidiKind::on,60}}))>0.02, "transport-running note produces signal sr="+label);
    expect(peak(instance.render(0.1,{257},nullptr,120,0,nullptr,false))==0.0, "playing-to-stopped transport transition clears old held notes sr="+label);
    const auto preview=instance.notes(0.6,{{17,MidiKind::on,60},{frame(sr,0.4),MidiKind::off,60}},{257},120,false);
    expect(std::abs(regionFrequency(preview,sr,0.1,0.35)-220.0)<1.2 && peak(preview,frame(sr,0.406))==0.0,
           "new stopped-transport preview notes work and release sr="+label);
    instance.stop();
}
void oscillationAndAutomationChecks(Instance& instance,double sr)
{
    const auto label=std::to_string(static_cast<int>(sr));
    fresh(instance,sr);
    instance.apply({{instance.id("Mix"),1},{instance.id("Repeats"),1},{instance.id("Oscillation"),1},
                    {instance.id("Delay"),instance.plain("Delay",200.0)}});
    const auto stopAt=frame(sr,30.0)+23;
    const auto longNote=instance.notes(30.25,{{11,MidiKind::on,60},{stopAt,MidiKind::off,60}},{17,1024,257});
    expect(peak(longNote,frame(sr,20),frame(sr,29))>0.02 && peak(longNote,stopAt+static_cast<std::size_t>(std::ceil(sr*0.005))+1)==0.0,
           "30-second held Oscillation remains finite and NoteOff gates all feedback within 5ms sr="+label);
    fresh(instance,sr);
    instance.notes(0.5,{{0,MidiKind::on,60}});
    auto muteChange=instance.change({{instance.id("Master"),0}});
    const auto masterFade=instance.render(0.4,{257},&muteChange);
    const double initialPeak=peak(masterFade,0,frame(sr,0.03));
    const double settledPeak=peak(masterFade,frame(sr,0.25));
    std::cout<<"MASTER_FADE sr="<<sr<<" initial_peak="<<initialPeak<<" settled_peak="<<settledPeak<<'\n';
    expect(initialPeak>0.001 && settledPeak<1.0e-6, "Master automation fades a MIDI-held note to settled silence without editor sr="+label);
    instance.apply({{instance.id("Master"),0.8},{instance.id("VCO Freq"),instance.plain("VCO Freq",880.0)}});
    const auto high=instance.render(0.8);
    expect(rms(high)>0.02 && std::abs(regionFrequency(high,sr,0.3,0.75)-880.0)<1.5,
           "VCO automation changes held-note pitch without editor sr="+label);
    instance.stop();
    instance.start(sr,64,1);
    instance.apply(instance.referenceSettings());
    expect(rms(instance.notes(0.5,{{3,MidiKind::on,60}},{17,64}))>0.02, "mono bus handles MIDI voice sr="+label);
    instance.stop();
}
void stateAndInstancesChecks(Module& module,Instance& first,const std::filesystem::path& oldStatePath)
{
    fresh(first,48000);
    const auto desired=changedSettings(first);
    first.apply(desired);
    std::map<ParamID,double> expected;
    for(const auto& item:desired) expected[item.first]=first.controller->getParamNormalized(item.first);
    first.stop();
    MemoryStream saved;
    require(first.component->getState(&saved)==kResultOk && !saved.bytes.empty(),"current component writes nonempty state");
    first.start(48000);
    auto changed=desired;
    for(auto& item:changed) item.second=item.second<0.5?1.0:0.0;
    first.apply(changed); first.stop(); saved.position=0;
    require(first.component->setState(&saved)==kResultOk,"current component state restores");
    saved.position=0; require(optionalControllerState(first.controller->setComponentState(&saved)),"current controller refreshes restored state");
    for(const auto& item:expected) expect(std::abs(first.controller->getParamNormalized(item.first)-item.second)<2.0e-5,
        "0.2 state roundtrip panel id="+std::to_string(item.first));
    Instance second(module);
    for(const auto& item:panelIds) expect(second.id(item.first)==item.second,"panel ID stable across instances: "+item.first);
    saved.position=0; require(second.component->setState(&saved)==kResultOk,"fresh current component restores state");
    saved.position=0; require(optionalControllerState(second.controller->setComponentState(&saved)),"fresh current controller restores state");
    first.start(48000); second.start(48000);
    expect(peak(first.render(0.1))==0.0 && peak(second.render(0.1))==0.0,"restored preset with Oscillation stays silent without MIDI");
    const auto a=first.notes(0.8,{{13,MidiKind::on,60}},{257});
    const auto b=second.notes(0.8,{{13,MidiKind::on,60}},{1,17,1024,64});
    expect(difference(a,b)<1.0e-7,"fresh restored MIDI instance reproduces audio across different block sequences");
    first.stop(); second.stop();

    std::ifstream input(oldStatePath,std::ios::binary);
    require(input.good(),"actual 0.1 state fixture is available: "+oldStatePath.u8string());
    MemoryStream legacy;
    legacy.bytes.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    require(legacy.bytes.size()==616,"0.1 fixture has the independently generated original byte count");
    require(first.component->setState(&legacy)==kResultOk,"current component accepts actual 0.1 state");
    legacy.position=0; require(optionalControllerState(first.controller->setComponentState(&legacy)),"current controller refreshes actual 0.1 state");
    for(const auto& item:desired) expect(std::abs(first.controller->getParamNormalized(item.first)-item.second)<2.0e-5,
        "0.1-to-current preset compatibility id="+std::to_string(item.first));
    first.start(48000);
    expect(peak(first.render(0.5))==0.0,"loading an autonomous 0.1 preset does not start sound in MIDI-gated current version");
    expect(rms(first.notes(0.8,{{0,MidiKind::on,60}}))>0.01,"loaded 0.1 preset sounds when a new MIDI note arrives");
    first.stop();

    fresh(first,48000); fresh(second,48000);
    first.notes(0.4,{{0,MidiKind::on,60}});
    second.apply({{second.id("Master"),0},{second.id("VCO Freq"),second.plain("VCO Freq",7100.0)}});
    second.notes(0.2,{{0,MidiKind::on,72},{17,MidiKind::cc,120,0}});
    const auto unaffected=first.render(0.6);
    expect(rms(unaffected)>0.02 && std::abs(regionFrequency(unaffected,48000,0.1,0.5)-220.0)<1.2,
           "other-instance note/controller/parameter activity cannot alter a held voice");
    first.stop(); second.stop();
    fresh(first,48000); fresh(second,48000);
    auto sync=first.referenceSettings(); sync[first.id("Sync")]=1;
    sync[first.id("VCO Freq")]=first.plain("VCO Freq",433.7); sync[first.id("LFO freq")]=first.plain("LFO freq",5);
    first.apply(sync); second.apply(sync); first.stop(); second.stop(); first.start(48000); second.start(48000);
    const auto slow=first.notes(1.0,{{0,MidiKind::on,60}},{257},60);
    const auto fast=second.notes(1.0,{{0,MidiKind::on,60}},{257},300);
    expect(difference(slow,fast)==0.0,"MIDI-gated internal Sync remains independent of BPM60/300");
    first.stop(); second.stop();
}
}
int main(int argc,char** argv)
{
    std::cout<<std::setprecision(10);
    if(argc<2 || argc>3){std::cerr<<"Usage: Signal2MMacVst3Checks <Signal2M.vst3-bundle> [actual-v0.1-state-fixture]\n";return 2;}
    auto fixture=argc==3?std::filesystem::u8path(argv[2]):std::filesystem::path("Tests/Fixtures/Signal2M-v0.1-state.bin");
    if(argc==2 && !std::filesystem::exists(fixture)) fixture=std::filesystem::path("../Tests/Fixtures/Signal2M-v0.1-state.bin");
    try
    {
        Module module(std::filesystem::u8path(argv[1]));
        Instance instance(module);
        metadataChecks(module,instance);
        for(const double sr:{44100.0,48000.0})
        {
            gateAndPitchChecks(instance,sr);
            retriggerAndOverlapChecks(instance,sr);
            channelModeAndTransportChecks(instance,sr);
            oscillationAndAutomationChecks(instance,sr);
        }
        stateAndInstancesChecks(module,instance,fixture);
    }
    catch(const std::exception& error){++failures;std::cerr<<"ERROR "<<error.what()<<'\n';}
    std::cout<<"RESULT assertions="<<assertions<<" failures="<<failures<<'\n';
    return failures==0?0:1;
}
