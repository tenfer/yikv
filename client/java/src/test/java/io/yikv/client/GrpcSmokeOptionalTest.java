package io.yikv.client;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledIfEnvironmentVariable;

/**
 * With {@code YIKV_GRPC_TARGET} set (e.g. {@code 127.0.0.1:9000}), opens a channel and closes; no RPC.
 */
@EnabledIfEnvironmentVariable(named = "YIKV_GRPC_TARGET", matches = ".+")
class GrpcSmokeOptionalTest {

  @Test
  void openAndClose() {
    String target = System.getenv("YIKV_GRPC_TARGET").trim();
    try (YikvDbClient c = GrpcYikvDbClient.create(target)) {
      // lazy connect
    }
  }
}
