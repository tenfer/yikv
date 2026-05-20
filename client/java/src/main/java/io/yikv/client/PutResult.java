package io.yikv.client;

/** Decoded {@link yikv.PutResponse}. */
public record PutResult(boolean ok, String err) {}
