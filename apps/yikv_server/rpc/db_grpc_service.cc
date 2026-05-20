#include "rpc/db_grpc_service.h"

#include "db/handlers.h"
#include "table_registry.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>

namespace yikv_server::rpc {

YikvDbGrpcService::YikvDbGrpcService(TableRegistry* reg) : reg_(reg) {}

void YikvDbGrpcService::Get(google::protobuf::RpcController* cntl_base,
                            const yikv::db::FbRpcRequest* request,
                            yikv::db::FbRpcResponse* response, google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl_base;
    std::string        resp;
    const std::string& pl = request->payload();
    yikv_server::db::HandleGet(reg_, pl.data(), pl.size(), &resp);
    response->set_payload(resp);
}

void YikvDbGrpcService::Put(google::protobuf::RpcController* cntl_base,
                            const yikv::db::FbRpcRequest* request,
                            yikv::db::FbRpcResponse* response, google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl_base;
    std::string        resp;
    const std::string& pl = request->payload();
    yikv_server::db::HandlePut(reg_, pl.data(), pl.size(), &resp);
    response->set_payload(resp);
}

void YikvDbGrpcService::PutBatch(google::protobuf::RpcController* cntl_base,
                                 const yikv::db::FbRpcRequest* request,
                                 yikv::db::FbRpcResponse* response,
                                 google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl_base;
    std::string        resp;
    const std::string& pl = request->payload();
    yikv_server::db::HandlePutBatch(reg_, pl.data(), pl.size(), &resp);
    response->set_payload(resp);
}

void YikvDbGrpcService::BatchGet(google::protobuf::RpcController* cntl_base,
                                 const yikv::db::FbRpcRequest* request,
                                 yikv::db::FbRpcResponse* response,
                                 google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    (void)cntl_base;
    std::string        resp;
    const std::string& pl = request->payload();
    yikv_server::db::HandleBatchGet(reg_, pl.data(), pl.size(), &resp);
    response->set_payload(resp);
}

}  // namespace yikv_server::rpc
