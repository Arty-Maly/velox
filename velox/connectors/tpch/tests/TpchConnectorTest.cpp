/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/connectors/tpch/TpchConnector.h"
#include <folly/init/Init.h>
#include "gtest/gtest.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/OperatorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"

namespace {

using namespace facebook::velox;
using namespace facebook::velox::connector::tpch;

using facebook::velox::exec::test::PlanBuilder;
using facebook::velox::tpch::Table;

class TpchConnectorTest : public exec::test::OperatorTestBase {
 public:
  const std::string kTpchConnectorId = "test-tpch";

  void SetUp() override {
    OperatorTestBase::SetUp();
    auto tpchConnector =
        connector::getConnectorFactory(
            connector::tpch::TpchConnectorFactory::kTpchConnectorName)
            ->newConnector(kTpchConnectorId, nullptr);
    connector::registerConnector(tpchConnector);
  }

  void TearDown() override {
    connector::unregisterConnector(kTpchConnectorId);
    OperatorTestBase::TearDown();
  }

  exec::Split makeTpchSplit() const {
    return exec::Split(std::make_shared<TpchConnectorSplit>(kTpchConnectorId));
  }

  RowVectorPtr getResults(
      const core::PlanNodePtr& planNode,
      std::vector<exec::Split>&& splits) {
    return exec::test::AssertQueryBuilder(planNode)
        .splits(std::move(splits))
        .copyResults(pool());
  }

  void runScaleFactorTest(size_t scaleFactor);
};

// Simple scan of first 5 rows of "nation".
TEST_F(TpchConnectorTest, simple) {
  auto plan = PlanBuilder()
                  .tableScan(
                      Table::TBL_NATION,
                      {"n_nationkey", "n_name", "n_regionkey", "n_comment"})
                  .limit(0, 5, false)
                  .planNode();

  auto output = getResults(plan, {makeTpchSplit()});
  auto expected = makeRowVector({
      // n_nationkey
      makeFlatVector<int64_t>({0, 1, 2, 3, 4}),
      // n_name
      makeFlatVector<StringView>({
          "ALGERIA",
          "ARGENTINA",
          "BRAZIL",
          "CANADA",
          "EGYPT",
      }),
      // n_regionkey
      makeFlatVector<int64_t>({0, 1, 1, 1, 4}),
      // n_comment
      makeFlatVector<StringView>({
          " haggle. carefully final deposits detect slyly agai",
          "al foxes promise slyly according to the regular accounts. bold requests alon",
          "y alongside of the pending deposits. carefully special packages are about the ironic forges. slyly special ",
          "eas hang ironic, silent packages. slyly regular packages are furiously over the tithes. fluffily bold",
          "y above the carefully unusual theodolites. final dugouts are quickly across the furiously regular d",
      }),
  });
  test::assertEqualVectors(expected, output);
}

// Extract single column from "nation".
TEST_F(TpchConnectorTest, singleColumn) {
  auto plan = PlanBuilder().tableScan(Table::TBL_NATION, {"n_name"}).planNode();

  auto output = getResults(plan, {makeTpchSplit()});
  auto expected = makeRowVector({makeFlatVector<StringView>({
      "ALGERIA",       "ARGENTINA", "BRAZIL", "CANADA",
      "EGYPT",         "ETHIOPIA",  "FRANCE", "GERMANY",
      "INDIA",         "INDONESIA", "IRAN",   "IRAQ",
      "JAPAN",         "JORDAN",    "KENYA",  "MOROCCO",
      "MOZAMBIQUE",    "PERU",      "CHINA",  "ROMANIA",
      "SAUDI ARABIA",  "VIETNAM",   "RUSSIA", "UNITED KINGDOM",
      "UNITED STATES",
  })});
  test::assertEqualVectors(expected, output);
  EXPECT_EQ("n_name", output->type()->asRow().nameOf(0));
}

// Check that aliases are correctly resolved.
TEST_F(TpchConnectorTest, singleColumnWithAlias) {
  const std::string aliasedName = "my_aliased_column_name";

  auto outputType = ROW({aliasedName}, {VARCHAR()});
  auto plan =
      PlanBuilder()
          .tableScan(
              outputType,
              std::make_shared<TpchTableHandle>(
                  kTpchConnectorId, Table::TBL_NATION),
              {
                  {aliasedName, std::make_shared<TpchColumnHandle>("n_name")},
                  {"other_name", std::make_shared<TpchColumnHandle>("n_name")},
                  {"third_column",
                   std::make_shared<TpchColumnHandle>("n_regionkey")},
              })
          .limit(0, 1, false)
          .planNode();

  auto output = getResults(plan, {makeTpchSplit()});
  auto expected = makeRowVector({makeFlatVector<StringView>({
      "ALGERIA",
  })});
  test::assertEqualVectors(expected, output);

  EXPECT_EQ(aliasedName, output->type()->asRow().nameOf(0));
  EXPECT_EQ(1, output->childrenSize());
}

void TpchConnectorTest::runScaleFactorTest(size_t scaleFactor) {
  auto plan = PlanBuilder()
                  .tableScan(
                      ROW({}, {}),
                      std::make_shared<TpchTableHandle>(
                          kTpchConnectorId, Table::TBL_SUPPLIER, scaleFactor),
                      {})
                  .singleAggregation({}, {"count(1)"})
                  .planNode();

  auto output = getResults(plan, {makeTpchSplit()});
  int64_t expectedRows =
      tpch::getRowCount(tpch::Table::TBL_SUPPLIER, scaleFactor);
  auto expected = makeRowVector(
      {makeFlatVector<int64_t>(std::vector<int64_t>{expectedRows})});
  test::assertEqualVectors(expected, output);
}

// Aggregation over a larger table.
TEST_F(TpchConnectorTest, simpleAggregation) {
  runScaleFactorTest(1);
  runScaleFactorTest(5);
  runScaleFactorTest(13);
}

TEST_F(TpchConnectorTest, unknownColumn) {
  EXPECT_THROW(
      {
        PlanBuilder()
            .tableScan(Table::TBL_NATION, {"does_not_exist"})
            .planNode();
      },
      VeloxUserError);
}

// Join nation and region.
TEST_F(TpchConnectorTest, join) {
  auto planNodeIdGenerator =
      std::make_shared<exec::test::PlanNodeIdGenerator>();
  core::PlanNodeId nationScanId;
  core::PlanNodeId regionScanId;
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .tableScan(
              tpch::Table::TBL_NATION, {"n_regionkey"}, 1 /*scaleFactor*/)
          .capturePlanNodeId(nationScanId)
          .hashJoin(
              {"n_regionkey"},
              {"r_regionkey"},
              PlanBuilder(planNodeIdGenerator)
                  .tableScan(
                      tpch::Table::TBL_REGION,
                      {"r_regionkey", "r_name"},
                      1 /*scaleFactor*/)
                  .capturePlanNodeId(regionScanId)
                  .planNode(),
              "", // extra filter
              {"r_name"})
          .singleAggregation({"r_name"}, {"count(1) as nation_cnt"})
          .orderBy({"r_name"}, false)
          .planNode();

  auto output = exec::test::AssertQueryBuilder(plan)
                    .split(nationScanId, makeTpchSplit())
                    .split(regionScanId, makeTpchSplit())
                    .copyResults(pool());

  auto expected = makeRowVector({
      makeFlatVector<StringView>(
          {"AFRICA", "AMERICA", "ASIA", "EUROPE", "MIDDLE EAST"}),
      makeConstant<int64_t>(5, 5),
  });
  test::assertEqualVectors(expected, output);
}

} // namespace

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv, false);
  return RUN_ALL_TESTS();
}
