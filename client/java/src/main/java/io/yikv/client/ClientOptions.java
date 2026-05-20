package io.yikv.client;

import java.util.Collections;
import java.util.Map;
import java.util.Objects;

/**
 * Client configuration: load balancing mode, optional {@code field_id} → column name map (same role
 * as {@code --schema-json} in {@code tools/yikv_db_sample.py}).
 */
public final class ClientOptions {

  /**
   * When true and there are multiple endpoints, open one channel per address and round-robin stubs
   * per RPC. When false (default), use one {@code ManagedChannel} with {@code static://...} and
   * {@code round_robin}.
   */
  private final boolean perAddressChannels;

  /** 0 = gRPC default. */
  private final int maxInboundMessageBytes;

  private final Map<Integer, String> fieldIdToName;

  private ClientOptions(Builder b) {
    this.perAddressChannels = b.perAddressChannels;
    this.maxInboundMessageBytes = b.maxInboundMessageBytes;
    this.fieldIdToName = b.fieldIdToName.isEmpty() ? Map.of() : Map.copyOf(b.fieldIdToName);
  }

  public static Builder builder() {
    return new Builder();
  }

  public static ClientOptions defaults() {
    return new Builder().build();
  }

  public boolean perAddressChannels() {
    return perAddressChannels;
  }

  public int maxInboundMessageBytes() {
    return maxInboundMessageBytes;
  }

  public Map<Integer, String> fieldIdToName() {
    return fieldIdToName;
  }

  public static final class Builder {
    private boolean perAddressChannels;
    private int maxInboundMessageBytes;
    private Map<Integer, String> fieldIdToName = Collections.emptyMap();

    public Builder perAddressChannels(boolean perAddressChannels) {
      this.perAddressChannels = perAddressChannels;
      return this;
    }

    public Builder maxInboundMessageBytes(int bytes) {
      if (bytes < 0) {
        throw new IllegalArgumentException("maxInboundMessageBytes must be >= 0");
      }
      this.maxInboundMessageBytes = bytes;
      return this;
    }

    public Builder fieldIdToName(Map<Integer, String> fieldIdToName) {
      this.fieldIdToName = Objects.requireNonNull(fieldIdToName, "fieldIdToName");
      return this;
    }

    public ClientOptions build() {
      return new ClientOptions(this);
    }
  }
}
