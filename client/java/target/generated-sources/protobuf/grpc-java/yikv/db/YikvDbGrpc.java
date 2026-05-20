package yikv.db;

import static io.grpc.MethodDescriptor.generateFullMethodName;

/**
 * <pre>
 * Same logical service as brpc BaiduMasterService (yikv.db.YikvDb / Get, Put, …).
 * </pre>
 */
@javax.annotation.Generated(
    value = "by gRPC proto compiler (version 1.63.0)",
    comments = "Source: yikv_grpc.proto")
@io.grpc.stub.annotations.GrpcGenerated
public final class YikvDbGrpc {

  private YikvDbGrpc() {}

  public static final java.lang.String SERVICE_NAME = "yikv.db.YikvDb";

  // Static method descriptors that strictly reflect the proto.
  private static volatile io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest,
      yikv.db.YikvGrpc.FbRpcResponse> getGetMethod;

  @io.grpc.stub.annotations.RpcMethod(
      fullMethodName = SERVICE_NAME + '/' + "Get",
      requestType = yikv.db.YikvGrpc.FbRpcRequest.class,
      responseType = yikv.db.YikvGrpc.FbRpcResponse.class,
      methodType = io.grpc.MethodDescriptor.MethodType.UNARY)
  public static io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest,
      yikv.db.YikvGrpc.FbRpcResponse> getGetMethod() {
    io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest, yikv.db.YikvGrpc.FbRpcResponse> getGetMethod;
    if ((getGetMethod = YikvDbGrpc.getGetMethod) == null) {
      synchronized (YikvDbGrpc.class) {
        if ((getGetMethod = YikvDbGrpc.getGetMethod) == null) {
          YikvDbGrpc.getGetMethod = getGetMethod =
              io.grpc.MethodDescriptor.<yikv.db.YikvGrpc.FbRpcRequest, yikv.db.YikvGrpc.FbRpcResponse>newBuilder()
              .setType(io.grpc.MethodDescriptor.MethodType.UNARY)
              .setFullMethodName(generateFullMethodName(SERVICE_NAME, "Get"))
              .setSampledToLocalTracing(true)
              .setRequestMarshaller(io.grpc.protobuf.ProtoUtils.marshaller(
                  yikv.db.YikvGrpc.FbRpcRequest.getDefaultInstance()))
              .setResponseMarshaller(io.grpc.protobuf.ProtoUtils.marshaller(
                  yikv.db.YikvGrpc.FbRpcResponse.getDefaultInstance()))
              .setSchemaDescriptor(new YikvDbMethodDescriptorSupplier("Get"))
              .build();
        }
      }
    }
    return getGetMethod;
  }

  private static volatile io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest,
      yikv.db.YikvGrpc.FbRpcResponse> getPutMethod;

  @io.grpc.stub.annotations.RpcMethod(
      fullMethodName = SERVICE_NAME + '/' + "Put",
      requestType = yikv.db.YikvGrpc.FbRpcRequest.class,
      responseType = yikv.db.YikvGrpc.FbRpcResponse.class,
      methodType = io.grpc.MethodDescriptor.MethodType.UNARY)
  public static io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest,
      yikv.db.YikvGrpc.FbRpcResponse> getPutMethod() {
    io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest, yikv.db.YikvGrpc.FbRpcResponse> getPutMethod;
    if ((getPutMethod = YikvDbGrpc.getPutMethod) == null) {
      synchronized (YikvDbGrpc.class) {
        if ((getPutMethod = YikvDbGrpc.getPutMethod) == null) {
          YikvDbGrpc.getPutMethod = getPutMethod =
              io.grpc.MethodDescriptor.<yikv.db.YikvGrpc.FbRpcRequest, yikv.db.YikvGrpc.FbRpcResponse>newBuilder()
              .setType(io.grpc.MethodDescriptor.MethodType.UNARY)
              .setFullMethodName(generateFullMethodName(SERVICE_NAME, "Put"))
              .setSampledToLocalTracing(true)
              .setRequestMarshaller(io.grpc.protobuf.ProtoUtils.marshaller(
                  yikv.db.YikvGrpc.FbRpcRequest.getDefaultInstance()))
              .setResponseMarshaller(io.grpc.protobuf.ProtoUtils.marshaller(
                  yikv.db.YikvGrpc.FbRpcResponse.getDefaultInstance()))
              .setSchemaDescriptor(new YikvDbMethodDescriptorSupplier("Put"))
              .build();
        }
      }
    }
    return getPutMethod;
  }

  private static volatile io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest,
      yikv.db.YikvGrpc.FbRpcResponse> getPutBatchMethod;

  @io.grpc.stub.annotations.RpcMethod(
      fullMethodName = SERVICE_NAME + '/' + "PutBatch",
      requestType = yikv.db.YikvGrpc.FbRpcRequest.class,
      responseType = yikv.db.YikvGrpc.FbRpcResponse.class,
      methodType = io.grpc.MethodDescriptor.MethodType.UNARY)
  public static io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest,
      yikv.db.YikvGrpc.FbRpcResponse> getPutBatchMethod() {
    io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest, yikv.db.YikvGrpc.FbRpcResponse> getPutBatchMethod;
    if ((getPutBatchMethod = YikvDbGrpc.getPutBatchMethod) == null) {
      synchronized (YikvDbGrpc.class) {
        if ((getPutBatchMethod = YikvDbGrpc.getPutBatchMethod) == null) {
          YikvDbGrpc.getPutBatchMethod = getPutBatchMethod =
              io.grpc.MethodDescriptor.<yikv.db.YikvGrpc.FbRpcRequest, yikv.db.YikvGrpc.FbRpcResponse>newBuilder()
              .setType(io.grpc.MethodDescriptor.MethodType.UNARY)
              .setFullMethodName(generateFullMethodName(SERVICE_NAME, "PutBatch"))
              .setSampledToLocalTracing(true)
              .setRequestMarshaller(io.grpc.protobuf.ProtoUtils.marshaller(
                  yikv.db.YikvGrpc.FbRpcRequest.getDefaultInstance()))
              .setResponseMarshaller(io.grpc.protobuf.ProtoUtils.marshaller(
                  yikv.db.YikvGrpc.FbRpcResponse.getDefaultInstance()))
              .setSchemaDescriptor(new YikvDbMethodDescriptorSupplier("PutBatch"))
              .build();
        }
      }
    }
    return getPutBatchMethod;
  }

  private static volatile io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest,
      yikv.db.YikvGrpc.FbRpcResponse> getBatchGetMethod;

  @io.grpc.stub.annotations.RpcMethod(
      fullMethodName = SERVICE_NAME + '/' + "BatchGet",
      requestType = yikv.db.YikvGrpc.FbRpcRequest.class,
      responseType = yikv.db.YikvGrpc.FbRpcResponse.class,
      methodType = io.grpc.MethodDescriptor.MethodType.UNARY)
  public static io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest,
      yikv.db.YikvGrpc.FbRpcResponse> getBatchGetMethod() {
    io.grpc.MethodDescriptor<yikv.db.YikvGrpc.FbRpcRequest, yikv.db.YikvGrpc.FbRpcResponse> getBatchGetMethod;
    if ((getBatchGetMethod = YikvDbGrpc.getBatchGetMethod) == null) {
      synchronized (YikvDbGrpc.class) {
        if ((getBatchGetMethod = YikvDbGrpc.getBatchGetMethod) == null) {
          YikvDbGrpc.getBatchGetMethod = getBatchGetMethod =
              io.grpc.MethodDescriptor.<yikv.db.YikvGrpc.FbRpcRequest, yikv.db.YikvGrpc.FbRpcResponse>newBuilder()
              .setType(io.grpc.MethodDescriptor.MethodType.UNARY)
              .setFullMethodName(generateFullMethodName(SERVICE_NAME, "BatchGet"))
              .setSampledToLocalTracing(true)
              .setRequestMarshaller(io.grpc.protobuf.ProtoUtils.marshaller(
                  yikv.db.YikvGrpc.FbRpcRequest.getDefaultInstance()))
              .setResponseMarshaller(io.grpc.protobuf.ProtoUtils.marshaller(
                  yikv.db.YikvGrpc.FbRpcResponse.getDefaultInstance()))
              .setSchemaDescriptor(new YikvDbMethodDescriptorSupplier("BatchGet"))
              .build();
        }
      }
    }
    return getBatchGetMethod;
  }

  /**
   * Creates a new async stub that supports all call types for the service
   */
  public static YikvDbStub newStub(io.grpc.Channel channel) {
    io.grpc.stub.AbstractStub.StubFactory<YikvDbStub> factory =
      new io.grpc.stub.AbstractStub.StubFactory<YikvDbStub>() {
        @java.lang.Override
        public YikvDbStub newStub(io.grpc.Channel channel, io.grpc.CallOptions callOptions) {
          return new YikvDbStub(channel, callOptions);
        }
      };
    return YikvDbStub.newStub(factory, channel);
  }

  /**
   * Creates a new blocking-style stub that supports unary and streaming output calls on the service
   */
  public static YikvDbBlockingStub newBlockingStub(
      io.grpc.Channel channel) {
    io.grpc.stub.AbstractStub.StubFactory<YikvDbBlockingStub> factory =
      new io.grpc.stub.AbstractStub.StubFactory<YikvDbBlockingStub>() {
        @java.lang.Override
        public YikvDbBlockingStub newStub(io.grpc.Channel channel, io.grpc.CallOptions callOptions) {
          return new YikvDbBlockingStub(channel, callOptions);
        }
      };
    return YikvDbBlockingStub.newStub(factory, channel);
  }

  /**
   * Creates a new ListenableFuture-style stub that supports unary calls on the service
   */
  public static YikvDbFutureStub newFutureStub(
      io.grpc.Channel channel) {
    io.grpc.stub.AbstractStub.StubFactory<YikvDbFutureStub> factory =
      new io.grpc.stub.AbstractStub.StubFactory<YikvDbFutureStub>() {
        @java.lang.Override
        public YikvDbFutureStub newStub(io.grpc.Channel channel, io.grpc.CallOptions callOptions) {
          return new YikvDbFutureStub(channel, callOptions);
        }
      };
    return YikvDbFutureStub.newStub(factory, channel);
  }

  /**
   * <pre>
   * Same logical service as brpc BaiduMasterService (yikv.db.YikvDb / Get, Put, …).
   * </pre>
   */
  public interface AsyncService {

    /**
     */
    default void get(yikv.db.YikvGrpc.FbRpcRequest request,
        io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse> responseObserver) {
      io.grpc.stub.ServerCalls.asyncUnimplementedUnaryCall(getGetMethod(), responseObserver);
    }

    /**
     */
    default void put(yikv.db.YikvGrpc.FbRpcRequest request,
        io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse> responseObserver) {
      io.grpc.stub.ServerCalls.asyncUnimplementedUnaryCall(getPutMethod(), responseObserver);
    }

    /**
     */
    default void putBatch(yikv.db.YikvGrpc.FbRpcRequest request,
        io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse> responseObserver) {
      io.grpc.stub.ServerCalls.asyncUnimplementedUnaryCall(getPutBatchMethod(), responseObserver);
    }

    /**
     */
    default void batchGet(yikv.db.YikvGrpc.FbRpcRequest request,
        io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse> responseObserver) {
      io.grpc.stub.ServerCalls.asyncUnimplementedUnaryCall(getBatchGetMethod(), responseObserver);
    }
  }

  /**
   * Base class for the server implementation of the service YikvDb.
   * <pre>
   * Same logical service as brpc BaiduMasterService (yikv.db.YikvDb / Get, Put, …).
   * </pre>
   */
  public static abstract class YikvDbImplBase
      implements io.grpc.BindableService, AsyncService {

    @java.lang.Override public final io.grpc.ServerServiceDefinition bindService() {
      return YikvDbGrpc.bindService(this);
    }
  }

  /**
   * A stub to allow clients to do asynchronous rpc calls to service YikvDb.
   * <pre>
   * Same logical service as brpc BaiduMasterService (yikv.db.YikvDb / Get, Put, …).
   * </pre>
   */
  public static final class YikvDbStub
      extends io.grpc.stub.AbstractAsyncStub<YikvDbStub> {
    private YikvDbStub(
        io.grpc.Channel channel, io.grpc.CallOptions callOptions) {
      super(channel, callOptions);
    }

    @java.lang.Override
    protected YikvDbStub build(
        io.grpc.Channel channel, io.grpc.CallOptions callOptions) {
      return new YikvDbStub(channel, callOptions);
    }

    /**
     */
    public void get(yikv.db.YikvGrpc.FbRpcRequest request,
        io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse> responseObserver) {
      io.grpc.stub.ClientCalls.asyncUnaryCall(
          getChannel().newCall(getGetMethod(), getCallOptions()), request, responseObserver);
    }

    /**
     */
    public void put(yikv.db.YikvGrpc.FbRpcRequest request,
        io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse> responseObserver) {
      io.grpc.stub.ClientCalls.asyncUnaryCall(
          getChannel().newCall(getPutMethod(), getCallOptions()), request, responseObserver);
    }

    /**
     */
    public void putBatch(yikv.db.YikvGrpc.FbRpcRequest request,
        io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse> responseObserver) {
      io.grpc.stub.ClientCalls.asyncUnaryCall(
          getChannel().newCall(getPutBatchMethod(), getCallOptions()), request, responseObserver);
    }

    /**
     */
    public void batchGet(yikv.db.YikvGrpc.FbRpcRequest request,
        io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse> responseObserver) {
      io.grpc.stub.ClientCalls.asyncUnaryCall(
          getChannel().newCall(getBatchGetMethod(), getCallOptions()), request, responseObserver);
    }
  }

  /**
   * A stub to allow clients to do synchronous rpc calls to service YikvDb.
   * <pre>
   * Same logical service as brpc BaiduMasterService (yikv.db.YikvDb / Get, Put, …).
   * </pre>
   */
  public static final class YikvDbBlockingStub
      extends io.grpc.stub.AbstractBlockingStub<YikvDbBlockingStub> {
    private YikvDbBlockingStub(
        io.grpc.Channel channel, io.grpc.CallOptions callOptions) {
      super(channel, callOptions);
    }

    @java.lang.Override
    protected YikvDbBlockingStub build(
        io.grpc.Channel channel, io.grpc.CallOptions callOptions) {
      return new YikvDbBlockingStub(channel, callOptions);
    }

    /**
     */
    public yikv.db.YikvGrpc.FbRpcResponse get(yikv.db.YikvGrpc.FbRpcRequest request) {
      return io.grpc.stub.ClientCalls.blockingUnaryCall(
          getChannel(), getGetMethod(), getCallOptions(), request);
    }

    /**
     */
    public yikv.db.YikvGrpc.FbRpcResponse put(yikv.db.YikvGrpc.FbRpcRequest request) {
      return io.grpc.stub.ClientCalls.blockingUnaryCall(
          getChannel(), getPutMethod(), getCallOptions(), request);
    }

    /**
     */
    public yikv.db.YikvGrpc.FbRpcResponse putBatch(yikv.db.YikvGrpc.FbRpcRequest request) {
      return io.grpc.stub.ClientCalls.blockingUnaryCall(
          getChannel(), getPutBatchMethod(), getCallOptions(), request);
    }

    /**
     */
    public yikv.db.YikvGrpc.FbRpcResponse batchGet(yikv.db.YikvGrpc.FbRpcRequest request) {
      return io.grpc.stub.ClientCalls.blockingUnaryCall(
          getChannel(), getBatchGetMethod(), getCallOptions(), request);
    }
  }

  /**
   * A stub to allow clients to do ListenableFuture-style rpc calls to service YikvDb.
   * <pre>
   * Same logical service as brpc BaiduMasterService (yikv.db.YikvDb / Get, Put, …).
   * </pre>
   */
  public static final class YikvDbFutureStub
      extends io.grpc.stub.AbstractFutureStub<YikvDbFutureStub> {
    private YikvDbFutureStub(
        io.grpc.Channel channel, io.grpc.CallOptions callOptions) {
      super(channel, callOptions);
    }

    @java.lang.Override
    protected YikvDbFutureStub build(
        io.grpc.Channel channel, io.grpc.CallOptions callOptions) {
      return new YikvDbFutureStub(channel, callOptions);
    }

    /**
     */
    public com.google.common.util.concurrent.ListenableFuture<yikv.db.YikvGrpc.FbRpcResponse> get(
        yikv.db.YikvGrpc.FbRpcRequest request) {
      return io.grpc.stub.ClientCalls.futureUnaryCall(
          getChannel().newCall(getGetMethod(), getCallOptions()), request);
    }

    /**
     */
    public com.google.common.util.concurrent.ListenableFuture<yikv.db.YikvGrpc.FbRpcResponse> put(
        yikv.db.YikvGrpc.FbRpcRequest request) {
      return io.grpc.stub.ClientCalls.futureUnaryCall(
          getChannel().newCall(getPutMethod(), getCallOptions()), request);
    }

    /**
     */
    public com.google.common.util.concurrent.ListenableFuture<yikv.db.YikvGrpc.FbRpcResponse> putBatch(
        yikv.db.YikvGrpc.FbRpcRequest request) {
      return io.grpc.stub.ClientCalls.futureUnaryCall(
          getChannel().newCall(getPutBatchMethod(), getCallOptions()), request);
    }

    /**
     */
    public com.google.common.util.concurrent.ListenableFuture<yikv.db.YikvGrpc.FbRpcResponse> batchGet(
        yikv.db.YikvGrpc.FbRpcRequest request) {
      return io.grpc.stub.ClientCalls.futureUnaryCall(
          getChannel().newCall(getBatchGetMethod(), getCallOptions()), request);
    }
  }

  private static final int METHODID_GET = 0;
  private static final int METHODID_PUT = 1;
  private static final int METHODID_PUT_BATCH = 2;
  private static final int METHODID_BATCH_GET = 3;

  private static final class MethodHandlers<Req, Resp> implements
      io.grpc.stub.ServerCalls.UnaryMethod<Req, Resp>,
      io.grpc.stub.ServerCalls.ServerStreamingMethod<Req, Resp>,
      io.grpc.stub.ServerCalls.ClientStreamingMethod<Req, Resp>,
      io.grpc.stub.ServerCalls.BidiStreamingMethod<Req, Resp> {
    private final AsyncService serviceImpl;
    private final int methodId;

    MethodHandlers(AsyncService serviceImpl, int methodId) {
      this.serviceImpl = serviceImpl;
      this.methodId = methodId;
    }

    @java.lang.Override
    @java.lang.SuppressWarnings("unchecked")
    public void invoke(Req request, io.grpc.stub.StreamObserver<Resp> responseObserver) {
      switch (methodId) {
        case METHODID_GET:
          serviceImpl.get((yikv.db.YikvGrpc.FbRpcRequest) request,
              (io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse>) responseObserver);
          break;
        case METHODID_PUT:
          serviceImpl.put((yikv.db.YikvGrpc.FbRpcRequest) request,
              (io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse>) responseObserver);
          break;
        case METHODID_PUT_BATCH:
          serviceImpl.putBatch((yikv.db.YikvGrpc.FbRpcRequest) request,
              (io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse>) responseObserver);
          break;
        case METHODID_BATCH_GET:
          serviceImpl.batchGet((yikv.db.YikvGrpc.FbRpcRequest) request,
              (io.grpc.stub.StreamObserver<yikv.db.YikvGrpc.FbRpcResponse>) responseObserver);
          break;
        default:
          throw new AssertionError();
      }
    }

    @java.lang.Override
    @java.lang.SuppressWarnings("unchecked")
    public io.grpc.stub.StreamObserver<Req> invoke(
        io.grpc.stub.StreamObserver<Resp> responseObserver) {
      switch (methodId) {
        default:
          throw new AssertionError();
      }
    }
  }

  public static final io.grpc.ServerServiceDefinition bindService(AsyncService service) {
    return io.grpc.ServerServiceDefinition.builder(getServiceDescriptor())
        .addMethod(
          getGetMethod(),
          io.grpc.stub.ServerCalls.asyncUnaryCall(
            new MethodHandlers<
              yikv.db.YikvGrpc.FbRpcRequest,
              yikv.db.YikvGrpc.FbRpcResponse>(
                service, METHODID_GET)))
        .addMethod(
          getPutMethod(),
          io.grpc.stub.ServerCalls.asyncUnaryCall(
            new MethodHandlers<
              yikv.db.YikvGrpc.FbRpcRequest,
              yikv.db.YikvGrpc.FbRpcResponse>(
                service, METHODID_PUT)))
        .addMethod(
          getPutBatchMethod(),
          io.grpc.stub.ServerCalls.asyncUnaryCall(
            new MethodHandlers<
              yikv.db.YikvGrpc.FbRpcRequest,
              yikv.db.YikvGrpc.FbRpcResponse>(
                service, METHODID_PUT_BATCH)))
        .addMethod(
          getBatchGetMethod(),
          io.grpc.stub.ServerCalls.asyncUnaryCall(
            new MethodHandlers<
              yikv.db.YikvGrpc.FbRpcRequest,
              yikv.db.YikvGrpc.FbRpcResponse>(
                service, METHODID_BATCH_GET)))
        .build();
  }

  private static abstract class YikvDbBaseDescriptorSupplier
      implements io.grpc.protobuf.ProtoFileDescriptorSupplier, io.grpc.protobuf.ProtoServiceDescriptorSupplier {
    YikvDbBaseDescriptorSupplier() {}

    @java.lang.Override
    public com.google.protobuf.Descriptors.FileDescriptor getFileDescriptor() {
      return yikv.db.YikvGrpc.getDescriptor();
    }

    @java.lang.Override
    public com.google.protobuf.Descriptors.ServiceDescriptor getServiceDescriptor() {
      return getFileDescriptor().findServiceByName("YikvDb");
    }
  }

  private static final class YikvDbFileDescriptorSupplier
      extends YikvDbBaseDescriptorSupplier {
    YikvDbFileDescriptorSupplier() {}
  }

  private static final class YikvDbMethodDescriptorSupplier
      extends YikvDbBaseDescriptorSupplier
      implements io.grpc.protobuf.ProtoMethodDescriptorSupplier {
    private final java.lang.String methodName;

    YikvDbMethodDescriptorSupplier(java.lang.String methodName) {
      this.methodName = methodName;
    }

    @java.lang.Override
    public com.google.protobuf.Descriptors.MethodDescriptor getMethodDescriptor() {
      return getServiceDescriptor().findMethodByName(methodName);
    }
  }

  private static volatile io.grpc.ServiceDescriptor serviceDescriptor;

  public static io.grpc.ServiceDescriptor getServiceDescriptor() {
    io.grpc.ServiceDescriptor result = serviceDescriptor;
    if (result == null) {
      synchronized (YikvDbGrpc.class) {
        result = serviceDescriptor;
        if (result == null) {
          serviceDescriptor = result = io.grpc.ServiceDescriptor.newBuilder(SERVICE_NAME)
              .setSchemaDescriptor(new YikvDbFileDescriptorSupplier())
              .addMethod(getGetMethod())
              .addMethod(getPutMethod())
              .addMethod(getPutBatchMethod())
              .addMethod(getBatchGetMethod())
              .build();
        }
      }
    }
    return result;
  }
}
