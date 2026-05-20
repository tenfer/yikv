#pragma once

#include <google/protobuf/service.h>

#include "proto/yikv_grpc.pb.h"

namespace yikv_server {
class TableRegistry;
}

namespace yikv_server::rpc {

// Implements protobuf yikv.db.YikvDb for standard gRPC (h2:grpc).
// Body = FlatBuffers in payload; routes to the correct KVIndex via TableRegistry.
class YikvDbGrpcService final : public yikv::db::YikvDb {
public:
    explicit YikvDbGrpcService(TableRegistry* reg);

    void Get(google::protobuf::RpcController* cntl, const yikv::db::FbRpcRequest* request,
             yikv::db::FbRpcResponse* response, google::protobuf::Closure* done) override;

    void Put(google::protobuf::RpcController* cntl, const yikv::db::FbRpcRequest* request,
             yikv::db::FbRpcResponse* response, google::protobuf::Closure* done) override;

    void PutBatch(google::protobuf::RpcController* cntl, const yikv::db::FbRpcRequest* request,
                  yikv::db::FbRpcResponse* response, google::protobuf::Closure* done) override;

    void BatchGet(google::protobuf::RpcController* cntl, const yikv::db::FbRpcRequest* request,
                  yikv::db::FbRpcResponse* response, google::protobuf::Closure* done) override;

private:
    TableRegistry* reg_;
};

}  // namespace yikv_server::rpc
