import Foundation

/// Holds a host state folder the way a running hostd does (ProcessLock): a
/// flock on host.lock and an OS record lock on host.lock-owner naming the
/// holder's PID. Clients trust host.ready only while that record lock is held
/// by the PID in it, so a fixture that serves the Host contract itself
/// publishes `pid` in its host.ready.
///
/// The locks live in a child process: the kernel reports a record lock only
/// to other processes, so one held by the test process would look free to the
/// client running in it.
public final class FixtureHostLock {
  public let pid: Int32
  private let process: Process
  private let input: Pipe

  public init(state: URL) throws {
    process = Process()
    input = Pipe()
    let output = Pipe()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/perl")
    // struct flock on Darwin: off_t l_start, off_t l_len, pid_t l_pid, short l_type, short l_whence.
    process.arguments = ["-MFcntl=:DEFAULT,:flock", "-MSys::Hostname", "-e", """
      my $d = shift;
      open(my $l, '>>', "$d/host.lock") or die "host.lock: $!";
      flock($l, LOCK_EX | LOCK_NB) or die "host.lock is held";
      sysopen(my $o, "$d/host.lock-owner", O_RDWR | O_CREAT, 0600) or die "host.lock-owner: $!";
      truncate($o, 0);
      syswrite($o, qq({"pid":$$,"application":"fixture","hostname":") . hostname() . qq("}));
      # fcntl writes the structure back, so it needs a variable, not a constant.
      my $lock = pack("q q l s s", 0, 0, 0, F_WRLCK, 0);
      fcntl($o, F_SETLK, $lock) or die "record lock: $!";
      $| = 1;
      print "locked\\n";
      while (<STDIN>) {}
      """, state.path]
    process.standardInput = input
    process.standardOutput = output
    try process.run()
    let line = output.fileHandleForReading.availableData
    guard String(decoding: line, as: UTF8.self) == "locked\n" else {
      process.waitUntilExit()
      throw POSIXError(.EBUSY)
    }
    pid = process.processIdentifier
  }

  /// The child exits on end of input; the kernel drops both locks with it.
  public func release() {
    try? input.fileHandleForWriting.close()
    process.waitUntilExit()
  }

  deinit { release() }
}
