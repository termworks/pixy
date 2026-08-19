fn main() {
    if let Err(error) = pixy::application::cli::run() {
        eprintln!("pixy: {error}");
        std::process::exit(error.exit_code());
    }
}
