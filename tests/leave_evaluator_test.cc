#include "scrabble/leave_evaluator.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "scrabble/move.h"
#include "test_util.h"

namespace scrabble {
namespace {

using testing::ParseOrDie;
using testing::RackOrDie;

double Value(const LeaveEvaluator& evaluator, const char* leave) {
  return evaluator.Value(RackOrDie(leave));
}

LeaveEvaluator ParseTable(const std::string& text) {
  std::istringstream in(text);
  return LeaveEvaluator::Parse(in, "test");
}

TEST(LeaveAfterTest, RemovesRackTilesOnly) {
  const Rack rack = RackOrDie("ABCDE?S");
  EXPECT_EQ(LeaveAfter(rack, ParseOrDie("8H Ab(X)C")), RackOrDie("BDES"));
  EXPECT_EQ(LeaveAfter(rack, ParseOrDie("-SEA")), RackOrDie("BCD?"));
  EXPECT_EQ(LeaveAfter(rack, Move::Pass()), rack);
}

TEST(LeaveEvaluatorTest, ZeroIsAlwaysZero) {
  const LeaveEvaluator zero = LeaveEvaluator::Zero();
  EXPECT_EQ(zero.kind(), LeaveEvaluator::Kind::kZero);
  EXPECT_EQ(zero.name(), "zero");
  EXPECT_EQ(Value(zero, ""), 0.0);
  EXPECT_EQ(Value(zero, "?S"), 0.0);
  EXPECT_EQ(Value(zero, "QQQVVWW"), 0.0);
}

TEST(LeaveEvaluatorTest, HeuristicIsOrderIndependentAndSensible) {
  const LeaveEvaluator h = LeaveEvaluator::Heuristic();
  EXPECT_EQ(h.kind(), LeaveEvaluator::Kind::kHeuristic);
  EXPECT_EQ(Value(h, ""), 0.0);
  EXPECT_EQ(Value(h, "ERS?"), Value(h, "?SRE"));
  // Tile quality.
  EXPECT_GT(Value(h, "?"), Value(h, "S"));
  EXPECT_GT(Value(h, "S"), Value(h, "E"));
  EXPECT_GT(Value(h, "E"), Value(h, "U"));
  EXPECT_GT(Value(h, "U"), Value(h, "Q"));
  // Duplicates cost, except for blanks.
  EXPECT_LT(Value(h, "II"), 2 * Value(h, "I"));
  EXPECT_DOUBLE_EQ(Value(h, "??"), 2 * Value(h, "?"));
  // Balance: a mixed leave beats an all-vowel or all-consonant one.
  EXPECT_GT(Value(h, "AER"), Value(h, "AEI"));
  EXPECT_GT(Value(h, "AER"), Value(h, "RTN"));
  EXPECT_GT(Value(h, "AEINST"), Value(h, "AEIOUY"));
}

TEST(LeaveEvaluatorTest, TableLookupUsesCanonicalLeaves) {
  const LeaveEvaluator table = ParseTable(
      "# learned leaves\n"
      "leave,value\n"
      "\n"
      "?,25.5\n"
      "se,4.25\n"
      " QU , -5 \n"
      "??ABCDE,12\n");
  EXPECT_EQ(table.kind(), LeaveEvaluator::Kind::kTable);
  EXPECT_EQ(table.size(), 4u);
  EXPECT_EQ(Value(table, "?"), 25.5);
  EXPECT_EQ(Value(table, "ES"), 4.25);
  EXPECT_EQ(Value(table, "SE"), 4.25);
  EXPECT_EQ(Value(table, "UQ"), -5.0);
  EXPECT_EQ(Value(table, "EDCBA??"), 12.0);
  // Missing and empty leaves are worth zero.
  EXPECT_EQ(Value(table, "S"), 0.0);
  EXPECT_EQ(Value(table, "ESS"), 0.0);
  EXPECT_EQ(Value(table, ""), 0.0);
}

// Every one- and two-tile leave, with distinct values, survives the hash
// table exactly.
TEST(LeaveEvaluatorTest, TableHoldsManyEntries) {
  std::string text;
  int n = 0;
  for (Letter a = 0; a < kAlphabetSize; ++a) {
    text += std::string(1, LetterToChar(a)) + "," + std::to_string(n++) +
            "\n";
    for (Letter b = a; b < kAlphabetSize; ++b) {
      text += std::string{LetterToChar(a), LetterToChar(b)} + "," +
              std::to_string(n++) + "\n";
    }
  }
  const LeaveEvaluator table = ParseTable(text);
  EXPECT_EQ(table.size(), static_cast<std::size_t>(n));
  n = 0;
  for (Letter a = 0; a < kAlphabetSize; ++a) {
    Rack one;
    one.Add(a);
    EXPECT_EQ(table.Value(one), n++);
    for (Letter b = a; b < kAlphabetSize; ++b) {
      Rack two = one;
      two.Add(b);
      EXPECT_EQ(table.Value(two), n++);
    }
  }
}

TEST(LeaveEvaluatorTest, ParseRejectsMalformedTables) {
  EXPECT_THROW(ParseTable("ES 4\n"), std::runtime_error);         // no comma
  EXPECT_THROW(ParseTable("E1,4\n"), std::runtime_error);         // bad tile
  EXPECT_THROW(ParseTable(",4\n"), std::runtime_error);           // empty
  EXPECT_THROW(ParseTable("ABCDEFGH,1\n"), std::runtime_error);   // too long
  EXPECT_THROW(ParseTable("ES,four\n"), std::runtime_error);      // value
  EXPECT_THROW(ParseTable("ES,1\nSE,2\n"), std::runtime_error);   // duplicate
  try {
    ParseTable("A,1\nB,x\n");
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("test:2"), std::string::npos)
        << e.what();
  }
}

TEST(LeaveEvaluatorTest, ModelValuesUnlistedLeaves) {
  const LeaveEvaluator table = ParseTable(
      "leave,value\n"
      "S,8\n"
      "@S1,5\n"
      "@E1,1\n"
      "@E2,-1.5\n"
      "@Q1,-3\n"
      "@len1,0.5\n"
      "@len2,-1\n"
      "@len3,-2\n"
      "@v1c0,0.25\n"
      "@v1c1,2\n"
      "@v2c1,4\n"
      "@qnou,-7\n");
  EXPECT_TRUE(table.has_model());
  EXPECT_EQ(table.size(), 1u);
  // Listed leaves come from the table, not the model.
  EXPECT_EQ(Value(table, "S"), 8.0);
  // Unlisted: tile terms + length term + balance term (+ Q without U).
  EXPECT_DOUBLE_EQ(Value(table, "E"), 1 + 0.5 + 0.25);
  EXPECT_DOUBLE_EQ(Value(table, "SE"), 5 + 1 - 1 + 2);
  EXPECT_DOUBLE_EQ(Value(table, "EES"), -1.5 + 5 - 2 + 4);
  EXPECT_DOUBLE_EQ(Value(table, "Q"), -3 + 0.5 - 7);
  EXPECT_DOUBLE_EQ(Value(table, "QU"), -3 - 1 + 2);
  // Absent terms are zero; blanks have no balance term.
  EXPECT_DOUBLE_EQ(Value(table, "?"), 0.5);
  EXPECT_DOUBLE_EQ(Value(table, "WY"), -1.0);  // only @len2 applies
  // The model covers 1-6 tiles only; empty and seven-tile leaves are 0.
  EXPECT_EQ(Value(table, ""), 0.0);
  EXPECT_EQ(Value(table, "EEEEEES"), 0.0);
}

TEST(LeaveEvaluatorTest, TableWithoutModelRowsHasNoModel) {
  const LeaveEvaluator table = ParseTable("S,8\n");
  EXPECT_FALSE(table.has_model());
  EXPECT_EQ(Value(table, "E"), 0.0);
  const LeaveEvaluator model_only = ParseTable("@E1,2\n");
  EXPECT_TRUE(model_only.has_model());
  EXPECT_EQ(model_only.size(), 0u);
  EXPECT_EQ(Value(model_only, "E"), 2.0);
}

TEST(LeaveEvaluatorTest, ParseRejectsMalformedModelTerms) {
  for (const char* row : {"@E0,1", "@E7,1", "@1E,1", "@len0,1", "@len7,1",
                          "@v0c0,1", "@v4c3,1", "@vxc1,1", "@qu,1", "@,1",
                          "@E1,x"}) {
    EXPECT_THROW(ParseTable(std::string(row) + "\n"), std::runtime_error)
        << row;
  }
  EXPECT_THROW(ParseTable("@E1,1\n@e1,2\n"), std::runtime_error);
  EXPECT_THROW(ParseTable("@len2,1\n@len2,1\n"), std::runtime_error);
  EXPECT_THROW(ParseTable("@v1c2,1\n@V1C2,1\n"), std::runtime_error);
  EXPECT_THROW(ParseTable("@qnou,1\n@qnou,1\n"), std::runtime_error);
}

TEST(LeaveEvaluatorTest, LoadReadsFile) {
  const std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / "leaves_test.csv";
  {
    std::ofstream out(path);
    out << "leave,value\nS,8\n";
  }
  const LeaveEvaluator table = LeaveEvaluator::Load(path);
  EXPECT_EQ(Value(table, "S"), 8.0);
  EXPECT_EQ(table.name(), "table:" + path.string());
  std::filesystem::remove(path);
  EXPECT_THROW(LeaveEvaluator::Load(path), std::runtime_error);
}

}  // namespace
}  // namespace scrabble
