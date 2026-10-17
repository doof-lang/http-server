import { CliSpec } from "std/cli"
import { runMainEventLoop } from "std/event"
import { Request, Response, Server } from "std/http-server"
import { parseInt } from "std/parse"
import { WorkerPool } from "./pool"
import { queryInt } from "./workload"

readonly MAX_WORKERS = 256

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

  // The server posts every request to this root-domain callback. Admin
  // endpoints touch pool state, so they stay here; everything else goes to a
  // worker actor.
  server := Server.listen{
    options: { port },
    onRequest: (request: Request): none => {
      case request.path {
        "/admin/workers" -> {
          if request.method == "POST" {
            count := queryInt(request, "count", 0, MAX_WORKERS)
            if count < 1 {
              request.respond(Response.text(400, "count must be 1..${MAX_WORKERS}\n")) else {}
              return
            }
            pool.resize(count)
          }
          request.respond(Response.text(200, "workers=${pool.size()}\n")) else {}
        },
        "/admin/stats" -> {
          request.respond(Response.text(200, pool.stats())) else {}
        },
        _ -> pool.dispatch(request),
      }
    },
  } else error {
    println("Could not start server: ${error.message}")
    return 1
  }

  println("Listening on http://${server.host}:${server.port}/ with ${workers} workers")
  runMainEventLoop()
  server.close()!
  return 0
}
