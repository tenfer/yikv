package io.yikv.client;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.List;
import org.junit.jupiter.api.Test;

class EndpointsTest {

  @Test
  void parseSingle() {
    Endpoints e = Endpoints.parse("127.0.0.1:9000");
    assertEquals(List.of(new Endpoint("127.0.0.1", 9000)), e.list());
    assertEquals("127.0.0.1:9000", e.grpcTarget());
  }

  @Test
  void parseMultiGrpcStatic() {
    Endpoints e = Endpoints.parse("127.0.0.1:9000, 127.0.0.1:9001");
    assertEquals(2, e.list().size());
    assertEquals("static://127.0.0.1:9000,127.0.0.1:9001", e.grpcTarget());
  }

  @Test
  void parseIpv6Brackets() {
    Endpoints e = Endpoints.parse("[::1]:9000");
    assertEquals(new Endpoint("::1", 9000), e.list().get(0));
    assertEquals("[::1]:9000", e.list().get(0).target());
  }

  @Test
  void parseEmptyFails() {
    assertThrows(IllegalArgumentException.class, () -> Endpoints.parse("  , "));
  }
}
