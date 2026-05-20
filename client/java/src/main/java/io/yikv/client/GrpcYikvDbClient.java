package io.yikv.client;

import com.google.protobuf.ByteString;
import io.grpc.ManagedChannel;
import io.grpc.ManagedChannelBuilder;
import io.grpc.netty.shaded.io.grpc.netty.NettyChannelBuilder;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import yikv.BatchGetResponse;
import yikv.GetResponse;
import yikv.PutBatchResponse;
import yikv.PutResponse;
import yikv.Row;
import yikv.db.YikvDbGrpc;
import yikv.db.YikvGrpc;

/**
 * gRPC implementation of {@link YikvDbClient}. Multiple endpoints default to {@code round_robin} on a
 * single {@code static://} channel; set {@link ClientOptions.Builder#perAddressChannels(boolean)} for
 * per-address channels with client-side round robin.
 */
public final class GrpcYikvDbClient implements YikvDbClient {

  private final List<ManagedChannel> channels;
  private final Map<Integer, String> fieldIdToName;
  private final AtomicInteger channelPicker = new AtomicInteger();

  public GrpcYikvDbClient(Endpoints endpoints, ClientOptions options) {
    this.fieldIdToName = options.fieldIdToName();
    List<ManagedChannel> built = new ArrayList<>();
    try {
      List<Endpoint> eps = endpoints.list();
      if (eps.size() > 1 && !options.perAddressChannels()) {
        ManagedChannelBuilder<?> b = channelBuilder(endpoints.grpcTarget());
        b.defaultLoadBalancingPolicy("round_robin");
        applyTransportOptions(b, options);
        built.add(b.build());
      } else if (eps.size() > 1) {
        for (Endpoint e : eps) {
          ManagedChannelBuilder<?> b = channelBuilder(e.target());
          applyTransportOptions(b, options);
          built.add(b.build());
        }
      } else {
        ManagedChannelBuilder<?> b = channelBuilder(endpoints.grpcTarget());
        applyTransportOptions(b, options);
        built.add(b.build());
      }
    } catch (RuntimeException ex) {
      for (ManagedChannel c : built) {
        c.shutdownNow();
      }
      throw ex;
    }
    this.channels = List.copyOf(built);
  }

  private static ManagedChannelBuilder<?> channelBuilder(String target) {
    return NettyChannelBuilder.forTarget(target).usePlaintext();
  }

  public static GrpcYikvDbClient create(String endpointsSpec, ClientOptions options) {
    return new GrpcYikvDbClient(Endpoints.parse(endpointsSpec), options);
  }

  public static GrpcYikvDbClient create(String endpointsSpec) {
    return create(endpointsSpec, ClientOptions.defaults());
  }

  private static void applyTransportOptions(ManagedChannelBuilder<?> b, ClientOptions o) {
    if (o.maxInboundMessageBytes() > 0) {
      b.maxInboundMessageSize(o.maxInboundMessageBytes());
    }
  }

  private YikvDbGrpc.YikvDbBlockingStub stub() {
    if (channels.size() == 1) {
      return YikvDbGrpc.newBlockingStub(channels.get(0));
    }
    int i = Math.floorMod(channelPicker.getAndIncrement(), channels.size());
    return YikvDbGrpc.newBlockingStub(channels.get(i));
  }

  private static YikvGrpc.FbRpcRequest request(byte[] flatBufferBody) {
    if (flatBufferBody == null) {
      throw new IllegalArgumentException("request bytes must not be null");
    }
    return YikvGrpc.FbRpcRequest.newBuilder()
        .setPayload(ByteString.copyFrom(flatBufferBody))
        .build();
  }

  private static byte[] unwrapPayload(YikvGrpc.FbRpcResponse response, String rpc) {
    if (!response.hasPayload()) {
      throw new YikvClientException(rpc + ": FbRpcResponse has no payload");
    }
    return response.getPayload().toByteArray();
  }

  @Override
  public GetResult get(byte[] flatBufferRequest) {
    byte[] pl = unwrapPayload(stub().get(request(flatBufferRequest)), "Get");
    GetResponse gr = GetResponse.getRootAsGetResponse(FieldValues.asLittleEndian(pl));
    return new GetResult(gr.found(), gr.err(), gr.indexGetNs(), FieldValues.rowToMap(gr.row(), fieldIdToName));
  }

  @Override
  public BatchGetResult batchGet(byte[] flatBufferRequest) {
    byte[] pl = unwrapPayload(stub().batchGet(request(flatBufferRequest)), "BatchGet");
    BatchGetResponse br = BatchGetResponse.getRootAsBatchGetResponse(FieldValues.asLittleEndian(pl));
    int n = br.rowsLength();
    List<Map<String, Object>> rows = new ArrayList<>(n);
    Row row = new Row();
    for (int j = 0; j < n; j++) {
      br.rows(row, j);
      rows.add(FieldValues.rowToMap(row, fieldIdToName));
    }
    return new BatchGetResult(br.err(), List.copyOf(rows));
  }

  @Override
  public PutResult put(byte[] flatBufferRequest) {
    byte[] pl = unwrapPayload(stub().put(request(flatBufferRequest)), "Put");
    PutResponse pr = PutResponse.getRootAsPutResponse(FieldValues.asLittleEndian(pl));
    return new PutResult(pr.ok(), pr.err());
  }

  @Override
  public PutBatchResult putBatch(byte[] flatBufferRequest) {
    byte[] pl = unwrapPayload(stub().putBatch(request(flatBufferRequest)), "PutBatch");
    PutBatchResponse pr = PutBatchResponse.getRootAsPutBatchResponse(FieldValues.asLittleEndian(pl));
    return new PutBatchResult(pr.ok(), pr.err());
  }

  @Override
  public byte[] getRaw(byte[] flatBufferRequest) {
    return unwrapPayload(stub().get(request(flatBufferRequest)), "Get(raw)");
  }

  @Override
  public byte[] batchGetRaw(byte[] flatBufferRequest) {
    return unwrapPayload(stub().batchGet(request(flatBufferRequest)), "BatchGet(raw)");
  }

  @Override
  public void close() {
    for (ManagedChannel c : channels) {
      c.shutdown();
    }
    InterruptedException interrupt = null;
    try {
      for (ManagedChannel c : channels) {
        try {
          if (!c.awaitTermination(5, TimeUnit.SECONDS)) {
            c.shutdownNow();
          }
        } catch (InterruptedException e) {
          interrupt = e;
          c.shutdownNow();
        }
      }
    } finally {
      if (interrupt != null) {
        Thread.currentThread().interrupt();
      }
    }
  }
}
