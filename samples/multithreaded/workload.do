import { Request, Response } from "std/http-server"
import { parseInt } from "std/parse"
import { Duration, Thread } from "std/time"
import { parseQueryParams } from "std/url"

// Reads a non-negative integer query parameter, falling back to `fallback`
// when it is absent or malformed and clamping it to `limit`.
export isolated function queryInt(request: Request, name: string, fallback: int, limit: int): int {
  params := parseQueryParams(request.queryString) else {
    return fallback
  }
  param := params.first(name) else {
    return fallback
  }
  text := param.value else {
    return fallback
  }
  value := parseInt(text) else {
    return fallback
  }
  if value < 0 {
    return fallback
  }
  return if value > limit then limit else value
}

// CPU-bound work: a modular hash chain the optimizer cannot skip because the
// result is returned to the client.
export isolated function spin(iterations: int): long {
  let hash = 17L
  for i of 0..<iterations {
    hash = (hash * 31L + long(i)) % 1_000_000_007L
  }
  return hash
}

// Produces the response for one request. It runs on whichever worker actor
// the pool chose, so it must be isolated: no shared mutable state.
export isolated function respondTo(workerId: int, request: Request): Response {
  return case request.path {
    "/hello" -> Response.text(200, "hello from worker ${workerId}\n"),

    // /cpu?n=200000 burns CPU on the worker; throughput scales with workers
    // until the machine runs out of cores.
    "/cpu" -> {
      iterations := queryInt(request, "n", 200_000, 50_000_000)
      yield Response.text(200, "worker ${workerId} hash ${spin(iterations)}\n")
    },

    // /sleep?ms=10 blocks the worker like a synchronous database or file
    // call would. Throughput scales with workers, not cores.
    "/sleep" -> {
      millis := queryInt(request, "ms", 10, 10_000)
      Thread.sleep(Duration.ofMillis(long(millis)))
      yield Response.text(200, "worker ${workerId} slept ${millis}ms\n")
    },

    _ -> Response.text(404, "not found\n"),
  }
}
