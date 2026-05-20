package io.yikv.client;

import java.util.Arrays;
import java.util.List;

/** Parsed list of {@link Endpoint}s from a comma-separated target string. */
public final class Endpoints {

  private final List<Endpoint> endpoints;

  private Endpoints(List<Endpoint> endpoints) {
    if (endpoints.isEmpty()) {
      throw new IllegalArgumentException("at least one endpoint required");
    }
    this.endpoints = List.copyOf(endpoints);
  }

  public static Endpoints parse(String spec) {
    String[] parts = spec.split(",");
    return new Endpoints(
        Arrays.stream(parts)
            .map(String::trim)
            .filter(s -> !s.isEmpty())
            .map(Endpoint::parseOne)
            .toList());
  }

  public List<Endpoint> list() {
    return endpoints;
  }

  /**
   * gRPC {@code static://h1:p1,h2:p2,...} when multiple endpoints; otherwise a single {@code host:port}.
   */
  public String grpcTarget() {
    if (endpoints.size() == 1) {
      return endpoints.get(0).target();
    }
    return "static://" + String.join(",", endpoints.stream().map(Endpoint::target).toList());
  }
}
