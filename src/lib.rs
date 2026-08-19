//! Lua-powered terminal rendering engine.

pub mod application;
pub mod model;
pub mod runtime;

pub use application::engine::Engine;
pub use application::{cli, engine};
pub use model::error::{PixyError, Result};
pub use model::output::{LineTarget, OutputRegion, RenderMode, RenderOutput, RenderRequest, Run};
pub use model::{context, error, output};
pub use runtime::{assets, config, host, scheduler};

pub fn name() -> &'static str {
    "pixy"
}

#[cfg(test)]
mod tests {
    #[test]
    fn exposes_name() {
        assert_eq!(super::name(), "pixy");
    }
}
