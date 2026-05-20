#pragma once

namespace yikv_server::rpc {

inline constexpr const char kServiceFullName[] = "yikv.db.YikvDb";
inline constexpr const char kMethodGet[]       = "Get";
inline constexpr const char kMethodPut[]       = "Put";
inline constexpr const char kMethodPutBatch[]  = "PutBatch";
inline constexpr const char kMethodBatchGet[]  = "BatchGet";

}  // namespace yikv_server::rpc
