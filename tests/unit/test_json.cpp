#include <gtest/gtest.h>

#include <gygax/core/json.hpp>

using gygax::json::parse;
using gygax::json::Value;

TEST(Json, ParsesScalarsAndContainers) {
    auto v = parse(R"({"a":[1,2.5,"s",null,true,false],"b":{"c":-3,"d":1e3}})");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->find("a")->asArray().size(), 6U);
    EXPECT_TRUE(v->find("a")->asArray()[0].isInt());
    EXPECT_DOUBLE_EQ(v->find("a")->asArray()[1].asDouble(), 2.5);
    EXPECT_EQ(v->find("b")->getInt("c"), -3);
    EXPECT_DOUBLE_EQ(v->find("b")->getDouble("d"), 1000.0);
    EXPECT_FALSE(v->find("b")->find("d")->isInt());
}

TEST(Json, RoundTripsUnicodeAndEscapes) {
    const std::string source = "{\"k\":\"line\\nbreak \\\"quoted\\\" \\u00e9 \\ud83d\\ude00\"}";
    auto v = parse(source);
    ASSERT_TRUE(v);
    EXPECT_EQ(v->getString("k"), "line\nbreak \"quoted\" \xC3\xA9 \xF0\x9F\x98\x80");
    auto again = parse(v->dump());
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, *v);
}

TEST(Json, RejectsMalformedDocuments) {
    for (const char* bad : {"", "{", "[1,]", "{\"a\":}", "01", "1.", "-", "\"abc", "\"\\x\"", "\"\\ud800\"", "\"\\udc00\"", "{\"a\":1} x",
                            "nul", "[1 2]", "{'a':1}", "\"\x01\""}) {
        std::string error;
        EXPECT_FALSE(parse(bad, &error)) << "accepted: " << bad;
        EXPECT_FALSE(error.empty()) << bad;
    }
}

TEST(Json, EnforcesDepthLimit) {
    std::string deep(200, '[');
    deep += std::string(200, ']');
    std::string error;
    EXPECT_FALSE(parse(deep, &error));
    EXPECT_NE(error.find("depth"), std::string::npos);
    EXPECT_TRUE(parse(deep, nullptr, 256));
}

TEST(Json, LargeIntegersFallBackToDouble) {
    auto v = parse("[9223372036854775807, 9223372036854775808]");
    ASSERT_TRUE(v);
    EXPECT_TRUE(v->asArray()[0].isInt());
    EXPECT_FALSE(v->asArray()[1].isInt());
}

TEST(Json, BuildsAndDumpsDeterministically) {
    Value v = Value::object();
    v["b"] = 2;
    v["a"] = "x";
    v["list"].push(1);
    v["list"].push("two");
    EXPECT_EQ(v.dump(), R"({"a":"x","b":2,"list":[1,"two"]})");
    EXPECT_NE(v.dump(2).find("\n  \"a\": \"x\""), std::string::npos);
}

TEST(Json, NonFiniteDoublesSerializeAsNull) {
    Value v = Value::object();
    v["nan"] = std::numeric_limits<double>::quiet_NaN();
    v["inf"] = std::numeric_limits<double>::infinity();
    EXPECT_EQ(v.dump(), R"({"inf":null,"nan":null})");
}

TEST(Json, TypedGettersFallBack) {
    auto v = parse(R"({"s":"x","n":3,"b":true})");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->getString("n", "fallback"), "fallback");
    EXPECT_EQ(v->getInt("s", 9), 9);
    EXPECT_TRUE(v->getBool("b"));
    EXPECT_FALSE(v->getBool("missing"));
    EXPECT_EQ(v->getInt("missing", 4), 4);
}
