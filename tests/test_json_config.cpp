// JSON parsing/serialisation and configuration validation.
#include <gtest/gtest.h>

#include <string>

#include "cachescope/config.hpp"
#include "cachescope/json.hpp"

namespace cachescope {
namespace {

const char* kMinimalConfig = R"({
  "name": "unit-test",
  "levels": [
    { "name": "l1d", "size_kb": 32, "assoc": 8, "block_size": 64, "hit_latency": 4 }
  ]
})";

TEST(Json, ParsesScalarsArraysAndObjects) {
  const Json j = Json::parse(R"({"a":1,"b":"two","c":true,"d":null,"e":[1,2,3],"f":{"g":-4.5}})");
  EXPECT_EQ(j.at("a").as_int(), 1);
  EXPECT_EQ(j.at("b").as_string(), "two");
  EXPECT_TRUE(j.at("c").as_bool());
  EXPECT_TRUE(j.at("d").is_null());
  ASSERT_TRUE(j.at("e").is_array());
  EXPECT_EQ(j.at("e").items().size(), 3u);
  EXPECT_DOUBLE_EQ(j.at("f").at("g").as_double(), -4.5);
}

TEST(Json, HasTreatsExplicitNullAsAbsent) {
  const Json j = Json::parse(R"({"present":1,"nulled":null})");
  EXPECT_TRUE(j.has("present"));
  EXPECT_FALSE(j.has("nulled")) << "an explicit null reads as 'not configured'";
  EXPECT_FALSE(j.has("missing"));
}

TEST(Json, AcceptsCommentsInConfigFiles) {
  const Json j = Json::parse(R"({
    // a line comment
    "a": 1,  // trailing comment
    /* a block
       comment */
    "b": 2
  })");
  EXPECT_EQ(j.at("a").as_int(), 1);
  EXPECT_EQ(j.at("b").as_int(), 2);
}

TEST(Json, HandlesEscapesInStrings) {
  const Json j = Json::parse(R"({"s":"a\"b\\c\nd\teA"})");
  EXPECT_EQ(j.at("s").as_string(), "a\"b\\c\nd\te" "A");
}

TEST(Json, ReportsLineAndColumnOnError) {
  try {
    Json::parse("{\n  \"a\": 1,\n  \"b\" 2\n}");
    FAIL() << "expected a parse error";
  } catch (const JsonError& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("line 3"), std::string::npos) << what;
  }
}

TEST(Json, RejectsTrailingGarbage) {
  EXPECT_THROW(Json::parse("{\"a\":1} extra"), JsonError);
}

TEST(Json, IntegralNumbersSerialiseWithoutADecimalPoint) {
  Json j = Json::object();
  j.set("count", 1234);
  j.set("rate", 0.25);
  const std::string text = j.dump();
  EXPECT_NE(text.find("\"count\":1234"), std::string::npos) << text;
  EXPECT_EQ(text.find("1234.0"), std::string::npos) << text;
  EXPECT_NE(text.find("0.25"), std::string::npos) << text;
}

TEST(Json, RoundTripsThroughDumpAndParse) {
  const Json original = Json::parse(kMinimalConfig);
  const Json again = Json::parse(original.dump(2));
  EXPECT_EQ(again.at("name").as_string(), "unit-test");
  EXPECT_EQ(again.at("levels").items()[0].at("size_kb").as_int(), 32);
}

TEST(Json, TypeMismatchThrows) {
  const Json j = Json::parse(R"({"s":"text","n":1})");
  EXPECT_THROW(j.at("s").as_double(), JsonError);
  EXPECT_THROW(j.at("n").as_string(), JsonError);
  EXPECT_THROW(j.at("nope"), JsonError);
}

// ---------------------------------------------------------------- config

TEST(Config, MinimalConfigFillsInDefaults) {
  const SimConfig c = SimConfig::from_json(Json::parse(kMinimalConfig));
  EXPECT_EQ(c.name, "unit-test");
  ASSERT_EQ(c.levels.size(), 1u);
  EXPECT_EQ(c.levels[0].size_bytes, 32u * 1024u);
  EXPECT_EQ(c.levels[0].num_sets(), 64u) << "32 KiB / (8 ways * 64 B) = 64 sets";
  EXPECT_EQ(c.levels[0].write_policy, WritePolicy::WriteBack);
  EXPECT_EQ(c.levels[0].alloc_policy, AllocPolicy::WriteAllocate);
  EXPECT_EQ(c.levels[0].replacement, ReplacementKind::LRU);
  EXPECT_EQ(c.levels[0].prefetcher.kind, PrefetcherKind::None);
  EXPECT_EQ(c.third_field, ThirdField::Size);
  EXPECT_EQ(c.inclusion, InclusionPolicy::NonInclusive);
}

TEST(Config, SizeMayBeGivenInBytesKibibytesOrMebibytes) {
  const char* cfg = R"({"levels":[
    {"name":"a","size_bytes":4096,"assoc":1,"block_size":64},
    {"name":"b","size_kb":64,"assoc":1,"block_size":64},
    {"name":"c","size_mb":2,"assoc":1,"block_size":64}
  ]})";
  const SimConfig c = SimConfig::from_json(Json::parse(cfg));
  EXPECT_EQ(c.levels[0].size_bytes, 4096u);
  EXPECT_EQ(c.levels[1].size_bytes, 64u * 1024u);
  EXPECT_EQ(c.levels[2].size_bytes, 2u * 1024u * 1024u);
}

TEST(Config, PrefetcherAcceptsAStringOrAnObject) {
  const char* cfg = R"({"levels":[
    {"name":"l1d","size_kb":32,"assoc":8,"block_size":64,"prefetcher":"next-line"},
    {"name":"l2","size_kb":256,"assoc":8,"block_size":64,
     "prefetcher":{"kind":"stride","degree":4,"distance":2,"table_entries":128,
                   "stride_confidence":3,"on_miss_only":false}}
  ]})";
  const SimConfig c = SimConfig::from_json(Json::parse(cfg));
  EXPECT_EQ(c.levels[0].prefetcher.kind, PrefetcherKind::NextLine);
  EXPECT_EQ(c.levels[0].prefetcher.degree, 1u) << "defaults apply to the shorthand form";

  EXPECT_EQ(c.levels[1].prefetcher.kind, PrefetcherKind::Stride);
  EXPECT_EQ(c.levels[1].prefetcher.degree, 4u);
  EXPECT_EQ(c.levels[1].prefetcher.distance, 2u);
  EXPECT_EQ(c.levels[1].prefetcher.table_entries, 128u);
  EXPECT_EQ(c.levels[1].prefetcher.stride_confidence, 3u);
  EXPECT_FALSE(c.levels[1].prefetcher.on_miss_only);
}

TEST(Config, PolicySpellingsAreAccepted) {
  const char* cfg = R"({"levels":[
    {"name":"l1d","size_kb":32,"assoc":8,"block_size":64,
     "write_policy":"wt","alloc_policy":"nwa","replacement":"fifo"}
  ]})";
  const SimConfig c = SimConfig::from_json(Json::parse(cfg));
  EXPECT_EQ(c.levels[0].write_policy, WritePolicy::WriteThrough);
  EXPECT_EQ(c.levels[0].alloc_policy, AllocPolicy::NoWriteAllocate);
  EXPECT_EQ(c.levels[0].replacement, ReplacementKind::FIFO);
}

TEST(Config, DefaultConfigIsAValidThreeLevelHierarchy) {
  const SimConfig c = default_config();
  ASSERT_EQ(c.levels.size(), 3u);
  EXPECT_EQ(c.levels[0].name, "l1d");
  EXPECT_EQ(c.levels[1].name, "l2");
  EXPECT_EQ(c.levels[2].name, "llc");
  EXPECT_NO_THROW(c.validate());
}

TEST(Config, SourceBucketsFollowLevelCount) {
  SimConfig three = default_config();
  EXPECT_EQ(three.source_for_level(0), ServiceSource::L1);
  EXPECT_EQ(three.source_for_level(1), ServiceSource::L2);
  EXPECT_EQ(three.source_for_level(2), ServiceSource::LLC);

  SimConfig two = default_config();
  two.levels.pop_back();
  EXPECT_EQ(two.source_for_level(0), ServiceSource::L1);
  EXPECT_EQ(two.source_for_level(1), ServiceSource::LLC)
      << "in a two-level hierarchy the second level IS the last-level cache";
}

TEST(Config, RoundTripsThroughJson) {
  const SimConfig a = default_config();
  const SimConfig b = SimConfig::from_json(a.to_json());
  ASSERT_EQ(b.levels.size(), a.levels.size());
  for (std::size_t i = 0; i < a.levels.size(); ++i) {
    EXPECT_EQ(b.levels[i].name, a.levels[i].name);
    EXPECT_EQ(b.levels[i].size_bytes, a.levels[i].size_bytes);
    EXPECT_EQ(b.levels[i].assoc, a.levels[i].assoc);
    EXPECT_EQ(b.levels[i].block_size, a.levels[i].block_size);
    EXPECT_EQ(b.levels[i].hit_latency, a.levels[i].hit_latency);
  }
  EXPECT_EQ(b.dram.latency_cycles, a.dram.latency_cycles);
  EXPECT_DOUBLE_EQ(b.dram.bytes_per_cycle, a.dram.bytes_per_cycle);
  EXPECT_EQ(b.branch.kind, a.branch.kind);
  EXPECT_DOUBLE_EQ(b.branch.density, a.branch.density);
}

// --- validation ---------------------------------------------------------

TEST(ConfigValidation, RejectsEmptyLevels) {
  EXPECT_THROW(SimConfig::from_json(Json::parse(R"({"levels":[]})")), ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(R"({})")), ConfigError);
}

TEST(ConfigValidation, RejectsUnnamedLevel) {
  EXPECT_THROW(SimConfig::from_json(Json::parse(R"({"levels":[{"size_kb":32}]})")), ConfigError);
}

TEST(ConfigValidation, RejectsSizeThatIsNotAMultipleOfWaysTimesBlock) {
  // 10 KiB with 8 ways of 64 B is 20 sets, not an integer power of two.
  const char* cfg = R"({"levels":[{"name":"l1d","size_bytes":10240,"assoc":8,"block_size":64}]})";
  EXPECT_THROW(SimConfig::from_json(Json::parse(cfg)), ConfigError);
}

TEST(ConfigValidation, RejectsNonPowerOfTwoSetCount) {
  // 3 sets x 1 way x 64 B = 192 B: a legal multiple but not a power of two.
  const char* cfg = R"({"levels":[{"name":"l1d","size_bytes":192,"assoc":1,"block_size":64}]})";
  EXPECT_THROW(SimConfig::from_json(Json::parse(cfg)), ConfigError);
}

TEST(ConfigValidation, RejectsBadBlockSize) {
  const char* odd = R"({"levels":[{"name":"l1d","size_bytes":4096,"assoc":1,"block_size":48}]})";
  EXPECT_THROW(SimConfig::from_json(Json::parse(odd)), ConfigError);
  const char* tiny = R"({"levels":[{"name":"l1d","size_bytes":4096,"assoc":1,"block_size":2}]})";
  EXPECT_THROW(SimConfig::from_json(Json::parse(tiny)), ConfigError);
}

TEST(ConfigValidation, RejectsZeroAssociativity) {
  const char* cfg = R"({"levels":[{"name":"l1d","size_bytes":4096,"assoc":0,"block_size":64}]})";
  EXPECT_THROW(SimConfig::from_json(Json::parse(cfg)), ConfigError);
}

TEST(ConfigValidation, RejectsDuplicateLevelNames) {
  const char* cfg = R"({"levels":[
    {"name":"l1d","size_kb":32,"assoc":8,"block_size":64},
    {"name":"l1d","size_kb":256,"assoc":8,"block_size":64}
  ]})";
  EXPECT_THROW(SimConfig::from_json(Json::parse(cfg)), ConfigError);
}

TEST(ConfigValidation, RejectsAnOuterLevelSmallerThanTheOneAbove) {
  const char* cfg = R"({"levels":[
    {"name":"l1d","size_kb":256,"assoc":8,"block_size":64},
    {"name":"l2","size_kb":32,"assoc":8,"block_size":64}
  ]})";
  EXPECT_THROW(SimConfig::from_json(Json::parse(cfg)), ConfigError);
}

TEST(ConfigValidation, RejectsShrinkingBlockSizeDownTheHierarchy) {
  const char* cfg = R"({"levels":[
    {"name":"l1d","size_kb":32,"assoc":8,"block_size":64},
    {"name":"l2","size_kb":256,"assoc":8,"block_size":32}
  ]})";
  EXPECT_THROW(SimConfig::from_json(Json::parse(cfg)), ConfigError);
}

TEST(ConfigValidation, RejectsMoreThanThreeLevels) {
  const char* cfg = R"({"levels":[
    {"name":"a","size_kb":32,"assoc":1,"block_size":64},
    {"name":"b","size_kb":64,"assoc":1,"block_size":64},
    {"name":"c","size_kb":128,"assoc":1,"block_size":64},
    {"name":"d","size_kb":256,"assoc":1,"block_size":64}
  ]})";
  EXPECT_THROW(SimConfig::from_json(Json::parse(cfg)), ConfigError);
}

TEST(ConfigValidation, RejectsInclusionWithOneLevel) {
  const char* cfg = R"({"inclusion":"inclusive","levels":[
    {"name":"l1d","size_kb":32,"assoc":8,"block_size":64}]})";
  EXPECT_THROW(SimConfig::from_json(Json::parse(cfg)), ConfigError);
}

TEST(ConfigValidation, RejectsUnknownEnumValues) {
  auto with = [](const std::string& field, const std::string& value) {
    return std::string(R"({"levels":[{"name":"l1d","size_kb":32,"assoc":8,"block_size":64,")") +
           field + R"(":")" + value + R"("}]})";
  };
  EXPECT_THROW(SimConfig::from_json(Json::parse(with("write_policy", "maybe"))), ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(with("alloc_policy", "sometimes"))), ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(with("replacement", "plru"))), ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(with("prefetcher", "markov"))), ConfigError);

  EXPECT_THROW(SimConfig::from_json(Json::parse(
                   R"({"third_field":"bogus","levels":[{"name":"a","size_kb":1,"assoc":1,"block_size":64}]})")),
               ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(
                   R"({"inclusion":"magic","levels":[{"name":"a","size_kb":1,"assoc":1,"block_size":64},
                       {"name":"b","size_kb":2,"assoc":1,"block_size":64}]})")),
               ConfigError);
}

TEST(ConfigValidation, RejectsOutOfRangeTimingAndBranchParameters) {
  auto base = [](const std::string& extra) {
    return std::string(R"({"levels":[{"name":"l1d","size_kb":32,"assoc":8,"block_size":64}],)") +
           extra + "}";
  };
  EXPECT_THROW(SimConfig::from_json(Json::parse(base(R"("dram":{"bytes_per_cycle":0})"))),
               ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(base(R"("timing":{"base_cpi":0})"))), ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(base(R"("timing":{"mlp_overlap":1.0})"))),
               ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(base(R"("timing":{"mlp_overlap":-0.1})"))),
               ConfigError);
  EXPECT_THROW(
      SimConfig::from_json(Json::parse(base(R"("timing":{"instructions_per_access":0.5})"))),
      ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(base(R"("branch":{"density":0})"))), ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(base(R"("branch":{"density":1.5})"))), ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(base(R"("branch":{"pc_index_bits":0})"))),
               ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(base(R"("branch":{"pc_index_bits":99})"))),
               ConfigError);
  EXPECT_THROW(SimConfig::from_json(Json::parse(base(R"("branch":{"perceptron_history":0})"))),
               ConfigError);
}

TEST(ConfigValidation, InstructionsPerAccessIsIgnoredInInstrGapMode) {
  // The exact instruction count comes from the trace, so the fallback factor
  // is not required to be sane.
  const char* cfg = R"({"third_field":"instr_gap",
    "timing":{"instructions_per_access":0.0},
    "levels":[{"name":"l1d","size_kb":32,"assoc":8,"block_size":64}]})";
  EXPECT_NO_THROW(SimConfig::from_json(Json::parse(cfg)));
}

// --- overrides ----------------------------------------------------------

TEST(Override, SetsNestedObjectFields) {
  Json doc = Json::parse(kMinimalConfig);
  apply_override(doc, "dram.latency_cycles=321");
  apply_override(doc, "timing.base_cpi=1.25");
  apply_override(doc, "branch.predictor=tournament");

  const SimConfig c = SimConfig::from_json(doc);
  EXPECT_EQ(c.dram.latency_cycles, 321u);
  EXPECT_DOUBLE_EQ(c.timing.base_cpi, 1.25);
  EXPECT_EQ(c.branch.kind, PredictorKind::Tournament);
}

TEST(Override, IndexesIntoTheLevelsArray) {
  Json doc = Json::parse(R"({"levels":[
    {"name":"l1d","size_kb":32,"assoc":8,"block_size":64},
    {"name":"l2","size_kb":256,"assoc":8,"block_size":64}
  ]})");
  apply_override(doc, "levels.1.size_kb=1024");
  apply_override(doc, "levels.0.replacement=fifo");
  apply_override(doc, "levels.1.prefetcher.kind=stride");

  const SimConfig c = SimConfig::from_json(doc);
  EXPECT_EQ(c.levels[1].size_bytes, 1024u * 1024u);
  EXPECT_EQ(c.levels[0].replacement, ReplacementKind::FIFO);
  EXPECT_EQ(c.levels[1].prefetcher.kind, PrefetcherKind::Stride);
}

TEST(Override, InfersValueTypes) {
  Json doc = Json::object();
  json_set_path(doc, "a.flag_true", "true");
  json_set_path(doc, "a.flag_false", "false");
  json_set_path(doc, "a.number", "42");
  json_set_path(doc, "a.real", "0.5");
  json_set_path(doc, "a.text", "write-back");
  json_set_path(doc, "a.not_a_number", "64kb");

  const Json& a = doc.at("a");
  EXPECT_TRUE(a.at("flag_true").is_bool());
  EXPECT_TRUE(a.at("flag_true").as_bool());
  EXPECT_FALSE(a.at("flag_false").as_bool());
  EXPECT_TRUE(a.at("number").is_number());
  EXPECT_EQ(a.at("number").as_int(), 42);
  EXPECT_DOUBLE_EQ(a.at("real").as_double(), 0.5);
  EXPECT_TRUE(a.at("text").is_string());
  EXPECT_TRUE(a.at("not_a_number").is_string())
      << "a partially numeric string must stay a string";
}

TEST(Override, RejectsMalformedAssignmentsAndPaths) {
  Json doc = Json::parse(kMinimalConfig);
  EXPECT_THROW(apply_override(doc, "no_equals_sign"), ConfigError);
  EXPECT_THROW(apply_override(doc, "=value"), ConfigError);
  EXPECT_THROW(json_set_path(doc, "levels..size_kb", "1"), ConfigError);
  EXPECT_THROW(json_set_path(doc, "levels.9.size_kb", "1"), ConfigError)
      << "index past the end of the array";
  EXPECT_THROW(json_set_path(doc, "name.0", "x"), ConfigError)
      << "indexing a string is not meaningful";
}

}  // namespace
}  // namespace cachescope
