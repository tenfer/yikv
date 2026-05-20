package io.yikv.client;

/** Decoded {@link yikv.PutBatchResponse}. */
public record PutBatchResult(boolean ok, String err) {}
