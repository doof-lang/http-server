import { CliSpec } from "std/cli"
import { runMainEventLoop } from "std/event"
import { Server } from "std/http-server"
import { parseInt } from "std/parse"
import { MAX_WORKERS, WorkerPool } from "./pool"

function main(args: string[]): int {
  spec := CliSpec.create("server", "Multithreaded HTTP server backed by a pool of worker actors")
    .option{ name: "workers", short: "w", valueName: "N", description: "worker actors", defaultValue: "4" }
    .option{ name: "port", short: "p", valueName: "PORT", description: "listen port", defaultValue: "8080" }
  options := spec.parse(args) else error {
    println(error.message)
    println(error.usage)
    return 2
  }
  workers := parseInt(options.string("workers") ?? "4") else {
    println("--workers must be an integer")
    return 2
  }
  port := parseInt(options.string("port") ?? "8080") else {
    println("--port must be an integer")
    return 2
  }
  if workers < 1 || workers > MAX_WORKERS {
    println("--workers must be between 1 and ${MAX_WORKERS}")
    return 2
  }

  pool := WorkerPool()
  pool.resize(workers)

  // The server posts each request straight to one of the workers' handlers,
  // in round-robin order. Workers forward /admin/* back to the pool here.
  server := Server.listenPool{
    options: { port },
    handlers: pool.activeHandlers(),
  } else error {
    println("Could not start server: ${error.message}")
    return 1
  }
  pool.attach(server)

  println("Listening on http://${server.host}:${server.port}/ with ${workers} workers")
  runMainEventLoop()
  server.close()!
  return 0
}
