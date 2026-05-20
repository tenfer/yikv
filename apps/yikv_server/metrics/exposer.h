#pragma once

// PromHttpService — a brpc service whose sole method renders Prometheus
// exposition text by delegating to Metrics::instance().Render().
//
// Routing: register on the brpc::Server with
//   server.AddService(&svc, brpc::SERVER_DOESNT_OWN_SERVICE,
//                     "/metrics => Scrape");

#include "metrics/metrics.pb.h"

namespace yikv_server::metrics {

class PromHttpService : public proto::PromHttpService {
public:
    void Scrape(::google::protobuf::RpcController* cntl_base,
                const proto::PromReq*              request,
                proto::PromResp*                   response,
                ::google::protobuf::Closure*       done) override;
};

}  // namespace yikv_server::metrics
