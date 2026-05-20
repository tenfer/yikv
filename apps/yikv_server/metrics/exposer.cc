#include "metrics/exposer.h"

#include <string>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>

#include "metrics/metrics.h"

namespace yikv_server::metrics {

void PromHttpService::Scrape(::google::protobuf::RpcController* cntl_base,
                              const proto::PromReq* /*request*/,
                              proto::PromResp*       /*response*/,
                              ::google::protobuf::Closure* done) {
    ::brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<::brpc::Controller*>(cntl_base);

    std::string body;
    body.reserve(4096);
    Metrics::instance().Render(&body);

    cntl->http_response().set_content_type(
        "text/plain; version=0.0.4; charset=utf-8");
    cntl->response_attachment().append(body);
}

}  // namespace yikv_server::metrics
