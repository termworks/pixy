use super::context::RenderContext;
use super::error::{PixyError, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

fn default_width() -> u16 {
    std::env::var("COLUMNS")
        .ok()
        .and_then(|value| value.parse::<u16>().ok())
        .filter(|value| *value > 0)
        .or_else(terminal_width)
        .unwrap_or(80)
}

#[cfg(unix)]
fn terminal_width() -> Option<u16> {
    use std::os::fd::AsRawFd;

    fn from_fd(fd: std::os::fd::RawFd) -> Option<u16> {
        let mut size = libc::winsize {
            ws_row: 0,
            ws_col: 0,
            ws_xpixel: 0,
            ws_ypixel: 0,
        };
        let result = unsafe { libc::ioctl(fd, libc::TIOCGWINSZ, &mut size) };
        (result == 0 && size.ws_col > 0).then_some(size.ws_col)
    }

    [libc::STDOUT_FILENO, libc::STDERR_FILENO, libc::STDIN_FILENO]
        .into_iter()
        .find_map(from_fd)
        .or_else(|| {
            std::fs::File::open("/dev/tty")
                .ok()
                .and_then(|file| from_fd(file.as_raw_fd()))
        })
}

#[cfg(not(unix))]
fn terminal_width() -> Option<u16> {
    None
}

fn default_height() -> u16 {
    1
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum RenderMode {
    #[default]
    Line,
    Run,
    Surface,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum LineTarget {
    Plain,
    #[default]
    Ansi,
    Bash,
    Zsh,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct RenderRequest {
    pub version: u16,
    pub select: Vec<String>,
    pub mode: RenderMode,
    #[serde(default)]
    pub target: Option<LineTarget>,
    #[serde(default = "default_width")]
    pub width: u16,
    #[serde(default = "default_height")]
    pub height: u16,
    #[serde(default)]
    pub now_ms: Option<u64>,
    #[serde(default)]
    pub context: RenderContext,
    #[serde(default)]
    pub ignore_missing: bool,
}

impl Default for RenderRequest {
    fn default() -> Self {
        Self {
            version: 1,
            select: Vec::new(),
            mode: RenderMode::Line,
            target: Some(LineTarget::Ansi),
            width: default_width(),
            height: default_height(),
            now_ms: None,
            context: RenderContext::default(),
            ignore_missing: false,
        }
    }
}

impl RenderRequest {
    pub fn validate(&self) -> Result<()> {
        if self.version != 1 {
            return Err(PixyError::Usage(format!(
                "unsupported request version {}",
                self.version
            )));
        }
        if self.select.is_empty() {
            return Err(PixyError::Usage("at least one selector is required".into()));
        }
        for selector in &self.select {
            if !valid_selector(selector) {
                return Err(PixyError::Usage(format!("invalid selector '{selector}'")));
            }
        }
        if self.mode == RenderMode::Line && self.target.is_none() {
            return Err(PixyError::Usage(
                "line render requests require a target".into(),
            ));
        }
        Ok(())
    }
}

pub fn valid_selector(selector: &str) -> bool {
    let mut chars = selector.chars();
    matches!(chars.next(), Some(first) if first.is_ascii_alphanumeric())
        && chars.all(|ch| ch.is_ascii_alphanumeric() || matches!(ch, '_' | '.' | '-'))
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct Run {
    pub text: String,
    #[serde(default)]
    pub style: String,
}

#[derive(Clone, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub struct OutputRegion {
    pub id: String,
    pub x: u16,
    pub y: u16,
    pub width: u16,
    pub height: u16,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub actions: BTreeMap<String, String>,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub hover_style: String,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub press_styles: BTreeMap<String, String>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(tag = "mode", rename_all = "lowercase")]
pub enum RenderOutput {
    Line {
        text: String,
        width: usize,
        next_frame_ms: Option<u64>,
        #[serde(default, skip_serializing_if = "Vec::is_empty")]
        regions: Vec<OutputRegion>,
    },
    Run {
        runs: Vec<Run>,
        width: usize,
        next_frame_ms: Option<u64>,
        #[serde(default, skip_serializing_if = "Vec::is_empty")]
        regions: Vec<OutputRegion>,
    },
    Surface {
        ansi: String,
        width: u16,
        height: u16,
        next_frame_ms: Option<u64>,
        #[serde(default, skip_serializing_if = "Vec::is_empty")]
        regions: Vec<OutputRegion>,
    },
}

impl RenderOutput {
    pub fn next_frame_ms(&self) -> Option<u64> {
        match self {
            Self::Line { next_frame_ms, .. }
            | Self::Run { next_frame_ms, .. }
            | Self::Surface { next_frame_ms, .. } => *next_frame_ms,
        }
    }

    pub fn regions(&self) -> &[OutputRegion] {
        match self {
            Self::Line { regions, .. }
            | Self::Run { regions, .. }
            | Self::Surface { regions, .. } => regions,
        }
    }
}
