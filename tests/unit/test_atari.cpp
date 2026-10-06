#include <gtest/gtest.h>

#include <gygax/inference/atari.hpp>

using namespace gygax::inference::atari;

TEST(AtariFrame, EncodesAndParsesCallResultErrorDone) {
    EXPECT_EQ(encodeCall("c0", "6*7"), "ATARI::CALL(t=c0;i=6*7)");
    auto call = parse(encodeCall("c0", "6*7"));
    ASSERT_EQ(call.type, FrameType::Call);
    EXPECT_EQ(call.get("t"), "c0");
    EXPECT_EQ(call.get("i"), "6*7");

    auto result = parse(encodeResult("c0", "42"));
    ASSERT_EQ(result.type, FrameType::Result);
    EXPECT_EQ(result.get("t"), "c0");
    EXPECT_EQ(result.get("o"), "42");

    auto err = parse(encodeError("c1", "unknown tool"));
    ASSERT_EQ(err.type, FrameType::Error);
    EXPECT_EQ(err.get("t"), "c1");
    EXPECT_EQ(err.get("e"), "unknown tool");

    auto done = parse(encodeDone("the answer is 42"));
    ASSERT_EQ(done.type, FrameType::Done);
    EXPECT_EQ(done.get("a"), "the answer is 42");
}

TEST(AtariFrame, RoundTripsValuesContainingDelimiters) {
    const auto weird = "has;semi)paren\\backslash and = signs = too";
    const auto frame = encodeCall("c0", weird);
    EXPECT_NE(frame.find("\\;"), std::string::npos);
    EXPECT_NE(frame.find("\\)"), std::string::npos);
    EXPECT_NE(frame.find("\\\\"), std::string::npos);
    const auto parsed = parse(frame);
    ASSERT_EQ(parsed.type, FrameType::Call);
    EXPECT_EQ(parsed.get("i"), weird);
}

TEST(AtariFrame, FindsAFrameEmbeddedInSurroundingText) {
    const auto text = "Let me think about this.\n" + encodeCall("c2", "sqrt(2)") + "\nthat should do it";
    const auto frame = parse(text);
    ASSERT_EQ(frame.type, FrameType::Call);
    EXPECT_EQ(frame.get("t"), "c2");
    EXPECT_EQ(frame.get("i"), "sqrt(2)");
}

TEST(AtariFrame, SkipsMalformedOccurrencesAndFindsALaterValidOne) {
    const auto text = "ATARI::NOPE(x=y) then ATARI::CALL no-parens-here then ATARI::DONE(a=ok)";
    const auto frame = parse(text);
    ASSERT_EQ(frame.type, FrameType::Done);
    EXPECT_EQ(frame.get("a"), "ok");
}

TEST(AtariFrame, UnknownWhenNoFrameIsPresent) {
    const auto frame = parse("just a plain final answer, no protocol here");
    EXPECT_EQ(frame.type, FrameType::Unknown);
    EXPECT_TRUE(frame.fields.empty());
    EXPECT_EQ(frame.get("a", "fallback"), "fallback");
}

TEST(AtariSchema, InternsAssignsShortSequentialCodesAndIsIdempotent) {
    Schema schema;
    const auto c0 = schema.intern("math.eval");
    const auto c1 = schema.intern("time.now");
    EXPECT_EQ(c0, "c0");
    EXPECT_EQ(c1, "c1");
    EXPECT_EQ(schema.intern("math.eval"), c0);
    EXPECT_EQ(schema.size(), 2U);
    EXPECT_TRUE(schema.contains("math.eval"));
    EXPECT_FALSE(schema.contains("node.info"));
    EXPECT_EQ(schema.resolve(c0).value(), "math.eval");
    EXPECT_EQ(schema.resolve(c1).value(), "time.now");
    EXPECT_FALSE(schema.resolve("c99").has_value());
}

TEST(AtariSchema, DeclareFrameOnlyEmitsWhatIsPending) {
    Schema schema;
    EXPECT_EQ(schema.declareFrame(), "");
    schema.intern("math.eval");
    schema.intern("time.now");
    const auto frame = schema.declareFrame();
    EXPECT_EQ(frame, "ATARI::SCHEMA(c0=math.eval;c1=time.now)");
    EXPECT_EQ(schema.declareFrame(), "");
    schema.intern("node.info");
    EXPECT_EQ(schema.declareFrame(), "ATARI::SCHEMA(c2=node.info)");
}

TEST(AtariSchema, MergeSchemaFrameLetsAPeerResolvePeerAssignedCodes) {
    Schema sender;
    sender.intern("math.eval");
    sender.intern("time.now");
    const auto frame = sender.declareFrame();

    Schema receiver;
    std::string error;
    ASSERT_TRUE(receiver.mergeSchemaFrame(frame, &error)) << error;
    EXPECT_EQ(receiver.resolve("c0").value(), "math.eval");
    EXPECT_EQ(receiver.resolve("c1").value(), "time.now");

    ASSERT_TRUE(receiver.mergeSchemaFrame(frame, &error)) << error;

    EXPECT_FALSE(receiver.mergeSchemaFrame("ATARI::CALL(t=c0;i=x)", &error));
    EXPECT_NE(error.find("SCHEMA"), std::string::npos);

    EXPECT_FALSE(receiver.mergeSchemaFrame("ATARI::SCHEMA(c0=node.info)", &error));
    EXPECT_NE(error.find("c0"), std::string::npos);
}

TEST(AtariProtocol, IsShorterThanTheEquivalentJsonToolCall) {
    Schema schema;
    const auto code = schema.intern("math.eval");
    const std::string json = R"({"tool": "math.eval", "input": "6*7"})";
    const std::string terse = encodeCall(code, "6*7");
    EXPECT_LT(terse.size(), json.size());

    const std::string jsonResult = R"(Tool result (math.eval): 42)";
    const std::string terseResult = encodeResult(code, "42");
    EXPECT_LT(terseResult.size(), jsonResult.size());
}
