package io.yikv.client;

import java.util.Map;

/**
 * Decoded FlatBuffers GetResponse ('yikv.GetResponse' generated class). {@code err} is non-null when the server populated an error
 * string; empty maps mean no row fields (e.g. miss).
 */
public record GetResult(boolean found, String err, long indexGetNs, Map<String, Object> row) {}
