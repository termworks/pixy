//! The painter socket.
//!
//! A host connects, sends one request, reads one response and closes. Each
//! message is a four-byte big-endian length followed by one JSON value. The
//! selector in a request is a zone name, so what any view contains is decided
//! by the configuration and never by this file.

use super::engine::Engine;
use crate::model::context::RenderContext;
use crate::model::error::{PixyError, Result};
use crate::model::output::{RenderMode, RenderOutput, RenderRequest};
use crate::runtime::config::{ConfigSource, Paths};
use serde::Deserialize;
use serde_json::{Value, json};
use std::io::{ErrorKind, Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

const MAX_FRAME: usize = 1024 * 1024;
const IO_TIMEOUT: Duration = Duration::from_secs(2);

#[derive(Deserialize)]
struct PainterRequest {
    #[serde(default)]
    select: Vec<String>,
    #[serde(default)]
    mode: Option<RenderMode>,
    #[serde(default)]
    width: Option<u16>,
    #[serde(default)]
    height: Option<u16>,
    #[serde(default)]
    now_ms: Option<u64>,
    #[serde(default)]
    context: Value,
}

pub fn serve(socket: Option<PathBuf>, config: Option<&Path>) -> Result<()> {
    let paths = Paths::discover()?;
    let (mut engine, config_path) = load(config, &paths)?;
    let mut stamp = modified(config_path.as_deref());
    let path = socket_path(socket)?;
    let listener = bind(&path)?;
    eprintln!("pixy serve: listening on {}", path.display());
    for stream in listener.incoming() {
        let Ok(mut stream) = stream else { continue };
        if let Some(next) = reload(config, &paths, config_path.as_deref(), &mut stamp) {
            engine = next;
        }
        let _ = stream.set_read_timeout(Some(IO_TIMEOUT));
        let _ = stream.set_write_timeout(Some(IO_TIMEOUT));
        let _ = answer(&engine, &mut stream);
    }
    let _ = std::fs::remove_file(&path);
    Ok(())
}

fn load(config: Option<&Path>, paths: &Paths) -> Result<(Engine, Option<PathBuf>)> {
    let source = ConfigSource::load(config, paths)?;
    let path = source.path.clone();
    Ok((Engine::load(source, paths)?, path))
}

fn modified(path: Option<&Path>) -> Option<SystemTime> {
    std::fs::metadata(path?)
        .and_then(|data| data.modified())
        .ok()
}

fn reload(
    config: Option<&Path>,
    paths: &Paths,
    path: Option<&Path>,
    stamp: &mut Option<SystemTime>,
) -> Option<Engine> {
    let current = modified(path);
    if current == *stamp {
        return None;
    }
    *stamp = current;
    match load(config, paths) {
        Ok((engine, _)) => Some(engine),
        Err(error) => {
            eprintln!("pixy serve: keeping the last config: {error}");
            None
        }
    }
}

fn answer(engine: &Engine, stream: &mut UnixStream) -> std::io::Result<()> {
    let Some(frame) = read_frame(stream)? else {
        return Ok(());
    };
    let response = match render(engine, &frame) {
        Ok(output) => json!({"version": 1, "ok": true, "output": output}),
        Err(error) => json!({"version": 1, "ok": false, "error": error.to_string()}),
    };
    write_frame(stream, &response)
}

fn render(engine: &Engine, frame: &[u8]) -> Result<RenderOutput> {
    let request = serde_json::from_slice::<PainterRequest>(frame)
        .map_err(|error| PixyError::Usage(format!("invalid request: {error}")))?;
    let request = RenderRequest {
        select: request.select,
        mode: request.mode.unwrap_or(RenderMode::Run),
        target: None,
        width: request.width.unwrap_or(80),
        height: request.height.unwrap_or(1),
        now_ms: request.now_ms,
        context: context(request.context),
        ..RenderRequest::default()
    };
    request.validate()?;
    engine.render(request)
}

/// Flattens a host context into `ctx.values`: the nested `values` map wins over
/// a top-level name of its own, and `env` stays the host environment.
fn context(value: Value) -> RenderContext {
    let mut context = RenderContext::default();
    let Value::Object(map) = value else {
        return context;
    };
    for (name, value) in &map {
        match name.as_str() {
            "values" => {}
            "env" => {
                if let Value::Object(env) = value {
                    for (name, value) in env {
                        context
                            .env
                            .insert(name.clone(), value.as_str().map(str::to_string));
                    }
                }
            }
            _ => {
                context.values.insert(name.clone(), value.clone());
            }
        }
    }
    if let Some(Value::Object(values)) = map.get("values") {
        for (name, value) in values {
            context.values.insert(name.clone(), value.clone());
        }
    }
    context
}

fn read_frame(stream: &mut UnixStream) -> std::io::Result<Option<Vec<u8>>> {
    let mut header = [0_u8; 4];
    if let Err(error) = stream.read_exact(&mut header) {
        return match error.kind() {
            ErrorKind::UnexpectedEof => Ok(None),
            _ => Err(error),
        };
    }
    let length = u32::from_be_bytes(header) as usize;
    if length > MAX_FRAME {
        return Ok(None);
    }
    let mut body = vec![0_u8; length];
    stream.read_exact(&mut body)?;
    Ok(Some(body))
}

fn write_frame(stream: &mut UnixStream, value: &Value) -> std::io::Result<()> {
    let body = serde_json::to_vec(value)?;
    let length = u32::try_from(body.len())
        .map_err(|_| std::io::Error::new(ErrorKind::InvalidData, "response is too large"))?;
    stream.write_all(&length.to_be_bytes())?;
    stream.write_all(&body)?;
    stream.flush()
}

fn bind(path: &Path) -> Result<UnixListener> {
    let _ = std::fs::remove_file(path);
    let listener = UnixListener::bind(path).map_err(|error| {
        PixyError::Transport(format!("failed to bind {}: {error}", path.display()))
    })?;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600))
        .map_err(|error| PixyError::Transport(error.to_string()))?;
    Ok(listener)
}

fn socket_path(socket: Option<PathBuf>) -> Result<PathBuf> {
    if let Some(path) = socket {
        return Ok(path);
    }
    if let Some(path) = std::env::var_os("HEXE_PAINTER_SOCKET") {
        return Ok(PathBuf::from(path));
    }
    let directory = match std::env::var_os("XDG_RUNTIME_DIR") {
        Some(runtime) => PathBuf::from(runtime).join("hexe"),
        None => PathBuf::from(format!("/tmp/hexe-{}", unsafe { libc::getuid() })),
    };
    std::fs::create_dir_all(&directory).map_err(|error| PixyError::Transport(error.to_string()))?;
    Ok(directory.join("painter.sock"))
}
