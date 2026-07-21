import { createChannel, runMainEventLoop } from "std/event"
import { Request, Response, Server, ServerOptions } from "std/http-server"
import { Router, StaticFileOptions } from "std/http-router"
import { currentWorkingDirectory, join } from "std/path"

function main(args: string[]): int {
  cwd := currentWorkingDirectory() else {
    println("Could not read current working directory: ${cwd.error}")
    return 1
  }

  documentRoot := if args.length > 0 then args[0] else join([cwd, "public"])

  router := Router()
    .staticFiles("/", { root: documentRoot })

  (requestSender, requestReceiver) := createChannel<Request>{
    capacity: 256,
    keepsAlive: true,
  }

  requestReceiver.onMessage((request: Request): none => {
    response := router.handle(request) ?? Response.text(404, "not found\n")
    try! request.respond(response)
  })

  server := Server.listen{
    options: { port: 8080 },
    requests: requestSender,
  } else {
    println("Could not start server: ${server.error.message}")
    return 1
  }

  println("Serving ${documentRoot} at http://${server.host}:${server.port}/")
  runMainEventLoop()
  try! server.close()
  return 0
}
