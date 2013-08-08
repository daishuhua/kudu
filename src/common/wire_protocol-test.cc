// Copyright (c) 2013, Cloudera, inc.

#include <boost/assign/list_of.hpp>
#include <gtest/gtest.h>
#include "common/row.h"
#include "common/schema.h"
#include "common/wire_protocol.h"
#include "util/status.h"
#include "util/test_macros.h"

namespace kudu {

TEST(WireProtocolTest, TestOKStatus) {
  Status s = Status::OK();
  AppStatusPB pb;
  StatusToPB(s, &pb);
  EXPECT_EQ(AppStatusPB::OK, pb.code());
  EXPECT_FALSE(pb.has_message());
  EXPECT_FALSE(pb.has_posix_code());

  Status s2 = StatusFromPB(pb);
  ASSERT_STATUS_OK(s2);
}

TEST(WireProtocolTest, TestBadStatus) {
  Status s = Status::NotFound("foo", "bar");
  AppStatusPB pb;
  StatusToPB(s, &pb);
  EXPECT_EQ(AppStatusPB::NOT_FOUND, pb.code());
  EXPECT_TRUE(pb.has_message());
  EXPECT_EQ("foo: bar", pb.message());
  EXPECT_FALSE(pb.has_posix_code());

  Status s2 = StatusFromPB(pb);
  EXPECT_TRUE(s2.IsNotFound());
  EXPECT_EQ(s.ToString(), s2.ToString());
}

TEST(WireProtocolTest, TestBadStatusWithPosixCode) {
  Status s = Status::NotFound("foo", "bar", 1234);
  AppStatusPB pb;
  StatusToPB(s, &pb);
  EXPECT_EQ(AppStatusPB::NOT_FOUND, pb.code());
  EXPECT_TRUE(pb.has_message());
  EXPECT_EQ("foo: bar", pb.message());
  EXPECT_TRUE(pb.has_posix_code());
  EXPECT_EQ(1234, pb.posix_code());

  Status s2 = StatusFromPB(pb);
  EXPECT_TRUE(s2.IsNotFound());
  EXPECT_EQ(1234, s2.posix_code());
  EXPECT_EQ(s.ToString(), s2.ToString());
}

TEST(WireProtocolTest, TestSchemaRoundTrip) {
  Schema schema1(boost::assign::list_of
                 (ColumnSchema("col1", STRING))
                 (ColumnSchema("col2", STRING))
                 (ColumnSchema("col3", UINT32, true /* nullable */)),
                 1);
  google::protobuf::RepeatedPtrField<ColumnSchemaPB> pbs;

  ASSERT_STATUS_OK(SchemaToColumnPBs(schema1, &pbs));
  ASSERT_EQ(3, pbs.size());

  // Column 0.
  EXPECT_TRUE(pbs.Get(0).is_key());
  EXPECT_EQ("col1", pbs.Get(0).name());
  EXPECT_EQ(STRING, pbs.Get(0).type());
  EXPECT_FALSE(pbs.Get(0).is_nullable());

  // Column 1.
  EXPECT_FALSE(pbs.Get(1).is_key());
  EXPECT_EQ("col2", pbs.Get(1).name());
  EXPECT_EQ(STRING, pbs.Get(1).type());
  EXPECT_FALSE(pbs.Get(1).is_nullable());

  // Column 2.
  EXPECT_FALSE(pbs.Get(2).is_key());
  EXPECT_EQ("col3", pbs.Get(2).name());
  EXPECT_EQ(UINT32, pbs.Get(2).type());
  EXPECT_TRUE(pbs.Get(2).is_nullable());

  // Convert back to a Schema object and verify they're identical.
  Schema schema2;
  ASSERT_STATUS_OK(ColumnPBsToSchema(pbs, &schema2));
  EXPECT_EQ(schema1.ToString(), schema2.ToString());
  EXPECT_EQ(schema1.num_key_columns(), schema2.num_key_columns());
}

// Test that, when non-contiguous key columns are passed, an error Status
// is returned.
TEST(WireProtocolTest, TestBadSchema_NonContiguousKey) {
  google::protobuf::RepeatedPtrField<ColumnSchemaPB> pbs;

  // Column 0: key
  ColumnSchemaPB* col_pb = pbs.Add();
  col_pb->set_name("c0");
  col_pb->set_type(STRING);
  col_pb->set_is_key(true);

  // Column 1: not a key
  col_pb = pbs.Add();
  col_pb->set_name("c1");
  col_pb->set_type(STRING);
  col_pb->set_is_key(false);

  // Column 2: marked as key. This is an error.
  col_pb = pbs.Add();
  col_pb->set_name("c2");
  col_pb->set_type(STRING);
  col_pb->set_is_key(true);

  Schema schema;
  Status s = ColumnPBsToSchema(pbs, &schema);
  ASSERT_STR_CONTAINS(s.ToString(), "Got out-of-order key column");
}

// Test that, when multiple columns with the same name are passed, an
// error Status is returned.
TEST(WireProtocolTest, TestBadSchema_DuplicateColumnName) {
  google::protobuf::RepeatedPtrField<ColumnSchemaPB> pbs;

  // Column 0:
  ColumnSchemaPB* col_pb = pbs.Add();
  col_pb->set_name("c0");
  col_pb->set_type(STRING);
  col_pb->set_is_key(true);

  // Column 1:
  col_pb = pbs.Add();
  col_pb->set_name("c1");
  col_pb->set_type(STRING);
  col_pb->set_is_key(false);

  // Column 2: same name as column 0
  col_pb = pbs.Add();
  col_pb->set_name("c0");
  col_pb->set_type(STRING);
  col_pb->set_is_key(false);

  Schema schema;
  Status s = ColumnPBsToSchema(pbs, &schema);
  ASSERT_STR_CONTAINS(s.ToString(), "Duplicate name present");
}

// Create a block of rows in protobuf form, then ensure that they
// can be read back out.
TEST(WireProtocolTest, TestRowBlockRoundTrip) {
  const int kNumRows = 10;

  Schema schema(boost::assign::list_of
                (ColumnSchema("col1", STRING))
                (ColumnSchema("col2", STRING))
                (ColumnSchema("col3", UINT32, true /* nullable */)),
                1);
  RowwiseRowBlockPB pb;

  // Build a set of rows into the protobuf.
  RowBuilder rb(schema);
  for (int i = 0; i < kNumRows; i++) {
    rb.Reset();
    rb.AddString(StringPrintf("col1 %d", i));
    rb.AddString(StringPrintf("col2 %d", i));
    if (i % 2 == 1) {
      rb.AddNull();
    } else {
      rb.AddUint32(i);
    }
    AddRowToRowBlockPB(rb.row(), &pb);
  }

  // Extract the rows back out, verify that the results are the same
  // as the input.
  vector<const uint8_t*> row_ptrs;
  ASSERT_STATUS_OK(ExtractRowsFromRowBlockPB(schema, &pb, &row_ptrs));
  ASSERT_EQ(kNumRows, row_ptrs.size());
  for (int i = 0; i < kNumRows; i++) {
    ConstContiguousRow row(schema, row_ptrs[i]);
    ASSERT_EQ(StringPrintf("col1 %d", i),
              schema.ExtractColumnFromRow<STRING>(row, 0)->ToString());
    ASSERT_EQ(StringPrintf("col2 %d", i),
              schema.ExtractColumnFromRow<STRING>(row, 1)->ToString());
    if (i % 2 == 1) {
      ASSERT_TRUE(row.is_null(schema, 2));
    } else {
      ASSERT_EQ(i, *schema.ExtractColumnFromRow<UINT32>(row, 2));
    }
  }
}

// Test that trying to extract rows from an invalid block correctly returns
// Corruption statuses.
TEST(WireProtocolTest, TestInvalidRowBlock) {
  Schema schema(boost::assign::list_of(ColumnSchema("col1", STRING)),
                1);
  RowwiseRowBlockPB pb;
  vector<const uint8_t*> row_ptrs;

  // Too short to be valid data.
  pb.mutable_rows()->assign("x");
  Status s = ExtractRowsFromRowBlockPB(schema, &pb, &row_ptrs);
  ASSERT_STR_CONTAINS(s.ToString(), "Corruption: Row block has 1 bytes of data");

  // Bad pointer into indirect data.
  pb.mutable_rows()->assign("xxxxxxxxxxxxxxxx");
  s = ExtractRowsFromRowBlockPB(schema, &pb, &row_ptrs);
  ASSERT_STR_CONTAINS(s.ToString(),
                      "Corruption: Row #0 contained bad indirect slice");
}



} // namespace kudu
