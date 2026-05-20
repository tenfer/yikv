package io.yikv.client;

/** Single gRPC authority {@code host:port} (IPv6 uses bracket form in specs, e.g. {@code [::1]:9000}). */
public record Endpoint(String host, int port) {

  /** Target string for gRPC channel builders. */
  public String target() {
    if (host.indexOf(':') >= 0 && !host.startsWith("[")) {
      return "[" + host + "]:" + port;
    }
    return host + ":" + port;
  }

  static Endpoint parseOne(String raw) {
    String s = raw.trim();
    if (s.isEmpty()) {
      throw new IllegalArgumentException("empty endpoint segment");
    }
    if (s.startsWith("[")) {
      int close = s.indexOf(']');
      if (close < 0 || close + 1 >= s.length() || s.charAt(close + 1) != ':') {
        throw new IllegalArgumentException("invalid IPv6 endpoint: " + raw);
      }
      String h = s.substring(1, close);
      int p = Integer.parseInt(s.substring(close + 2));
      if (p <= 0 || p > 65535) {
        throw new IllegalArgumentException("invalid port: " + raw);
      }
      return new Endpoint(h, p);
    }
    int colon = s.lastIndexOf(':');
    if (colon <= 0 || colon == s.length() - 1) {
      throw new IllegalArgumentException("expected host:port, got: " + raw);
    }
    String h = s.substring(0, colon);
    int p = Integer.parseInt(s.substring(colon + 1));
    if (p <= 0 || p > 65535) {
      throw new IllegalArgumentException("invalid port: " + raw);
    }
    return new Endpoint(h, p);
  }
}
