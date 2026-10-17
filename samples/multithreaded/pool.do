import { Request, Response } from "std/http-server"
import { respondTo } from "./workload"

// One worker's mutable state. Each worker lives inside its own Actor<Worker>,
// so its mailbox serializes the requests it receives while different workers
// run in parallel on the runtime's thread pool.
export class Worker {
  readonly id: int
  let served: long = 0L

  // Returns a callback owned by this worker's actor. Posting a Request to it
  // queues `handle` on this worker instead of on the caller.
  handler(): (request: Request): none {
    return (request: Request): none => this.handle(request)
  }

  handle(request: Request): none {
    this.served += 1L
    request.respond(respondTo(this.id, request)) else error {
      // The client disconnected or timed out before we answered.
      println("worker ${this.id}: could not respond: ${error.message}")
    }
  }

  servedCount(): long => this.served
}

// Lives on the root domain and spreads requests across worker actors in
// round-robin order. Dispatch only posts a message, so the root event loop
// stays cheap while the workers do the real work.
export class WorkerPool {
  private workers: Actor<Worker>[] = []
  private handlers: ((request: Request): none)[] = []
  private let active: int = 0
  private let next: int = 0
  private let rejected: long = 0L

  // Changes how many workers receive new requests. Growing creates actors;
  // shrinking parks the extra ones (they finish what they already accepted).
  resize(count: int): none {
    while this.workers.length < count {
      worker := Actor<Worker>(this.workers.length)
      this.handlers.push(worker.handler())
      this.workers.push(worker)
    }
    this.active = count
    this.next = 0
  }

  size(): int => this.active

  dispatch(request: Request): none {
    handler := this.handlers[this.next]
    this.next = (this.next + 1) % this.active

    // tryPost never blocks the event loop. A full worker mailbox sheds the
    // request with 503 instead of queueing without bound.
    case handler.tryPost(request) {
      _: Success -> {},
      f: Failure -> {
        this.rejected += 1L
        request.respond(Response.text(503, "worker ${f.error}\n")) else {}
      },
    }
  }

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
    return "workers=${this.active} total=${total} rejected=${this.rejected} served=${perWorker}\n"
  }
}
