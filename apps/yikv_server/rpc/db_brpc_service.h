#pragma once

#include <brpc/baidu_master_service.h>

namespace yikv_server {
class TableRegistry;
}

namespace yikv_server::rpc {

// brpc adapter: BaiduMasterService + SerializedRequest/Response bodies = FlatBuffers.
// Routes to the correct KVIndex via TableRegistry using table_name in the request.
class DbBrpcService final : public brpc::BaiduMasterService {
public:
    explicit DbBrpcService(TableRegistry* reg);

    void ProcessRpcRequest(brpc::Controller*              cntl,
                           const brpc::SerializedRequest* request,
                           brpc::SerializedResponse*      response,
                           ::google::protobuf::Closure*   done) override;

private:
    TableRegistry* reg_;
};

}  // namespace yikv_server::rpc
