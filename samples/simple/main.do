import { runMainEventLoop } from "std/event"
import { Request, Response, Server, ServerOptions } from "std/http-server"
import { Router, StaticFileOptions } from "std/http-router"
import { currentWorkingDirectory, join } from "std/path"

function main(args: string[]): int {
  cwd := currentWorkingDirectory() else error {
    println("Could not read current working directory: ${error}")
    return 1
  }

  documentRoot := if args.length > 0 then args[0] else join([cwd, "public"])

  router := Router()
    .staticFiles("/", { root: documentRoot })

  server := Server.listen{
    options: { port: 8080 },
    onRequest: (request: Request): none => {
      response := router.handle(request) ?? Response.text(404, "not found\n")
      request.respond(response)!
    },
  } else error {
    println("Could not start server: ${error.message}")
    return 1
  }

  println("Serving ${documentRoot} at http://${server.host}:${server.port}/")
  runMainEventLoop()
  server.close()!
  return 0
}
