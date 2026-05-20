package io.yikv.client;

/** Unchecked exception for wiring, transport, or unexpected response errors. */
public final class YikvClientException extends RuntimeException {

  public YikvClientException(String message) {
    super(message);
  }

  public YikvClientException(String message, Throwable cause) {
    super(message, cause);
  }
}
