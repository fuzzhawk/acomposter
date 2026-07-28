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
#include "../src/core/Json.h"
#include "../src/core/Parameter.h"
#include "../src/core/Transport.h"
#include "../src/dsp/Dsp.h"
#include "../src/meta/Metasurface.h"
#include "../src/nodes/NodeFactory.h"
#include "../src/patch/Patch.h"

#include <cmath>
#include <cstdio>
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

    std::printf("----------------\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
