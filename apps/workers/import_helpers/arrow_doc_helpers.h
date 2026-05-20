#pragma once

#include "index/doc.h"
#include "schema/schema.h"

#include <arrow/api.h>

namespace yikv_import {

bool ArrowCellIsNull(const arrow::Array& col, int64_t row);

arrow::Status ApplyField(yikv::index::Doc* doc, const yikv::schema::FieldDef& def,
                         const std::shared_ptr<arrow::Array>& col, int64_t row);

}  // namespace yikv_import
