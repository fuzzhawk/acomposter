// acomposter test runner.
//
// Deliberately dependency-free and portable: these cover the parts of the engine
// that have nothing to do with Windows (graph scheduling, parameter mapping,
// file codecs, snapshot interpolation, patch round-tripping), so they can be run
// on the build host as well as on the target.
//
//   g++ -std=c++20 -I src tests/tests.cpp <sources> -o tests && ./tests

#include "../src/audio/AudioFile.h"
#include "../src/core/Engine.h"
#include "../src/core/Graph.h"
#include "../src/core/Base64.h"
#include "../src/core/FileIo.h"
#include "../src/core/Json.h"
#include "../src/core/Parameter.h"
#include "../src/core/Transport.h"
#include "../src/dsp/Dsp.h"
#include "../src/meta/Metasurface.h"
#include "../src/dsp/Fft.h"
#include "../src/library/AudioAnalysis.h"
#include "../src/library/FileIndex.h"
#include "../src/library/Slicer.h"
#include "../src/library/Classify.h"
#include "../src/library/ChainPreset.h"
#include "../src/library/Library.h"
#include "../src/nodes/BuildNode.h"
#include "../src/nodes/DropNode.h"
#include "../src/nodes/UtilityNodes.h"
#include "../src/nodes/ColorNode.h"
#include "../src/nodes/NodeFactory.h"
#include "../src/nodes/StemPlayerNode.h"
#include "../src/patch/Patch.h"
#include "../src/vst2/PeArchitecture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_currentTest = "";

void check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s:%d  %s\n", g_currentTest, line, expression);
    }
}

void checkClose(double a, double b, double tolerance, const char* expression, int line) {
    check(std::fabs(a - b) <= tolerance, expression, line);
    if (std::fabs(a - b) > tolerance)
        std::printf("        (%.9g vs %.9g, tolerance %.3g)\n", a, b, tolerance);
}

#define CHECK(expr)              check((expr), #expr, __LINE__)
#define CHECK_CLOSE(a, b, tol)   checkClose((a), (b), (tol), #a " ~= " #b, __LINE__)

struct TestScope {
    explicit TestScope(const char* name) {
        g_currentTest = name;
        std::printf("* %s\n", name);
    }
};

#define TEST(name) TestScope scope_##__LINE__(name)

// ---------------------------------------------------------------------------

using namespace acm;

void testJson() {
    TEST("json round trip");

    JsonValue root = JsonValue::object();
    root.set("name", "acomposter");
    root.set("version", 1);
    root.set("ratio", 0.3333333333333333);
    root.set("enabled", true);
    root.set("nothing", JsonValue());

    JsonValue list = JsonValue::array();
    for (int i = 0; i < 4; ++i) list.push(JsonValue(i * 1.5));
    root.set("values", list);

    JsonValue nested = JsonValue::object();
    nested.set("escaped", std::string("quote\" backslash\\ newline\n tab\t"));
    nested.set("unicode", std::string("\xE2\x88\x9E"));  // infinity sign
    root.set("nested", nested);

    const std::string text = root.dump(2);

    std::string error;
    const JsonValue parsed = JsonValue::parse(text, &error);
    CHECK(error.empty());
    CHECK(parsed.isObject());
    CHECK(parsed.getString("name") == "acomposter");
    CHECK(parsed.getInt("version", 0) == 1);
    CHECK_CLOSE(parsed.getDouble("ratio", 0.0), 0.3333333333333333, 1e-15);
    CHECK(parsed.getBool("enabled", false));
    CHECK(parsed.find("nothing") != nullptr && parsed.find("nothing")->isNull());
    CHECK(parsed.find("values")->size() == 4);
    CHECK_CLOSE(parsed.find("values")->at(3).asDouble(), 4.5, 1e-12);
    CHECK(parsed.find("nested")->getString("escaped") == "quote\" backslash\\ newline\n tab\t");
    CHECK(parsed.find("nested")->getString("unicode") == "\xE2\x88\x9E");

    // Key order is preserved so saved patches diff cleanly.
    CHECK(parsed.members().size() == 7);
    CHECK(parsed.members()[0].first == "name");
    CHECK(parsed.members()[6].first == "nested");

    // Malformed input reports rather than throws.
    std::string badError;
    const JsonValue bad = JsonValue::parse("{ \"a\": }", &badError);
    CHECK(!badError.empty());
    CHECK(bad.isNull());

    // Line comments are accepted.
    const JsonValue commented = JsonValue::parse("{ // a note\n \"a\": 1 }", &error);
    CHECK(error.empty() && commented.getInt("a", 0) == 1);
}

void testParameterMapping() {
    TEST("parameter mapping");

    Parameter gain("gain", "Gain", ParamKind::Float, -96.0f, 12.0f, 0.0f);
    gain.setCurve(ParamCurve::Decibels).setUnit("dB");
    CHECK_CLOSE(gain.toNormalised(-96.0f), 0.0, 1e-6);
    CHECK_CLOSE(gain.toNormalised(12.0f), 1.0, 1e-6);
    CHECK(gain.toText(-96.0f) == "-inf dB");

    // A skewed frequency parameter must put the centre where we asked for it.
    Parameter cutoff("cutoff", "Cutoff", ParamKind::Float, 20.0f, 20000.0f, 1000.0f);
    cutoff.setSkewForCentre(1000.0f);
    CHECK_CLOSE(cutoff.toNormalised(1000.0f), 0.5, 1e-4);
    CHECK_CLOSE(cutoff.fromNormalised(0.5f), 1000.0, 1.0);
    CHECK_CLOSE(cutoff.fromNormalised(cutoff.toNormalised(7345.0f)), 7345.0, 1.0);

    // Stepped kinds quantise and never blend.
    Parameter mode("mode", "Mode", ParamKind::Choice, 0.0f, 1.0f, 0.0f);
    mode.setChoices({ "forward", "reverse", "ping-pong" });
    CHECK(mode.maxValue() == 2.0f);
    CHECK(mode.blend() == ParamBlend::Stepped);
    mode.setNormalised(0.6f);
    CHECK(mode.value() == 1.0f);
    CHECK(mode.toText() == "reverse");

    float parsed = 0.0f;
    CHECK(mode.fromText("ping-pong", parsed) && parsed == 2.0f);
    CHECK(gain.fromText("-6.0", parsed) && std::fabs(parsed + 6.0f) < 1e-4f);
    CHECK(!gain.fromText("banana", parsed));

    Parameter flag("flag", "Flag", ParamKind::Bool, 0.0f, 1.0f, 0.0f);
    CHECK(flag.fromText("on", parsed) && parsed == 1.0f);
    CHECK(flag.toText(1.0f) == "on");
}

void testSmoothing() {
    TEST("smoothed value");

    SmoothedValue v;
    v.reset(1000.0, 0.01);         // 10 steps
    v.setCurrentAndTarget(0.0f);
    v.setTarget(1.0f);

    CHECK(v.smoothing());
    for (int i = 0; i < 9; ++i) v.next();
    CHECK(v.next() >= 0.999f);
    CHECK(!v.smoothing());
}

void testWavRoundTrip() {
    TEST("wav encode/decode round trip");

    constexpr int channels = 2;
    constexpr std::int64_t frames = 5000;

    SampleBuffer source(channels, frames, 48000.0);
    for (std::int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        source.channelForWrite(0)[i] = static_cast<float>(0.8 * std::sin(2.0 * 3.14159265 * 440.0 * t));
        source.channelForWrite(1)[i] = static_cast<float>(-0.5 * std::cos(2.0 * 3.14159265 * 220.0 * t));
    }

    struct Case { audiofile::WavFormat format; const char* name; double tolerance; };
    const Case cases[] = {
        { audiofile::WavFormat::Pcm16,   "16 bit", 1.0 / 32768.0 * 2.0 },
        { audiofile::WavFormat::Pcm24,   "24 bit", 1.0 / 8388608.0 * 2.0 },
        { audiofile::WavFormat::Pcm32,   "32 bit", 1e-6 },
        { audiofile::WavFormat::Float32, "float",  1e-7 },
    };

    for (const Case& c : cases) {
        const std::vector<std::uint8_t> encoded = audiofile::encodeWav(source, c.format);
        CHECK(encoded.size() > 44);

        std::string error;
        auto decoded = audiofile::decode(encoded.data(), encoded.size(), "test.wav", &error);
        CHECK(decoded != nullptr);
        if (!decoded) { std::printf("        %s: %s\n", c.name, error.c_str()); continue; }

        CHECK(decoded->channels() == channels);
        CHECK(decoded->frames() == frames);
        CHECK_CLOSE(decoded->sampleRate(), 48000.0, 1e-9);

        double worst = 0.0;
        for (int ch = 0; ch < channels; ++ch)
            for (std::int64_t i = 0; i < frames; ++i)
                worst = std::max(worst, std::fabs(static_cast<double>(decoded->channel(ch)[i])
                                                - static_cast<double>(source.channel(ch)[i])));
        checkClose(worst, 0.0, c.tolerance, c.name, __LINE__);

        // The overview must span the same dynamic range as the audio.
        CHECK(decoded->overview().buckets > 0);
        CHECK(decoded->peakLevel() > 0.7f && decoded->peakLevel() <= 1.0f);
    }
}

void testBpmDetection() {
    TEST("bpm guessing from file names");

    CHECK_CLOSE(audiofile::guessBpmFromName("amen_174bpm.wav"), 174.0, 1e-9);
    CHECK_CLOSE(audiofile::guessBpmFromName("Break 90 BPM.aiff"), 90.0, 1e-9);
    CHECK_CLOSE(audiofile::guessBpmFromName("128 - deep groove.wav"), 128.0, 1e-9);
    CHECK_CLOSE(audiofile::guessBpmFromName("no tempo here.wav"), 0.0, 1e-9);
    // Implausible values are rejected rather than believed.
    CHECK_CLOSE(audiofile::guessBpmFromName("9000bpm.wav"), 0.0, 1e-9);
}

void testFadeLaws() {
    TEST("crossfade laws");

    float a = 0.0f, b = 0.0f;

    dsp::crossfadeGains(dsp::FadeLaw::ConstantPower, 0.0f, a, b);
    CHECK_CLOSE(a, 1.0, 1e-5); CHECK_CLOSE(b, 0.0, 1e-5);

    dsp::crossfadeGains(dsp::FadeLaw::ConstantPower, 1.0f, a, b);
    CHECK_CLOSE(a, 0.0, 1e-5); CHECK_CLOSE(b, 1.0, 1e-5);

    // Constant power holds the sum of squares at unity across the sweep.
    for (int i = 0; i <= 10; ++i) {
        dsp::crossfadeGains(dsp::FadeLaw::ConstantPower, static_cast<float>(i) / 10.0f, a, b);
        CHECK_CLOSE(a * a + b * b, 1.0, 1e-5);
    }

    // Linear sums to unity instead.
    for (int i = 0; i <= 10; ++i) {
        dsp::crossfadeGains(dsp::FadeLaw::Linear, static_cast<float>(i) / 10.0f, a, b);
        CHECK_CLOSE(a + b, 1.0, 1e-5);
    }

    float l = 0.0f, r = 0.0f;
    dsp::panGains(0.0f, l, r);
    CHECK_CLOSE(l, r, 1e-6);
    CHECK_CLOSE(l * l + r * r, 1.0, 1e-5);
}

void testInterpolation() {
    TEST("interpolation");

    const float data[] = { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f };

    CHECK_CLOSE(dsp::interpolateLinear(data, 5, 0.0), 0.0, 1e-6);
    CHECK_CLOSE(dsp::interpolateLinear(data, 5, 1.5), 1.5, 1e-6);
    CHECK_CLOSE(dsp::interpolateLinear(data, 5, 3.25), 3.25, 1e-6);

    // Hermite reproduces a linear ramp exactly in the interior.
    CHECK_CLOSE(dsp::interpolateHermite(data, 5, 2.5), 2.5, 1e-5);
    CHECK_CLOSE(dsp::interpolateHermite(data, 5, 1.0), 1.0, 1e-6);

    // Out-of-range reads are silence, not a crash or garbage.
    CHECK_CLOSE(dsp::interpolateHermite(data, 5, -1.0), 0.0, 1e-9);
    CHECK_CLOSE(dsp::interpolateHermite(data, 5, 99.0), 0.0, 1e-9);
    CHECK_CLOSE(dsp::interpolateLinear(nullptr, 0, 0.0), 0.0, 1e-9);
}

void testTransport() {
    TEST("transport");

    Transport transport;
    transport.prepare(48000.0);
    transport.setBpm(120.0);
    transport.setPlaying(true);

    TransportState state;
    // At 120 bpm one beat is half a second: 24000 frames.
    transport.beginBlock(state, 24000);
    CHECK_CLOSE(state.samplesPerBeat, 24000.0, 1e-6);
    CHECK_CLOSE(state.ppqPosition, 0.0, 1e-9);

    transport.beginBlock(state, 24000);
    CHECK_CLOSE(state.ppqPosition, 1.0, 1e-9);

    transport.beginBlock(state, 0);
    CHECK_CLOSE(state.ppqPosition, 2.0, 1e-9);
    CHECK(state.bar() == 1);
    CHECK(state.beatInBar() == 3);

    // Looping wraps the position back inside the range.
    transport.setLoop(true, 0.0, 4.0);
    for (int i = 0; i < 20; ++i) transport.beginBlock(state, 24000);
    CHECK(transport.ppqPosition() >= 0.0 && transport.ppqPosition() < 4.0);

    // Tap tempo converges on the tapped interval.
    Transport tapper;
    tapper.prepare(48000.0);
    for (int i = 0; i < 5; ++i) tapper.tap(i * 0.5);  // 120 bpm
    CHECK_CLOSE(tapper.bpm(), 120.0, 0.5);
}

// A node that writes a constant, so graph wiring can be observed in the output.
class ConstantNode : public Node {
public:
    explicit ConstantNode(float value) : Node("test.constant", NodeCategory::Source), value_(value) {
        addOutput("out", 2);
    }
    void process(ProcessContext& ctx) override {
        for (int c = 0; c < ctx.output(0).numChannels; ++c) {
            float* d = ctx.output(0).chan(c);
            for (int i = 0; i < ctx.frames; ++i) d[i] = value_;
        }
    }
private:
    float value_;
};

// Records what it was given, and passes it through with a gain.
class ProbeNode : public Node {
public:
    ProbeNode() : Node("test.probe", NodeCategory::Analysis) {
        addInput("in", 2);
        addOutput("out", 2);
    }
    void process(ProcessContext& ctx) override {
        lastInput = ctx.frames > 0 ? ctx.input(0).chan(0)[0] : 0.0f;
        wasConnected = ctx.input(0).connected;
        ++callCount;
        for (int c = 0; c < ctx.output(0).numChannels; ++c) {
            const float* s = ctx.input(0).chan(c % ctx.input(0).numChannels);
            float* d = ctx.output(0).chan(c);
            for (int i = 0; i < ctx.frames; ++i) d[i] = s[i] * gain;
        }
    }
    float lastInput = 0.0f;
    bool wasConnected = false;
    int callCount = 0;
    float gain = 1.0f;
};

void testGraphRouting() {
    TEST("graph routing and summing");

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    auto probeOwner = std::make_unique<ProbeNode>();
    ProbeNode* probe = probeOwner.get();

    const NodeId a = graph.addNode(std::make_unique<ConstantNode>(0.25f));
    const NodeId b = graph.addNode(std::make_unique<ConstantNode>(0.5f));
    const NodeId p = graph.addNode(std::move(probeOwner));

    TransportState transport;
    transport.sampleRate = 48000.0;

    // Nothing patched in: the probe sees silence and knows it is unconnected.
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK(probe->callCount == 1);
    CHECK(!probe->wasConnected);
    CHECK_CLOSE(probe->lastInput, 0.0, 1e-9);

    // One source.
    CHECK(graph.connect(a, 0, p, 0) != kInvalidConnection);
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK(probe->wasConnected);
    CHECK_CLOSE(probe->lastInput, 0.25, 1e-6);

    // Two sources into the same input sum.
    CHECK(graph.connect(b, 0, p, 0) != kInvalidConnection);
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK_CLOSE(probe->lastInput, 0.75, 1e-6);

    // Illegal connections are refused with a reason.
    std::string reason;
    CHECK(!graph.canConnect(p, 0, p, 0, &reason));
    CHECK(!reason.empty());
    CHECK(!graph.canConnect(a, 0, p, 0, &reason));   // already patched
    CHECK(graph.connect(a, 0, p, 5) == kInvalidConnection);  // no such port

    // Disconnecting one source leaves the other.
    graph.disconnect(a, 0, p, 0);
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK_CLOSE(probe->lastInput, 0.5, 1e-6);

    // Removing a node tears down its connections too.
    CHECK(graph.removeNode(b));
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK(!probe->wasConnected);
    CHECK(graph.connections().empty());
}

void testGraphFeedback() {
    TEST("graph feedback resolution");

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    auto firstOwner = std::make_unique<ProbeNode>();
    auto secondOwner = std::make_unique<ProbeNode>();
    ProbeNode* first = firstOwner.get();
    ProbeNode* second = secondOwner.get();

    const NodeId source = graph.addNode(std::make_unique<ConstantNode>(1.0f));
    const NodeId n1 = graph.addNode(std::move(firstOwner));
    const NodeId n2 = graph.addNode(std::move(secondOwner));

    graph.connect(source, 0, n1, 0);
    graph.connect(n1, 0, n2, 0);
    // Close the loop: this must be scheduled as a one-block delay, not a hang.
    const ConnectionId loop = graph.connect(n2, 0, n1, 0);
    CHECK(loop != kInvalidConnection);
    CHECK(graph.scheduledFeedbackEdges() == 1);
    CHECK(graph.isFeedbackEdge(loop));

    second->gain = 0.5f;

    TransportState transport;
    transport.sampleRate = 48000.0;

    // Block 1: the feedback buffer is still empty, so n1 sees only the source.
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK_CLOSE(first->lastInput, 1.0, 1e-6);

    // Block 2: n1 now also sees last block's n2 output (1.0 * 0.5).
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK_CLOSE(first->lastInput, 1.5, 1e-6);

    // Block 3: the loop has converged one more step (1.0 + 1.5 * 0.5).
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK_CLOSE(first->lastInput, 1.75, 1e-6);

    CHECK(first->callCount == 3 && second->callCount == 3);
}

void testChannelAdaptation() {
    TEST("channel count adaptation");

    class MonoSource : public Node {
    public:
        MonoSource() : Node("test.mono", NodeCategory::Source) { addOutput("out", 1); }
        void process(ProcessContext& ctx) override {
            float* d = ctx.output(0).chan(0);
            for (int i = 0; i < ctx.frames; ++i) d[i] = 0.4f;
        }
    };

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    auto probeOwner = std::make_unique<ProbeNode>();
    ProbeNode* probe = probeOwner.get();

    const NodeId mono = graph.addNode(std::make_unique<MonoSource>());
    const NodeId p = graph.addNode(std::move(probeOwner));
    graph.connect(mono, 0, p, 0);

    TransportState transport;
    transport.sampleRate = 48000.0;
    graph.render(transport, 64, 0, nullptr, nullptr);

    // A mono source feeding a stereo input must land in both channels.
    CHECK_CLOSE(probe->lastInput, 0.4, 1e-6);
}

void testBypass() {
    TEST("node bypass passes audio through");

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    auto middleOwner = std::make_unique<ProbeNode>();
    auto tailOwner = std::make_unique<ProbeNode>();
    ProbeNode* middle = middleOwner.get();
    ProbeNode* tail = tailOwner.get();

    const NodeId source = graph.addNode(std::make_unique<ConstantNode>(0.6f));
    const NodeId mid = graph.addNode(std::move(middleOwner));
    const NodeId end = graph.addNode(std::move(tailOwner));

    graph.connect(source, 0, mid, 0);
    graph.connect(mid, 0, end, 0);

    middle->gain = 0.0f;   // would silence the chain if it ran

    TransportState transport;
    transport.sampleRate = 48000.0;

    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK_CLOSE(tail->lastInput, 0.0, 1e-9);

    middle->setBypassed(true);
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK_CLOSE(tail->lastInput, 0.6, 1e-6);
    CHECK(middle->callCount == 1);   // not called while bypassed
}

void testMetasurfaceInterpolation() {
    TEST("metasurface interpolation");

    registerBuiltinNodes();

    BlockCounter clock{ 0 };
    Engine engine;
    engine.prepare(48000.0, 128, 2, 2);
    Graph& graph = engine.graph();
    (void)clock;

    const NodeId fader = graph.addNode(NodeFactory::instance().create("crossfader"));
    CHECK(fader != kInvalidNode);
    Parameter* position = graph.node(fader)->findParameter("position");
    Parameter* curve = graph.node(fader)->findParameter("curve");
    CHECK(position != nullptr && curve != nullptr);

    Metasurface surface;

    position->setValue(0.0f);
    curve->setValue(0.0f);   // constant power
    const SnapshotId left = surface.capture(graph, "left", Point2{ 0.0f, 0.5f });

    position->setValue(1.0f);
    curve->setValue(1.0f);   // linear
    const SnapshotId right = surface.capture(graph, "right", Point2{ 1.0f, 0.5f });

    CHECK(surface.snapshotCount() == 2);
    CHECK(surface.find(left) != nullptr && surface.find(right) != nullptr);

    // Landing exactly on a snapshot must reproduce it, not merely approach it.
    surface.applyAt(Point2{ 0.0f, 0.5f }, graph);
    CHECK_CLOSE(position->value(), 0.0, 1e-5);
    CHECK_CLOSE(curve->value(), 0.0, 1e-5);

    surface.applyAt(Point2{ 1.0f, 0.5f }, graph);
    CHECK_CLOSE(position->value(), 1.0, 1e-5);
    CHECK_CLOSE(curve->value(), 1.0, 1e-5);

    // Halfway between blends the continuous parameter...
    surface.applyAt(Point2{ 0.5f, 0.5f }, graph);
    CHECK_CLOSE(position->value(), 0.5, 1e-4);

    // ...but the stepped one must land on a real choice, never between two.
    const float curveValue = curve->value();
    CHECK(curveValue == 0.0f || curveValue == 1.0f);

    // Weights are a partition of unity everywhere on the surface.
    std::vector<float> weights;
    for (int i = 0; i <= 8; ++i) {
        for (int j = 0; j <= 8; ++j) {
            surface.computeWeights(Point2{ i / 8.0f, j / 8.0f }, weights);
            CHECK(weights.size() == 2);
            CHECK_CLOSE(weights[0] + weights[1], 1.0, 1e-4);
            CHECK(weights[0] >= 0.0f && weights[1] >= 0.0f);
        }
    }

    // Closer to the left snapshot means the left snapshot dominates.
    surface.computeWeights(Point2{ 0.2f, 0.5f }, weights);
    CHECK(weights[0] > weights[1]);

    // Nearest mode is a hard switch.
    surface.setMode(InterpolationMode::Nearest);
    surface.applyAt(Point2{ 0.49f, 0.5f }, graph);
    CHECK_CLOSE(position->value(), 0.0, 1e-5);
    surface.applyAt(Point2{ 0.51f, 0.5f }, graph);
    CHECK_CLOSE(position->value(), 1.0, 1e-5);

    // Radial basis still sums to one, including far outside every radius.
    surface.setMode(InterpolationMode::RadialBasis);
    surface.setRadius(0.05f);
    surface.computeWeights(Point2{ 0.5f, 1.0f }, weights);
    CHECK_CLOSE(weights[0] + weights[1], 1.0, 1e-4);

    // Excluding a parameter leaves it alone when the surface is applied.
    surface.setMode(InterpolationMode::InverseDistance);
    const ParamAddress positionAddress{ fader, graph.node(fader)->indexOfParameter("position") };
    surface.setExcluded(positionAddress, true);
    position->setValue(0.33f);
    surface.applyAt(Point2{ 1.0f, 0.5f }, graph);
    CHECK_CLOSE(position->value(), 0.33, 1e-5);

    // Deleting the node prunes the dangling references.
    surface.setExcluded(positionAddress, false);
    graph.removeNode(fader);
    surface.pruneMissing(graph);
    CHECK(surface.snapshots()[0].values.empty());
}

void testMetasurfacePath() {
    TEST("metasurface automation path");

    Metasurface surface;
    surface.beginPathRecording();
    CHECK(surface.recordingPath());

    surface.addPathPoint(Point2{ 0.0f, 0.0f }, 0.0);
    surface.addPathPoint(Point2{ 1.0f, 0.0f }, 1.0);
    surface.addPathPoint(Point2{ 1.0f, 1.0f }, 2.0);
    surface.endPathRecording();

    CHECK(!surface.recordingPath());
    CHECK(surface.path().size() == 3);
    CHECK_CLOSE(surface.pathDuration(), 2.0, 1e-9);

    // Sampling interpolates between the recorded points.
    Point2 midpoint = surface.samplePath(0.25);
    CHECK_CLOSE(midpoint.x, 0.5, 1e-5);
    CHECK_CLOSE(midpoint.y, 0.0, 1e-5);

    midpoint = surface.samplePath(0.75);
    CHECK_CLOSE(midpoint.x, 1.0, 1e-5);
    CHECK_CLOSE(midpoint.y, 0.5, 1e-5);

    // A phase past the end wraps rather than running off.
    const Point2 wrapped = surface.samplePath(1.25);
    CHECK_CLOSE(wrapped.x, 0.5, 1e-5);

    // Synced playback follows the musical position, so two bars in at 8 beats
    // per lap is exactly one full lap.
    surface.setPathPlaying(true);
    surface.setPathSynced(true);
    surface.setPathBeats(8.0);
    const Point2 atStart = surface.advancePath(0.0, 0.0);
    CHECK_CLOSE(atStart.x, 0.0, 1e-5);
    const Point2 afterLap = surface.advancePath(0.0, 8.0);
    CHECK_CLOSE(afterLap.x, 0.0, 1e-5);
    const Point2 halfLap = surface.advancePath(0.0, 4.0);
    CHECK_CLOSE(halfLap.x, 1.0, 1e-5);
}

void testPatchRoundTrip() {
    TEST("patch save and load round trip");

    registerBuiltinNodes();

    Engine engine;
    engine.prepare(48000.0, 256, 2, 2);
    Metasurface surface;
    control::Surface panel;
    PatchViewState view;
    PatchMetadata metadata;

    patch::buildDefaultPatch(engine, surface);

    const std::size_t originalNodes = engine.graph().nodeCount();
    const std::size_t originalConnections = engine.graph().connections().size();
    CHECK(originalNodes == 6);
    CHECK(originalConnections == 6);
    CHECK(surface.snapshotCount() == 2);

    // Make the state distinctive so a lossy round trip would show up.
    engine.transport().setBpm(174.0);
    engine.transport().setTimeSignature(7, 8);
    engine.setMasterGainDb(-4.5f);
    view.zoom = 1.75f;
    view.canvasX = -320.0f;
    metadata.title = "night lab";
    metadata.notes = "two decks, one surface";

    NodeId faderId = kInvalidNode;
    for (const auto& node : engine.graph().nodes())
        if (node->typeName() == "crossfader") faderId = node->id();
    CHECK(faderId != kInvalidNode);

    engine.graph().node(faderId)->findParameter("position")->setValue(0.375f);
    engine.graph().node(faderId)->setName("the fader");
    engine.graph().node(faderId)->canvasX = 512.0f;
    engine.graph().node(faderId)->comment = "left hand";

    const JsonValue saved = patch::save(engine, surface, panel, view, metadata);
    const std::string text = saved.dump(2);
    CHECK(text.size() > 200);

    // Reload into a completely separate engine.
    Engine reloaded;
    reloaded.prepare(48000.0, 256, 2, 2);
    Metasurface reloadedSurface;
    control::Surface reloadedPanel;
    PatchViewState reloadedView;
    PatchMetadata reloadedMetadata;

    std::string parseError;
    const JsonValue parsed = JsonValue::parse(text, &parseError);
    CHECK(parseError.empty());

    const PatchLoadResult result =
        patch::load(parsed, reloaded, reloadedSurface, reloadedPanel, reloadedView, reloadedMetadata);
    CHECK(result.ok);
    if (!result.ok) std::printf("        %s\n", result.error.c_str());
    for (const std::string& warning : result.warnings)
        std::printf("        warning: %s\n", warning.c_str());
    CHECK(result.warnings.empty());

    CHECK(reloaded.graph().nodeCount() == originalNodes);
    CHECK(reloaded.graph().connections().size() == originalConnections);
    CHECK_CLOSE(reloaded.transport().bpm(), 174.0, 1e-9);
    CHECK(reloaded.transport().timeSigNumerator() == 7);
    CHECK(reloaded.transport().timeSigDenominator() == 8);
    CHECK_CLOSE(reloaded.masterGainDb(), -4.5, 1e-5);
    CHECK_CLOSE(reloadedView.zoom, 1.75, 1e-6);
    CHECK_CLOSE(reloadedView.canvasX, -320.0, 1e-6);
    CHECK(reloadedMetadata.title == "night lab");
    CHECK(reloadedMetadata.notes == "two decks, one surface");

    // Node identity survives, which is what the snapshots depend on.
    const Node* fader = reloaded.graph().node(faderId);
    CHECK(fader != nullptr);
    if (fader) {
        CHECK(fader->typeName() == "crossfader");
        CHECK(fader->name() == "the fader");
        CHECK(fader->comment == "left hand");
        CHECK_CLOSE(fader->canvasX, 512.0, 1e-6);
        CHECK_CLOSE(fader->findParameter("position")->value(), 0.375, 1e-6);
    }

    CHECK(reloadedSurface.snapshotCount() == 2);
    CHECK(reloadedSurface.snapshots()[0].name == "A");
    CHECK(!reloadedSurface.snapshots()[0].values.empty());

    // Saving the reload produces the same document: no drift across cycles.
    // Checked before anything below mutates the reloaded patch.
    const std::string second = patch::save(reloaded, reloadedSurface, reloadedPanel, reloadedView, reloadedMetadata).dump(2);
    CHECK(second == text);

    // Applying a reloaded snapshot must reach the same value it captured.
    reloadedSurface.applyAt(reloadedSurface.snapshots()[1].position, reloaded.graph());
    CHECK_CLOSE(reloaded.graph().node(faderId)->findParameter("position")->value(), 1.0, 1e-4);
}

void testPatchRejectsRubbish() {
    TEST("patch loader rejects and degrades gracefully");

    registerBuiltinNodes();

    Engine engine;
    engine.prepare(48000.0, 256, 2, 2);
    Metasurface surface;
    control::Surface panel;
    PatchViewState view;
    PatchMetadata metadata;

    // Not a patch at all.
    JsonValue wrong = JsonValue::object();
    wrong.set("format", "something-else");
    CHECK(!patch::load(wrong, engine, surface, panel, view, metadata).ok);

    // A patch referring to a node type we do not have should still open, with
    // the unknown node reported rather than silently swallowed.
    JsonValue root = JsonValue::object();
    root.set("format", patch::kFormatId);
    root.set("version", 1);

    JsonValue nodes = JsonValue::array();
    JsonValue good = JsonValue::object();
    good.set("id", 1);
    good.set("type", "util.gain");
    nodes.push(good);

    JsonValue bad = JsonValue::object();
    bad.set("id", 2);
    bad.set("type", "vst2.something.we.do.not.have");
    nodes.push(bad);
    root.set("nodes", nodes);

    JsonValue connections = JsonValue::array();
    JsonValue dangling = JsonValue::object();
    dangling.set("from", 1);
    dangling.set("fromPort", 0);
    dangling.set("to", 2);
    dangling.set("toPort", 0);
    connections.push(dangling);
    root.set("connections", connections);

    const PatchLoadResult result = patch::load(root, engine, surface, panel, view, metadata);
    CHECK(result.ok);
    CHECK(engine.graph().nodeCount() == 1);
    CHECK(engine.graph().connections().empty());
    CHECK(result.warnings.size() == 2);
}

void testPeArchitecture() {
    TEST("PE architecture detection");

    // Builds the smallest thing readPeArchitecture will accept: a DOS stub whose
    // e_lfanew points at a PE signature, followed by the COFF machine field and
    // the characteristics word. This is the check that decides whether a plugin
    // can be loaded in-process or has to go through the bridge, so it is worth
    // pinning down rather than only exercising against whatever is installed.
    const auto buildImage = [](std::uint16_t machine, std::uint16_t characteristics) {
        std::vector<std::uint8_t> image(0x100, 0);
        image[0] = 'M';
        image[1] = 'Z';

        constexpr std::uint32_t peOffset = 0x80;
        image[0x3C] = static_cast<std::uint8_t>(peOffset & 0xFF);
        image[0x3D] = static_cast<std::uint8_t>((peOffset >> 8) & 0xFF);

        image[peOffset + 0] = 'P';
        image[peOffset + 1] = 'E';
        image[peOffset + 2] = 0;
        image[peOffset + 3] = 0;
        image[peOffset + 4] = static_cast<std::uint8_t>(machine & 0xFF);
        image[peOffset + 5] = static_cast<std::uint8_t>((machine >> 8) & 0xFF);
        image[peOffset + 22] = static_cast<std::uint8_t>(characteristics & 0xFF);
        image[peOffset + 23] = static_cast<std::uint8_t>((characteristics >> 8) & 0xFF);
        return image;
    };

    const std::string path = "acomposter-pe-test.bin";
    constexpr std::uint16_t kDllFlag = 0x2000;

    auto image = buildImage(0x8664, kDllFlag);
    CHECK(writeFileBytes(path, image.data(), image.size()));
    CHECK(vst2::readPeArchitecture(path) == vst2::Architecture::X64);
    CHECK(vst2::isDynamicLibrary(path));

    image = buildImage(0x014C, kDllFlag);
    CHECK(writeFileBytes(path, image.data(), image.size()));
    CHECK(vst2::readPeArchitecture(path) == vst2::Architecture::X86);

    // An executable is a valid PE but not something to try loading as a plugin.
    image = buildImage(0x8664, 0);
    CHECK(writeFileBytes(path, image.data(), image.size()));
    CHECK(!vst2::isDynamicLibrary(path));

    // ARM64 is a real machine type we deliberately do not support.
    image = buildImage(0xAA64, kDllFlag);
    CHECK(writeFileBytes(path, image.data(), image.size()));
    CHECK(vst2::readPeArchitecture(path) == vst2::Architecture::Unknown);

    // Not a PE at all.
    const char* garbage = "this is not a windows binary at all, not even close";
    CHECK(writeFileBytes(path, garbage, std::strlen(garbage)));
    CHECK(vst2::readPeArchitecture(path) == vst2::Architecture::Unknown);

    // A missing file must report rather than crash.
    std::string error;
    CHECK(vst2::readPeArchitecture("no-such-file-anywhere.dll", &error)
              == vst2::Architecture::Unknown);
    CHECK(!error.empty());

    std::remove(path.c_str());
}

void testBase64() {
    TEST("base64 round trip");

    const char* cases[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    for (const char* text : cases) {
        const std::string source(text);
        const std::string encoded = base64Encode(source.data(), source.size());
        const std::vector<std::uint8_t> decoded = base64Decode(encoded);
        CHECK(std::string(decoded.begin(), decoded.end()) == source);
    }

    // Binary, including the bytes that would break a naive text round trip.
    std::vector<std::uint8_t> binary;
    for (int i = 0; i < 512; ++i) binary.push_back(static_cast<std::uint8_t>(i * 7 + i / 3));
    const std::vector<std::uint8_t> decoded = base64Decode(base64Encode(binary));
    CHECK(decoded == binary);

    // Whitespace inside the encoded form is tolerated.
    CHECK(base64Decode("Zm9v\n YmFy") == base64Decode("Zm9vYmFy"));
}


// ---------------------------------------------------------------------------
// Stem player
// ---------------------------------------------------------------------------

// Renders `beats` worth of blocks and returns the transport position reached,
// so a test can assert about what happened across a musical span rather than a
// sample count.
double renderBeats(Graph& graph, TransportState& transport, double beats, int blockSize) {
    const double beatsPerBlock = transport.bpm * blockSize / (60.0 * transport.sampleRate);
    const int blocks = static_cast<int>(std::ceil(beats / beatsPerBlock));

    for (int i = 0; i < blocks; ++i) {
        graph.render(transport, blockSize, 0, nullptr, nullptr);
        transport.ppqPosition += beatsPerBlock;
    }
    return transport.ppqPosition;
}


// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------


// The library tests write real files, so each one starts by clearing its own
// directory. Without this they pass once and then fail on every later run,
// which is a worse failure mode than not testing at all.
void clearTestLibrary(const std::string& root) {
    for (const DirectoryEntry& file : listDirectory(pathJoin(root, "entries"), { ".json" }))
        deleteFile(file.fullPath);
    deleteFile(pathJoin(root, "tags.json"));
}

void testLibraryRoundTrip() {
    TEST("library entries survive being written and re-read");

    const std::string root = "acomposter-test-tmp/library";
    clearTestLibrary(root);

    {
        library::Library lib;
        CHECK(lib.open(root));

        const std::string song = lib.create(library::EntryKind::Song, "Night Drive");
        CHECK(!song.empty());

        library::Entry* entry = lib.find(song);
        CHECK(entry != nullptr);
        entry->notes = "second verse needs a lift";
        entry->lyrics = "one line\ntwo line";
        entry->bpm = 124.0;
        CHECK(lib.save(song));

        CHECK(lib.addFile(song, "C:\\stems\\drums.wav"));
        // Adding the same file twice is what a second drag does, and must not
        // produce a duplicate.
        CHECK(lib.addFile(song, "C:\\stems\\drums.wav"));
        CHECK(lib.find(song)->files.size() == 1);

        const std::string project = lib.create(library::EntryKind::Project, "Album One");
        CHECK(lib.addMember(project, song));
    }

    // Re-opened from disk in a fresh object: this is the property that matters,
    // because the files are meant to be the source of truth rather than a cache.
    {
        library::Library lib;
        CHECK(lib.open(root));

        const auto songs = lib.entriesOfKind(library::EntryKind::Song);
        CHECK(songs.size() == 1);
        CHECK(songs[0]->name == "Night Drive");
        CHECK(songs[0]->notes == "second verse needs a lift");
        CHECK(songs[0]->lyrics == "one line\ntwo line");
        CHECK_CLOSE(songs[0]->bpm, 124.0, 1e-9);
        CHECK(songs[0]->files.size() == 1);

        const auto projects = lib.entriesOfKind(library::EntryKind::Project);
        CHECK(projects.size() == 1);
        CHECK(projects[0]->members.size() == 1);
        CHECK(projects[0]->members[0] == songs[0]->id);
    }
}

void testLibraryMultipleMembership() {
    TEST("a file can belong to several songs at once");

    const std::string root = "acomposter-test-tmp/library-2";
    clearTestLibrary(root);

    library::Library lib;
    CHECK(lib.open(root));

    const std::string a = lib.create(library::EntryKind::Song, "A Side");
    const std::string b = lib.create(library::EntryKind::Song, "B Side");

    const std::string shared = "C:\\stems\\shared-pad.wav";
    CHECK(lib.addFile(a, shared));
    CHECK(lib.addFile(b, shared));

    // The question the library exists to answer before anything is reorganised.
    const auto users = lib.entriesContaining(shared);
    CHECK(users.size() == 2);

    // Removing it from one leaves the other alone - nothing here owns a file.
    CHECK(lib.removeFile(a, shared));
    CHECK(lib.entriesContaining(shared).size() == 1);
}

void testLibraryTagging() {
    TEST("tagging a file makes an asset entry and survives a rename");

    const std::string root = "acomposter-test-tmp/library-3";
    clearTestLibrary(root);

    library::Library lib;
    CHECK(lib.open(root));
    CHECK(lib.palette().count() > 0);

    const std::string tagId = lib.palette().tags()[0].id;
    const std::string path = "C:\\stems\\kick-01.wav";

    CHECK(lib.setTagForFile(path, tagId));
    CHECK(lib.tagForFile(path) == tagId);

    // Renaming a tag must not orphan what is already tagged with it: entries
    // reference the id, and the id deliberately does not follow the name.
    lib.palette().rename(0, "low thump");
    CHECK(lib.tagForFile(path) == tagId);
    CHECK(lib.palette().tags()[0].name == "low thump");

    // Clearing works through the same call, which is what lets one control both
    // assign and unassign.
    CHECK(lib.setTagForFile(path, {}));
    CHECK(lib.tagForFile(path).empty());
}

void testLibrarySurvivesBadEntry() {
    TEST("one unreadable entry costs one entry, not the library");

    const std::string root = "acomposter-test-tmp/library-4";
    clearTestLibrary(root);

    {
        library::Library lib;
        CHECK(lib.open(root));
        lib.create(library::EntryKind::Song, "Good One");
    }

    // A half-written file, of the sort a crash mid-save would leave.
    writeFileText(pathJoin(pathJoin(root, "entries"), "broken.json"), "{ this is not json");

    library::Library lib;
    int skipped = 0;
    CHECK(lib.open(root, &skipped));
    CHECK(skipped == 1);
    CHECK(lib.entriesOfKind(library::EntryKind::Song).size() == 1);
}



void testStemSnippetExtraction() {
    TEST("a snippet is cut from the stem and snapped to the grid");

    const double rate = 48000.0;
    auto buffer = std::make_shared<SampleBuffer>(1, static_cast<std::int64_t>(rate * 8.0), rate);
    for (std::int64_t i = 0; i < buffer->frames(); ++i)
        buffer->channelForWrite(0)[i] = static_cast<float>(i) / static_cast<float>(buffer->frames());

    StemPlayerNode stems;
    stems.setStemFromBuffer(0, buffer, "source");
    stems.setStemBpm(120.0);   // a sixteenth is 0.125 s

    StemPlayerNode::Snippet snip;
    snip.slot = 0;
    snip.startSeconds = 1.0;
    snip.lengthSeconds = 0.47;      // not on the grid
    snip.tempoMatched = true;
    stems.setSnippet(snip);

    const auto matched = stems.extractSnippet(4);
    CHECK(matched != nullptr);
    // 0.47 s rounds to four sixteenths, which is half a second at 120.
    CHECK_CLOSE(matched->durationSeconds(), 0.5, 1e-6);

    // The audio really is the requested range, not the start of the file.
    CHECK_CLOSE(matched->channel(0)[0], 1.0 / 8.0, 1e-4);

    // Free-form takes the drag exactly.
    snip.tempoMatched = false;
    stems.setSnippet(snip);
    const auto free = stems.extractSnippet(4);
    CHECK(free != nullptr);
    CHECK_CLOSE(free->durationSeconds(), 0.47, 1e-3);

    // A selection too short to be a grain source is refused rather than
    // producing a buffer nothing can read.
    snip.lengthSeconds = 0.0001;
    snip.tempoMatched = false;
    stems.setSnippet(snip);
    CHECK(stems.extractSnippet(4) == nullptr);
}

void testBuildTrimStaysValid() {
    TEST("trim handles cannot cross or collapse");

    BuildNode build;

    build.setTrim(0.2f, 0.8f);
    CHECK_CLOSE(build.trimStart(), 0.2, 1e-6);
    CHECK_CLOSE(build.trimEnd(), 0.8, 1e-6);

    // Dragging the end past the start would give the grain scheduler a window
    // of zero or negative length to pick positions from.
    build.setTrim(0.6f, 0.1f);
    CHECK(build.trimEnd() > build.trimStart());

    build.setTrim(1.0f, 1.0f);
    CHECK(build.trimEnd() > build.trimStart());
    CHECK(build.trimEnd() <= 1.0f);
}

// Captures the loudest sample it was handed, so "did that fire" is one number.
class PeakProbeNode : public Node {
public:
    PeakProbeNode() : Node("test.peak", NodeCategory::Analysis) { addInput("in", 2); }
    void process(ProcessContext& ctx) override {
        peak = 0.0f;
        peakFrame = -1;
        const AudioBus& in = ctx.input(0);
        for (int c = 0; c < in.numChannels; ++c) {
            for (int f = 0; f < ctx.frames; ++f) {
                const float level = std::fabs(in.chan(c)[f]);
                if (level > peak) { peak = level; peakFrame = f; }
            }
        }
    }
    float peak = 0.0f;
    int peakFrame = -1;
};

std::shared_ptr<SampleBuffer> makeClick() {
    auto click = std::make_shared<SampleBuffer>(2, 64, 48000.0);
    for (int c = 0; c < 2; ++c) click->channelForWrite(c)[0] = 1.0f;
    return click;
}

void testSurfaceMacroRanges() {
    TEST("one control drives several parameters over their own ranges");

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    auto gainOwner = std::make_unique<GainNode>();
    auto filterOwner = std::make_unique<FilterNode>();
    const NodeId gainId = graph.addNode(std::move(gainOwner));
    const NodeId filterId = graph.addNode(std::move(filterOwner));

    Node* gain = graph.node(gainId);
    Node* filter = graph.node(filterId);
    CHECK(gain != nullptr && filter != nullptr);

    const ParamAddress gainAddress{ gainId, gain->indexOfParameter("gain") };
    const ParamAddress cutoffAddress{ filterId, filter->indexOfParameter("frequency") };
    CHECK(gainAddress.valid() && cutoffAddress.valid());

    control::Surface surface;
    const int knob = surface.add(control::ControlKind::Knob, "energy", 0, 0, 2, 2);
    CHECK(knob != 0);

    // Bound with the knob down, so the parameter's current value becomes the
    // bottom of its range and the top is left at the extreme - which is how a
    // macro actually gets built, rather than by typing two numbers.
    gain->parameter(gainAddress.param).setNormalised(0.2f);
    CHECK(surface.bind(knob, gainAddress, graph));
    CHECK(surface.bind(knob, cutoffAddress, graph));

    // The second target is deliberately given only the top half of its travel,
    // which is the whole point of per-target ranges.
    CHECK(surface.setTargetRange(knob, cutoffAddress, 0.5f, 1.0f));

    surface.setValue(knob, 0.0f, graph);
    CHECK_CLOSE(gain->parameter(gainAddress.param).normalised(), 0.2, 1e-5);
    CHECK_CLOSE(filter->parameter(cutoffAddress.param).normalised(), 0.5, 1e-5);

    surface.setValue(knob, 1.0f, graph);
    CHECK_CLOSE(gain->parameter(gainAddress.param).normalised(), 1.0, 1e-5);
    CHECK_CLOSE(filter->parameter(cutoffAddress.param).normalised(), 1.0, 1e-5);

    surface.setValue(knob, 0.5f, graph);
    CHECK_CLOSE(gain->parameter(gainAddress.param).normalised(), 0.6, 1e-5);
    CHECK_CLOSE(filter->parameter(cutoffAddress.param).normalised(), 0.75, 1e-5);

    // An inverted target runs backwards. One knob that opens a filter as it
    // closes a gate is a real thing to want, so low above high is a setting
    // rather than a mistake to be corrected.
    CHECK(surface.setTargetRange(knob, gainAddress, 1.0f, 0.0f));
    surface.setValue(knob, 0.25f, graph);
    CHECK_CLOSE(gain->parameter(gainAddress.param).normalised(), 0.75, 1e-5);
}

void testSurfaceSurvivesReloadAndPruning() {
    TEST("a surface reloads with its bindings, and drops the dead ones");

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    const NodeId gainId = graph.addNode(std::make_unique<GainNode>());
    const NodeId filterId = graph.addNode(std::make_unique<FilterNode>());

    const ParamAddress gainAddress{ gainId, graph.node(gainId)->indexOfParameter("gain") };
    const ParamAddress cutoffAddress{ filterId,
                                      graph.node(filterId)->indexOfParameter("frequency") };

    control::Surface surface;
    surface.setGrid(16, 10);

    const int pad = surface.add(control::ControlKind::XYPad, "space", 3, 1, 4, 4);
    CHECK(surface.bind(pad, gainAddress, graph, false));
    CHECK(surface.bind(pad, cutoffAddress, graph, true));

    const int page = surface.addPage("drums");
    surface.setActivePage(page);
    const int button = surface.add(control::ControlKind::Button, "kill", 0, 0, 2, 1);
    control::Control* buttonControl = surface.find(button);
    CHECK(buttonControl != nullptr);
    buttonControl->momentary = true;

    const std::string text = surface.toJson().dump(2);

    control::Surface reloaded;
    std::string parseError;
    reloaded.fromJson(JsonValue::parse(text, &parseError));
    CHECK(parseError.empty());

    CHECK(reloaded.columns() == 16);
    CHECK(reloaded.rows() == 10);
    CHECK(reloaded.pageCount() == 2);

    reloaded.setActivePage(0);
    const control::Control* reloadedPad = reloaded.find(pad);
    CHECK(reloadedPad != nullptr);
    CHECK(reloadedPad->kind == control::ControlKind::XYPad);
    CHECK(reloadedPad->targets.size() == 1);
    CHECK(reloadedPad->targetsY.size() == 1);
    CHECK(reloadedPad->targetsY.front().address == cutoffAddress);

    reloaded.setActivePage(1);
    const control::Control* reloadedButton = reloaded.find(button);
    CHECK(reloadedButton != nullptr);
    CHECK(reloadedButton->momentary);

    // A control added after a reload must not collide with one that came out of
    // the file - the id counter has to clear the highest id it read.
    const int fresh = reloaded.add(control::ControlKind::Knob, "new", 8, 0, 2, 2);
    CHECK(fresh != button);
    CHECK(fresh != pad);

    // Losing the node the pad was bound to costs the binding, not the control.
    CHECK(graph.removeNode(filterId));
    reloaded.pruneMissing(graph);

    reloaded.setActivePage(0);
    const control::Control* pruned = reloaded.find(pad);
    CHECK(pruned != nullptr);
    CHECK(pruned->targets.size() == 1);
    CHECK(pruned->targetsY.empty());
}

void testSurfaceGridKeepsControlsReachable() {
    TEST("shrinking the grid pulls controls back inside it");

    control::Surface surface;
    surface.setGrid(24, 16);

    const int knob = surface.add(control::ControlKind::Knob, "far", 20, 12, 3, 3);

    // A control left outside the grid is a control that cannot be seen or
    // clicked, so it is not left there.
    surface.setGrid(8, 6);

    const control::Control* control = surface.find(knob);
    CHECK(control != nullptr);
    CHECK(control->column + control->width <= 8);
    CHECK(control->row + control->height <= 6);

    // The last page is never removed: every other function here would need a
    // special case for a surface with nowhere to put a control.
    CHECK(!surface.removePage(0));
    CHECK(surface.pageCount() == 1);
}

void testDropFiresOnBuildRelease() {
    TEST("the drop lands on the grid line the build released to");

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    auto buildOwner = std::make_unique<BuildNode>();
    BuildNode* build = buildOwner.get();
    auto dropOwner = std::make_unique<DropNode>();
    DropNode* drop = dropOwner.get();
    auto probeOwner = std::make_unique<PeakProbeNode>();
    PeakProbeNode* probe = probeOwner.get();

    const NodeId buildId = graph.addNode(std::move(buildOwner));
    const NodeId dropId = graph.addNode(std::move(dropOwner));
    const NodeId probeId = graph.addNode(std::move(probeOwner));
    CHECK(graph.connect(dropId, 0, probeId, 0) != kInvalidConnection);

    drop->setBuildNode(buildId);
    drop->setLayer(0, makeClick(), "click");

    TransportState transport;
    transport.sampleRate = 48000.0;
    transport.bpm = 120.0;
    transport.playing = true;

    // 120 bpm at 48 kHz is 24000 frames to the beat. Start 200 frames short of
    // beat 1 so the release is asked for several blocks before it happens,
    // which is what "let go early, land on the grid" actually looks like.
    constexpr double kFramesPerBeat = 24000.0;
    constexpr int kFramesEarly = 200;
    transport.ppqPosition = 1.0 - kFramesEarly / kFramesPerBeat;

    const auto step = [&](int frames) {
        graph.render(transport, frames, 0, nullptr, nullptr);
        transport.ppqPosition += static_cast<double>(frames) / kFramesPerBeat;
        return probe->peak;
    };

    build->parameter(build->indexOfParameter("release")).setValue(1.0f);   // next beat

    // Engaging is not a drop. A node that fired on the way up would be firing
    // at the start of the build rather than at the end of it.
    build->parameter(build->indexOfParameter(BuildNode::kEngageParam)).setValue(1.0f);
    CHECK(step(64) < 1.0e-6f);

    // Let go, 200 frames before the beat. Nothing should sound yet: the drop
    // has been told when, not told to go.
    build->parameter(build->indexOfParameter(BuildNode::kEngageParam)).setValue(0.0f);
    CHECK(step(64) < 1.0e-6f);   // the release block itself, 136 frames early
    CHECK(step(64) < 1.0e-6f);   // 72 frames early

    // The block that contains the beat is the one that speaks - and it speaks
    // 8 frames in, not at the top of the block. Landing on the block boundary
    // instead would put the impact up to a buffer's length off the beat, which
    // at 512 frames is 10 ms and audible.
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK(probe->peak > 0.5f);
    CHECK(probe->peakFrame == 8);
    transport.ppqPosition += 64.0 / kFramesPerBeat;

    // And it fires once. The build holds "released but not yet at the line" for
    // every block in between, so an announcement made on that condition rather
    // than on its edge would fire the drop once per block.
    CHECK(step(64) < 1.0e-6f);
    CHECK(step(64) < 1.0e-6f);
}

void testDropIgnoresReleasesFromBeforeItExisted() {
    TEST("a drop added after a build has run does not fire on its history");

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    auto buildOwner = std::make_unique<BuildNode>();
    BuildNode* build = buildOwner.get();
    const NodeId buildId = graph.addNode(std::move(buildOwner));

    TransportState transport;
    transport.sampleRate = 48000.0;
    transport.bpm = 120.0;
    transport.playing = true;

    build->parameter(build->indexOfParameter("release")).setValue(2.0f);

    // A whole build runs through before the drop node exists at all.
    build->parameter(build->indexOfParameter(BuildNode::kEngageParam)).setValue(1.0f);
    graph.render(transport, 64, 0, nullptr, nullptr);
    build->parameter(build->indexOfParameter(BuildNode::kEngageParam)).setValue(0.0f);
    graph.render(transport, 64, 0, nullptr, nullptr);

    auto dropOwner = std::make_unique<DropNode>();
    DropNode* drop = dropOwner.get();
    auto probeOwner = std::make_unique<PeakProbeNode>();
    PeakProbeNode* probe = probeOwner.get();

    const NodeId dropId = graph.addNode(std::move(dropOwner));
    const NodeId probeId = graph.addNode(std::move(probeOwner));
    CHECK(graph.connect(dropId, 0, probeId, 0) != kInvalidConnection);

    drop->setBuildNode(buildId);
    drop->setLayer(0, makeClick(), "click");

    // The release counter is already non-zero. Firing on it would mean a drop
    // node added to a patch mid-set going off the instant it was created.
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK(probe->peak < 1.0e-6f);

    // But the next release still reaches it. Two blocks, because the graph is
    // free to schedule the drop ahead of the build - there is no audio
    // connection between them to order it - in which case the announcement is
    // seen on the following block.
    build->parameter(build->indexOfParameter(BuildNode::kEngageParam)).setValue(1.0f);
    graph.render(transport, 64, 0, nullptr, nullptr);
    build->parameter(build->indexOfParameter(BuildNode::kEngageParam)).setValue(0.0f);

    float peak = 0.0f;
    for (int block = 0; block < 2; ++block) {
        graph.render(transport, 64, 0, nullptr, nullptr);
        peak = std::max(peak, probe->peak);
    }
    CHECK(peak > 0.5f);
}

void testDropMuteAndManualTrigger() {
    TEST("a muted layer stays silent, and the manual trigger works alone");

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    auto dropOwner = std::make_unique<DropNode>();
    DropNode* drop = dropOwner.get();
    auto probeOwner = std::make_unique<PeakProbeNode>();
    PeakProbeNode* probe = probeOwner.get();

    const NodeId dropId = graph.addNode(std::move(dropOwner));
    const NodeId probeId = graph.addNode(std::move(probeOwner));
    CHECK(graph.connect(dropId, 0, probeId, 0) != kInvalidConnection);

    drop->setLayer(0, makeClick(), "click");

    TransportState transport;
    transport.sampleRate = 48000.0;

    // No build wired at all: the button still has to work, or a drop node
    // cannot be balanced before it is patched.
    drop->trigger();
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK(probe->peak > 0.5f);

    drop->parameter(drop->indexOfParameter("aMute")).setValue(1.0f);
    drop->trigger();
    graph.render(transport, 64, 0, nullptr, nullptr);
    CHECK(probe->peak < 1.0e-6f);
}

void testStemTagRouting() {
    TEST("a stem's tag decides its output until it is pinned");

    StemPlayerNode stems;

    // The palette's map, as the application publishes it each frame.
    stems.setTagRouting({ { "bass", 1 }, { "pads", 3 } });

    // Untagged stems stay on their own output rather than collapsing onto one.
    CHECK(stems.resolvedRoute(0) == 0);
    CHECK(stems.resolvedRoute(4) == 4);

    stems.setStemTag(0, "bass");
    CHECK(stems.resolvedRoute(0) == 1);

    stems.setStemTag(2, "pads");
    stems.setStemTag(3, "pads");
    // Two stems on one bus is the normal case, not a clash.
    CHECK(stems.resolvedRoute(2) == 3);
    CHECK(stems.resolvedRoute(3) == 3);

    // Pinning one of them breaks the link without retagging it.
    stems.setStemRoute(3, 6);
    CHECK(stems.resolvedRoute(3) == 6);
    CHECK(stems.resolvedRoute(2) == 3);

    // Handing it back returns it to the tag.
    stems.setStemRoute(3, -1);
    CHECK(stems.resolvedRoute(3) == 3);

    // A tag with no output assigned leaves the stem where it was.
    stems.setStemTag(5, "nothing-mapped-here");
    CHECK(stems.resolvedRoute(5) == 5);
}

void testStemRoutingPersists() {
    TEST("tags and pins survive a save and load");

    StemPlayerNode source;
    source.setTagRouting({ { "bass", 1 } });
    source.setStemTag(0, "bass");
    source.setStemRoute(2, 7);

    JsonValue state = JsonValue::object();
    source.saveExtraState(state);

    StemPlayerNode loaded;
    loaded.loadExtraState(state);
    loaded.setTagRouting({ { "bass", 1 } });

    CHECK(loaded.stemTag(0) == "bass");
    CHECK(loaded.resolvedRoute(0) == 1);
    CHECK(loaded.stemRoute(2) == 7);
    CHECK(loaded.resolvedRoute(2) == 7);
}

void testFftRoundTripAndPeak() {
    TEST("the FFT finds a tone where it is, and inverts to what it was");

    dsp::Fft fft(1024);
    CHECK(fft.size() == 1024);

    // A size that is not a power of two rounds up rather than misbehaving.
    dsp::Fft odd(1000);
    CHECK(odd.size() == 1024);

    constexpr double kRate = 48000.0;
    constexpr double kHertz = 1000.0;

    std::vector<float> tone(1024);
    for (int i = 0; i < 1024; ++i)
        tone[static_cast<std::size_t>(i)] = static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * kHertz * i / kRate));

    std::vector<float> magnitude;
    fft.magnitude(tone.data(), 1024, magnitude);
    CHECK(magnitude.size() == 512);

    int peakBin = 0;
    for (int i = 1; i < 512; ++i)
        if (magnitude[static_cast<std::size_t>(i)] > magnitude[static_cast<std::size_t>(peakBin)])
            peakBin = i;

    // 1 kHz at 48 kHz over 1024 points is bin 21.33, so 21 either way.
    CHECK(std::abs(fft.binFrequency(peakBin, kRate) - kHertz) < 60.0);

    // Forward then inverse returns the input. Without this the transform can be
    // wrong in a way a magnitude peak would not show - a bit-reversal bug that
    // scrambles phase leaves the spectrum looking perfect.
    std::vector<float> real = tone;
    std::vector<float> imag(1024, 0.0f);
    fft.forward(real.data(), imag.data());
    fft.inverse(real.data(), imag.data());

    double worst = 0.0;
    for (int i = 0; i < 1024; ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(real[static_cast<std::size_t>(i)])
                                          - tone[static_cast<std::size_t>(i)]));
    CHECK(worst < 1e-4);
}

void testNoteNaming() {
    TEST("frequencies name the right notes, above and below A4");

    int semitones = 0;
    double cents = 0.0;

    CHECK(dsp::frequencyToNote(440.0, semitones, cents));
    CHECK(semitones == 0);
    CHECK(std::fabs(cents) < 1.0);
    CHECK(std::string(dsp::noteNameForSemitone(semitones)) == "A");
    CHECK(dsp::octaveForSemitone(semitones) == 4);

    // Middle C, three semitones below A4 by name and nine below by count.
    CHECK(dsp::frequencyToNote(261.626, semitones, cents));
    CHECK(semitones == -9);
    CHECK(std::string(dsp::noteNameForSemitone(semitones)) == "C");
    CHECK(dsp::octaveForSemitone(semitones) == 4);

    // Two octaves down. C++ truncates toward zero, so negative semitone counts
    // are where a naive modulo names the wrong note.
    CHECK(dsp::frequencyToNote(65.406, semitones, cents));
    CHECK(std::string(dsp::noteNameForSemitone(semitones)) == "C");
    CHECK(dsp::octaveForSemitone(semitones) == 2);

    CHECK(dsp::frequencyToNote(880.0, semitones, cents));
    CHECK(dsp::octaveForSemitone(semitones) == 5);

    // Detuned upward lands on the same note and says how far off it is. A
    // third of a semitone rather than exactly half, because half is ambiguous
    // by construction and rounds whichever way the standard library does.
    CHECK(dsp::frequencyToNote(440.0 * std::pow(2.0, 0.3 / 12.0), semitones, cents));
    CHECK(semitones == 0);
    CHECK(std::fabs(cents - 30.0) < 2.0);

    CHECK(!dsp::frequencyToNote(4.0, semitones, cents));
}

void testAnalysisSeparatesSounds() {
    TEST("analysis tells a bass note from a hiss, and finds its pitch");

    constexpr double kRate = 48000.0;
    constexpr std::int64_t kFrames = 24000;   // half a second

    // A 110 Hz tone: A2.
    auto bass = std::make_shared<SampleBuffer>(1, kFrames, kRate);
    for (std::int64_t i = 0; i < kFrames; ++i)
        bass->channelForWrite(0)[i] = 0.8f * static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * 110.0 * static_cast<double>(i) / kRate));

    const library::Analysis bassAnalysis = library::analyse(*bass, "bass_note_03.wav");
    CHECK(bassAnalysis.valid);
    CHECK(std::fabs(bassAnalysis.pitchHz - 110.0) < 3.0);
    CHECK(bassAnalysis.pitchConfidence > 0.5f);
    CHECK(bassAnalysis.noteName == "A2");
    CHECK_CLOSE(bassAnalysis.peak, 0.8, 1e-3);

    // Numbers come out of the name in the order they appear.
    CHECK(bassAnalysis.filenameNumbers.size() == 1);
    CHECK(bassAnalysis.filenameNumbers[0] == 3);

    // Noise: the same length and level, no pitch anyone should trust.
    // nextFloat() is already bipolar - rescaling it as if it were 0..1 adds a
    // large DC offset, and DC correlates perfectly at every lag, which reads as
    // a confidently pitched signal.
    auto hiss = std::make_shared<SampleBuffer>(1, kFrames, kRate);
    dsp::Xorshift random{ 0x1234u };
    for (std::int64_t i = 0; i < kFrames; ++i)
        hiss->channelForWrite(0)[i] = random.nextFloat() * 0.8f;

    const library::Analysis hissAnalysis = library::analyse(*hiss, "hat.wav");
    CHECK(hissAnalysis.valid);
    // Noise correlates with itself about as well as chance allows, which is
    // the number the harmonic search leans on to leave it out.
    CHECK(hissAnalysis.pitchConfidence < 0.2f);
    CHECK(hissAnalysis.noteName.empty());

    // Brightness separates them without any further thought, which is the whole
    // reason the centroid is here.
    CHECK(hissAnalysis.centroidHz > bassAnalysis.centroidHz * 4.0);

    // A tone is like itself and unlike noise.
    CHECK(library::similarity(bassAnalysis, bassAnalysis) > 0.95f);
    CHECK(library::similarity(bassAnalysis, hissAnalysis) < 0.5f);

    // And a fifth above is still more like the bass than the hiss is.
    auto fifth = std::make_shared<SampleBuffer>(1, kFrames, kRate);
    for (std::int64_t i = 0; i < kFrames; ++i)
        fifth->channelForWrite(0)[i] = 0.8f * static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * 164.81 * static_cast<double>(i) / kRate));

    const library::Analysis fifthAnalysis = library::analyse(*fifth, "e.wav");
    CHECK(library::similarity(bassAnalysis, fifthAnalysis)
          > library::similarity(bassAnalysis, hissAnalysis));
}

void testFilenameNumbersAndPitchClass() {
    TEST("numbers are pulled out of names, and octaves fold to one key");

    const std::vector<int> numbers = library::extractNumbers("kick_03_174bpm take2.wav");
    CHECK(numbers.size() == 3);       // 03, 174, 2
    CHECK(numbers[0] == 3);
    CHECK(numbers[1] == 174);
    CHECK(numbers[2] == 2);

    CHECK(library::extractNumbers("no digits here").empty());

    // A run too long to be a take number is skipped rather than overflowing.
    const std::vector<int> huge = library::extractNumbers("bounce_12345678901234.wav");
    CHECK(huge.empty());

    // A C2 bass and a C4 pad are in the same key, which is the comparison a
    // harmonic search has to make.
    CHECK(library::sameePitchClass(-9, 3));
    CHECK(library::sameePitchClass(-21, -9));
    CHECK(!library::sameePitchClass(-9, -8));
}

// Builds a buffer from a generator, for the classifier and slicer tests.
std::shared_ptr<SampleBuffer> makeTone(double hz, double seconds, double decay,
                                       double rate = 48000.0) {
    const auto frames = static_cast<std::int64_t>(rate * seconds);
    auto buffer = std::make_shared<SampleBuffer>(1, frames, rate);
    for (std::int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        buffer->channelForWrite(0)[i] =
            static_cast<float>(0.8 * std::sin(2.0 * 3.14159265358979323846 * hz * t)
                               * std::exp(-decay * t));
    }
    buffer->computePeak();
    return buffer;
}

std::shared_ptr<SampleBuffer> makeNoise(double seconds, double decay, std::uint32_t seed,
                                        double rate = 48000.0) {
    const auto frames = static_cast<std::int64_t>(rate * seconds);
    auto buffer = std::make_shared<SampleBuffer>(1, frames, rate);
    dsp::Xorshift random{ seed };
    for (std::int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        buffer->channelForWrite(0)[i] =
            static_cast<float>(0.8 * random.nextFloat() * std::exp(-decay * t));
    }
    buffer->computePeak();
    return buffer;
}

void testClassifierNamesWhatItHears() {
    TEST("the classifier separates a kick, a hat and a bass note");

    // A kick: a low sine swept down, decaying fast.
    auto kick = std::make_shared<SampleBuffer>(1, 16000, 48000.0);
    for (std::int64_t i = 0; i < 16000; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        const double hz = 60.0 * std::exp(-16.0 * t);
        kick->channelForWrite(0)[i] =
            static_cast<float>(0.9 * std::sin(2.0 * 3.14159265358979323846 * hz * t)
                               * std::exp(-22.0 * t));
    }
    kick->computePeak();

    const library::Classification kickGuess = library::classify(
        library::analyse(*kick, "bd_01.wav"));
    CHECK(kickGuess.instrument == library::Instrument::Kick);
    CHECK(kickGuess.confidence > 0.5f);
    // The reason is part of the output, not decoration: a person approving a
    // hundred guesses needs to see why each was made.
    CHECK(!kickGuess.reason.empty());

    const library::Classification hatGuess = library::classify(
        library::analyse(*makeNoise(0.08, 60.0, 0x51u), "ch.wav"));
    CHECK(hatGuess.instrument == library::Instrument::HiHat);

    // The same sound three times as long is a snare, not a hat. At the old
    // 0.4 s cutoff the hat rule caught it before the snare rule ever ran.
    const library::Classification snareGuess = library::classify(
        library::analyse(*makeNoise(0.30, 14.0, 0x52u), "sd.wav"));
    CHECK(snareGuess.instrument != library::Instrument::HiHat);

    // A low sine that rings for a second is a bass note; the same spectrum
    // over 200 ms is a kick. Duration is the only thing that separates them.
    const library::Classification bassGuess = library::classify(
        library::analyse(*makeTone(55.0, 1.2, 2.0), "sub.wav"));
    CHECK(bassGuess.instrument == library::Instrument::Bass);

    const library::Classification leadGuess = library::classify(
        library::analyse(*makeTone(660.0, 0.9, 2.0), "stab.wav"));
    CHECK(leadGuess.instrument == library::Instrument::Lead);

    // Long and unpitched is the bucket of last resort, and says so with a low
    // confidence rather than by claiming to know.
    const library::Classification fxGuess = library::classify(
        library::analyse(*makeNoise(3.0, 0.4, 0x77u), "wash.wav"));
    CHECK(fxGuess.instrument == library::Instrument::Fx);
    CHECK(fxGuess.confidence < 0.5f);

    // An empty analysis is Unknown rather than a confident guess about silence.
    CHECK(library::classify(library::Analysis{}).instrument == library::Instrument::Unknown);
}

void testInstrumentTagsMatchByName() {
    TEST("instruments find their tag by name, through renames");

    library::TagPalette palette;
    palette.loadDefaults();

    const std::string kickTag = library::tagForInstrument(palette, library::Instrument::Kick);
    CHECK(!kickTag.empty());
    CHECK(palette.find(kickTag)->name == "kick");

    CHECK(!library::tagForInstrument(palette, library::Instrument::Bass).empty());
    CHECK(!library::tagForInstrument(palette, library::Instrument::Pad).empty());

    // Renaming a tag keeps its id, and matching by name has to follow the new
    // name - which is the whole reason it is not matched by id.
    palette.rename(palette.indexOf(kickTag), "bass drum");
    const std::string renamed = library::tagForInstrument(palette, library::Instrument::Kick);
    CHECK(renamed == kickTag);

    // A palette with nothing suitable returns nothing rather than the first tag.
    library::TagPalette sparse;
    sparse.add("weird", 0xFF000000u);
    CHECK(library::tagForInstrument(sparse, library::Instrument::Kick).empty());
}

void testProposedNamesKeepWhatTheNameKnew() {
    TEST("a proposed name keeps the number the old one carried");

    library::Analysis analysis;
    analysis.valid = true;
    analysis.filenameNumbers = { 7 };

    CHECK(library::proposeName(analysis, library::Instrument::Kick, 99) == "kick-07");

    // The last number, not the first: "sub_a2_05" has a digit in its note name
    // and the take number is the one at the end.
    analysis.filenameNumbers = { 2, 5 };
    CHECK(library::proposeName(analysis, library::Instrument::Kick, 99) == "kick-05");
    analysis.filenameNumbers = { 7 };

    // A pitched sound is named by its note, because anything that has to sit in
    // a key is easier to find that way than by a number.
    analysis.noteName = "C2";
    CHECK(library::proposeName(analysis, library::Instrument::Bass, 99) == "bass-c2-07");

    // Nothing in the old name means the index it was handed.
    analysis.filenameNumbers.clear();
    CHECK(library::proposeName(analysis, library::Instrument::Bass, 3) == "bass-c2-03");
}

void testSlicerFindsHits() {
    TEST("the slicer cuts a loop where the hits are");

    constexpr double kRate = 48000.0;
    constexpr double kSpacing = 0.5;
    constexpr int kHits = 6;

    auto loop = std::make_shared<SampleBuffer>(1, static_cast<std::int64_t>(kRate * 3.0), kRate);
    dsp::Xorshift random{ 0x99u };

    // Six noise bursts, half a second apart. Each decays into the next, which
    // is the case plain amplitude detection gets wrong.
    for (int hit = 0; hit < kHits; ++hit) {
        const auto start = static_cast<std::int64_t>(kRate * kSpacing * hit);
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(kRate * 0.4); ++i) {
            if (start + i >= loop->frames()) break;
            const double t = static_cast<double>(i) / kRate;
            loop->channelForWrite(0)[start + i] +=
                static_cast<float>(0.7 * random.nextFloat() * std::exp(-9.0 * t));
        }
    }
    loop->computePeak();

    const std::vector<std::int64_t> points = library::findSlicePoints(*loop);

    // Six hits, and no more than a couple of extras from the decay tails.
    CHECK(points.size() >= 6);
    CHECK(points.size() <= 8);

    // Each expected hit has a slice point near it. "Near" is a hop of the
    // analysis window, which is what the resolution actually is.
    for (int hit = 0; hit < kHits; ++hit) {
        const auto expected = static_cast<std::int64_t>(kRate * kSpacing * hit);
        bool found = false;
        for (const std::int64_t point : points)
            if (std::llabs(point - expected) < static_cast<std::int64_t>(kRate * 0.03))
                found = true;
        CHECK(found);
    }

    // The head of the file is always a slice: dropping it would silently lose
    // one hit from every loop.
    CHECK(points.front() < static_cast<std::int64_t>(kRate * 0.02));

    // Extracting one gives a buffer of the right length, faded at both ends so
    // a cut mid-cycle does not click.
    auto slice = library::extractSlice(*loop, points[1], points[2]);
    CHECK(slice != nullptr);
    CHECK(slice->frames() == points[2] - points[1]);
    CHECK(std::fabs(slice->channel(0)[0]) < 1e-6f);

    // Silence produces nothing to cut rather than a slice per window.
    auto quiet = std::make_shared<SampleBuffer>(1, static_cast<std::int64_t>(kRate), kRate);
    CHECK(library::findSlicePoints(*quiet).size() <= 1);
}

void testFileIndexCacheAndQuery() {
    TEST("the index caches its analysis and answers questions about it");

    const std::string root = "acomposter-test-tmp/index";
    createDirectories(root);
    for (const DirectoryEntry& file : listDirectory(root, { ".json", ".wav" }))
        deleteFile(file.fullPath);

    library::FileIndex index;

    // Built by hand rather than scanned: a scan needs real files on disk and a
    // thread, and what is being tested here is the querying and the cache.
    const auto make = [](const char* name, double seconds, double centroid,
                         const char* note, int semitones, float rms) {
        library::IndexedFile file;
        file.path = std::string("C:\\samples\\") + name;
        file.name = name;
        file.sizeBytes = 1000;
        file.modifiedTime = 42;
        file.analysis.valid = true;
        file.analysis.durationSeconds = seconds;
        file.analysis.centroidHz = centroid;
        file.analysis.noteName = note;
        file.analysis.semitonesFromA4 = semitones;
        file.analysis.rms = rms;
        file.analysis.filenameNumbers = library::extractNumbers(name);
        for (int i = 0; i < library::kSpectrumBands; ++i)
            file.analysis.bands[i] = 1.0f / library::kSpectrumBands;
        return file;
    };

    // Reaching the index's private list is not possible, so it is loaded from a
    // cache file - which also exercises the path the application uses.
    {
        JsonValue root_object = JsonValue::object();
        JsonValue array = JsonValue::array();

        const library::IndexedFile files[] = {
            make("kick_01.wav", 0.4, 200.0, "", 0, 0.5f),
            make("bass_c2.wav", 1.2, 300.0, "C2", -21, 0.4f),
            make("pad_c4.wav", 8.0, 1200.0, "C4", 3, 0.2f),
            make("hat_09.wav", 0.1, 9000.0, "", 0, 0.3f),
        };

        for (const library::IndexedFile& file : files) {
            JsonValue entry = JsonValue::object();
            entry.set("path", file.path);
            entry.set("name", file.name);
            entry.set("size", file.sizeBytes);
            entry.set("modified", file.modifiedTime);

            JsonValue analysis = JsonValue::object();
            analysis.set("duration", file.analysis.durationSeconds);
            analysis.set("centroid", file.analysis.centroidHz);
            analysis.set("note", file.analysis.noteName);
            analysis.set("semitones", file.analysis.semitonesFromA4);
            analysis.set("rms", file.analysis.rms);

            JsonValue numbers = JsonValue::array();
            for (const int number : file.analysis.filenameNumbers) numbers.push(number);
            analysis.set("numbers", std::move(numbers));

            entry.set("analysis", std::move(analysis));
            array.push(std::move(entry));
        }

        root_object.set("files", std::move(array));
        CHECK(writeFileText(pathJoin(root, "cache.json"), root_object.dump(1)));
    }

    CHECK(index.loadCache(pathJoin(root, "cache.json")));
    CHECK(index.files().size() == 4);

    library::Filter filter;

    // Sorting by something the analysis found is most of what the index is for.
    auto byLength = index.query(filter, library::SortKey::Duration, false);
    CHECK(byLength.size() == 4);
    CHECK(byLength.front()->name == "hat_09.wav");
    CHECK(byLength.back()->name == "pad_c4.wav");

    auto byBright = index.query(filter, library::SortKey::Brightness, true);
    CHECK(byBright.front()->name == "hat_09.wav");

    // Harmonic search folds octaves: a C2 bass and a C4 pad are the same key,
    // which is the comparison that makes it useful at all.
    filter.semitonesFromA4 = 3;   // C
    auto inC = index.query(filter, library::SortKey::Name, false);
    CHECK(inC.size() == 2);
    CHECK(inC.front()->name == "bass_c2.wav");

    // An unpitched file is never in a key, whichever key is asked for.
    filter.semitonesFromA4 = -128;
    filter.pitchedOnly = true;
    CHECK(index.query(filter, library::SortKey::Name, false).size() == 2);

    filter.pitchedOnly = false;
    filter.text = "kick";
    CHECK(index.query(filter, library::SortKey::Name, false).size() == 1);

    filter.text.clear();
    filter.minSeconds = 0.2;
    filter.maxSeconds = 2.0;
    auto inRange = index.query(filter, library::SortKey::Name, false);
    CHECK(inRange.size() == 2);

    // The cache round-trips, which is what makes reopening a folder instant.
    CHECK(index.saveCache(pathJoin(root, "again.json")));

    library::FileIndex reloaded;
    CHECK(reloaded.loadCache(pathJoin(root, "again.json")));
    CHECK(reloaded.files().size() == 4);

    const library::IndexedFile* bass = reloaded.find("C:\\samples\\bass_c2.wav");
    CHECK(bass != nullptr);
    CHECK(bass->analysis.noteName == "C2");
    CHECK(bass->analysis.filenameNumbers.size() == 1);
    CHECK(bass->analysis.filenameNumbers[0] == 2);

    // A file that was never indexed is absent rather than a default-constructed
    // entry pretending it was analysed.
    CHECK(reloaded.find("C:\\samples\\nothing.wav") == nullptr);
}

void testProjectRunningOrder() {
    TEST("a project's running order is the project's, not the songs'");

    const std::string root = "acomposter-test-tmp/library-6";
    clearTestLibrary(root);

    library::Library lib;
    CHECK(lib.open(root));

    const std::string album = lib.create(library::EntryKind::Project, "Album");
    const std::string other = lib.create(library::EntryKind::Project, "Compilation");
    const std::string a = lib.create(library::EntryKind::Song, "Opener");
    const std::string b = lib.create(library::EntryKind::Song, "Middle");
    const std::string c = lib.create(library::EntryKind::Song, "Closer");

    CHECK(lib.addMember(album, a));
    CHECK(lib.addMember(album, b));
    CHECK(lib.addMember(album, c));

    CHECK(lib.find(album)->members.size() == 3);
    CHECK(lib.find(album)->members[0] == a);

    // Moving is by places, and running off either end does nothing rather than
    // wrapping - a song at the top of the order stays there.
    CHECK(lib.moveMember(album, c, -1));
    CHECK(lib.find(album)->members[1] == c);
    CHECK(lib.find(album)->members[2] == b);

    CHECK(!lib.moveMember(album, a, -1));
    CHECK(lib.find(album)->members[0] == a);

    // The same song in two projects sits in a different place in each. This is
    // why the order lives on the project: a position field on the song could
    // only ever answer for one of them.
    CHECK(lib.addMember(other, c));
    CHECK(lib.addMember(other, a));
    CHECK(lib.find(other)->members[0] == c);
    CHECK(lib.find(album)->members[0] == a);

    // Removing from one project leaves the other and leaves the song.
    CHECK(lib.removeMember(album, c));
    CHECK(lib.find(album)->members.size() == 2);
    CHECK(lib.find(other)->members.size() == 2);
    CHECK(lib.find(c) != nullptr);

    // And the order survives being written and read back.
    library::Library reopened;
    CHECK(reopened.open(root));
    CHECK(reopened.find(album)->members.size() == 2);
    CHECK(reopened.find(other)->members[0] == c);

    // Deleting a song takes it out of every project that referenced it - and
    // stays gone after a reopen, which is the half that a memory-only removal
    // would quietly fail.
    CHECK(reopened.remove(a));
    CHECK(reopened.find(album)->members.size() == 1);

    library::Library third;
    CHECK(third.open(root));
    CHECK(third.find(album)->members.size() == 1);
    CHECK(third.find(other)->members.size() == 1);
}

void testChainPresetRoundTrip() {
    TEST("chain presets survive being written and re-read");

    library::ChainStore store;
    store.open("acomposter-test-tmp/chains");

    library::ChainPreset preset;
    preset.name = "tight bass";

    library::ChainPlugin plugin;
    plugin.name = "Test Gain";
    plugin.path = "C:\\vstplugins\\TestGain.dll";
    plugin.uniqueId = 0x54475831;
    plugin.state = { 0x00, 0x01, 0x02, 0xFF, 0x80 };
    preset.plugins.push_back(plugin);

    library::ChainPlugin second;
    second.name = "No State";
    second.path = "C:\\vstplugins\\Other.dll";
    second.parameters = { 0.25f, 0.5f, 0.75f };
    preset.plugins.push_back(second);

    CHECK(store.save(preset));

    library::ChainPreset loaded;
    CHECK(store.load("tight bass", loaded));
    CHECK(loaded.name == "tight bass");
    CHECK(loaded.plugins.size() == 2);

    // Opaque plugin state has to come back byte for byte; it is the whole
    // reason a preset is worth more than a plugin list.
    CHECK(loaded.plugins[0].state.size() == 5);
    CHECK(loaded.plugins[0].state[3] == 0xFF);
    CHECK(loaded.plugins[0].uniqueId == 0x54475831);

    CHECK(loaded.plugins[1].parameters.size() == 3);
    CHECK_CLOSE(loaded.plugins[1].parameters[2], 0.75, 1e-6);

    const auto names = store.names();
    CHECK(std::find(names.begin(), names.end(), "tight bass") != names.end());
}

void testChainStoreNamesSurviveSanitising() {
    TEST("a chain name that is not a file name still loads back");

    const std::string root = "acomposter-test-tmp/chains-2";
    for (const DirectoryEntry& file : listDirectory(pathJoin(root, "chains"), { ".json" }))
        deleteFile(file.fullPath);

    library::ChainStore store;
    store.open(root);

    // The name comes from a text field, so it can contain anything. It is
    // sanitised on the way to disk, and the display name is read back out of
    // the file - which is the only reason loading by the name the user typed
    // works at all.
    library::ChainPreset preset;
    preset.name = "drums: bus / glue";
    preset.plugins.push_back({});
    preset.plugins.back().name = "Comp";
    preset.plugins.back().path = "C:\\vst\\Comp.dll";
    CHECK(store.save(preset));

    const auto names = store.names();
    CHECK(std::find(names.begin(), names.end(), "drums: bus / glue") != names.end());

    library::ChainPreset loaded;
    CHECK(store.load("drums: bus / glue", loaded));
    CHECK(loaded.plugins.size() == 1);

    CHECK(store.remove("drums: bus / glue"));
    CHECK(!store.load("drums: bus / glue", loaded));
}

void testLibraryOpensItsChainStore() {
    TEST("a library carries its chains as well as its tags");

    const std::string root = "acomposter-test-tmp/library-5";
    clearTestLibrary(root);
    for (const DirectoryEntry& file : listDirectory(pathJoin(root, "chains"), { ".json" }))
        deleteFile(file.fullPath);

    library::Library lib;
    CHECK(lib.open(root));
    CHECK(lib.chains().isOpen());

    library::ChainPreset preset;
    preset.name = "pad wash";
    preset.plugins.push_back({});
    preset.plugins.back().name = "Verb";
    CHECK(lib.chains().save(preset));

    // Reopening the same directory has to find it: chains living beside the
    // tags is the whole reason a library folder is portable.
    library::Library reopened;
    CHECK(reopened.open(root));
    const auto names = reopened.chains().names();
    CHECK(std::find(names.begin(), names.end(), "pad wash") != names.end());
}

void testStemSectionLaunch() {
    TEST("stem player defers a section change to the loop boundary");

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    auto owner = std::make_unique<StemPlayerNode>();
    StemPlayerNode* stems = owner.get();
    graph.addNode(std::move(owner));

    // Two four-bar sections back to back.
    stems->addSection(StemSection{ "a", 0, 4, 0.0f });
    stems->addSection(StemSection{ "b", 4, 4, 0.5f });
    CHECK(stems->sectionCount() == 2);

    TransportState transport;
    transport.sampleRate = 48000.0;
    transport.bpm = 120.0;
    transport.timeSigNumerator = 4;
    transport.playing = true;

    // Two bars in - halfway through section A's loop - ask for section B.
    renderBeats(graph, transport, 8.0, 128);
    CHECK(stems->activeSection() == 0);

    stems->requestSection(1);
    renderBeats(graph, transport, 1.0, 128);

    // Still playing A, with B queued: the whole point of the deferred launch.
    CHECK(stems->activeSection() == 0);
    CHECK(stems->pendingSection() == 1);

    // Past the end of the sixteen-beat loop, B has taken over.
    renderBeats(graph, transport, 8.0, 128);
    CHECK(stems->activeSection() == 1);
    CHECK(stems->pendingSection() == -1);
}

void testStemImmediateLaunch() {
    TEST("stem player switches at once when asked to");

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    auto owner = std::make_unique<StemPlayerNode>();
    StemPlayerNode* stems = owner.get();
    graph.addNode(std::move(owner));

    stems->addSection(StemSection{ "a", 0, 4, 0.0f });
    stems->addSection(StemSection{ "b", 4, 4, 0.5f });

    const ParamIndex launch = stems->indexOfParameter("launch");
    CHECK(launch >= 0);
    stems->parameter(launch).setValue(3.0f);   // immediate

    TransportState transport;
    transport.sampleRate = 48000.0;
    transport.bpm = 120.0;
    transport.playing = true;

    renderBeats(graph, transport, 2.0, 128);
    stems->requestSection(1);
    renderBeats(graph, transport, 0.25, 128);

    CHECK(stems->activeSection() == 1);
}

void testStemSectionPersistence() {
    TEST("stem sections survive a save and load");

    StemPlayerNode source;
    source.addSection(StemSection{ "intro", 0, 8, 0.1f });
    source.addSection(StemSection{ "drop", 16, 16, 0.7f });

    JsonValue state = JsonValue::object();
    source.saveExtraState(state);

    StemPlayerNode loaded;
    loaded.loadExtraState(state);

    CHECK(loaded.sectionCount() == 2);
    CHECK(loaded.sections()[1].name == "drop");
    CHECK(loaded.sections()[1].startBar == 16);
    CHECK(loaded.sections()[1].lengthBars == 16);
}

// ---------------------------------------------------------------------------
// Colour engine
// ---------------------------------------------------------------------------


void testStemTempoDetection() {
    TEST("stem player works a tempo back from the stem length");

    StemPlayerNode stems;

    // 16 bars of 4/4 at 128 bpm is exactly 30 seconds.
    auto buffer = std::make_shared<SampleBuffer>(2, static_cast<std::int64_t>(48000.0 * 30.0), 48000.0);
    stems.setStemFromBuffer(0, buffer, "sixteen-bars");

    int bars = 0;
    const double bpm = stems.detectBpm(4, &bars);

    CHECK_CLOSE(bpm, 128.0, 0.01);
    CHECK(bars == 16);

    // Setting it by hand overrides, and zero goes back to following the project.
    stems.setStemBpm(174.0);
    CHECK_CLOSE(stems.stemBpm(), 174.0, 1e-6);
    stems.setStemBpm(0.0);
    CHECK_CLOSE(stems.stemBpm(), 0.0, 1e-6);
}

void testStemSpectrum() {
    TEST("stem spectrum separates low from high");

    // A low sine and a high sine, each in its own stem.
    const double rate = 48000.0;
    const std::int64_t frames = static_cast<std::int64_t>(rate);

    auto low = std::make_shared<SampleBuffer>(1, frames, rate);
    auto high = std::make_shared<SampleBuffer>(1, frames, rate);
    for (std::int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        low->channelForWrite(0)[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 60.0 * t));
        high->channelForWrite(0)[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 8000.0 * t));
    }

    StemPlayerNode stems;
    stems.setStemFromBuffer(0, low, "low");
    stems.setStemFromBuffer(1, high, "high");

    const auto& lowSpectrum = stems.spectrum(0);
    const auto& highSpectrum = stems.spectrum(1);
    CHECK(!lowSpectrum.empty());
    CHECK(!highSpectrum.empty());

    // Sampled from the middle, away from the filters settling at the start.
    const auto& a = lowSpectrum[lowSpectrum.size() / 2];
    const auto& b = highSpectrum[highSpectrum.size() / 2];

    CHECK(a.low > a.high);
    CHECK(b.high > b.low);
}


// ---------------------------------------------------------------------------
// Stem effect racks
// ---------------------------------------------------------------------------

void testDownstreamChain() {
    TEST("chain discovery follows a series and stops at a split");

    registerBuiltinNodes();

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    const NodeId stems = graph.addNode(NodeFactory::instance().create("stem.player"));
    const NodeId a = graph.addNode(NodeFactory::instance().create("util.gain"));
    const NodeId b = graph.addNode(NodeFactory::instance().create("util.filter"));

    graph.connect(stems, 0, a, 0);
    graph.connect(a, 0, b, 0);

    const std::vector<NodeId> chain = downstreamChain(graph, stems, 0);
    CHECK(chain.size() == 2);
    CHECK(chain[0] == a);
    CHECK(chain[1] == b);

    // A second stem with nothing on it has an empty chain, and chains do not
    // leak across ports.
    CHECK(downstreamChain(graph, stems, 1).empty());

    // Splitting the tail ends the chain there: past a split the audio is no
    // longer only this stem's.
    const NodeId c = graph.addNode(NodeFactory::instance().create("util.gain"));
    const NodeId d = graph.addNode(NodeFactory::instance().create("util.gain"));
    graph.connect(b, 0, c, 0);
    graph.connect(b, 0, d, 0);

    const std::vector<NodeId> split = downstreamChain(graph, stems, 0);
    CHECK(split.size() == 2);
}

void testChainStopsAtSummingNode() {
    TEST("chain discovery stops where two sources meet");

    registerBuiltinNodes();

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    const NodeId stems = graph.addNode(NodeFactory::instance().create("stem.player"));
    const NodeId other = graph.addNode(NodeFactory::instance().create("util.tone"));
    const NodeId shared = graph.addNode(NodeFactory::instance().create("util.gain"));

    graph.connect(stems, 0, shared, 0);
    graph.connect(other, 0, shared, 0);

    // `shared` carries something other than this stem, so it is not part of
    // this stem's rack and must not be adopted into a per-stem colour chain.
    CHECK(downstreamChain(graph, stems, 0).empty());
}

void testColorAdoptsStemChains() {
    TEST("colour engine links every stem rack in one go");

    registerBuiltinNodes();

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    const NodeId stems = graph.addNode(NodeFactory::instance().create("stem.player"));

    // A gain in the rack stands in for a plugin's neighbour: it is in the
    // chain but is not a plugin, and must be left alone so a colour preset
    // does not fight the mix.
    const NodeId gain = graph.addNode(NodeFactory::instance().create("util.gain"));
    graph.connect(stems, 0, gain, 0);

    auto owner = std::make_unique<ColorNode>();
    ColorNode* color = owner.get();
    graph.addNode(std::move(owner));

    int plugins = -1;
    const int added = color->adoptStemChains(graph, &plugins);

    CHECK(plugins == 0);
    CHECK(added == 0);
    CHECK(color->targets().empty());
}

void testColorNeutralIsUnchanged() {
    TEST("colour engine leaves the middle untouched");

    registerBuiltinNodes();

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    // A plain gain node stands in for a plugin: what matters is that it has an
    // automatable parameter, not what it does with it.
    const NodeId target = graph.addNode(NodeFactory::instance().create("util.gain"));
    CHECK(target != kInvalidNode);

    auto owner = std::make_unique<ColorNode>();
    ColorNode* color = owner.get();
    const NodeId colorId = graph.addNode(std::move(owner));
    CHECK(colorId != kInvalidNode);

    Node* gain = graph.node(target);
    const ParamIndex gainParam = gain->indexOfParameter("gain");
    CHECK(gainParam >= 0);

    // Neutral is captured where the chain already sits.
    gain->parameter(gainParam).setNormalised(0.5f);
    color->addTarget(ParamAddress{ target, gainParam }, graph);
    color->captureEnd(ColorNode::End::Neutral, graph);

    gain->parameter(gainParam).setNormalised(0.0f);
    color->captureEnd(ColorNode::End::Red, graph);

    gain->parameter(gainParam).setNormalised(1.0f);
    color->captureEnd(ColorNode::End::Blue, graph);

    // Dead centre puts it back exactly where neutral was captured, whatever the
    // two ends are - the property that makes the knob safe to leave alone.
    color->setColor(0.0f);
    color->serviceFromMessageThread();
    CHECK_CLOSE(gain->parameter(gainParam).normalised(), 0.5, 1e-4);

    color->setColor(-1.0f);
    color->serviceFromMessageThread();
    CHECK_CLOSE(gain->parameter(gainParam).normalised(), 0.0, 1e-4);

    color->setColor(1.0f);
    color->serviceFromMessageThread();
    CHECK_CLOSE(gain->parameter(gainParam).normalised(), 1.0, 1e-4);

    // Half way to red is half way between neutral and red, not between red and
    // blue - the two half-axes have to be independent for an asymmetric preset.
    color->setColor(-0.5f);
    color->serviceFromMessageThread();
    CHECK_CLOSE(gain->parameter(gainParam).normalised(), 0.25, 1e-4);
}

void testColorPresetRebinds() {
    TEST("colour preset re-binds by name, not by index");

    registerBuiltinNodes();

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    const NodeId target = graph.addNode(NodeFactory::instance().create("util.gain"));
    graph.node(target)->setName("volcano");

    auto owner = std::make_unique<ColorNode>();
    ColorNode* color = owner.get();
    graph.addNode(std::move(owner));

    Node* gain = graph.node(target);
    const ParamIndex gainParam = gain->indexOfParameter("gain");
    color->addTarget(ParamAddress{ target, gainParam }, graph);
    color->captureEnd(ColorNode::End::Neutral, graph);
    gain->parameter(gainParam).setNormalised(0.2f);
    color->captureEnd(ColorNode::End::Red, graph);

    const JsonValue preset = color->savePreset("tight", graph);

    // Into a second graph where the same node has a different id.
    BlockCounter otherClock{ 0 };
    Graph other;
    other.setClock(&otherClock);
    other.prepare(48000.0, 128);

    other.addNode(NodeFactory::instance().create("util.gain"));   // shifts the ids along
    const NodeId moved = other.addNode(NodeFactory::instance().create("util.gain"));
    other.node(moved)->setName("volcano");

    auto secondOwner = std::make_unique<ColorNode>();
    ColorNode* second = secondOwner.get();
    other.addNode(std::move(secondOwner));

    std::vector<std::string> unmatched;
    const int bound = second->loadPreset(preset, other, &unmatched);

    CHECK(bound == 1);
    CHECK(unmatched.empty());
    CHECK(second->targets()[0].address.node == moved);
    CHECK_CLOSE(second->targets()[0].redValue, 0.2, 1e-4);
}

void testColorPresetReportsMissing() {
    TEST("colour preset reports what it could not bind");

    registerBuiltinNodes();

    BlockCounter clock{ 0 };
    Graph graph;
    graph.setClock(&clock);
    graph.prepare(48000.0, 128);

    const NodeId target = graph.addNode(NodeFactory::instance().create("util.gain"));
    graph.node(target)->setName("aether");

    auto owner = std::make_unique<ColorNode>();
    ColorNode* color = owner.get();
    graph.addNode(std::move(owner));

    color->addTarget(ParamAddress{ target, graph.node(target)->indexOfParameter("gain") }, graph);
    const JsonValue preset = color->savePreset("wash", graph);

    // A graph with no plugin of that name at all.
    BlockCounter emptyClock{ 0 };
    Graph empty;
    empty.setClock(&emptyClock);
    empty.prepare(48000.0, 128);

    auto secondOwner = std::make_unique<ColorNode>();
    ColorNode* second = secondOwner.get();
    empty.addNode(std::move(secondOwner));

    std::vector<std::string> unmatched;
    const int bound = second->loadPreset(preset, empty, &unmatched);

    // Nothing bound, and it said so rather than pointing at the wrong control.
    CHECK(bound == 0);
    CHECK(unmatched.size() == 1);
    CHECK(second->targets().empty());
}

} // namespace

int main() {
    std::printf("acomposter tests\n----------------\n");

    testJson();
    testParameterMapping();
    testSmoothing();
    testWavRoundTrip();
    testBpmDetection();
    testFadeLaws();
    testInterpolation();
    testTransport();
    testGraphRouting();
    testGraphFeedback();
    testChannelAdaptation();
    testBypass();
    testLibraryRoundTrip();
    testLibraryMultipleMembership();
    testLibraryTagging();
    testLibrarySurvivesBadEntry();
    testStemSnippetExtraction();
    testBuildTrimStaysValid();
    testDropFiresOnBuildRelease();
    testDropIgnoresReleasesFromBeforeItExisted();
    testDropMuteAndManualTrigger();
    testSurfaceMacroRanges();
    testSurfaceSurvivesReloadAndPruning();
    testSurfaceGridKeepsControlsReachable();
    testStemTagRouting();
    testStemRoutingPersists();
    testFftRoundTripAndPeak();
    testNoteNaming();
    testAnalysisSeparatesSounds();
    testFilenameNumbersAndPitchClass();
    testClassifierNamesWhatItHears();
    testInstrumentTagsMatchByName();
    testProposedNamesKeepWhatTheNameKnew();
    testSlicerFindsHits();
    testFileIndexCacheAndQuery();
    testProjectRunningOrder();
    testChainPresetRoundTrip();
    testChainStoreNamesSurviveSanitising();
    testLibraryOpensItsChainStore();
    testStemSectionLaunch();
    testStemImmediateLaunch();
    testStemSectionPersistence();
    testStemTempoDetection();
    testStemSpectrum();
    testDownstreamChain();
    testChainStopsAtSummingNode();
    testColorAdoptsStemChains();
    testColorNeutralIsUnchanged();
    testColorPresetRebinds();
    testColorPresetReportsMissing();
    testMetasurfaceInterpolation();
    testMetasurfacePath();
    testPatchRoundTrip();
    testPatchRejectsRubbish();
    testPeArchitecture();
    testBase64();

    std::printf("----------------\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
