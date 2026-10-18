import { Request, RequestHandler, Response, Server, requestHandler } from "std/http-server"
import { queryInt, respondTo } from "./workload"

export readonly MAX_WORKERS = 256

// One worker's mutable state. Each worker lives inside its own Actor<Worker>,
// so its mailbox serializes the requests it receives while different workers
// run in parallel on the runtime's thread pool.
export class Worker {
  readonly id: int
  // Root-domain callback for /admin/* requests, which touch pool state.
  readonly admin: (request: Request): none
  let served: long = 0L

  // Returns a handler owned by this worker's actor. The server posts requests
  // straight to it, so they run here without passing through the root domain.
  handler(): RequestHandler {
    return requestHandler((request: Request): none => this.handle(request))
  }

  handle(request: Request): none {
    if request.path.startsWith("/admin/") {
      case this.admin.tryPost(request) {
        _: Success -> {},
        f: Failure -> request.respond(Response.text(503, "admin ${f.error}\n")) else {},
      }
      return
    }
    this.served += 1L
    request.respond(respondTo(this.id, request)) else error {
      // The client disconnected or timed out before we answered.
      println("worker ${this.id}: could not respond: ${error.message}")
    }
  }

  servedCount(): long => this.served
}

// Lives on the root domain. It owns the worker actors and tells the server
// which of them receive new requests; it is not on the request path.
export class WorkerPool {
  private workers: Actor<Worker>[] = []
  private handlers: RequestHandler[] = []
  private let active: int = 0
  private let server: Server | none = none

  // Changes how many workers receive new requests. Growing creates actors;
  // shrinking parks the extra ones (they finish what they already accepted).
  resize(count: int): none {
    while this.workers.length < count {
      worker := Actor<Worker>(this.workers.length, (request: Request): none => this.handleAdmin(request))
      this.handlers.push(worker.handler())
      this.workers.push(worker)
    }
    this.active = count
    server := this.server else {
      return
    }
    server.setHandlers(this.activeHandlers()) else error {
      println("could not resize pool: ${error.message}")
    }
  }

  // The handlers of the first `size()` workers, for Server.listenPool.
  activeHandlers(): readonly RequestHandler[] {
    selected: RequestHandler[] := []
    for i of 0..<this.active {
      selected.push(this.handlers[i])
    }
    return selected.drainToReadonly()
  }

  attach(server: Server): none {
    this.server = server
  }

  size(): int => this.active

  // Per-worker request counts. Each call waits for that worker to finish the
  // requests already in its mailbox, so call it when the server is quiet.
  stats(): string {
    let total = 0L
    let perWorker = ""
    for i of 0..<this.workers.length {
      served := this.workers[i].servedCount()
      total += served
      perWorker += if i == 0 then "${served}" else ",${served}"
    }
    server := this.server else {
      return "workers=${this.active} total=${total} served=${perWorker}\n"
    }
    rejected := server.shedCount()
    return "workers=${this.active} total=${total} rejected=${rejected} served=${perWorker}\n"
  }

  private handleAdmin(request: Request): none {
    case request.path {
      "/admin/workers" -> {
        if request.method == "POST" {
          count := queryInt(request, "count", 0, MAX_WORKERS)
          if count < 1 {
            request.respond(Response.text(400, "count must be 1..${MAX_WORKERS}\n")) else {}
            return
          }
          this.resize(count)
        }
        request.respond(Response.text(200, "workers=${this.size()}\n")) else {}
      },
      "/admin/stats" -> {
        request.respond(Response.text(200, this.stats())) else {}
      },
      _ -> request.respond(Response.text(404, "not found\n")) else {},
    }
  }
}
