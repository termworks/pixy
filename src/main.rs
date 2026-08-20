fn main() {
    // `pixy names | head` closes the pipe early. Rust's default SIGPIPE is
    // ignore, which turns that into a panic on the next write; restoring the
    // default makes the process exit quietly like every other CLI.
    unsafe { libc::signal(libc::SIGPIPE, libc::SIG_DFL) };
    if let Err(error) = pixy::application::cli::run() {
        eprintln!("{}: {error}", pixy::application::cli::error_prefix());
        std::process::exit(error.exit_code());
    }
}
