// Copyright (c) 2013, Cloudera, inc.

#include "common/wire_protocol.h"

#include <vector>

#include "common/row.h"
#include "util/safe_math.h"

using google::protobuf::RepeatedPtrField;
using std::vector;

namespace kudu {

void StatusToPB(const Status& status, AppStatusPB* pb) {
  pb->Clear();
  bool is_unknown = false;
  if (status.ok()) {
    pb->set_code(AppStatusPB::OK);
    // OK statuses don't have any message or posix code.
    return;
  } else if (status.IsNotFound()) {
    pb->set_code(AppStatusPB::NOT_FOUND);
  } else if (status.IsCorruption()) {
    pb->set_code(AppStatusPB::CORRUPTION);
  } else if (status.IsNotSupported()) {
    pb->set_code(AppStatusPB::NOT_SUPPORTED);
  } else if (status.IsInvalidArgument()) {
    pb->set_code(AppStatusPB::INVALID_ARGUMENT);
  } else if (status.IsIOError()) {
    pb->set_code(AppStatusPB::IO_ERROR);
  } else if (status.IsRuntimeError()) {
    pb->set_code(AppStatusPB::RUNTIME_ERROR);
  } else if (status.IsNetworkError()) {
    pb->set_code(AppStatusPB::NETWORK_ERROR);
  } else {
    LOG(WARNING) << "Unknown error code translation from internal error "
                 << status.ToString() << ": sending UNKNOWN_ERROR";
    pb->set_code(AppStatusPB::UNKNOWN_ERROR);
    is_unknown = true;
  }
  if (is_unknown) {
    // For unknown status codes, include the original stringified error
    // code.
    pb->set_message(status.CodeAsString() + ": " +
                    status.message().ToString());
  } else {
    // Otherwise, just encode the message itself, since the other end
    // will reconstruct the other parts of the ToString() response.
    pb->set_message(status.message().ToString());
  }
  if (status.posix_code() != -1) {
    pb->set_posix_code(status.posix_code());
  }
}

Status StatusFromPB(const AppStatusPB& pb) {
  int posix_code = pb.has_posix_code() ? pb.posix_code() : -1;

  switch (pb.code()) {
    case AppStatusPB::OK:
      return Status::OK();
    case AppStatusPB::NOT_FOUND:
      return Status::NotFound(pb.message(), "", posix_code);
    case AppStatusPB::CORRUPTION:
      return Status::Corruption(pb.message(), "", posix_code);
    case AppStatusPB::NOT_SUPPORTED:
      return Status::NotSupported(pb.message(), "", posix_code);
    case AppStatusPB::INVALID_ARGUMENT:
      return Status::InvalidArgument(pb.message(), "", posix_code);
    case AppStatusPB::IO_ERROR:
      return Status::IOError(pb.message(), "", posix_code);
    case AppStatusPB::ALREADY_PRESENT:
      return Status::AlreadyPresent(pb.message(), "", posix_code);
    case AppStatusPB::RUNTIME_ERROR:
      return Status::RuntimeError(pb.message(), "", posix_code);
    case AppStatusPB::NETWORK_ERROR:
      return Status::NetworkError(pb.message(), "", posix_code);
    case AppStatusPB::UNKNOWN_ERROR:
    default:
      LOG(WARNING) << "Unknown error code in status: " << pb.ShortDebugString();
      return Status::RuntimeError("(unknown error code)", pb.message(), posix_code);
  }
}

void ColumnSchemaToPB(const ColumnSchema& col_schema, ColumnSchemaPB *pb) {
  pb->set_name(col_schema.name());
  pb->set_type(col_schema.type_info().type());
  pb->set_is_nullable(col_schema.is_nullable());
}

ColumnSchema ColumnSchemaFromPB(const ColumnSchemaPB& pb) {
  return ColumnSchema(pb.name(), pb.type(), pb.is_nullable());
}

Status ColumnPBsToSchema(const RepeatedPtrField<ColumnSchemaPB>& column_pbs,
                         Schema* schema) {

  vector<ColumnSchema> columns;
  columns.reserve(column_pbs.size());
  int num_key_columns = 0;
  bool is_handling_key = true;
  BOOST_FOREACH(const ColumnSchemaPB& pb, column_pbs) {
    columns.push_back(ColumnSchemaFromPB(pb));
    if (pb.is_key()) {
      if (!is_handling_key) {
        return Status::InvalidArgument(
          "Got out-of-order key column", pb.ShortDebugString());
      }
      num_key_columns++;
    } else {
      is_handling_key = false;
    }
  }

  DCHECK_LE(num_key_columns, columns.size());

  // TODO(perf): could make the following faster by adding a
  // Reset() variant which actually takes ownership of the column
  // vector.
  return schema->Reset(columns, num_key_columns);
}

Status SchemaToColumnPBs(const Schema& schema,
                         RepeatedPtrField<ColumnSchemaPB>* cols) {
  cols->Clear();
  int idx = 0;
  BOOST_FOREACH(const ColumnSchema& col, schema.columns()) {
    ColumnSchemaPB* col_pb = cols->Add();
    ColumnSchemaToPB(col, col_pb);
    col_pb->set_is_key(idx < schema.num_key_columns());
    idx++;
  }
  return Status::OK();
}


Status ExtractRowsFromRowBlockPB(const Schema& schema,
                                 RowwiseRowBlockPB *rowblock_pb,
                                 vector<const uint8_t*>* rows) {
  // TODO: cheating here so we can rewrite the request as it arrived and
  // change any indirect data pointers back to "real" pointers instead of
  // on-the-wire pointers. Maybe the RPC layer should give us a non-const
  // request? Maybe we should suck it up and copy the data when we mutate?
  string* row_data = rowblock_pb->mutable_rows();
  const string& indir_data = rowblock_pb->indirect_data();
  size_t row_size = ContiguousRowHelper::row_size(schema);

  if (PREDICT_FALSE(row_data->size() % row_size != 0)) {
    return Status::Corruption(
      StringPrintf("Row block has %zd bytes of data which is not a multiple of "
                   "row size %zd", row_data->size(), row_size));
  }

  for (int i = 0; i < schema.num_columns(); i++) {
    const ColumnSchema& col = schema.column(i);
    if (col.type_info().type() != STRING) {
      continue;
    }

    int row_idx = 0;
    size_t offset = 0;
    while (offset < row_data->size()) {
      ContiguousRow row(schema, reinterpret_cast<uint8_t*>(&row_data->at(offset)));
      uint8_t* dst_cell = row.mutable_cell_ptr(schema, i);

      if (!col.is_nullable() || !row.is_null(schema, i)) {
        // The pointer is currently an offset into indir_data. Need to replace it
        // with the actual pointer into indir_data
        Slice *slice = reinterpret_cast<Slice *>(dst_cell);
        size_t offset_in_indirect = reinterpret_cast<uintptr_t>(slice->data());
        // TODO: there's an overflow bug here:
        bool overflowed = false;
        size_t max_offset = AddWithOverflowCheck(offset_in_indirect, slice->size(), &overflowed);
        if (PREDICT_FALSE(overflowed || max_offset > indir_data.size())) {
          return Status::Corruption(
            StringPrintf("Row #%d contained bad indirect slice for column %s: (%zd, %zd)",
                         row_idx, col.ToString().c_str(),
                         reinterpret_cast<uintptr_t>(slice->data()),
                         slice->size()));
        }
        *slice = Slice(&indir_data[offset_in_indirect], slice->size());
      }

      // Advance to next row
      offset += row_size;
      row_idx++;
    }
  }

  size_t offset = 0;
  while (offset < row_data->size()) {
    rows->push_back(reinterpret_cast<const uint8_t*>(&row_data->at(offset)));
    offset += row_size;
  }
  return Status::OK();
}

void AddRowToRowBlockPB(const ConstContiguousRow& row, RowwiseRowBlockPB* pb) {
  const Schema& schema = row.schema();
  // Append the row directly to the data.
  // This will append a host-local pointer for any slice data, so we need
  // to then relocate those pointers into the 'indirect_data' part of the protobuf.
  string* data_buf = pb->mutable_rows();
  size_t appended_offset = data_buf->size();

  data_buf->append(reinterpret_cast<const char*>(row.row_data()),
                   row.row_size());

  uint8_t* copied_rowdata = reinterpret_cast<uint8_t*>(&data_buf->at(appended_offset));
  ContiguousRow copied_row(schema, copied_rowdata);
  for (int i = 0; i < schema.num_columns(); i++) {
    const ColumnSchema& col = schema.column(i);
    uint8_t* dst_cell = copied_row.mutable_cell_ptr(schema, i);
    if (col.is_nullable() && row.is_null(schema, i)) {
      // Zero the data so we don't leak any uninitialized memory to another
      // host/security domain.
      memset(dst_cell, 0, col.type_info().size());
      continue;
    }

    if (col.type_info().type() == STRING) {
      // Copy the slice data into the 'indirect_data' field, and replace
      // the pointer with an offset into that field.
      Slice *slice = reinterpret_cast<Slice *>(dst_cell);
      size_t offset_in_indirect = pb->mutable_indirect_data()->size();
      pb->mutable_indirect_data()->append(reinterpret_cast<const char*>(slice->data()),
                                          slice->size());
      *slice = Slice(reinterpret_cast<const uint8_t*>(offset_in_indirect),
                     slice->size());
    }
  }
}

} // namespace kudu
