use std::fmt;

pub type Result<T> = std::result::Result<T, PixyError>;

#[derive(Debug)]
pub enum PixyError {
    Usage(String),
    Config(String),
    Render(String),
    Transport(String),
}

impl PixyError {
    pub fn exit_code(&self) -> i32 {
        match self {
            Self::Usage(_) => 2,
            Self::Config(_) => 3,
            Self::Render(_) => 4,
            Self::Transport(_) => 5,
        }
    }
}

impl fmt::Display for PixyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Usage(message)
            | Self::Config(message)
            | Self::Render(message)
            | Self::Transport(message) => f.write_str(message),
        }
    }
}

impl std::error::Error for PixyError {}
