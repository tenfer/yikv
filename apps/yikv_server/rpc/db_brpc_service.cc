#include "rpc/db_brpc_service.h"

#include "db/handlers.h"
#include "rpc/rpc_constants.h"
#include "table_registry.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>

namespace yikv_server::rpc {

DbBrpcService::DbBrpcService(TableRegistry* reg) : reg_(reg) {}

void DbBrpcService::ProcessRpcRequest(brpc::Controller* cntl,
                                      const brpc::SerializedRequest* request,
                                      brpc::SerializedResponse* response,
                                      ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (!cntl->sampled_request()) {
        cntl->SetFailed(brpc::EREQUEST, "missing sampled_request meta");
        return;
    }
    const auto& meta = cntl->sampled_request()->meta;
    if (meta.service_name() != kServiceFullName) {
        cntl->SetFailed(brpc::ENOSERVICE, "unknown service=%s", meta.service_name().c_str());
        return;
    }

    const butil::IOBuf& body = request->serialized_data();
    std::string         req_bytes;
    body.copy_to(&req_bytes);

    std::string  resp;
    const char*  req_ptr = req_bytes.empty() ? "" : req_bytes.data();
    const size_t req_len = req_bytes.size();

    if (meta.method_name() == kMethodGet) {
        yikv_server::db::HandleGet(reg_, req_ptr, req_len, &resp);
    } else if (meta.method_name() == kMethodPut) {
        yikv_server::db::HandlePut(reg_, req_ptr, req_len, &resp);
    } else if (meta.method_name() == kMethodPutBatch) {
        yikv_server::db::HandlePutBatch(reg_, req_ptr, req_len, &resp);
    } else if (meta.method_name() == kMethodBatchGet) {
        yikv_server::db::HandleBatchGet(reg_, req_ptr, req_len, &resp);
    } else {
        cntl->SetFailed(brpc::ENOMETHOD, "unknown method=%s", meta.method_name().c_str());
        return;
    }

    response->serialized_data().append(resp.data(), resp.size());
}

}  // namespace yikv_server::rpc
