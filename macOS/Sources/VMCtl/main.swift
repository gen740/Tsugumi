// tsugumi-vmctl: drives the shared VM without the app (tests, diagnosis).
//
//   tsugumi-vmctl --support DIR [--console] [--cpus N] [--memory GiB]
//   (TSUGUMI_VM_CMDLINE replaces the kernel command line, for diagnosis)
//
// Reads commands from stdin, one per line, and answers on stdout:
//   start <project folder>   -> ready <id> <ws url> <control folder> | error <message>
//   stop <project folder>    -> stopped | error <message>
//   delete <project folder>  -> deleted | error <message>
//   vm <request>             -> ok <reply> | error <message>   (raw tsugumi-vmd request)
//   sh <id> <command>        -> the command's terminal output, then "exit <status>"
//                               (runs in the project's terminal environment)
//   quit                     -> stops every project and the VM, then exits
// With --console the VM console goes to stderr.
// Needs the com.apple.security.virtualization entitlement (codesign).
import Foundation
import TsugumiLauncher

var support: URL?
var echoConsole = false
var resources = VMResources.default
var args = CommandLine.arguments.dropFirst()
while let a = args.popFirst() {
  switch a {
  case "--support": support = args.popFirst().map { URL(fileURLWithPath: $0) }
  case "--console": echoConsole = true
  case "--cpus": resources.cpus = Int(args.popFirst() ?? "") ?? resources.cpus
  case "--memory": resources.memoryGiB = Int(args.popFirst() ?? "") ?? resources.memoryGiB
  default:
    FileHandle.standardError.write(Data("usage: tsugumi-vmctl --support DIR [--console]\n".utf8))
    exit(2)
  }
}
guard let support else {
  FileHandle.standardError.write(Data("--support is required\n".utf8))
  exit(2)
}
setvbuf(stdout, nil, _IOLBF, 0)
let manager = EnvironmentManager(support: support, resources: resources)
if echoConsole {
  _ = manager.vm.console.observe { line in FileHandle.standardError.write(Data((line + "\n").utf8)) }
}

func say(_ s: String) { print(s) }

Task {
  while let line = readLine() {
    let parts = line.split(separator: " ", maxSplits: 1).map(String.init)
    guard let cmd = parts.first else { continue }
    let arg = parts.count > 1 ? parts[1] : ""
    do {
      switch cmd {
      case "start":
        let p = try ProjectIdentity(url: URL(fileURLWithPath: arg))
        let ep = try await manager.start(p)
        say("ready \(p.id) \(ep.url.absoluteString) \(manager.controlDir(p).path)")
      case "stop":
        try await manager.stop(try ProjectIdentity(url: URL(fileURLWithPath: arg)))
        say("stopped")
      case "delete":
        try await manager.deleteData(try ProjectIdentity(url: URL(fileURLWithPath: arg)))
        say("deleted")
      case "vm":
        say("ok " + (try await manager.vm.request(arg)))
      case "sh":
        let sp = arg.split(separator: " ", maxSplits: 1).map(String.init)
        guard sp.count == 2 else { say("error usage: sh <id> <command>"); continue }
        let fd = try await manager.vm.connect(port: SharedVM.controlPort)
        defer { close(fd) }
        let req = "shell \(sp[0]) 50 200\n"
        _ = req.withCString { write(fd, $0, strlen($0)) }
        var reply: [UInt8] = []
        var ch: UInt8 = 0
        while read(fd, &ch, 1) == 1, ch != 10 { reply.append(ch) }
        let r = String(decoding: reply, as: UTF8.self)
        guard r.hasPrefix("ok") else { say(r); continue }
        // No echo, a fixed marker for the status, then leave.
        let script = "stty -echo; PS1=; \(sp[1]); echo __TSUGUMI_EXIT=$?; exit\n"
        _ = script.withCString { write(fd, $0, strlen($0)) }
        var out = Data()
        var buf = [UInt8](repeating: 0, count: 65536)
        while true {
          let n = read(fd, &buf, buf.count)
          if n <= 0 { break }
          out.append(contentsOf: buf[0..<n])
        }
        let text = String(decoding: out, as: UTF8.self).replacingOccurrences(of: "\r\n", with: "\n")
        var status = "?"
        for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
          if let r = line.range(of: "__TSUGUMI_EXIT=") { status = String(line[r.upperBound...]).trimmingCharacters(in: .whitespaces) }
          else if !line.contains("stty -echo") { print(line) }
        }
        say("exit \(status)")
      case "quit":
        await manager.shutdown()
        say("bye")
        exit(0)
      default:
        say("error unknown command \(cmd)")
      }
    } catch {
      say("error " + "\(error)".replacingOccurrences(of: "\n", with: " "))
    }
  }
  await manager.shutdown()
  exit(0)
}
dispatchMain()
